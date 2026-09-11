/* Autonomous single-player bot. Navigation uses player-sized collision probes,
   never visibility as a substitute for a walkable route. Commands use the same
   movement, weapon, USE and crossing rules as a human player. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <limits.h>
#include "doomstat.h"
#include "p_local.h"
#include "p_spec.h"
#include "p_bot.h"
#include "m_bbox.h"
#include "i_system.h"

boolean bot_active = false;
extern int lookdir, mlook;

#define GRID 8
#define INF 0x3fffffff
#define MAX_CELLS (1024 * 1024)
/* Exact tangency is not a usable corridor: fixed-point thrust drifts by a
   fraction of a unit. In particular, 32-wide gaps between bars are impassable. */
#define BOT_RADIUS (PLAYERRADIUS + FRACUNIT/64)
typedef struct {
    fixed_t floor;
    int stamp, dist, parent, heap;
    boolean clear, comfortable, ledge_safe;
} bot_cell_t;
typedef enum { GO_NONE, GO_USE, GO_WALK, GO_ITEM, GO_EXPLORE, GO_RUN } bot_goaltype_t;
typedef struct {
    bot_goaltype_t type;
    fixed_t x, y, aimx, aimy;
    int line, cell, score;
} bot_goal_t;

static bot_cell_t *cells;
static int *heap, *path, *line_tries, *line_retry, *line_used, *sector_visits, *run_marks;
static int width, height, count, stamp, heap_count, path_len, path_step;
static fixed_t orgx, orgy;
static bot_goal_t goal;
static int next_plan, last_sector, stuck_since, last_progress;
static fixed_t last_x, last_y, progress_dist;
static int use_until, last_keys;
static mobj_t *combat_target, *ignored_target;
static int combat_health, combat_progress, ignore_until;
static fixed_t probe_radius = BOT_RADIUS;
static int commit_forward_until = 0;
static int exit_commit_until = 0;

/* Combat hysteresis.  A target must be genuinely shootable to steal the
   navigation angle.  Short LOS flickers keep the old view angle, but never
   restart combat movement by themselves. */
static boolean combat_has_shot = false;
static int combat_seen_until = 0;
static int combat_commit_until = 0;
static angle_t combat_last_aim = 0;
static fixed_t combat_last_x = 0, combat_last_y = 0;
static int combat_strafe_side = 1;
static int combat_strafe_until = 0;
static int combat_strafe_pause_until = 0;
static fixed_t combat_strafe_tx = 0, combat_strafe_ty = 0;
static fixed_t combat_anchor_x = 0, combat_anchor_y = 0;
static int combat_anchor_sector = -1;
static boolean combat_anchor_valid = false;

/* Incoming-projectile avoidance is independent of target strafing.  Dodges are
   short fixed bursts inside a combat leash; recomputing a 72-unit target from
   the player's NEW position every tic was why the old bot could strafe all the
   way into the next room. */
static int missile_dodge_until = 0;
static int missile_dodge_pause_until = 0;
static fixed_t missile_dodge_tx = 0, missile_dodge_ty = 0;
static fixed_t missile_anchor_x = 0, missile_anchor_y = 0;
static int missile_anchor_until = 0;

/* Precision traversal keeps a recent point with generous drop clearance.  If
   Doom momentum starts carrying the player toward a pit, brake and retreat to
   this anchor instead of trying to correct while already on the lip. */
static fixed_t ledge_anchor_x = 0, ledge_anchor_y = 0;
static boolean ledge_anchor_valid = false;
static int ledge_recover_until = 0;

/* Momentum traversal (classic Doom has no jump button).  GO_RUN means: walk
   normally to a run-up point, align with the landing, then hold full forward
   through a short gap/window. */
static int run_stamp = 0;
static int run_state = 0;              /* 0 idle, 1 align, 2 full-speed run */
static int run_until = 0;
static fixed_t run_sx, run_sy, run_tx, run_ty, run_start_z;
static fixed_t failed_run_sx, failed_run_sy, failed_run_tx, failed_run_ty;
static int failed_run_until = 0;

/* After an off-mesh run, keep pursuing the item that justified the jump.
   Otherwise a nearby ammo pickup can win the next plan and make the bot walk
   back off the ledge before collecting the key. */
static fixed_t run_goal_x, run_goal_y, run_goal_z;
static fixed_t forced_item_x, forced_item_y;
static int forced_item_until = 0;
static boolean forced_item_active = false;

/* A normal USE door is a two-stage interaction: press it, then cross it.
   Planning items immediately after the press used to make the bot turn around
   for ammo while the door was opening. */
static int door_commit_until = 0;
static int door_commit_line = -1;
static int door_commit_side = 0;
static fixed_t door_ax, door_ay, door_tx, door_ty;

/* Switch memory.  Repeatable SR/S1-like progression switches often keep their
   linedef special after a successful activation.  Without memory the planner
   eventually treats the same already-used switch as fresh progression and
   walks back to press it again. */
static int pending_use_line = -1;
static int pending_use_special = 0;
static int pending_use_until = 0;
static int use_fail_line = -1;
static int use_fail_since = 0;

#define USED_SWITCH_HARD_COOLDOWN 350   /* 10 s: never immediately revisit */
#define USED_SWITCH_SOFT_PENALTY  7000  /* later: only if genuinely out of ideas */
#define USE_ALIGN_TIMEOUT         24

#define RUN_MIN_DISTANCE 56
#define RUN_MAX_DISTANCE 176
#define RUN_MIN_RUNUP    28
#define RUN_MAX_GAP      88

static boolean Bot_IsExitSpecial(int special)
{
    return special == 11 || special == 51 || special == 52 || special == 124;
}

static boolean Bot_CanUsePoint(fixed_t x, fixed_t y, angle_t angle, line_t *wanted);

/* Separate scratch state: P_CheckPosition can collect pickups, damage things,
   and overwrite the physics spechit array. Planning must do none of that. */
static struct {
    fixed_t x, y, box[4], floor, ceiling;
    boolean actors;
    mobj_t *self;
} probe;

static boolean Bot_ProbeLine(line_t *li)
{
    if (probe.box[BOXRIGHT] <= li->bbox[BOXLEFT] ||
        probe.box[BOXLEFT] >= li->bbox[BOXRIGHT] ||
        probe.box[BOXTOP] <= li->bbox[BOXBOTTOM] ||
        probe.box[BOXBOTTOM] >= li->bbox[BOXTOP] ||
        P_BoxOnLineSide(probe.box, li) != -1) return true;
    if (!li->backsector || (li->flags & ML_BLOCKING)) return false;
    if (li->frontsector->floorheight > probe.floor) probe.floor = li->frontsector->floorheight;
    if (li->backsector->floorheight > probe.floor) probe.floor = li->backsector->floorheight;
    if (li->frontsector->ceilingheight < probe.ceiling) probe.ceiling = li->frontsector->ceilingheight;
    if (li->backsector->ceilingheight < probe.ceiling) probe.ceiling = li->backsector->ceilingheight;
    return probe.ceiling - probe.floor >= 56 * FRACUNIT;
}

static boolean Bot_ProbeThing(mobj_t *mo)
{
    fixed_t radius;
    if (mo == probe.self || !(mo->flags & MF_SOLID)) return true;
    /* Moving actors are handled by steering and combat, not baked into routes. */
    if (!probe.actors && (mo->flags & MF_SHOOTABLE)) return true;
    radius = mo->radius + probe_radius;
    return abs(mo->x - probe.x) >= radius || abs(mo->y - probe.y) >= radius;
}

static boolean Bot_Position(fixed_t x, fixed_t y, mobj_t *self, boolean actors, fixed_t *floor)
{
    int bx, by, xl, xh, yl, yh;
    sector_t *sec = R_PointInSubsector(x, y)->sector;
    probe.x = x; probe.y = y; probe.self = self; probe.actors = actors;
    probe.floor = sec->floorheight; probe.ceiling = sec->ceilingheight;
    probe.box[BOXLEFT] = x - probe_radius; probe.box[BOXRIGHT] = x + probe_radius;
    probe.box[BOXBOTTOM] = y - probe_radius; probe.box[BOXTOP] = y + probe_radius;
    if (probe.ceiling - probe.floor < 56 * FRACUNIT) return false;
    ++validcount;
    xl = (probe.box[BOXLEFT] - bmaporgx) >> MAPBLOCKSHIFT;
    xh = (probe.box[BOXRIGHT] - bmaporgx) >> MAPBLOCKSHIFT;
    yl = (probe.box[BOXBOTTOM] - bmaporgy) >> MAPBLOCKSHIFT;
    yh = (probe.box[BOXTOP] - bmaporgy) >> MAPBLOCKSHIFT;
    for (by = yl; by <= yh; ++by) for (bx = xl; bx <= xh; ++bx)
        if (!P_BlockLinesIterator(bx, by, Bot_ProbeLine)) return false;
    xl = (probe.box[BOXLEFT] - bmaporgx - MAXRADIUS) >> MAPBLOCKSHIFT;
    xh = (probe.box[BOXRIGHT] - bmaporgx + MAXRADIUS) >> MAPBLOCKSHIFT;
    yl = (probe.box[BOXBOTTOM] - bmaporgy - MAXRADIUS) >> MAPBLOCKSHIFT;
    yh = (probe.box[BOXTOP] - bmaporgy + MAXRADIUS) >> MAPBLOCKSHIFT;
    for (by = yl; by <= yh; ++by) for (bx = xl; bx <= xh; ++bx)
        if (!P_BlockThingsIterator(bx, by, Bot_ProbeThing)) return false;
    if (floor) *floor = probe.floor;
    return true;
}

static boolean Bot_Walk(fixed_t x, fixed_t y, fixed_t z, fixed_t tx, fixed_t ty,
                        mobj_t *self, boolean actors)
{
    int n, steps = (P_AproxDistance(tx-x, ty-y) / FRACUNIT) / 8 + 1;
    fixed_t floor;
    for (n = 1; n <= steps; ++n) {
        fixed_t px = x + (fixed_t)((long long)(tx-x) * n / steps);
        fixed_t py = y + (fixed_t)((long long)(ty-y) * n / steps);
        if (!Bot_Position(px, py, self, actors, &floor) || floor - z > 24*FRACUNIT ||
            probe.ceiling - z < 56*FRACUNIT) return false;
        z = floor;
    }
    return true;
}

/* Normal navigation should not "shortcut" by stepping off a tall ledge.
   Unlike Bot_Walk, this version rejects a sudden downward step too.  We only
   use it when the intended destination is on roughly the current height;
   deliberate one-way drops remain representable by the ordinary flood. */
static boolean Bot_WalkStable(fixed_t x, fixed_t y, fixed_t z, fixed_t tx, fixed_t ty,
                              mobj_t *self, boolean actors)
{
    int n, steps = (P_AproxDistance(tx-x, ty-y) / FRACUNIT) / 8 + 1;
    fixed_t floor;
    for (n = 1; n <= steps; ++n) {
        fixed_t px = x + (fixed_t)((long long)(tx-x) * n / steps);
        fixed_t py = y + (fixed_t)((long long)(ty-y) * n / steps);
        if (!Bot_Position(px, py, self, actors, &floor) ||
            floor - z > 24*FRACUNIT || z - floor > 24*FRACUNIT ||
            probe.ceiling - z < 56*FRACUNIT) return false;
        z = floor;
    }
    return true;
}

static fixed_t Bot_X(int c) { return orgx + (c % width) * (GRID * FRACUNIT); }
static fixed_t Bot_Y(int c) { return orgy + (c / width) * (GRID * FRACUNIT); }

/* Grid distance alone likes to shave corners.  On a high catwalk that means
   walking with the player's centre almost on the drop line, which is legal to
   the pathfinder but fragile under momentum.  Sample support around the cell
   and make such cells expensive (not forbidden -- some maps require them). */
static boolean Bot_LedgeSafe(fixed_t x, fixed_t y)
{
    static const int ox[8] = { 1,-1, 0, 0, 1, 1,-1,-1 };
    static const int oy[8] = { 0, 0, 1,-1, 1,-1, 1,-1 };
    /* Smaller than the player radius on purpose: on a 32-wide stair the
       centre line must remain a valid safe lane, while the 8-unit edge lanes
       should be discouraged. */
    const fixed_t r = PLAYERRADIUS - 3*FRACUNIT;
    fixed_t base = R_PointInSubsector(x,y)->sector->floorheight;
    int i;

    for (i=0; i<8; ++i) {
        fixed_t sx = x + (ox[i] ? (ox[i] > 0 ? r : -r) : 0);
        fixed_t sy = y + (oy[i] ? (oy[i] > 0 ? r : -r) : 0);
        fixed_t sf = R_PointInSubsector(sx,sy)->sector->floorheight;
        if (sf < base - 24*FRACUNIT) return false;
    }
    return true;
}

/* Stronger corridor test used while completing a momentum objective.
   Bot_Position checks collision around the player, but it does not require the
   whole footprint to be supported by floor. Sampling ledge support along the
   segment prevents path smoothing from cutting diagonally across stairwell
   edges after a successful run-jump. */
static boolean Bot_WalkLedgeSafe(fixed_t x, fixed_t y, fixed_t z, fixed_t tx, fixed_t ty,
                                 mobj_t *self, boolean actors)
{
    int n, steps = (P_AproxDistance(tx-x, ty-y) / FRACUNIT) / 4 + 1;
    fixed_t floor;
    fixed_t center_floor = R_PointInSubsector(x,y)->sector->floorheight;
    for (n = 1; n <= steps; ++n) {
        fixed_t px = x + (fixed_t)((long long)(tx-x) * n / steps);
        fixed_t py = y + (fixed_t)((long long)(ty-y) * n / steps);
        fixed_t next_center = R_PointInSubsector(px,py)->sector->floorheight;
        if (!Bot_Position(px, py, self, actors, &floor) ||
            floor - z > 24*FRACUNIT || z - floor > 24*FRACUNIT ||
            center_floor - next_center > 24*FRACUNIT ||
            floor - next_center > 24*FRACUNIT ||
            probe.ceiling - z < 56*FRACUNIT || !Bot_LedgeSafe(px,py))
            return false;
        z = floor;
        center_floor = next_center;
    }
    return true;
}

/* The path graph can be correct and Doom momentum can still carry the player
   over a drop between tics.  Look several momentum steps ahead using the
   player's CENTER (the condition that actually makes Doom change floor).  This
   is intentionally conservative only while finishing an off-mesh objective. */
static boolean Bot_LedgeMomentumDanger(mobj_t *mo)
{
    fixed_t base;
    int n;
    if (!mo) return false;
    base = R_PointInSubsector(mo->x,mo->y)->sector->floorheight;

    /* Five tics was too late on MAP02: by the time the predicted centre crossed
       the hole, normal Doom momentum could already be impossible to cancel in
       one command.  Look farther ahead.  This intentionally overestimates the
       travel because real Doom friction will reduce momx/momy each tic. */
    for (n=1; n<=10; ++n) {
        fixed_t px = mo->x + (fixed_t)((long long)mo->momx*n);
        fixed_t py = mo->y + (fixed_t)((long long)mo->momy*n);
        fixed_t pf = R_PointInSubsector(px,py)->sector->floorheight;
        if (pf < base - 24*FRACUNIT) return true;
    }
    return false;
}

/* Approximate distance from a point to a REACHABLE tall drop.  Raw
   R_PointInSubsector samples can see a low sector through a solid wall, so a
   low sample only counts when ordinary movement from the centre could reach
   it.  The value is only used during the short post-momentum key traversal. */
static int Bot_DropClearance(fixed_t x, fixed_t y)
{
    static const int radii[] = { 8, 12, 16, 20, 24, 28, 32, 40 };
    int r, k;
    fixed_t base = R_PointInSubsector(x,y)->sector->floorheight;

    for (r=0; r<(int)(sizeof(radii)/sizeof(radii[0])); ++r) {
        for (k=0; k<16; ++k) {
            int an=(k*FINEANGLES)/16;
            fixed_t sx=x+FixedMul(radii[r]*FRACUNIT,finecosine[an]);
            fixed_t sy=y+FixedMul(radii[r]*FRACUNIT,finesine[an]);
            fixed_t sf=R_PointInSubsector(sx,sy)->sector->floorheight;
            if (sf >= base-24*FRACUNIT) continue;
            if (Bot_Walk(x,y,base,sx,sy,NULL,false))
                return radii[r];
        }
    }
    return 48;
}

/* Never aim directly at the item after the flood path has ended.  On MAP02
   that final innocent-looking vector is exactly what cuts across the hole in
   front of the red key.  The goal cell was selected because it is reachable;
   find a nearby, supported pickup point and let Doom's pickup radius do the
   rest. */
static boolean Bot_FindSafeItemApproach(mobj_t *mo, fixed_t ix, fixed_t iy, int cell,
                                        fixed_t *outx, fixed_t *outy)
{
    static const int ring[] = { 0, 8, 16, 24, 28 };
    fixed_t bestx=0, besty=0;
    int best=INT_MAX, r, k;

    if (!mo || !outx || !outy) return false;

    /* The flood-selected cell is the best first candidate and is normally
       already within pickup distance of a key. */
    if (cell >= 0 && cell < count) {
        fixed_t x=Bot_X(cell), y=Bot_Y(cell), floor;
        if (Bot_Position(x,y,mo,true,&floor) && abs(floor-mo->z) <= 24*FRACUNIT &&
            Bot_LedgeSafe(x,y) && Bot_WalkLedgeSafe(mo->x,mo->y,mo->z,x,y,mo,true)) {
            bestx=x; besty=y;
            best=P_AproxDistance(x-mo->x,y-mo->y)/FRACUNIT;
        }
    }

    /* If the grid centre is awkward, search a small ring around the item.  We
       stay within 28 units, comfortably inside the usual player+pickup touch
       radius, but reject any point whose footprint faces a tall drop. */
    for (r=0; r<(int)(sizeof(ring)/sizeof(ring[0])); ++r) {
        for (k=0; k<16; ++k) {
            int an=(k*FINEANGLES)/16;
            fixed_t x=ix + FixedMul(ring[r]*FRACUNIT,finecosine[an]);
            fixed_t y=iy + FixedMul(ring[r]*FRACUNIT,finesine[an]);
            fixed_t floor;
            int score;
            if (!Bot_Position(x,y,mo,true,&floor) || abs(floor-mo->z)>24*FRACUNIT) continue;
            if (!Bot_LedgeSafe(x,y)) continue;
            if (!Bot_WalkLedgeSafe(mo->x,mo->y,mo->z,x,y,mo,true)) continue;
            score=P_AproxDistance(x-mo->x,y-mo->y)/FRACUNIT;
            if (score < best) { best=score; bestx=x; besty=y; }
        }
    }
    if (best==INT_MAX) return false;
    *outx=bestx; *outy=besty;
    return true;
}

static boolean Bot_Cell(int c)
{
    if (cells[c].stamp != stamp) {
        cells[c].stamp = stamp; cells[c].dist = INF; cells[c].parent = -1; cells[c].heap = -1;
        cells[c].clear = Bot_Position(Bot_X(c), Bot_Y(c), NULL, false, &cells[c].floor);
        probe_radius = PLAYERRADIUS+2*FRACUNIT;
        cells[c].comfortable = cells[c].clear && Bot_Position(Bot_X(c), Bot_Y(c),NULL,false,NULL);
        probe_radius = BOT_RADIUS;
        cells[c].ledge_safe = cells[c].clear && Bot_LedgeSafe(Bot_X(c),Bot_Y(c)) &&
            cells[c].floor - R_PointInSubsector(Bot_X(c),Bot_Y(c))->sector->floorheight <= 24*FRACUNIT;
    }
    return cells[c].clear;
}

static void Bot_HeapUp(int pos)
{
    int c = heap[pos];
    while (pos && cells[heap[(pos-1)/2]].dist > cells[c].dist) {
        heap[pos] = heap[(pos-1)/2]; cells[heap[pos]].heap = pos; pos = (pos-1)/2;
    }
    heap[pos] = c; cells[c].heap = pos;
}
static int Bot_HeapPop(void)
{
    int result = heap[0], c = heap[--heap_count], pos = 0;
    while (pos*2+1 < heap_count) {
        int child = pos*2+1;
        if (child+1 < heap_count && cells[heap[child+1]].dist < cells[heap[child]].dist) ++child;
        if (cells[c].dist <= cells[heap[child]].dist) break;
        heap[pos] = heap[child]; cells[heap[pos]].heap = pos; pos = child;
    }
    if (heap_count) { heap[pos] = c; cells[c].heap = pos; }
    cells[result].heap = -2;
    return result;
}

static int Bot_NearCell(fixed_t x, fixed_t y, fixed_t z, boolean reached)
{
    int gx = (x-orgx + GRID*FRACUNIT/2) / (GRID*FRACUNIT);
    int gy = (y-orgy + GRID*FRACUNIT/2) / (GRID*FRACUNIT);
    int dx, dy, best = -1, score = INF;
    for (dy = -2; dy <= 2; ++dy) for (dx = -2; dx <= 2; ++dx) {
        int nx = gx+dx, ny = gy+dy, c, d;
        if (nx < 0 || ny < 0 || nx >= width || ny >= height) continue;
        c = ny*width+nx;
        if (reached && (cells[c].stamp != stamp || cells[c].dist == INF)) continue;
        if (!Bot_Cell(c)) continue;
        d = P_AproxDistance(x-Bot_X(c), y-Bot_Y(c)) / FRACUNIT;
        if (reached) d += cells[c].dist;
        if (d >= score) continue;
        if (reached ? !Bot_Walk(Bot_X(c), Bot_Y(c), cells[c].floor, x,y, NULL,false) :
                      !Bot_Walk(x,y,z, Bot_X(c),Bot_Y(c), NULL,false)) continue;
        score = d; best = c;
    }
    return best;
}


/* A conservative physical test for a Doom-style "jump": there is no jump
   command, so the player must build horizontal momentum on a high ledge and
   cross a short lower-floor gap before gravity drops him too far.  We reject
   walls, low ceilings, upward obstacles and routes that are already walkable. */
static boolean Bot_RunLinkGeometry(int start, int land)
{
    fixed_t sx, sy, tx, ty, start_floor, end_floor;
    fixed_t floor;
    double dx, dy, len_units, step_units;
    int steps, n, gap_first = -1, gap_last = -1;

    if (start < 0 || land < 0 || start >= count || land >= count) return false;
    if (!Bot_Cell(start) || !Bot_Cell(land) || !cells[start].comfortable) return false;

    sx = Bot_X(start); sy = Bot_Y(start);
    tx = Bot_X(land);  ty = Bot_Y(land);
    start_floor = cells[start].floor;
    end_floor = cells[land].floor;

    dx = (double)tx - sx;
    dy = (double)ty - sy;
    len_units = sqrt(dx*dx + dy*dy) / FRACUNIT;
    if (len_units < RUN_MIN_DISTANCE || len_units > RUN_MAX_DISTANCE) return false;

    /* No special traversal is needed if normal grounded movement works. */
    if (Bot_Walk(sx, sy, start_floor, tx, ty, NULL, false)) return false;

    /* Landing at roughly the launch height is what running gaps can solve.
       Large upward steps are not magically made possible by momentum. */
    if (end_floor > start_floor + 24*FRACUNIT ||
        end_floor < start_floor - 40*FRACUNIT) return false;

    steps = (int)(len_units / 4.0) + 1;
    step_units = len_units / steps;

    for (n = 0; n <= steps; ++n) {
        fixed_t px = sx + (fixed_t)((long long)(tx-sx) * n / steps);
        fixed_t py = sy + (fixed_t)((long long)(ty-sy) * n / steps);

        if (!Bot_Position(px, py, NULL, false, &floor)) return false;

        /* Test clearance at launch height instead of snapping the player to the
           bottom of the gap like Bot_Walk does. */
        if (probe.ceiling < start_floor + 56*FRACUNIT) return false;
        if (floor > start_floor + 24*FRACUNIT) return false;

        if (floor < start_floor - 24*FRACUNIT) {
            if (gap_first < 0) gap_first = n;
            gap_last = n;
        }
    }

    if (gap_first < 0) return false;
    if (gap_first * step_units < RUN_MIN_RUNUP) return false;
    if ((gap_last - gap_first + 1) * step_units > RUN_MAX_GAP) return false;
    if ((steps - gap_last) * step_units < 6.0) return false;

    return true;
}

/* Mark the ordinary-walk component containing an otherwise unreachable item,
   then look backwards from its boundary for a reachable straight run-up.  This
   keeps expensive momentum-link discovery focused on useful unreachable items
   instead of adding long edges from every navigation cell. */
static boolean Bot_RunCandidateToPoint(fixed_t x, fixed_t y, fixed_t z, int priority)
{
    static const int ndx[8] = {1,-1,0,0,1,1,-1,-1};
    static const int ndy[8] = {0,0,1,-1,1,-1,1,-1};
    static const int rdx[16] = {1,-1,0,0, 1,1,-1,-1, 2,2,-2,-2, 1,1,-1,-1};
    static const int rdy[16] = {0,0,1,-1, 1,-1,1,-1, 1,-1,1,-1, 2,-2,2,-2};
    int seed, head = 0, tail = 0, qi;
    int best_start = -1, best_land = -1, best_score = INF;

    if (!run_marks) return false;
    seed = Bot_NearCell(x, y, z, false);
    if (seed < 0) return false;

    if (++run_stamp <= 0) {
        memset(run_marks, 0, count*sizeof(*run_marks));
        run_stamp = 1;
    }

    run_marks[seed] = run_stamp;
    path[tail++] = seed; /* path is scratch until the final goal path is built */

    while (head < tail) {
        int u = path[head++], ux = u%width, uy = u/width, d;
        for (d = 0; d < 8; ++d) {
            int nx = ux+ndx[d], ny = uy+ndy[d], v;
            if (nx < 0 || ny < 0 || nx >= width || ny >= height) continue;
            v = ny*width+nx;
            if (run_marks[v] == run_stamp || !Bot_Cell(v)) continue;

            /* Reverse flood: v belongs if a player standing at v can normally
               walk to u, eventually reaching the item. */
            if (cells[u].floor - cells[v].floor > 24*FRACUNIT) continue;
            if (!Bot_Walk(Bot_X(v),Bot_Y(v),cells[v].floor,
                          Bot_X(u),Bot_Y(u),NULL,false)) continue;

            run_marks[v] = run_stamp;
            if (tail < count) path[tail++] = v;
        }
    }

    for (qi = 0; qi < tail; ++qi) {
        int land = path[qi], lx = land%width, ly = land/width;
        int boundary = 0, d, k;

        for (d = 0; d < 8; ++d) {
            int nx=lx+ndx[d], ny=ly+ndy[d];
            if (nx < 0 || ny < 0 || nx >= width || ny >= height ||
                run_marks[ny*width+nx] != run_stamp) { boundary = 1; break; }
        }
        if (!boundary) continue;

        for (d = 0; d < 16; ++d) {
            for (k = 3; k <= 20; ++k) {
                int sx = lx-rdx[d]*k, sy = ly-rdy[d]*k, start, score;
                fixed_t dsx, dsy;
                if (sx < 0 || sy < 0 || sx >= width || sy >= height) continue;
                start = sy*width+sx;
                if (cells[start].stamp != stamp || cells[start].dist == INF) continue;

                dsx = Bot_X(land)-Bot_X(start);
                dsy = Bot_Y(land)-Bot_Y(start);
                if (P_AproxDistance(dsx,dsy) < RUN_MIN_DISTANCE*FRACUNIT ||
                    P_AproxDistance(dsx,dsy) > RUN_MAX_DISTANCE*FRACUNIT) continue;

                if (leveltime < failed_run_until &&
                    P_AproxDistance(Bot_X(start)-failed_run_sx,Bot_Y(start)-failed_run_sy) < 12*FRACUNIT &&
                    P_AproxDistance(Bot_X(land)-failed_run_tx,Bot_Y(land)-failed_run_ty) < 12*FRACUNIT)
                    continue;

                if (!Bot_RunLinkGeometry(start, land)) continue;

                score = cells[start].dist + priority +
                        P_AproxDistance(dsx,dsy)/FRACUNIT * 2 + 350;
                /* Prefer a run-up whose staging cell is not itself hanging over
                   a side edge.  Keep unsafe starts possible for narrow WADs. */
                if (!cells[start].ledge_safe) score += 500;
                if (score < best_score) {
                    best_score = score;
                    best_start = start;
                    best_land = land;
                }
            }
        }
    }

    if (best_start < 0 || best_score >= goal.score) return false;

    goal.type = GO_RUN;
    goal.x = Bot_X(best_start); goal.y = Bot_Y(best_start);
    goal.aimx = Bot_X(best_land); goal.aimy = Bot_Y(best_land);
    goal.line = -1; goal.cell = best_start; goal.score = best_score;
    run_goal_x = x; run_goal_y = y; run_goal_z = z;
    I_Log("Bot: momentum link run=(%d,%d) land=(%d,%d) score=%d\n",
          goal.x/FRACUNIT,goal.y/FRACUNIT,goal.aimx/FRACUNIT,goal.aimy/FRACUNIT,best_score);
    return true;
}

static void Bot_Flood(player_t *player)
{
    static const int dx[8] = {1,-1,0,0,1,1,-1,-1};
    static const int dy[8] = {0,0,1,-1,1,-1,1,-1};
    int start, i;
    boolean protect_ledge = (forced_item_active && leveltime < forced_item_until);
    ++stamp; heap_count = 0;
    start = Bot_NearCell(player->mo->x, player->mo->y, player->mo->z, false);
    if (start < 0) return;
    cells[start].dist = 0; heap[heap_count++] = start; cells[start].heap = 0;
    while (heap_count) {
        int u = Bot_HeapPop(), ux = u%width, uy = u/width;
        for (i = 0; i < 8; ++i) {
            int vx = ux+dx[i], vy = uy+dy[i], v, cost;
            if (vx < 0 || vy < 0 || vx >= width || vy >= height) continue;
            v = vy*width+vx;
            if (!Bot_Cell(v) || cells[v].heap == -2 || cells[v].floor - cells[u].floor > 24*FRACUNIT) continue;

            /* After a momentum landing the objective is usually above a fatal
               drop (MAP02's red-key house is the canonical case). During that
               short sticky phase, do not let Dijkstra save distance by walking
               off the stair/ledge and climbing around again. */
            if (protect_ledge && cells[u].floor - cells[v].floor > 24*FRACUNIT) continue;
            /* A cost penalty still admits pit cells. Use the same hard support
               rule as the controller, including the floor under the centre. */
            if (protect_ledge && !cells[v].ledge_safe) continue;

            /* The MAP02 red-key staircase exposed a subtler failure: even if
               both corner cells are individually valid, a diagonal edge lets
               steering aim across the inside of the stairwell and momentum
               carries the player over the drop.  The protected route is short,
               so use only cardinal grid edges until the forced item is picked. */
            if (protect_ledge && i >= 4) continue;

            cost = cells[u].dist + (i<4 ? GRID : 11);
            if (!cells[v].comfortable) cost += 24;
            /* Strongly prefer the middle of ledges/stairs. Still traversable
               when a map gives us no safer route. */
            if (!cells[v].ledge_safe) cost += protect_ledge ? 4000 : 220;
            if (cells[u].floor - cells[v].floor > 24*FRACUNIT) {
                int drop = (cells[u].floor-cells[v].floor)/FRACUNIT;
                /* Falling remains legal, but it is a last resort.  Without this
                   the shortest path up a stairwell can literally be "walk off
                   the inner edge", which destroys progress toward an upper key. */
                cost += 700 + (drop-24)*4;
            }
            if (R_PointInSubsector(Bot_X(v),Bot_Y(v))->sector->special == 5 ||
                R_PointInSubsector(Bot_X(v),Bot_Y(v))->sector->special == 7 ||
                R_PointInSubsector(Bot_X(v),Bot_Y(v))->sector->special == 16) cost += 48;
            if (cost >= cells[v].dist || !Bot_Walk(Bot_X(u),Bot_Y(u),cells[u].floor,
                    Bot_X(v),Bot_Y(v),NULL,false)) continue;
            if (protect_ledge && !Bot_WalkLedgeSafe(Bot_X(u),Bot_Y(u),cells[u].floor,
                    Bot_X(v),Bot_Y(v),NULL,false)) continue;
            cells[v].dist = cost; cells[v].parent = u;
            if (cells[v].heap < 0) { cells[v].heap = heap_count; heap[heap_count++] = v; }
            Bot_HeapUp(cells[v].heap);
        }
    }
}

static boolean Bot_HasKey(player_t *p, int key)
{
    return key < 0 || p->cards[key] || p->cards[key+3];
}
static int Bot_Key(int special)
{
    switch (special) {
        case 26: case 32: case 99: case 133: return it_bluecard;
        case 28: case 33: case 134: case 135: return it_redcard;
        case 27: case 34: case 136: case 137: return it_yellowcard;
    }
    return -1;
}
static boolean Bot_Manual(int s)
{
    return s==1 || s==26 || s==27 || s==28 || s==31 || s==32 || s==33 || s==34 || s==117 || s==118;
}
static boolean Bot_UseSpecial(int s)
{
    if (Bot_Manual(s)) return true;
    switch (s) {
        case 7: case 9: case 11: case 14: case 15: case 18: case 20: case 21:
        case 23: case 29: case 45: case 51: case 55: case 60: case 61: case 62:
        case 63: case 64: case 66: case 67: case 68: case 69: case 70: case 71:
        case 99: case 101: case 102: case 103: case 111: case 112: case 114:
        case 115: case 122: case 123: case 127: case 131: case 132: case 133:
        case 134: case 135: case 136: case 137: case 140: return true;
    }
    return false;
}
static boolean Bot_WalkSpecial(int s)
{
    switch (s) {
        case 2: case 3: case 4: case 5: case 6: case 8: case 10: case 12: case 13:
        case 16: case 17: case 19: case 22: case 25: case 30: case 35: case 36:
        case 37: case 38: case 39: case 40: case 44: case 52: case 53: case 54:
        case 56: case 57: case 58: case 59: case 72: case 73: case 74: case 75:
        case 76: case 77: case 79: case 80: case 81: case 82: case 83: case 84:
        case 86: case 87: case 88: case 89: case 90: case 91: case 92: case 93:
        case 94: case 95: case 96: case 97: case 98: case 100: case 104: case 105:
        case 106: case 107: case 108: case 109: case 110: case 119: case 120:
        case 121: case 124: case 125: case 126: case 128: case 129: case 130: return true;
    }
    return false;
}

static boolean Bot_Candidate(bot_goaltype_t type, fixed_t x, fixed_t y, fixed_t ax,
                             fixed_t ay, int line, int priority)
{
    int c = Bot_NearCell(x,y,0,true), score;
    if (c < 0) return false;
    score = cells[c].dist + priority;
    if (score < goal.score) {
        goal.type = type; goal.x = x; goal.y = y; goal.aimx = ax; goal.aimy = ay;
        goal.line = line; goal.cell = c; goal.score = score;
    }
    return true; /* reachable, even if another goal currently scores better */
}

static int Bot_ItemPriority(player_t *p, mobj_t *mo)
{
    switch (mo->sprite) {
        case SPR_BKEY: case SPR_BSKU: return Bot_HasKey(p, it_bluecard) ? INF : -2500;
        case SPR_RKEY: case SPR_RSKU: return Bot_HasKey(p, it_redcard)  ? INF : -2500;
        case SPR_YKEY: case SPR_YSKU: return Bot_HasKey(p, it_yellowcard)? INF : -2500;

        case SPR_SHOT: return p->weaponowned[wp_shotgun] ? (p->ammo[am_shell] < 24 ? -800 : INF) : -3500;
        case SPR_SGN2: return p->weaponowned[wp_supershotgun] ? INF : -2500;
        case SPR_MGUN: return p->weaponowned[wp_chaingun] ? INF : -1800;
        case SPR_PLAS: return p->weaponowned[wp_plasma] ? INF : -2000;
        case SPR_LAUN: return p->weaponowned[wp_missile] ? INF : -900;
        case SPR_BFUG: return p->weaponowned[wp_bfg] ? INF : -800;

        case SPR_STIM: case SPR_MEDI:
            if (p->health >= 90) return INF;
            if (p->health < 40) return -10000;
            return -1800;

        case SPR_SOUL: case SPR_MEGA:
            return p->health < 180 ? -2200 : INF;

        case SPR_ARM1: case SPR_ARM2:
            return p->armorpoints < 100 ? -1400 : INF;

        /* === Патроны — теперь подбираем заранее === */
        case SPR_CLIP: case SPR_AMMO:
            if (p->ammo[am_clip] < 50) return -1600;   // было 30
            if (p->ammo[am_clip] < 100) return -600;   // даже когда средне — всё равно полезно
            return INF;

        case SPR_SHEL: case SPR_SBOX:
            if (!p->weaponowned[wp_shotgun] && !p->weaponowned[wp_supershotgun]) return INF;
            if (p->ammo[am_shell] < 20) return -1800;
            if (p->ammo[am_shell] < 40) return -700;
            return INF;

        case SPR_CELL: case SPR_CELP:
            if (!p->weaponowned[wp_plasma] && !p->weaponowned[wp_bfg]) return INF;
            if (p->ammo[am_cell] < 60) return -1600;
            if (p->ammo[am_cell] < 120) return -600;
            return INF;

        case SPR_ROCK: case SPR_BROK:
            if (!p->weaponowned[wp_missile]) return INF;
            if (p->ammo[am_misl] < 6) return -1200;
            if (p->ammo[am_misl] < 12) return -500;
            return INF;

        case SPR_BPAK:
            return !p->backpack ? -900 : INF;
    }
    return INF;
}

/* Build a staging point on Doom's usable (front) side of a special line.
   This removes dependence on the linedef's authoring direction for wall
   buttons and recessed switches. */
static void Bot_UseApproachPoint(line_t *li, fixed_t ax, fixed_t ay, double len,
                                 fixed_t *x, fixed_t *y)
{
    fixed_t nx = (fixed_t)((double)li->dy/len * 40*FRACUNIT);
    fixed_t ny = (fixed_t)(-(double)li->dx/len * 40*FRACUNIT);
    *x = ax + nx; *y = ay + ny;
    if (P_PointOnLineSide(*x,*y,li) != 0) {
        *x = ax - nx; *y = ay - ny;
    }
}

/* Manual doors already have a sector thinker while they are opening/closing.
   Re-pressing USE during that motion only produces the click-spam seen on
   exit/secret doors. */
static boolean Bot_LineMoving(line_t *li)
{
    if (!li) return false;
    return (li->frontsector && li->frontsector->specialdata) ||
           (li->backsector  && li->backsector->specialdata);
}

/* Remote switches usually move sectors selected by linedef tag, not one of the
   two sectors touching the switch itself.  Check both cases so a successful
   press can be distinguished from a missed USE. */
static boolean Bot_LineActionActive(line_t *li)
{
    int i;
    if (!li) return false;
    if (Bot_LineMoving(li)) return true;
    if (!li->tag) return false;
    for (i = 0; i < numsectors; ++i)
        if (sectors[i].tag == li->tag && sectors[i].specialdata) return true;
    return false;
}

static void Bot_CheckPendingUse(void)
{
    line_t *li;
    boolean fired;

    if (pending_use_line < 0) return;
    if (pending_use_line >= numlines) {
        pending_use_line = -1;
        return;
    }

    li = &lines[pending_use_line];
    fired = li->special != pending_use_special || Bot_LineActionActive(li);
    if (fired) {
        line_used[pending_use_line] = leveltime;
        I_Log("Bot: confirmed switch line=%d special=%d\n",
              pending_use_line,pending_use_special);
        pending_use_line = -1;
        return;
    }

    /* Give P_UseLines / thinker spawning a couple of tics.  If nothing changed,
       treat it as a missed click and do NOT poison this switch as already used. */
    if (leveltime >= pending_use_until) pending_use_line = -1;
}

static void Bot_Plan(player_t *p)
{
    int i, n;
    thinker_t *th;
    boolean forced_seen = false;
    Bot_Flood(p);
	boolean prefer_forward = (leveltime < commit_forward_until);
    goal.type = GO_NONE; goal.score = INF; goal.line = -1; path_len = path_step = 0;
    for (i = 0; i < numlines; ++i) {
        line_t *li = &lines[i];
        int special = li->special, priority;
        double dx = (double)li->dx, dy = (double)li->dy, len = sqrt(dx*dx + dy*dy);
        if (!special || len < FRACUNIT || line_retry[i] > leveltime || !Bot_HasKey(p,Bot_Key(special))) continue;
        if (Bot_UseSpecial(special)) {
            int used_age = line_used[i] ? leveltime-line_used[i] : INT_MAX;

            /* A remote/repeatable switch that already fired is not fresh
               progression.  Hard-ignore it for a while, then keep a large
               penalty so unexplored space, keys and new switches win.  Manual
               doors and exits have their own continuation semantics. */
            if (!Bot_Manual(special) && !Bot_IsExitSpecial(special) && line_used[i]) {
                if (used_age < USED_SWITCH_HARD_COOLDOWN) continue;
            }

            /* Already-open manual doors need no interaction.  Test the actual
               line opening, not just the back sector's nominal height. */
            if (Bot_Manual(special) && li->backsector) {
                P_LineOpening(li);
                if (openrange >= 56*FRACUNIT) {
                    /* An opened door that we already touched is unfinished
                       traversal. Reacquire its far side instead of turning back
                       for ammo when the explicit door commit expires. */
                    if (line_tries[i] > 0) {
                        int side = P_PointOnLineSide(p->mo->x,p->mo->y,li) ? 1 : -1;
                        for (n = 1; n <= 3; ++n) {
                            fixed_t ax = li->v1->x + (fixed_t)((long long)li->dx*n/4);
                            fixed_t ay = li->v1->y + (fixed_t)((long long)li->dy*n/4);
                            Bot_Candidate(GO_WALK,
                                ax+(fixed_t)(side*dy/len*56*FRACUNIT),
                                ay-(fixed_t)(side*dx/len*56*FRACUNIT),
                                ax,ay,i,-5000);
                        }
                    }
                    continue;
                }
            }
            /* One missed click must not make a progression switch lose to
               generic exploration. Attempts are only a mild tie-breaker. */
            priority = special==11 ? -20000 : special==51 ? -15000 :
                       250 + (line_tries[i] > 4 ? 4 : line_tries[i]) * 250;
            if (!Bot_Manual(special) && !Bot_IsExitSpecial(special) && line_used[i])
                priority += USED_SWITCH_SOFT_PENALTY;
            if (prefer_forward && special != 11 && special != 51) {
                priority += 3000;   // во время commit сильно не любит другие двери/секретки
            }
            for (n = 1; n <= 3; ++n) {
                fixed_t ax = li->v1->x + (fixed_t)((long long)li->dx*n/4);
                fixed_t ay = li->v1->y + (fixed_t)((long long)li->dy*n/4);
                fixed_t x, y;
                angle_t a;
                Bot_UseApproachPoint(li,ax,ay,len,&x,&y);
                a = R_PointToAngle2(x,y,ax,ay);
                /* Do not choose a quarter-point whose USE ray is occluded by
                   the switch recess/corner.  The old planner could walk to such
                   a point forever because reaching the staging point was
                   mistaken for being able to operate the linedef. */
                if (!Bot_CanUsePoint(x,y,a,li)) continue;
                Bot_Candidate(GO_USE,x,y,ax,ay,i,priority);
            }
        } else if (Bot_WalkSpecial(special) && li->backsector) {
            int side = P_PointOnLineSide(p->mo->x,p->mo->y,li) ? 1 : -1;
            priority = special==52 ? -20000 : special==124 ? -15000 : 800+line_tries[i]*1000;
            for (n = 1; n <= 3; ++n) {
                fixed_t ax = li->v1->x+(fixed_t)((long long)li->dx*n/4);
                fixed_t ay = li->v1->y+(fixed_t)((long long)li->dy*n/4);
                Bot_Candidate(GO_WALK,ax+(fixed_t)(side*dy/len*40*FRACUNIT),
                    ay-(fixed_t)(side*dx/len*40*FRACUNIT),ax,ay,i,priority);
            }
        }
    }
    for (th = thinkercap.next; th != &thinkercap; th = th->next) {
        mobj_t *mo;
        int priority;
        if (th->function.acp1 != (actionf_p1)P_MobjThinker) continue;
        mo = (mobj_t*)th;
        if (!(mo->flags & MF_SPECIAL)) continue;
        priority = Bot_ItemPriority(p,mo);
        if (priority != INF) {
            if (forced_item_active && leveltime < forced_item_until &&
                P_AproxDistance(mo->x-forced_item_x,mo->y-forced_item_y) < 48*FRACUNIT) {
                /* Finish what the momentum link was for before reconsidering
                   incidental ammo/health on the landing island.  Refresh the
                   watchdog while the original item still physically exists. */
                priority -= 24000;
                forced_seen = true;
                forced_item_until = leveltime + 2100;
            }
            if (prefer_forward) {
                // Во время commit почти полностью игнорируем предметы
                // (кроме критического здоровья)
                if (priority > -5000) priority += 2500;
            }
            if (!Bot_Candidate(GO_ITEM, mo->x, mo->y, mo->x, mo->y, -1, priority)) {
                /* Keys and other high-value items can live on islands that are
                   reachable only by carrying running momentum over a gap. */
                if (priority <= -2400)
                    Bot_RunCandidateToPoint(mo->x, mo->y, mo->z, priority);
            }
        }
    }
    if (forced_item_active && (!forced_seen || leveltime >= forced_item_until)) {
        /* The thinker disappeared: the item was picked up (or otherwise
           removed). Only now may ordinary navigation replace precision mode. */
        forced_item_active = false;
        forced_item_until = 0;
        I_Log("Bot: forced item lock released\n");
    }

    /* Explore reachable sectors when no useful interaction/item/exit is known.
       This also crosses unmarked stairs and visits boss arenas. */
    for (i = 0; i < count; ++i) {
        int sec, score;
        if (cells[i].stamp != stamp || cells[i].dist == INF || cells[i].dist < 96) continue;
        sec = R_PointInSubsector(Bot_X(i),Bot_Y(i))->sector-sectors;
        score = 1800 + cells[i].dist + sector_visits[sec]*2000;
        if (prefer_forward) {
            /* After using an interaction, exploration must become LESS attractive.
               The old -600 made the bot more willing to wander off. */
            score += 2500;
        }
        if (score < goal.score) {
            goal.type = GO_EXPLORE; goal.line = -1; goal.cell = i; goal.x = Bot_X(i); goal.y = Bot_Y(i); goal.score = score;
        }
    }
    if (goal.type != GO_NONE) {
        i = goal.cell;
        while (i >= 0 && path_len < count) { path[path_len++] = i; i = cells[i].parent; }
        for (i = 0; i < path_len/2; ++i) { n = path[i]; path[i] = path[path_len-1-i]; path[path_len-1-i] = n; }
        path_step = 0;
    }
    next_plan = leveltime+350;
    last_progress = leveltime; progress_dist = INF;
    I_Log("Bot: plan type=%d line=%d goal=(%d,%d) path=%d\n",goal.type,goal.line,goal.x/FRACUNIT,goal.y/FRACUNIT,path_len);
}

void Bot_InitLevel(void)
{
    long long total;
    free(cells); free(heap); free(path); free(line_tries); free(line_retry); free(line_used); free(sector_visits); free(run_marks);
    cells = NULL; heap = path = line_tries = line_retry = line_used = sector_visits = run_marks = NULL;
    orgx = (bmaporgx >> (FRACBITS+3)) * (GRID*FRACUNIT);
    orgy = (bmaporgy >> (FRACBITS+3)) * (GRID*FRACUNIT);
    width = bmapwidth*(128/GRID)+2; height = bmapheight*(128/GRID)+2; total = (long long)width*height;
    count = 0;
    if (total <= 0 || total > MAX_CELLS) { I_Log("Bot: level exceeds navigation grid limit\n"); return; }
    count = (int)total;
    cells = calloc(count,sizeof(*cells)); heap = malloc(count*sizeof(*heap)); path = malloc(count*sizeof(*path));
    line_tries = calloc(numlines,sizeof(int)); line_retry = calloc(numlines,sizeof(int)); line_used = calloc(numlines,sizeof(int)); sector_visits = calloc(numsectors,sizeof(int));
    run_marks = calloc(count,sizeof(*run_marks));
    if (!cells || !heap || !path || !line_tries || !line_retry || !line_used || !sector_visits || !run_marks) I_Error("Bot: navigation allocation failed");
    stamp = 0; path_len = path_step = 0; goal.type = GO_NONE;
    next_plan = use_until = 0; last_sector = -1; last_keys = 0;
    stuck_since = last_progress = 0; last_x = last_y = 0;
    combat_target = ignored_target = NULL; combat_progress = ignore_until = 0;
    combat_has_shot = false; combat_seen_until = combat_commit_until = 0; combat_last_aim = 0;
    combat_last_x = combat_last_y = 0;
    combat_strafe_side = 1; combat_strafe_until = 0; combat_strafe_pause_until = 0;
    combat_strafe_tx = combat_strafe_ty = 0;
    combat_anchor_x = combat_anchor_y = 0; combat_anchor_sector = -1; combat_anchor_valid = false;
    missile_dodge_until = 0; missile_dodge_pause_until = 0;
    missile_dodge_tx = missile_dodge_ty = 0;
    missile_anchor_x = missile_anchor_y = 0; missile_anchor_until = 0;
    ledge_anchor_x = ledge_anchor_y = 0; ledge_anchor_valid = false; ledge_recover_until = 0;
    commit_forward_until = 0;
    exit_commit_until = 0;
    run_stamp = run_state = run_until = 0; failed_run_until = 0;
    run_sx = run_sy = run_tx = run_ty = run_start_z = 0;
    run_goal_x = run_goal_y = run_goal_z = 0;
    forced_item_x = forced_item_y = 0; forced_item_until = 0; forced_item_active = false;
    door_commit_until = 0; door_commit_line = -1; door_commit_side = 0;
    door_ax = door_ay = door_tx = door_ty = 0;
    pending_use_line = -1; pending_use_special = 0; pending_use_until = 0;
    use_fail_line = -1; use_fail_since = 0;
    I_Log("Bot: collision grid %dx%d\n",width,height);
}
void Bot_Init(void) { bot_active = false; }

static mobj_t *Bot_Threat(player_t *p)
{
    thinker_t *th;
    mobj_t *best = NULL, *current_visible = NULL;
    boolean current_alive = false;
    fixed_t bestdist = 1000*FRACUNIT, current_dist = INT_MAX;

    combat_has_shot = false;

    for (th = thinkercap.next; th != &thinkercap; th = th->next) {
        mobj_t *mo;
        fixed_t dist;
        angle_t angle;

        if (th->function.acp1 != (actionf_p1)P_MobjThinker) continue;
        mo = (mobj_t*)th;
        if (!(mo->flags & MF_SHOOTABLE) || mo->health <= 0 || mo == p->mo ||
            mo->player || mo->type == MT_BARREL) continue;
        if (mo == combat_target) current_alive = true;
        if (mo == ignored_target && leveltime < ignore_until) continue;

        dist = P_AproxDistance(mo->x-p->mo->x,mo->y-p->mo->y);
        if (dist >= 1000*FRACUNIT || !P_CheckSight(p->mo,mo)) continue;

        /* A visible sprite is not necessarily shootable through a sill/corner.
           Only a real weapon trace is allowed to acquire or replace a target. */
        angle = R_PointToAngle2(p->mo->x,p->mo->y,mo->x,mo->y);
        P_AimLineAttack(p->mo,angle,dist+64*FRACUNIT);
        if (linetarget != mo) continue;

        if (mo == combat_target) {
            current_visible = mo;
            current_dist = dist;
        }
        if (dist < bestdist) {
            bestdist = dist;
            best = mo;
        }
    }

    /* Once we have started shooting somebody, finish that fight instead of
       snapping 180 degrees because another monster became a little closer.
       Only an immediate point-blank threat may pre-empt the current target. */
    if (current_visible) {
        if (!best || best == current_visible ||
            !(bestdist < 96*FRACUNIT && current_dist > 192*FRACUNIT)) {
            best = current_visible;
            bestdist = current_dist;
        }
    }

    if (best) {
        if (best != combat_target) {
            combat_target = best;
            combat_health = best->health;
            combat_progress = leveltime;
            combat_strafe_side = ((best->x ^ best->y) & FRACUNIT) ? 1 : -1;
            combat_strafe_until = 0;
            combat_strafe_pause_until = leveltime + 4;
            combat_strafe_tx = combat_strafe_ty = 0;
            combat_anchor_x = p->mo->x;
            combat_anchor_y = p->mo->y;
            combat_anchor_sector = p->mo->subsector->sector-sectors;
            combat_anchor_valid = true;
        } else if (best->health < combat_health) {
            combat_health = best->health;
            combat_progress = leveltime;
        }

        combat_has_shot = true;
        combat_last_x = best->x;
        combat_last_y = best->y;
        combat_last_aim = R_PointToAngle2(p->mo->x,p->mo->y,best->x,best->y);

        /* Keep an engagement for about a second after a LOS flicker.  Movement
           may remain under navigation control, but the first-person view does
           not instantly whip back toward a goal behind us. */
        combat_seen_until = leveltime + 35;
        combat_commit_until = leveltime + 52;

        /* Do not orbit an awkward target forever.  Damage refreshes progress;
           otherwise five seconds is enough to decide this firing solution is
           pathological and let navigation continue. */
        if (leveltime - combat_progress > 175) {
            ignored_target = best;
            ignore_until = leveltime + 175;
            combat_target = NULL;
            combat_has_shot = false;
            combat_seen_until = combat_commit_until = 0;
            combat_anchor_valid = false;
            combat_strafe_until = combat_strafe_pause_until = 0;
            return NULL;
        }
        return best;
    }

    if (combat_target && !current_alive) {
        combat_target = NULL;
        combat_seen_until = combat_commit_until = 0;
        combat_anchor_valid = false;
        combat_strafe_until = combat_strafe_pause_until = 0;
        return NULL;
    }

    /* Preserve the engagement through a brief occlusion.  We intentionally do
       not return the hidden monster as a shootable threat; the caller only
       holds the last-known facing and waits for a real trace to reacquire it. */
    if (combat_target && leveltime < combat_commit_until)
        return NULL;

    if (!combat_target || leveltime >= combat_seen_until) {
        combat_target = NULL;
        combat_seen_until = combat_commit_until = 0;
        combat_anchor_valid = false;
        combat_strafe_until = combat_strafe_pause_until = 0;
    }
    return NULL;
}

/* Keep combat movement local.  Ordinary target-strafing and projectile dodges
   may move inside this leash, but should not turn a gunfight in one room into
   an accidental expedition through the next doorway. */
static boolean Bot_CombatPointAllowed(mobj_t *mo, fixed_t x, fixed_t y,
                                      fixed_t anchorx, fixed_t anchory, int leash)
{
    sector_t *sec;
    if (!mo) return false;
    if (P_AproxDistance(x-anchorx,y-anchory) > leash*FRACUNIT) return false;
    sec=R_PointInSubsector(x,y)->sector;
    if (sec->special==5 || sec->special==7 || sec->special==16) return false;
    return Bot_WalkStable(mo->x,mo->y,mo->z,x,y,mo,true);
}

/* Risk score for ALL incoming missiles at a candidate point.  v8 dodged only
   the single nearest predicted impact and then rebuilt a far-away target every
   tic.  Scoring all trajectories lets the bot choose the free side/corridor
   between two fireballs instead of dodging one directly into another. */
static int Bot_MissileRiskAt(mobj_t *mo, fixed_t px, fixed_t py, int *danger_count)
{
    thinker_t *th;
    int risk=0, dangers=0;

    for (th=thinkercap.next; th!=&thinkercap; th=th->next) {
        mobj_t *m;
        double rx,ry,vx,vy,v2,t,cx,cy,clear,hit,margin;
        double mz,pz0,pz1;

        if (th->function.acp1 != (actionf_p1)P_MobjThinker) continue;
        m=(mobj_t*)th;
        if (!(m->flags & MF_MISSILE) || m==mo || m->target==mo) continue;

        rx=(double)(m->x-px)/FRACUNIT;
        ry=(double)(m->y-py)/FRACUNIT;
        if (rx*rx+ry*ry > 576.0*576.0) continue;

        /* Candidate movement is a short burst; assume the bot will mostly have
           reached the point during the dangerous part of the projectile path.
           Using missile velocity alone is more stable than feeding current
           player momentum into every candidate score. */
        vx=(double)m->momx/FRACUNIT;
        vy=(double)m->momy/FRACUNIT;
        v2=vx*vx+vy*vy;
        if (v2<0.01) continue;

        t=-(rx*vx+ry*vy)/v2;
        if (t<0.0 || t>22.0) continue;
        cx=rx+vx*t; cy=ry+vy*t;
        clear=sqrt(cx*cx+cy*cy);
        hit=(double)(mo->radius+m->radius)/FRACUNIT+8.0;

        mz=(double)m->z+(double)m->momz*t;
        pz0=(double)mo->z;
        pz1=(double)(mo->z+mo->height);
        if (mz > pz1+8*FRACUNIT || mz+m->height < pz0-8*FRACUNIT) continue;

        margin=clear-hit;
        if (margin<48.0) {
            int local;
            ++dangers;
            if (margin<=0.0)
                local=120000+(int)((22.0-t)*2200.0);
            else
                local=(int)((48.0-margin)*900.0+(22.0-t)*160.0);
            if (local>0 && risk<INT_MAX-local) risk+=local;
        }
    }

    if (danger_count) *danger_count=dangers;
    return risk;
}

/* Multi-projectile tactical dodge.  The result is a FIXED short destination,
   not a point 72 units away from the bot's new position every tic.  A combat
   leash and a brief pause between bursts keep dodging inside the current room. */
static boolean Bot_ProjectileDodge(mobj_t *mo, fixed_t *outx, fixed_t *outy)
{
    static const int radii[] = { 28, 36 };
    int current_risk, dangers, bestscore=INT_MAX;
    fixed_t anchorx, anchory, bestx=0, besty=0;
    int r,k;

    if (!mo || !outx || !outy) return false;

    current_risk=Bot_MissileRiskAt(mo,mo->x,mo->y,&dangers);
    if (!dangers || current_risk<=0) {
        missile_dodge_until=0;
        return false;
    }

    /* Reuse the current dodge point for the whole short burst.  This is the
       key difference from v8's endless drifting strafe. */
    if (leveltime < missile_dodge_until &&
        P_AproxDistance(mo->x-missile_dodge_tx,mo->y-missile_dodge_ty) > 5*FRACUNIT &&
        Bot_WalkLedgeSafe(mo->x,mo->y,mo->z,missile_dodge_tx,missile_dodge_ty,mo,true)) {
        *outx=missile_dodge_tx; *outy=missile_dodge_ty;
        return true;
    }

    missile_dodge_until=0;

    /* If fighting, inherit the engagement anchor.  Otherwise create a temporary
       local anchor so repeated dodges still cannot walk across the map. */
    if (combat_anchor_valid) {
        anchorx=combat_anchor_x; anchory=combat_anchor_y;
    } else {
        if (leveltime>=missile_anchor_until) {
            missile_anchor_x=mo->x; missile_anchor_y=mo->y;
            missile_anchor_until=leveltime+35;
        }
        anchorx=missile_anchor_x; anchory=missile_anchor_y;
    }

    /* A tiny recovery pause prevents left/right machine-gun oscillation.  Very
       high immediate risk is allowed to break the pause. */
    if (leveltime < missile_dodge_pause_until && current_risk < 150000)
        return false;

    for (r=0; r<(int)(sizeof(radii)/sizeof(radii[0])); ++r) {
        for (k=0; k<16; ++k) {
            int an=(k*FINEANGLES)/16;
            fixed_t x=mo->x+FixedMul(radii[r]*FRACUNIT,finecosine[an]);
            fixed_t y=mo->y+FixedMul(radii[r]*FRACUNIT,finesine[an]);
            int risk, dummy, score, anchordist;
            sector_t *sec;

            if (!Bot_CombatPointAllowed(mo,x,y,anchorx,anchory,68)) continue;
            if (!Bot_WalkLedgeSafe(mo->x,mo->y,mo->z,x,y,mo,true)) continue;

            risk=Bot_MissileRiskAt(mo,x,y,&dummy);
            anchordist=P_AproxDistance(x-anchorx,y-anchory)/FRACUNIT;
            sec=R_PointInSubsector(x,y)->sector;

            score=risk+anchordist*90;
            /* Strong preference for staying in the room/sector we already
               occupy.  Crossing a doorway remains possible if it is genuinely
               the only safe escape from an imminent projectile. */
            if (sec != mo->subsector->sector) score += 9000;
            if (r) score += 400; /* prefer the shorter 28-unit sidestep */

            if (score<bestscore) {
                bestscore=score; bestx=x; besty=y;
            }
        }
    }

    if (bestscore==INT_MAX) return false;

    /* Do not move just because a technically different point exists.  It must
       materially improve the predicted missile situation, unless the current
       point is an actual impact course. */
    if (bestscore >= current_risk + 2000 && current_risk < 120000)
        return false;

    missile_dodge_tx=bestx; missile_dodge_ty=besty;
    missile_dodge_until=leveltime+9;
    missile_dodge_pause_until=leveltime+14;
    *outx=bestx; *outy=besty;
    return true;
}

static void Bot_Weapon(ticcmd_t *cmd, player_t *p, fixed_t dist)
{
    weapontype_t best = wp_fist;
    if (p->weaponowned[wp_chainsaw]) best = wp_chainsaw;
    if (p->ammo[am_clip] > 0) best = wp_pistol;
    if (p->weaponowned[wp_chaingun] && p->ammo[am_clip]>0) best = wp_chaingun;
    if (p->weaponowned[wp_shotgun] && p->ammo[am_shell]>0) best = wp_shotgun;
    if (p->weaponowned[wp_supershotgun] && p->ammo[am_shell]>=2) best = wp_supershotgun;
    if (p->weaponowned[wp_missile] && p->ammo[am_misl]>0 && dist>400*FRACUNIT) best = wp_missile;
    if (p->weaponowned[wp_plasma] && p->ammo[am_cell]>0) best = wp_plasma;
    if (best != p->readyweapon && p->pendingweapon == wp_nochange) {
        /* The classic command has only three weapon bits; SSG uses slot 3. */
        int slot = best == wp_supershotgun ? wp_shotgun : best;
        cmd->buttons |= BT_CHANGE | (slot << BT_WEAPONSHIFT);
    }
}

static line_t *use_line;
static boolean Bot_UseTrace(intercept_t *in)
{
    line_t *li = in->d.line;
    if (li->special) { use_line = li; return false; }
    if (!li->backsector) return false;
    P_LineOpening(li);
    return openrange > 0;
}
static boolean Bot_CanUsePoint(fixed_t x, fixed_t y, angle_t angle, line_t *wanted)
{
    int an = angle >> ANGLETOFINESHIFT;
    use_line = NULL;
    P_PathTraverse(x,y,x+FixedMul(USERANGE,finecosine[an]),
                   y+FixedMul(USERANGE,finesine[an]),PT_ADDLINES,Bot_UseTrace);
    return use_line == wanted && P_PointOnLineSide(x,y,wanted) == 0;
}

static boolean Bot_CanUse(mobj_t *mo, angle_t angle, line_t *wanted)
{
    return Bot_CanUsePoint(mo->x,mo->y,angle,wanted);
}

static int Bot_Clamp(int value, int limit) { return value>limit ? limit : value < -limit ? -limit : value; }
static void Bot_MoveEx(ticcmd_t *cmd, mobj_t *mo, fixed_t tx, fixed_t ty, boolean stop, double maxspeed)
{
    double dx = (double)tx-mo->x, dy = (double)ty-mo->y, len = sqrt(dx*dx+dy*dy);
    double speed = len/FRACUNIT*0.35, vx = 0, vy = 0, ax, ay;
    int an = (mo->angle + (angle_t)((int)cmd->angleturn*65536)) >> ANGLETOFINESHIFT;
    if (speed > maxspeed) speed = maxspeed;
    if (!stop && len > 0) { vx = dx/len*speed*FRACUNIT; vy = dy/len*speed*FRACUNIT; }
    /* Velocity feedback brakes before corners and switches; full forward thrust
       every tic otherwise overshoots small waypoints and oscillates forever. */
    ax = vx-mo->momx; ay = vy-mo->momy;
    cmd->forwardmove = Bot_Clamp((int)((ax*finecosine[an]+ay*finesine[an])/FRACUNIT/2048),50);
    cmd->sidemove = Bot_Clamp((int)((ax*finesine[an]-ay*finecosine[an])/FRACUNIT/2048),50);
}


static void Bot_Move(ticcmd_t *cmd, mobj_t *mo, fixed_t tx, fixed_t ty, boolean stop)
{
    Bot_MoveEx(cmd,mo,tx,ty,stop,7.0);
}

/* Validate the command, not just the straight line to its target. Residual
   lateral momentum survives a waypoint/aim change. Include the distance needed
   to brake with legal ticcmd thrust before allowing a precision step. */
static boolean Bot_LedgeCommandSafe(mobj_t *mo, const ticcmd_t *cmd)
{
    mobj_t predicted = *mo;
    ticcmd_t next = *cmd;
    int tic;
    for (tic=0; tic<12; ++tic) {
        int an;
        fixed_t x, y;
        predicted.angle += (angle_t)((int)next.angleturn*65536);
        an = predicted.angle >> ANGLETOFINESHIFT;
        predicted.momx += FixedMul(next.forwardmove*2048,finecosine[an]) +
                          FixedMul(next.sidemove*2048,finesine[an]);
        predicted.momy += FixedMul(next.forwardmove*2048,finesine[an]) -
                          FixedMul(next.sidemove*2048,finecosine[an]);
        x = predicted.x+predicted.momx; y = predicted.y+predicted.momy;
        if (!Bot_WalkLedgeSafe(predicted.x,predicted.y,predicted.z,x,y,mo,true)) return false;
        predicted.x=x; predicted.y=y; predicted.z=probe.floor;
        predicted.momx=FixedMul(predicted.momx,0xe800);
        predicted.momy=FixedMul(predicted.momy,0xe800);
        if (P_AproxDistance(predicted.momx,predicted.momy)<FRACUNIT/16) break;
        memset(&next,0,sizeof(next));
        Bot_MoveEx(&next,&predicted,x,y,true,0.0);
    }
    return true;
}

static boolean Bot_RunCommand(ticcmd_t *cmd, player_t *p)
{
    mobj_t *mo = p->mo;
    angle_t aim;
    int turn;
    fixed_t dist;

    if (!run_state) return false;

    aim = R_PointToAngle2(mo->x,mo->y,run_tx,run_ty);
    turn = (short)((aim-mo->angle)>>16);
    dist = P_AproxDistance(run_tx-mo->x,run_ty-mo->y);
    if (mlook) lookdir = 0;

    if (run_state == 1) {
        /* Rotate in place first.  A crooked launch is far worse than waiting a
           fraction of a second, especially for narrow windows. */
        cmd->angleturn = Bot_Clamp(turn, 1600);
        if (abs(turn) < 220) {
            run_state = 2;
            run_until = leveltime + 28;
            cmd->forwardmove = 50;
        } else if (leveltime >= run_until) {
            failed_run_sx=run_sx; failed_run_sy=run_sy;
            failed_run_tx=run_tx; failed_run_ty=run_ty;
            failed_run_until=leveltime+175;
            run_state=0; goal.type=GO_NONE; next_plan=leveltime+1;
            I_Log("Bot: momentum align failed\n");
        }
        return true;
    }

    /* Once committed, use full classic-Doom running thrust.  Steering is kept
       deliberately tiny so the bot cannot "correct" itself off the window. */
    cmd->angleturn = Bot_Clamp(turn, 450);
    cmd->forwardmove = 50;
    cmd->sidemove = 0;

    /* Near the landing in XY is not a landing: the player may still be above
       the gap. Hand control back only after reaching supported floor. */
    if (dist < 20*FRACUNIT && mo->z <= mo->floorz &&
        mo->floorz >= run_start_z-24*FRACUNIT &&
        R_PointInSubsector(mo->x,mo->y)->sector->floorheight >= mo->floorz-24*FRACUNIT) {
        run_state = 0;
        goal.type = GO_NONE;
        /* Keep the original off-mesh objective sticky long enough to walk the
           landing island/stairs and physically collect it. */
        forced_item_x = run_goal_x;
        forced_item_y = run_goal_y;
        /* Keep the precision objective until the item is actually gone.
           v8 used only 350 tics (~10 s); once careful stair movement became
           slower than that, protection silently expired right before MAP02's
           red key and the bot resumed the fatal diagonal shortcut. */
        forced_item_active = true;
        forced_item_until = leveltime + 2100; /* watchdog, not normal expiry */
        next_plan = leveltime + 1;
        Bot_MoveEx(cmd,mo,mo->x,mo->y,true,0.0);
        I_Log("Bot: momentum landing reached at (%d,%d), keep item=(%d,%d)\n",
              mo->x/FRACUNIT,mo->y/FRACUNIT,forced_item_x/FRACUNIT,forced_item_y/FRACUNIT);
        return true;
    }

    if (leveltime >= run_until ||
        (mo->z < run_start_z-64*FRACUNIT && dist > 32*FRACUNIT)) {
        failed_run_sx=run_sx; failed_run_sy=run_sy;
        failed_run_tx=run_tx; failed_run_ty=run_ty;
        failed_run_until=leveltime+175;
        run_state=0; goal.type=GO_NONE; next_plan=leveltime+1;
        cmd->forwardmove=0;
        I_Log("Bot: momentum run failed at (%d,%d)\n",mo->x/FRACUNIT,mo->y/FRACUNIT);
        return true;
    }

    return true;
}

/* Find a short point just across the operated door line.  The old fixed
   52-unit continuation could land inside the opposite wall of a shallow
   secret closet.  Then the door visibly opened but Bot_Walk() kept failing for
   the entire commit timeout. */
static boolean Bot_FindDoorCrossPoint(mobj_t *mo, int line_index, boolean actors,
                                      fixed_t *outx, fixed_t *outy)
{
    static const int across[] = { 20, 28, 36, 44 };
    static const int along[]  = { 0, -12, 12, -24, 24 };
    line_t *li;
    double ldx, ldy, len, txv, tyv, nx, ny;
    int a, t;

    if (!mo || line_index < 0 || line_index >= numlines) return false;
    li = &lines[line_index];
    ldx = (double)li->dx; ldy = (double)li->dy;
    len = sqrt(ldx*ldx + ldy*ldy);
    if (len < FRACUNIT) return false;

    txv = ldx/len; tyv = ldy/len;
    nx = ldy/len;  ny = -ldx/len;

    for (t=0; t<(int)(sizeof(along)/sizeof(along[0])); ++t) {
        fixed_t bx = door_ax + (fixed_t)(txv*along[t]*FRACUNIT);
        fixed_t by = door_ay + (fixed_t)(tyv*along[t]*FRACUNIT);
        for (a=0; a<(int)(sizeof(across)/sizeof(across[0])); ++a) {
            fixed_t px = bx + (fixed_t)(nx*across[a]*FRACUNIT);
            fixed_t py = by + (fixed_t)(ny*across[a]*FRACUNIT);

            if (P_PointOnLineSide(px,py,li) == door_commit_side) {
                px = bx - (fixed_t)(nx*across[a]*FRACUNIT);
                py = by - (fixed_t)(ny*across[a]*FRACUNIT);
            }
            if (P_PointOnLineSide(px,py,li) == door_commit_side) continue;
            if (!Bot_Walk(mo->x,mo->y,mo->z,px,py,mo,actors)) continue;

            if (outx) *outx = px;
            if (outy) *outy = py;
            return true;
        }
    }
    return false;
}

static void Bot_StartDoorCommit(mobj_t *mo, int line_index, fixed_t ax, fixed_t ay)
{
    line_t *li;
    double ldx, ldy, len, nx, ny;
    fixed_t p1x, p1y, p2x, p2y;

    if (line_index < 0 || line_index >= numlines) return;
    li = &lines[line_index];
    ldx = (double)li->dx; ldy = (double)li->dy;
    len = sqrt(ldx*ldx+ldy*ldy);
    if (len < FRACUNIT) return;

    door_commit_line = line_index;
    door_commit_side = P_PointOnLineSide(mo->x,mo->y,li);
    door_ax = ax; door_ay = ay;

    /* Seed only 24 units past the line. Runtime probing may choose a farther
       point once the doorway is actually open. */
    nx = ldy/len; ny = -ldx/len;
    p1x = ax + (fixed_t)(nx*24*FRACUNIT);
    p1y = ay + (fixed_t)(ny*24*FRACUNIT);
    p2x = ax - (fixed_t)(nx*24*FRACUNIT);
    p2y = ay - (fixed_t)(ny*24*FRACUNIT);
    if (P_PointOnLineSide(p1x,p1y,li) != door_commit_side) {
        door_tx=p1x; door_ty=p1y;
    } else {
        door_tx=p2x; door_ty=p2y;
    }
    door_commit_until = leveltime + 175;
}

static void Bot_ClearDoorCommit(void)
{
    door_commit_until = 0;
    door_commit_line = -1;
}

static boolean Bot_PathTurnsAt(int step)
{
    int a, b, c;
    int ax, ay, bx, by;
    if (step <= 0 || step+1 >= path_len) return false;
    a = path[step-1]; b = path[step]; c = path[step+1];
    ax = (b%width) - (a%width);
    ay = (b/width) - (a/width);
    bx = (c%width) - (b%width);
    by = (c/width) - (b/width);
    return ax != bx || ay != by;
}

void Bot_BuildTiccmd(ticcmd_t *cmd, player_t *p)
{
    mobj_t *mo, *threat;
    fixed_t tx, ty, dist;
    angle_t aim;
    int i, sec, keys = 0, turn;
    boolean stop = false;
    boolean ready_to_use = false;
    boolean ready_to_run = false;
    boolean exit_goal = false;
    boolean interaction_lock = false;
    boolean door_cross = false;
    boolean door_waiting = false;
    boolean ledge_protect = false;
    double ledge_speed = 2.4;
    memset(cmd,0,sizeof(*cmd));
    if (!p || !(mo=p->mo) || !usergame || gamestate != GS_LEVEL) return;
    if (p->playerstate == PST_DEAD) { if ((leveltime%35)==0) cmd->buttons = BT_USE; return; }
    if (!count) return;
    if (run_state && Bot_RunCommand(cmd,p)) return;

    Bot_CheckPendingUse();

    if (door_commit_line >= 0 && leveltime < door_commit_until) {
        if (P_PointOnLineSide(mo->x,mo->y,&lines[door_commit_line]) != door_commit_side ||
            P_AproxDistance(mo->x-door_tx,mo->y-door_ty) < 16*FRACUNIT) {
            I_Log("Bot: crossed door line=%d\n",door_commit_line);
            Bot_ClearDoorCommit();
            next_plan = 0;
        }
    } else if (door_commit_line >= 0) {
        Bot_ClearDoorCommit();
    }

    sec = mo->subsector->sector-sectors;
    if (sec != last_sector) { ++sector_visits[sec]; last_sector = sec; }
    for (i=0;i<3;++i) if (Bot_HasKey(p,i)) keys |= 1<<i;
    if (keys != last_keys) { next_plan = 0; last_keys = keys; }
    if (P_AproxDistance(mo->x-last_x,mo->y-last_y) > 96*FRACUNIT) next_plan = 0;
    last_x = mo->x; last_y = mo->y;
    if (leveltime >= next_plan) Bot_Plan(p);
    ledge_protect = (forced_item_active && leveltime < forced_item_until &&
                     goal.type == GO_ITEM &&
                     P_AproxDistance(goal.x-forced_item_x,goal.y-forced_item_y) < 64*FRACUNIT);

    if (!ledge_protect) {
        ledge_anchor_valid=false;
        ledge_recover_until=0;
    } else {
        int clearance=Bot_DropClearance(mo->x,mo->y);
        /* Remember the last genuinely roomy point, not merely the last legal
           grid cell.  It gives the controller somewhere real to retreat to if
           it reaches the lip with residual momentum. */
        if (clearance>=24) {
            ledge_anchor_x=mo->x;
            ledge_anchor_y=mo->y;
            ledge_anchor_valid=true;
        }
        if (clearance<=12 && ledge_anchor_valid)
            ledge_recover_until=leveltime+12;
    }

    /* A used door may need a short time to open.  Drop the interaction goal,
       but keep the delay selected by the USE handler instead of replanning on
       the very next tic while the doorway is still physically closed. */
    if (goal.type == GO_USE && goal.line >= 0 && line_retry[goal.line] > leveltime) {
        goal.type = GO_NONE;
    }
    tx = mo->x; ty = mo->y;
    if (goal.type != GO_NONE) {
        if (ledge_protect) {
            /* On a narrow staircase, merely making the graph cardinal is not
               enough: Doom momentum survives the instant waypoint switch and
               carries the player diagonally through the next 90-degree turn.
               A corner waypoint is therefore not consumed until the player is
               centered on it AND lateral momentum has almost died. */
            while (path_step < path_len) {
                fixed_t wpdist = P_AproxDistance(mo->x-Bot_X(path[path_step]),
                                                 mo->y-Bot_Y(path[path_step]));
                fixed_t speednow = P_AproxDistance(mo->momx,mo->momy);
                boolean checkpoint = Bot_PathTurnsAt(path_step);
                if (path_step>0 && cells[path[path_step]].floor != cells[path[path_step-1]].floor)
                    checkpoint=true;
                if (path_step+1<path_len && cells[path[path_step]].floor != cells[path[path_step+1]].floor)
                    checkpoint=true;
                if (wpdist >= 2*FRACUNIT) break;
                /* Turns AND stair-height transitions are stop-and-go control
                   points.  A straight-looking riser can still carry lateral
                   momentum toward MAP02's central hole. */
                if (checkpoint && speednow > FRACUNIT/3) break;
                ++path_step;
            }
        } else {
            while (path_step < path_len &&
                   P_AproxDistance(mo->x-Bot_X(path[path_step]),mo->y-Bot_Y(path[path_step])) < 3*FRACUNIT)
                ++path_step;
        }
        tx = goal.x; ty = goal.y;
        probe_radius = PLAYERRADIUS+2*FRACUNIT;
        {
            boolean stable_goal = goal.cell >= 0 && goal.cell < count &&
                                  cells[goal.cell].floor >= mo->z-24*FRACUNIT;
            boolean direct_ok;
            int lookahead = 10;
            fixed_t maxlook = 96 * FRACUNIT;

            if (ledge_protect) {
                /* No path smoothing at all while finishing the post-jump item.
                   Follow one cardinal flood cell at a time. */
                direct_ok = false;
                if (path_step < path_len) {
                    fixed_t wpdist;
                    fixed_t speednow;
                    boolean corner;
                    tx = Bot_X(path[path_step]);
                    ty = Bot_Y(path[path_step]);
                    wpdist = P_AproxDistance(tx-mo->x,ty-mo->y);
                    speednow = P_AproxDistance(mo->momx,mo->momy);
                    corner = Bot_PathTurnsAt(path_step);
                    if (path_step>0 && cells[path[path_step]].floor != cells[path[path_step-1]].floor)
                        corner=true;
                    if (path_step+1<path_len && cells[path[path_step]].floor != cells[path[path_step+1]].floor)
                        corner=true;

                    /* Brake before every geometric turn OR stair transition.
                       The latter matters on MAP02 because the dangerous hole is
                       beside a staircase even where the grid direction itself
                       remains straight. */
                    if (corner && wpdist < 8*FRACUNIT) {
                        if (speednow > FRACUNIT/3) {
                            stop = true;
                        } else {
                            ledge_speed = 1.0;
                        }
                    }
                } else {
                    /* Critical v8 change: never switch from the final safe grid
                       cell to the raw item coordinates.  That last diagonal was
                       bypassing all the careful cardinal stair routing. */
                    if (!Bot_FindSafeItemApproach(mo,goal.x,goal.y,goal.cell,&tx,&ty)) {
                        if (goal.cell >= 0 && goal.cell < count) {
                            tx=Bot_X(goal.cell); ty=Bot_Y(goal.cell);
                        } else {
                            tx=mo->x; ty=mo->y; stop=true;
                        }
                    }
                    ledge_speed = 1.0;
                }
                /* Keep the controller target equal to the arrival checkpoint.
                   Shifting it every tic left the original cell unreached and
                   could turn a cardinal path back into a diagonal pit shortcut. */

                if (!stop && !Bot_WalkLedgeSafe(mo->x,mo->y,mo->z,tx,ty,mo,true)) {
                    stop = true;
                    next_plan = leveltime + 1;
                }

                /* Slow down based on actual free floor around the player's
                   centre, not just whether the route is technically legal. */
                {
                    int clearance=Bot_DropClearance(mo->x,mo->y);
                    if (clearance<=12 && ledge_speed>0.55) ledge_speed=0.55;
                    else if (clearance<=20 && ledge_speed>0.85) ledge_speed=0.85;
                    else if (clearance<=28 && ledge_speed>1.25) ledge_speed=1.25;
                }

                /* If projected momentum is heading toward a tall drop, enter a
                   recovery state.  First brake; once nearly stopped, retreat to
                   the last point with generous clearance.  This is much safer
                   than trying to steer tangentially while already on the lip. */
                if (Bot_LedgeMomentumDanger(mo) && ledge_anchor_valid)
                    ledge_recover_until=leveltime+12;

                if (ledge_anchor_valid && leveltime<ledge_recover_until) {
                    fixed_t speednow=P_AproxDistance(mo->momx,mo->momy);
                    fixed_t backdist=P_AproxDistance(mo->x-ledge_anchor_x,mo->y-ledge_anchor_y);
                    if (backdist<4*FRACUNIT) {
                        ledge_recover_until=0;
                    } else if (speednow>FRACUNIT/4) {
                        stop=true;
                        ledge_speed=0.0;
                    } else if (Bot_WalkStable(mo->x,mo->y,mo->z,
                                              ledge_anchor_x,ledge_anchor_y,mo,true)) {
                        tx=ledge_anchor_x; ty=ledge_anchor_y;
                        stop=false; ledge_speed=0.70;
                    } else {
                        stop=true;
                    }
                } else if (!stop && Bot_LedgeMomentumDanger(mo)) {
                    stop=true;
                    ledge_speed=0.0;
                }
            } else {
                direct_ok = stable_goal ?
                    Bot_WalkStable(mo->x,mo->y,mo->z,tx,ty,mo,true) :
                    Bot_Walk(mo->x,mo->y,mo->z,tx,ty,mo,true);

                if (!direct_ok) {
                    if (path_step < path_len) { tx = Bot_X(path[path_step]); ty = Bot_Y(path[path_step]); }
                    for (i=path_step; i<path_len && i<path_step+lookahead; ++i) {
                        fixed_t px=Bot_X(path[i]), py=Bot_Y(path[i]);
                        boolean stable_step = cells[path[i]].floor >= mo->z-24*FRACUNIT;
                        boolean step_ok;
                        if (P_AproxDistance(px-mo->x,py-mo->y)>maxlook) break;
                        step_ok = stable_step ?
                            Bot_WalkStable(mo->x,mo->y,mo->z,px,py,mo,true) :
                            Bot_Walk(mo->x,mo->y,mo->z,px,py,mo,true);
                        if (!step_ok) break;
                        tx=px; ty=py; path_step=i;
                    }
                }
            }
        }
        probe_radius = BOT_RADIUS;
        dist = P_AproxDistance(goal.x-mo->x,goal.y-mo->y);
        if (goal.type == GO_USE) {
            angle_t useaim = R_PointToAngle2(mo->x,mo->y,goal.aimx,goal.aimy);
            fixed_t linedist = P_AproxDistance(goal.aimx-mo->x,goal.aimy-mo->y);
            boolean canuse = linedist <= USERANGE &&
                             Bot_CanUse(mo,useaim,&lines[goal.line]);

            /* Stop only when Doom can actually reach this linedef.  Reaching
               the abstract staging point alone is not enough: around recessed
               panels/corners that used to freeze the bot nose-first forever. */
            if (canuse) {
                tx=goal.aimx; ty=goal.aimy; stop=true; ready_to_use=true;
                use_fail_line = -1; use_fail_since = 0;
            } else if (dist < 12*FRACUNIT) {
                if (use_fail_line != goal.line) {
                    use_fail_line = goal.line;
                    use_fail_since = leveltime;
                }
                if (leveltime-use_fail_since >= USE_ALIGN_TIMEOUT) {
                    int badline = goal.line;
                    /* This approach point cannot see the switch.  Leave it and
                       replan instead of pinning movement+combat at the wall. */
                    line_retry[badline] = leveltime + 35;
                    next_plan = leveltime + 1;
                    goal.type = GO_NONE;
                    path_len = path_step = 0;
                    stop = false;
                    interaction_lock = false;
                    use_fail_line = -1; use_fail_since = 0;
                    I_Log("Bot: abandon unusable switch approach line=%d\n",badline);
                } else {
                    /* Keep moving gently toward the line while trying to obtain
                       a valid USE ray; do not freeze just because the waypoint
                       itself was reached. */
                    tx=goal.aimx; ty=goal.aimy; stop=false;
                    interaction_lock = true;
                }
            } else {
                use_fail_line = -1; use_fail_since = 0;
                interaction_lock = linedist < 88*FRACUNIT;
            }
            if (goal.type == GO_USE)
                interaction_lock = interaction_lock || ready_to_use || linedist < 88*FRACUNIT;
        } else if (goal.type == GO_RUN && dist < 7*FRACUNIT) {
            tx=goal.aimx; ty=goal.aimy; stop=true; ready_to_run=true;
        } else if (dist < 12*FRACUNIT && goal.type != GO_USE && goal.type != GO_RUN) {
            if (goal.line>=0) { ++line_tries[goal.line]; line_retry[goal.line]=leveltime+350; }
            if (goal.type == GO_EXPLORE) ++sector_visits[R_PointInSubsector(goal.x,goal.y)->sector-sectors];
            next_plan=leveltime+1; stop=true;
        }
    } else stop=true;

    /* Door continuation overrides whatever the normal planner picked (ammo,
       explore, etc.).  Wait at the opening while closed; as soon as the current
       sector heights make the route legal, walk straight through it. */
    if (door_commit_line >= 0 && leveltime < door_commit_until) {
        boolean geometry_open;
        boolean actor_open;
        fixed_t crossx=door_tx, crossy=door_ty;
        probe_radius = PLAYERRADIUS+2*FRACUNIT;

        /* Once the sector opens, find a reachable point just across this
           particular doorway instead of insisting on one fixed deep target. */
        geometry_open = Bot_FindDoorCrossPoint(mo,door_commit_line,false,&crossx,&crossy);
        actor_open = geometry_open && Bot_Walk(mo->x,mo->y,mo->z,crossx,crossy,mo,true);
        if (geometry_open) {
            door_tx=crossx; door_ty=crossy;
        }

        if (actor_open) {
            tx=door_tx; ty=door_ty; stop=false; door_cross=true;
            interaction_lock = true;
        } else if (!geometry_open) {
            /* Door itself is still shut: face it, wait/re-use, ignore combat. */
            tx=door_ax; ty=door_ay; stop=true; door_waiting=true;
            interaction_lock = true;
        } else {
            /* Geometry is open but a shootable actor occupies the doorway.
               Keep the door continuation, but allow combat to clear the path. */
            tx=door_ax; ty=door_ay; stop=true;
            interaction_lock = false;
        }
        probe_radius = BOT_RADIUS;
    }

    aim = R_PointToAngle2(mo->x, mo->y, tx, ty);

    if (goal.line >= 0)
        exit_goal = Bot_IsExitSpecial(lines[goal.line].special);

    /* Never let combat steal the facing angle while actually operating a
       switch/door.  Exit goals are locked even while approaching them. */
    if (interaction_lock || ready_to_run || exit_goal || door_cross || door_waiting ||
        leveltime < exit_commit_until)
        threat = NULL;
    else
        threat = Bot_Threat(p);

    /* A short lost-sight grace holds the previous view direction, but movement
       remains under navigation control until a real weapon trace reacquires the
       monster.  This prevents path-angle / monster-angle ping-pong at corners. */
    boolean combat_view_grace = (!threat && combat_target && leveltime < combat_commit_until);
    boolean prefer_combat = false;

    if (threat && combat_has_shot) {
        fixed_t d = P_AproxDistance(threat->x - mo->x, threat->y - mo->y);
        fixed_t mymid = mo->z + mo->height/2;
        fixed_t hismid = threat->z + threat->height/2;
        fixed_t dz = abs(hismid-mymid);
        boolean combat_moved = false;

        prefer_combat = d < 500*FRACUNIT;

        /* When the target is almost overhead, its XY bearing can flip by 90-180
           degrees from a one-unit movement.  Do not let that pathological
           azimuth steal the first-person view; move out from underneath first. */
        if (!(dz > 40*FRACUNIT && d < 96*FRACUNIT)) {
            aim = combat_last_aim;
            Bot_Weapon(cmd, p, d);
        } else {
            prefer_combat = false;
        }

        /* Ordinary combat strafing is now a short burst around the position
           where this engagement began.  v8 rebuilt a 48-unit side target from
           the NEW player position every tic, which effectively meant "keep
           walking sideways forever" and could carry the bot into another room. */
        if (prefer_combat && !ledge_protect && d < 350*FRACUNIT && d > 96*FRACUNIT && dz < 40*FRACUNIT) {
            fixed_t burst_dist;

            if (!combat_anchor_valid) {
                combat_anchor_x=mo->x; combat_anchor_y=mo->y;
                combat_anchor_sector=mo->subsector->sector-sectors;
                combat_anchor_valid=true;
            }

            burst_dist=P_AproxDistance(mo->x-combat_strafe_tx,mo->y-combat_strafe_ty);

            /* Finish or expire the current burst, then deliberately pause.
               This produces human-looking tap-strafes rather than orbiting. */
            if (combat_strafe_until &&
                (leveltime >= combat_strafe_until || burst_dist < 5*FRACUNIT)) {
                combat_strafe_until=0;
                combat_strafe_pause_until=leveltime+9;
                combat_strafe_tx=combat_strafe_ty=0;
                combat_strafe_side=-combat_strafe_side;
            }

            if (!combat_strafe_until && leveltime >= combat_strafe_pause_until) {
                int pass;
                static const int distances[] = { 32, 24 };
                boolean found=false;

                for (pass=0; pass<2 && !found; ++pass) {
                    int side=pass ? -combat_strafe_side : combat_strafe_side;
                    int an=(aim+(side>0 ? ANG90 : -ANG90))>>ANGLETOFINESHIFT;
                    int di;
                    for (di=0; di<(int)(sizeof(distances)/sizeof(distances[0])); ++di) {
                        fixed_t sx=mo->x+FixedMul(distances[di]*FRACUNIT,finecosine[an]);
                        fixed_t sy=mo->y+FixedMul(distances[di]*FRACUNIT,finesine[an]);
                        sector_t *ssec=R_PointInSubsector(sx,sy)->sector;

                        /* For ordinary circle-strafe, changing sectors is not
                           worth it.  If the room is too small, stand and shoot.
                           Projectile avoidance below may still cross a doorway
                           when that is the only way not to eat a fireball. */
                        if ((int)(ssec-sectors) != combat_anchor_sector) continue;
                        if (!Bot_CombatPointAllowed(mo,sx,sy,
                                                   combat_anchor_x,combat_anchor_y,60))
                            continue;

                        combat_strafe_side=side;
                        combat_strafe_tx=sx; combat_strafe_ty=sy;
                        combat_strafe_until=leveltime+10;
                        found=true;
                        break;
                    }
                }

                if (!found)
                    combat_strafe_pause_until=leveltime+12;
            }

            if (combat_strafe_until &&
                Bot_CombatPointAllowed(mo,combat_strafe_tx,combat_strafe_ty,
                                       combat_anchor_x,combat_anchor_y,60)) {
                tx=combat_strafe_tx; ty=combat_strafe_ty;
                stop=false; combat_moved=true;
            } else if (combat_strafe_until) {
                combat_strafe_until=0;
                combat_strafe_pause_until=leveltime+10;
            }
        }
        /* Do not keep executing a navigation goal away from somebody we are
           actively shooting.  If a safe combat strafe was found it owns the
           movement; otherwise plant our feet and finish the target. */
        if (prefer_combat && !ledge_protect && !combat_moved)
            stop = true;
        if (mlook) lookdir = 0;
    } else if (combat_view_grace && !interaction_lock && !ready_to_run && !exit_goal && !door_cross && !door_waiting) {
        /* Recompute the bearing to the last-known position. A frozen absolute
           angle becomes wrong as the player moves and can itself cause a snap. */
        aim = R_PointToAngle2(mo->x,mo->y,combat_last_x,combat_last_y);
        if (mlook) lookdir = 0;
    } else if (mlook) {
        lookdir = 0;
    }

    /* Target strafing above only reacts to enemy position.  Missiles need their
       own avoidance pass: predict impact and override movement while preserving
       the aim on the monster.  Do not do this during precision traversal or
       while operating/crossing a door. */
    if (!ledge_protect && !ready_to_run && !interaction_lock && !door_cross && !door_waiting &&
        leveltime >= exit_commit_until) {
        fixed_t dodgex, dodgey;
        if (Bot_ProjectileDodge(mo,&dodgex,&dodgey)) {
            tx=dodgex; ty=dodgey; stop=false;
        }
    }

    turn = (short)((aim - mo->angle) >> 16);
    cmd->angleturn = Bot_Clamp(turn, 2400);

    if (ready_to_run) {
        run_state = 1;
        run_until = leveltime + 70;
        run_sx = mo->x; run_sy = mo->y;
        run_tx = goal.aimx; run_ty = goal.aimy;
        run_start_z = mo->z;
        I_Log("Bot: momentum launch (%d,%d) -> (%d,%d)\n",
              run_sx/FRACUNIT,run_sy/FRACUNIT,run_tx/FRACUNIT,run_ty/FRACUNIT);
        path_len = path_step = 0;
        goal.type = GO_NONE;
        memset(cmd,0,sizeof(*cmd));
        Bot_RunCommand(cmd,p);
        return;
    }

    /* While a manual door is still closed, periodically repeat USE instead of
       abandoning the doorway.  This also handles a press that happened one tic
       too early while the player was still settling. */
    if (door_waiting && leveltime >= use_until && door_commit_line >= 0 &&
        !Bot_LineMoving(&lines[door_commit_line])) {
        angle_t useangle = mo->angle + (angle_t)((int)cmd->angleturn * 65536);
        if (abs(turn) < 650 && Bot_CanUse(mo,useangle,&lines[door_commit_line])) {
            cmd->buttons |= BT_USE;
            use_until = leveltime + 35;
        }
    }

    /* Only press USE when the angle the player will actually have this tic can
       hit the intended linedef. The old code traced along the desired angle
       while the real player was still turning, so recessed MAP01-style buttons
       often consumed a try without ever receiving P_UseLines. */
    if (goal.type == GO_USE && ready_to_use && leveltime >= use_until &&
        abs(turn) < 650) {
        angle_t useangle = mo->angle + (angle_t)((int)cmd->angleturn * 65536);
        if (Bot_CanUse(mo,useangle,&lines[goal.line])) {

        cmd->buttons |= BT_USE;
        ++line_tries[goal.line];

        int sp = lines[goal.line].special;

        // После любого использования свитча/двери — временно запрещаем возврат
        commit_forward_until = leveltime + 110;

        if (sp == 11 || sp == 51) {
            /* S1 exits are switches, not doors to walk through.  Stay on the
               switch, keep combat from stealing aim, and retry USE until the
               level transition happens.  Do not blacklist the exit after one
               possibly-missed press. */
            line_retry[goal.line] = 0;
            next_plan = leveltime + 70;
            use_until = leveltime + 8;
            commit_forward_until = leveltime + 70;
            exit_commit_until = leveltime + 70;
            stop = true;
        }
        else if (Bot_Manual(sp)) {
            /* Press -> cross is one atomic navigation action.  Do not allow an
               ammo pickup behind us to replace the second half while the door
               is opening. */
            Bot_StartDoorCommit(mo,goal.line,goal.aimx,goal.aimy);
            line_retry[goal.line] = leveltime + 40;
            next_plan = leveltime + 24;
            use_until = leveltime + 35;
        } else {
            /* Remember the attempted remote switch, but only mark it as used
               after the next tics prove that its special changed or a tagged
               sector thinker started.  This prevents both missed-click
               blacklisting and endless revisits to repeatable platform switches. */
            pending_use_line = goal.line;
            pending_use_special = sp;
            pending_use_until = leveltime + 4;
            line_retry[goal.line] = leveltime + 18;
            next_plan = leveltime + 8;
            stop = false;
            goal.type = GO_NONE;
            use_until = leveltime + 14;
        }

        I_Log("Bot: use line=%d special=%d\n", goal.line, sp);
        }
    }
    else if (threat && prefer_combat && abs(turn) < 900) {
        angle_t shot = mo->angle + (angle_t)((int)cmd->angleturn * 65536);
        P_AimLineAttack(mo, shot, MISSILERANGE);
        if (linetarget == threat) {
            cmd->buttons |= BT_ATTACK;
        }
    }
    if (!stop && !door_cross &&
        !(ledge_protect ?
          Bot_WalkLedgeSafe(mo->x,mo->y,mo->z,tx,ty,mo,true) :
          (R_PointInSubsector(tx,ty)->sector->floorheight >= mo->z-24*FRACUNIT ?
           Bot_WalkStable(mo->x,mo->y,mo->z,tx,ty,mo,true) :
           Bot_Walk(mo->x,mo->y,mo->z,tx,ty,mo,true)))) {
        if (ledge_protect) {
            /* Do not invent an angular side-detour on the staircase: that was
               another path by which local avoidance could steer over the inner
               edge. Stop and rebuild the cardinal route instead. */
            stop=true;
            next_plan=leveltime+1;
            if (++stuck_since>25) stuck_since=0;
        } else {
            /* Local detour around moving actors. Static lamps and pillars already
               shaped the route. Keep a side for a full second instead of alternating. */
            int k, side=(leveltime/70)&1 ? 1 : -1;
            boolean found=false;
            for (k=1;k<=4;++k) {
                int an=(aim+(angle_t)(side*k*(ANG45/2)))>>ANGLETOFINESHIFT;
                fixed_t px=mo->x+FixedMul(32*FRACUNIT,finecosine[an]);
                fixed_t py=mo->y+FixedMul(32*FRACUNIT,finesine[an]);
                if (Bot_WalkStable(mo->x,mo->y,mo->z,px,py,mo,true)) {
                    tx=px; ty=py; found=true; break;
                }
            }
            if (!found) stop=true;
            if (++stuck_since>25) { next_plan=leveltime+1; stuck_since=0; }
        }
    } else stuck_since=0;
    if (ledge_protect && !door_cross) {
        Bot_MoveEx(cmd,mo,tx,ty,stop,ledge_speed);
        if (!Bot_LedgeCommandSafe(mo,cmd))
            Bot_MoveEx(cmd,mo,mo->x,mo->y,true,0.0);
    } else
        Bot_Move(cmd,mo,tx,ty,stop);
}
