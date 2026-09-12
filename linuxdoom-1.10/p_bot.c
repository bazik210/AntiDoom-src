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
#include "w_wad.h"

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
static unsigned char *line_key_memory;
static int width, height, count, stamp, heap_count, path_len, path_step;
static fixed_t orgx, orgy;
static bot_goal_t goal;
static int next_plan, last_sector, stuck_since, last_progress;
static int explore_cooldown_cell = -1;
static int explore_cooldown_until = 0;
static int next_ledge_diagnostic;
static fixed_t last_x, last_y, progress_dist;
static int use_until, last_keys;
static int next_key_memory_scan;
static boolean had_ranged_ammo;

/* Short progression lock:
   prevents explore/loot from stealing control after important actions */
static int action_commit_until = 0;
static int action_commit_line = -1;

/* After acquiring a key, keep progression intent alive even when the matching
   keyed line is currently outside the reachable flood component (for example
   MAP02: ordinary door first, red-key columns behind it). */
static int key_progress_mask = 0;
/* Mandatory follow-up interaction after opening special progression gates.
   Prevents explore from stealing control before a nearby button is pressed. */
static int forced_progress_line = -1;
static int forced_progress_until = 0;
static mobj_t *combat_target, *ignored_target;
static int combat_health, combat_progress, ignore_until;
static fixed_t probe_radius = BOT_RADIUS;
static int commit_forward_until = 0;
static int exit_commit_until = 0;

/* After a remote switch/platform button, do not instantly abandon the room for
   old ammo.  Briefly watch the interaction, then keep the next decisions local
   while the triggered geometry has time to move. */
static int post_use_hold_until = 0;
static int post_use_local_until = 0;
static int post_use_line = -1;
static int post_use_sector = -1;
static fixed_t post_use_x = 0, post_use_y = 0;
static fixed_t post_use_look_x = 0, post_use_look_y = 0;
static int progress_sector = -1;
static int progress_until = 0;
static fixed_t progress_x = 0, progress_y = 0;

/* A lift call is not just "press a switch". It is a multi-stage navigation
   action: call -> wait for the floor to become reachable -> step onto it ->
   stay aboard until the platform finishes its trip. Without this state the
   planner sees an unreachable raised floor, forgets why it pressed USE and
   wanders away before the lift reaches the player. */
static int lift_commit_line = -1;
static int lift_commit_tag = 0;
static int lift_commit_sector = -1; /* used for tag-0/local lifts */
static int lift_commit_until = 0;
static boolean lift_commit_boarded = false;
static fixed_t lift_commit_target_x = 0, lift_commit_target_y = 0;
static boolean map03_route = false;
static boolean map03_platform_done = false;
static boolean map03_teleport_done = false;
static boolean map03_final_lift_done = false;
static boolean map03_red_door_done = false;
static boolean map03_blue_door_done = false;
static int combat_pause_until = 0;
static int nearby_attackers = 0;
static mobj_t *perch_enemy = NULL;
static boolean map04_route = false;
static boolean map04_lift_triggered = false;
static boolean map04_lift_done = false;
static boolean map04_crate_route = false;
static boolean map04_crate_retry = false;
static int map04_yellow_state = 0;
#include "p_bot_routes.h"

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
static boolean Bot_Manual(int s);

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
    fixed_t probe_bottom, probe_top, probe_height;

    if (mo == probe.self || !(mo->flags & MF_SOLID)) return true;

    /* Moving actors are handled by steering and combat, not baked into routes. */
    if (!probe.actors && (mo->flags & MF_SHOOTABLE) && mo->type != MT_BARREL) return true;

    /* Bot_Position has already resolved the floor under this candidate point.
       Respect Z overlap just like the real actor collision does.  Previously a
       zombie standing on a 64-unit crate blocked the navigation probe at floor
       level as if the monster were an infinitely tall pillar.  That made local
       avoidance hug the crate instead of simply routing around its geometry. */
    probe_bottom = probe.floor;
    probe_height = probe.self ? probe.self->height : 56*FRACUNIT;
    probe_top = probe_bottom + probe_height;
    if (probe_top <= mo->z || probe_bottom >= mo->z + mo->height)
        return true;

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
    /* Keep almost the whole player footprint supported.  The old 13-unit
       sample radius still let a 16-radius player hang several units over the
       inner edge of a narrow stair; with residual momentum that was enough to
       turn an otherwise legal waypoint into a fall.  One unit of tolerance is
       kept for blockmap / fixed-point boundary noise. */
    const fixed_t r = PLAYERRADIUS - FRACUNIT;
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

/* Pick a final item approach without ever inventing a diagonal shortcut.
   The flood-selected cell is at most a few grid cells from the item.  Project
   the item onto one of the two cardinal lanes through that cell and stop as
   soon as we are comfortably inside pickup range.  This is deliberately less
   clever than the old radial search: the radial search re-selected the point
   nearest to the player every tic, which could turn the last MAP02 stair step
   into exactly the diagonal across the hole that precision mode was meant to
   forbid. */
static boolean Bot_FindSafeItemApproach(mobj_t *mo, fixed_t ix, fixed_t iy, int cell,
                                        fixed_t *outx, fixed_t *outy)
{
    const double pickup = 20.0; /* player + key/item touch radius has margin */
    fixed_t fx, fy, floor;
    int first_axis = 0;         /* 1 = X/horizontal, 2 = Y/vertical */
    int pass;

    if (!mo || !outx || !outy || cell < 0 || cell >= count) return false;
    fx = Bot_X(cell);
    fy = Bot_Y(cell);

    /* Prefer continuing in the direction of the final flood edge.  If that
       projection is unsafe, try one 90-degree cardinal turn on the landing
       platform.  We never use an arbitrary-angle ring point here. */
    if (path_len >= 2 && path[path_len-1] == cell) {
        int prev = path[path_len-2];
        int dx = (cell%width) - (prev%width);
        int dy = (cell/width) - (prev/width);
        if (dx && !dy) first_axis = 1;
        else if (dy && !dx) first_axis = 2;
    }

    if (!first_axis) {
        /* A one-cell path is rare here.  Choose the axis that needs less
           perpendicular offset to enter the item's pickup circle. */
        first_axis = abs(iy-fy) <= abs(ix-fx) ? 1 : 2;
    }

    for (pass=0; pass<2; ++pass) {
        int axis = pass ? (first_axis==1 ? 2 : 1) : first_axis;
        fixed_t x=fx, y=fy;
        double perp, along_allow, delta, adelta, sign;

        if (axis == 1) {
            /* Stay on the final Y lane and advance only in X. */
            perp = fabs((double)(iy-fy))/FRACUNIT;
            if (perp > pickup) continue;
            along_allow = sqrt(pickup*pickup - perp*perp);
            delta = (double)(ix-fx)/FRACUNIT;
            adelta = fabs(delta);
            if (adelta > along_allow) {
                sign = delta < 0.0 ? -1.0 : 1.0;
                x = ix - (fixed_t)(sign*along_allow*FRACUNIT);
            }
        } else {
            /* Stay on the final X lane and advance only in Y. */
            perp = fabs((double)(ix-fx))/FRACUNIT;
            if (perp > pickup) continue;
            along_allow = sqrt(pickup*pickup - perp*perp);
            delta = (double)(iy-fy)/FRACUNIT;
            adelta = fabs(delta);
            if (adelta > along_allow) {
                sign = delta < 0.0 ? -1.0 : 1.0;
                y = iy - (fixed_t)(sign*along_allow*FRACUNIT);
            }
        }

        if (!Bot_Position(x,y,mo,true,&floor) || abs(floor-mo->z)>24*FRACUNIT) continue;
        if (!Bot_LedgeSafe(x,y)) continue;
        if (!Bot_WalkLedgeSafe(mo->x,mo->y,mo->z,x,y,mo,true)) continue;
        *outx=x; *outy=y;
        return true;
    }

    /* Last resort: hold the final flood cell.  Do not trade safety for a raw
       item vector; the next replan may find a different safe final cell. */
    if (Bot_Position(fx,fy,mo,true,&floor) && abs(floor-mo->z)<=24*FRACUNIT &&
        Bot_LedgeSafe(fx,fy) && Bot_WalkLedgeSafe(mo->x,mo->y,mo->z,fx,fy,mo,true)) {
        *outx=fx; *outy=fy;
        return true;
    }
    return false;
}

static boolean Bot_Cell(int c)
{
    if (cells[c].stamp != stamp) {
        boolean progress_cell = map03_route && progress_sector >= 0 &&
            (int)(R_PointInSubsector(Bot_X(c),Bot_Y(c))->sector-sectors) == progress_sector;
        cells[c].stamp = stamp; cells[c].dist = INF; cells[c].parent = -1; cells[c].heap = -1;
        /* Tagged switch targets can be exactly one player diameter wide. The
           normal safety margin is correct for ordinary routing, but would make
           the newly opened destination disappear from the flood entirely. */
        probe_radius = progress_cell ? PLAYERRADIUS : BOT_RADIUS;
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


/* Precision traversal needs a stronger start-cell rule than ordinary routing.
   The player may be a few units off the 8-unit grid after a momentum landing.
   Bot_NearCell() is allowed to choose a cell reachable only by stepping down,
   which is fine for normal navigation but fatal beside MAP02's stairwell.

   Search a slightly wider neighborhood and require a fully ledge-safe segment
   from the player's real position to the candidate grid centre. */
static int Bot_NearLedgeStart(mobj_t *mo)
{
    int gx, gy, dx, dy;
    int best=-1, bestscore=INF;

    if (!mo) return -1;

    gx = (mo->x-orgx + GRID*FRACUNIT/2) / (GRID*FRACUNIT);
    gy = (mo->y-orgy + GRID*FRACUNIT/2) / (GRID*FRACUNIT);

    for (dy=-4; dy<=4; ++dy) for (dx=-4; dx<=4; ++dx) {
        int nx=gx+dx, ny=gy+dy, c, score;

        if (nx<0 || ny<0 || nx>=width || ny>=height) continue;
        c=ny*width+nx;
        if (!Bot_Cell(c) || !cells[c].ledge_safe) continue;
        if (abs(cells[c].floor-mo->z) > 24*FRACUNIT) continue;

        if (!Bot_WalkLedgeSafe(mo->x,mo->y,mo->z,
                               Bot_X(c),Bot_Y(c),mo,false))
            continue;

        score=P_AproxDistance(mo->x-Bot_X(c),mo->y-Bot_Y(c))/FRACUNIT;
        if (!cells[c].comfortable) score += 8;

        if (score < bestscore) {
            bestscore=score;
            best=c;
        }
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

    /* The collision probe can borrow the floor of a nearby stair while its
       centre is still over a pit. Such an endpoint cannot satisfy the landing
       check in Bot_RunCommand and makes the bot keep running past the ledge. */
    if (end_floor - R_PointInSubsector(tx,ty)->sector->floorheight > 24*FRACUNIT)
        return false;

    dx = (double)tx - sx;
    dy = (double)ty - sy;
    len_units = sqrt(dx*dx + dy*dy) / FRACUNIT;
    if (len_units < RUN_MIN_DISTANCE || len_units > RUN_MAX_DISTANCE) return false;

    /* No special traversal is needed if normal grounded movement works. */
    if (Bot_Walk(sx, sy, start_floor, tx, ty, NULL, false) &&
        !(map04_crate_route && !Bot_WalkLedgeSafe(sx,sy,start_floor,tx,ty,NULL,false)))
        return false;

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

        if (floor < start_floor - 24*FRACUNIT ||
            (map04_crate_route && R_PointInSubsector(px,py)->sector->floorheight <
             start_floor-24*FRACUNIT)) {
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

        /* Off-mesh traversal is for unreachable landings. A blocked item
           centre must not turn an already reachable staircase into a jump. */
        if (cells[land].stamp == stamp && cells[land].dist < INF) continue;

        for (d = 0; d < 8; ++d) {
            int nx=lx+ndx[d], ny=ly+ndy[d];
            if (nx < 0 || ny < 0 || nx >= width || ny >= height ||
                run_marks[ny*width+nx] != run_stamp) { boundary = 1; break; }
            /* The reverse flood includes cells hanging over the lip. Also
               search the actual floor edge, otherwise rejecting those false
               landings would discard the legitimate first jump as well. */
            if (cells[ny*width+nx].floor -
                R_PointInSubsector(Bot_X(ny*width+nx),Bot_Y(ny*width+nx))->sector->floorheight > 24*FRACUNIT) {
                boundary = 1; break;
            }
        }
        if (map04_crate_route) {
            /* Land inside a crate, leaving room to brake running momentum. */
            if (Bot_DropClearance(Bot_X(land),Bot_Y(land)) < 24) continue;
        } else if (!boundary) continue;

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
    if (protect_ledge)
        start = Bot_NearLedgeStart(player->mo);
    else
        start = Bot_NearCell(player->mo->x, player->mo->y, player->mo->z, false);

    /* Do not fall back to an ordinary, drop-permitting start during precision
       traversal. A transient off-grid position is better handled by a quick
       retry than by constructing a path whose very first edge is unusable. */
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

/* Remember key-locked interactions that the player has actually encountered.
   The planner knows all linedefs geometrically, but before this memory it simply
   discarded a colored door while the key was missing.  After obtaining the key
   there was therefore no "unfinished business" signal and generic exploration
   could win.

   We intentionally use proximity rather than omniscient map-wide marking:
   getting within 256 map units is enough to count as having seen/encountered the
   colored door/column. This remains true even if the key is already owned:
   bars first discovered behind a newly opened colored door still become
   unfinished business. The memory persists until the interaction is completed. */
static void Bot_RememberNearbyLockedLines(player_t *p)
{
    int i;
    mobj_t *mo;

    if (!p || !(mo=p->mo) || !line_key_memory) return;
    if (leveltime < next_key_memory_scan) return;
    next_key_memory_scan = leveltime + 8;

    for (i=0; i<numlines; ++i) {
        line_t *li=&lines[i];
        int key=Bot_Key(li->special);
        double vx, vy, wx, wy, vv, t;
        fixed_t px, py;

        if (key < 0 || line_key_memory[i]) continue;

        /* If this keyed line was already conclusively used, it is no longer
           unfinished business. Manual doors are handled by their open/cross
           state below and may keep their special after activation. */
        if (line_used[i]) continue;

        vx=(double)li->v2->x-li->v1->x;
        vy=(double)li->v2->y-li->v1->y;
        vv=vx*vx+vy*vy;
        if (vv <= 0.0) continue;

        wx=(double)mo->x-li->v1->x;
        wy=(double)mo->y-li->v1->y;
        t=(wx*vx+wy*vy)/vv;
        if (t<0.0) t=0.0;
        else if (t>1.0) t=1.0;

        px=li->v1->x+(fixed_t)(vx*t);
        py=li->v1->y+(fixed_t)(vy*t);

        if (P_AproxDistance(px-mo->x,py-mo->y) <= 256*FRACUNIT) {
            line_key_memory[i]=1;
            I_Log("Bot: remembered locked line=%d key=%d\n",i,key);
        }
    }
}
static boolean Bot_Manual(int s)
{
    return s==1 || s==26 || s==27 || s==28 || s==31 || s==32 || s==33 || s==34 || s==117 || s==118;
}

/* Some doors use repeatable tagged switch actions on the doorway itself.
   MAP02 lines 101/102 are type 114 and operate their adjacent sector (tag 5).
   They close again, so neither switch-use penalties nor "press and observe"
   handling apply. A remote wall button with the same action stays a switch. */
static boolean Bot_UseDoor(line_t *li)
{
    if (Bot_Manual(li->special)) return true;
    switch (li->special) {
        /* Locked doors are still manual door transactions.  Treating the
           blue/red variants as ordinary remote switches lets the planner
           reselect the same linedef while it is opening and abandons the
           required cross-door continuation (MAP04's blue doors expose this). */
        case 26: case 27: case 28:
        case 32: case 33: case 34:
        case 61: case 63: case 99: case 114: case 115:
        case 133: case 134: case 135: case 136: case 137:
            /* A narrow pillar face is a remote gate control, not a doorway
               the player's whole body can cross (MAP02 red bars). */
            return li->tag && li->backsector && li->backsector->tag == li->tag &&
                   P_AproxDistance(li->dx,li->dy) > 2*BOT_RADIUS;
    }
    return false;
}

static void Bot_ClearKeyProgressForSpecial(int special)
{
    int key=Bot_Key(special);
    if (key < 0) return;

    key_progress_mask &= ~(1<<key);
}
static boolean Bot_LiftUseSpecial(int s)
{
    switch (s) {
        case 21:  /* S1 lift lower-wait-raise */
        case 62:  /* SR lift */
        case 122: /* S1 fast lift */
        case 123: /* SR fast lift */
        case 10: case 88:  /* walk-over lifts */
        case 120: case 121: /* fast walk-over lifts */
            return true;
    }
    return false;
}

static void Bot_ClearLiftCommit(void)
{
    lift_commit_line = -1;
    lift_commit_tag = 0;
    lift_commit_sector = -1;
    lift_commit_until = 0;
    lift_commit_boarded = false;
    lift_commit_target_x = lift_commit_target_y = 0;

}

static boolean Bot_LiftSectorMatches(int sec)
{
    if (sec < 0 || sec >= numsectors || lift_commit_line < 0) return false;

    /* A non-zero tag is the authoritative target and can legitimately address
       more than one sector. For old/local tag-0 lifts, use the sector whose
       thinker actually started. */
    if (lift_commit_tag)
        return sectors[sec].tag == lift_commit_tag;

    return lift_commit_sector >= 0 && sec == lift_commit_sector;
}

static void Bot_ResolveLiftCommit(line_t *li)
{
    int i;

    if (!li || lift_commit_line < 0) return;

    /* For a local/tag-0 lift the adjacent sector is the only useful identity. */
    if (!lift_commit_tag) {
        if (li->backsector && li->backsector->specialdata) {
            lift_commit_sector = (int)(li->backsector-sectors);
            return;
        }
        if (li->frontsector && li->frontsector->specialdata) {
            lift_commit_sector = (int)(li->frontsector-sectors);
            return;
        }
    }

    /* Tagged remote lift: remembering one active sector is useful for logs and
       tag-0 fallback, while Bot_LiftSectorMatches accepts every sector sharing
       the non-zero tag. */
    if (lift_commit_tag) {
        for (i=0; i<numsectors; ++i) {
            if (sectors[i].tag == lift_commit_tag && sectors[i].specialdata) {
                lift_commit_sector = i;
                return;
            }
        }
    }
}

static void Bot_StartLiftCommit(mobj_t *mo, int line_index)
{
    line_t *li;

    if (line_index < 0 || line_index >= numlines) return;
    li=&lines[line_index];

    lift_commit_line=line_index;
    lift_commit_tag=li->tag;
    lift_commit_sector=-1;
    lift_commit_boarded=false;

    /* Slow lifts may travel, wait three seconds, then travel again. Give the
       complete call->board->ride transaction enough time instead of treating
       the bottom arrival as an ordinary four-second post-use context. */
    lift_commit_until=leveltime +
        ((map04_route && line_index == 408) ? 700 : 525); /* 20 s / 15 s watchdog */

    post_use_line=line_index;
    post_use_sector=mo ? (int)(mo->subsector->sector-sectors) : -1;
    post_use_x=mo ? mo->x : 0;
    post_use_y=mo ? mo->y : 0;
    post_use_hold_until=leveltime+10;
    post_use_local_until=lift_commit_until;
    commit_forward_until=lift_commit_until;

    I_Log("Bot: lift commit line=%d special=%d tag=%d\n",
          line_index,li->special,lift_commit_tag);
}

/* Build the continuation for an active lift call.
   - When the lift floor becomes reachable, GO_WALK directly onto its sector.
   - Before that, stay on the closest reachable frontier instead of wandering.
   Re-running this every few tics naturally notices the moving floor height. */
static boolean Bot_LiftCommitGoal(player_t *p)
{
    int i;
    int best_platform=-1, best_platform_score=INF;
    int best_frontier=-1, best_frontier_score=INF;
    int nearest_platform=-1, nearest_platform_dist=INF;
    fixed_t target_x=0, target_y=0;
    mobj_t *mo;

    if (!p || !(mo=p->mo) || lift_commit_line < 0 ||
        leveltime >= lift_commit_until)
        return false;

    /* First locate the lift footprint geometrically, even if the current flood
       cannot enter it yet because its floor is still too high. */
    for (i=0; i<count; ++i) {
        int sec=(int)(R_PointInSubsector(Bot_X(i),Bot_Y(i))->sector-sectors);
        int d;

        if (!Bot_LiftSectorMatches(sec)) continue;

        /* A boundary cell plus arrival tolerance can stop the player outside
           the platform. Board with the centre at least one grid step inside. */
        if (R_PointInSubsector(Bot_X(i)+8*FRACUNIT,Bot_Y(i))->sector != &sectors[sec] ||
            R_PointInSubsector(Bot_X(i)-8*FRACUNIT,Bot_Y(i))->sector != &sectors[sec] ||
            R_PointInSubsector(Bot_X(i),Bot_Y(i)+8*FRACUNIT)->sector != &sectors[sec] ||
            R_PointInSubsector(Bot_X(i),Bot_Y(i)-8*FRACUNIT)->sector != &sectors[sec])
            continue;

        /* Board well inside the 48-unit-wide MAP03 lift, not a boundary cell
           whose arrival tolerance leaves the player outside the sector. */
        if (map03_route && sec == 14 &&
            (Bot_X(i) != 2728*FRACUNIT || Bot_Y(i) != 3360*FRACUNIT))
            continue;

        d=P_AproxDistance(Bot_X(i)-mo->x,Bot_Y(i)-mo->y)/FRACUNIT;
        if (d < nearest_platform_dist) {
            nearest_platform_dist=d;
            nearest_platform=i;
            target_x=Bot_X(i);
            target_y=Bot_Y(i);
        }

        /* The flood proves the moving floor is low enough to step onto NOW. */
        if (cells[i].stamp == stamp && cells[i].dist < INF && cells[i].clear) {
            int score=cells[i].dist;
            if (!cells[i].comfortable) score += 48;
            if (score < best_platform_score) {
                best_platform_score=score;
                best_platform=i;
            }
        }
    }

    if (best_platform >= 0) {
        lift_commit_target_x=Bot_X(best_platform);
        lift_commit_target_y=Bot_Y(best_platform);
        goal.type=GO_WALK;
        goal.x=Bot_X(best_platform);
        goal.y=Bot_Y(best_platform);
        goal.aimx=goal.x; goal.aimy=goal.y;
        goal.line=-1;
        goal.cell=best_platform;
        goal.score=best_platform_score-50000;

        return true;
    }

    if (nearest_platform < 0)
        return false;

    lift_commit_target_x=target_x;
    lift_commit_target_y=target_y;

    /* Still too high: move only to the reachable frontier closest to the lift.
       This gives the bot the human behavior of waiting beside the arriving
       platform instead of selecting ammo/exploration in another room. */
    for (i=0; i<count; ++i) {
        int score;
        fixed_t to_lift;

        if (cells[i].stamp != stamp || cells[i].dist == INF || !cells[i].clear)
            continue;

        to_lift=P_AproxDistance(Bot_X(i)-target_x,Bot_Y(i)-target_y);
        score=(int)(to_lift/FRACUNIT) + cells[i].dist/3;

        if (score < best_frontier_score) {
            best_frontier_score=score;
            best_frontier=i;
        }
    }

    if (best_frontier >= 0) {
        goal.type=GO_WALK;
        goal.x=Bot_X(best_frontier);
        goal.y=Bot_Y(best_frontier);
        goal.aimx=target_x; goal.aimy=target_y;
        goal.line=-1;
        goal.cell=best_frontier;
        goal.score=best_frontier_score-46000;
        return true;
    }

    return false;
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

/* Items are collected by touching them, not by standing at their origin.
   MAP02's red skull is exactly PLAYERRADIUS from a wall: BOT_RADIUS makes
   walking to that origin impossible even though the pickup lane is clear. */
static boolean Bot_ItemCandidate(player_t *p, mobj_t *item, int priority)
{
    fixed_t pickup = p->mo->radius + item->radius - 4*FRACUNIT;
    int gx = (item->x-orgx + GRID*FRACUNIT/2) / (GRID*FRACUNIT);
    int gy = (item->y-orgy + GRID*FRACUNIT/2) / (GRID*FRACUNIT);
    int dx, dy, reach, best = -1, best_dist = INF;
    boolean protect_ledge = forced_item_active && leveltime < forced_item_until;

    /* Stay well inside Doom's square touch range, including waypoint error.
       The final precision approach uses the same 20-unit pickup envelope. */
    if (pickup > 20*FRACUNIT) pickup = 20*FRACUNIT;
    if (pickup <= 0) return false;
    reach = pickup / (GRID*FRACUNIT) + 1;
    for (dy = -reach; dy <= reach; ++dy) for (dx = -reach; dx <= reach; ++dx) {
        int nx = gx+dx, ny = gy+dy, c;
        fixed_t delta;
        if (nx < 0 || ny < 0 || nx >= width || ny >= height) continue;
        c = ny*width+nx;
        if (cells[c].stamp != stamp || cells[c].dist == INF || !cells[c].clear) continue;
        if (protect_ledge && !cells[c].ledge_safe) continue;
        if (P_AproxDistance(item->x-Bot_X(c),item->y-Bot_Y(c)) > pickup) continue;
        /* P_TouchSpecialThing also requires the item to be within vertical
           reach. A nearby cell at the bottom of a pit is not a pickup route. */
        delta = item->z-cells[c].floor;
        if (delta > p->mo->height || delta < -8*FRACUNIT) continue;
        if (cells[c].dist < best_dist) { best = c; best_dist = cells[c].dist; }
    }
    if (best < 0) return false;
    if (best_dist + priority < goal.score) {
        goal.type = GO_ITEM;

        /* x/y are the reachable pickup checkpoint.  aimx/aimy keep the real
           item origin for forced-item identity and diagnostics.  Keeping these
           two meanings separate is important: after the previous change the
           bot moved to goal.cell but completion still measured distance to the
           raw item origin, so it could sit motionless until the 350-tic replan. */
        goal.x = Bot_X(best);
        goal.y = Bot_Y(best);
        goal.aimx = item->x;
        goal.aimy = item->y;

        goal.line = -1; goal.cell = best; goal.score = best_dist + priority;
    }
    return true;
}

/* Reserve combat movement for weapons that can actually hurt a distant
   target. Empty fists must not suppress the navigation needed to rearm. */
static boolean Bot_HasRangedAmmo(player_t *p)
{
    int w;
    for (w=wp_pistol; w<NUMWEAPONS; ++w) {
        ammotype_t ammo=weaponinfo[w].ammo;
        int needed=w==wp_bfg ? 40 : w==wp_supershotgun ? 2 : 1;
        if (p->weaponowned[w] && ammo!=am_noammo && p->ammo[ammo]>=needed)
            return true;
    }
    return false;
}

static boolean Bot_RearmPickup(player_t *p, mobj_t *mo)
{
    switch (mo->sprite) {
    case SPR_CLIP: case SPR_AMMO: case SPR_SHOT: case SPR_SGN2:
    case SPR_MGUN: case SPR_PLAS: case SPR_LAUN: case SPR_BFUG:
    case SPR_BPAK: return true;
    case SPR_SHEL: case SPR_SBOX:
        return p->weaponowned[wp_shotgun] || p->weaponowned[wp_supershotgun];
    case SPR_CELL: case SPR_CELP:
        return p->weaponowned[wp_plasma] || p->weaponowned[wp_bfg];
    case SPR_ROCK: case SPR_BROK: return p->weaponowned[wp_missile];
    default: return false;
    }
}

static int Bot_ItemPriority(player_t *p, mobj_t *mo)
{
    if (!Bot_HasRangedAmmo(p) && Bot_RearmPickup(p,mo)) return -40000;
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

        /* === РџР°С‚СЂРѕРЅС‹ вЂ” С‚РµРїРµСЂСЊ РїРѕРґР±РёСЂР°РµРј Р·Р°СЂР°РЅРµРµ === */
        case SPR_CLIP: case SPR_AMMO:
            if (p->ammo[am_clip] < 50) return -1600;   // Р±С‹Р»Рѕ 30
            if (p->ammo[am_clip] < 100) return -600;   // РґР°Р¶Рµ РєРѕРіРґР° СЃСЂРµРґРЅРµ вЂ” РІСЃС‘ СЂР°РІРЅРѕ РїРѕР»РµР·РЅРѕ
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

static mobj_t *Bot_BlueKey(void)
{
    thinker_t *th;
    for (th=thinkercap.next; th!=&thinkercap; th=th->next) {
        mobj_t *mo;
        if (th->function.acp1 != (actionf_p1)P_MobjThinker) continue;
        mo=(mobj_t *)th;
        if ((mo->sprite == SPR_BKEY || mo->sprite == SPR_BSKU) &&
            mo->health >= 0)
            return mo;
    }
    return NULL;
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
        if (pending_use_line == lift_commit_line)
            Bot_ResolveLiftCommit(li);

        line_used[pending_use_line] = leveltime;
        if (line_key_memory)
            line_key_memory[pending_use_line] = 0;
        Bot_ClearKeyProgressForSpecial(pending_use_special);

        /* MAP02: after opening the red-key bars, the nearby switch is the next
           mandatory progression step. Remember it as a short story objective
           instead of allowing explore/combat to pull the bot away. */
        if (map02_route && Bot_Key(pending_use_special) == it_redcard) {
            int i;
            fixed_t cx = li->v1->x + li->dx/2;
            fixed_t cy = li->v1->y + li->dy/2;
            fixed_t best = INT_MAX;
            for (i=0; i<numlines; ++i) {
                line_t *cand = &lines[i];
                int d;
                if (!Bot_UseSpecial(cand->special) || line_used[i]) continue;
                if (Bot_Key(cand->special) >= 0) continue;
                d = P_AproxDistance((cand->v1->x+cand->dx/2)-cx,
                                    (cand->v1->y+cand->dy/2)-cy);
                if (d < best && d < 768*FRACUNIT) {
                    best = d;
                    forced_progress_line = i;
                }
            }
            if (forced_progress_line >= 0) {
                forced_progress_until = leveltime + TICRATE*30;
                next_plan = 0;
                I_Log("Bot: forced progression switch line=%d\n",
                      forced_progress_line);
            }
        }

        /* A real tagged action just started. Keep the bot nearby long enough to
           observe/use the result instead of immediately shopping for ammo. */
        if (pending_use_line == post_use_line && leveltime + 105 > post_use_local_until)
            post_use_local_until = leveltime + 105;

        /* A remote floor/door switch is useful because it changes a tagged
           sector elsewhere. Keep that sector as the next destination instead
           of falling back to generic exploration around the switch. */
        if (map03_route && li->tag && !Bot_UseDoor(li) &&
            !Bot_LiftUseSpecial(pending_use_special)) {
            int i, best=-1, best_active=-1;
            fixed_t best_dist=INT_MAX, best_active_dist=INT_MAX;
            for (i=0; i<numsectors; ++i) {
                if (sectors[i].tag == li->tag) {
                    fixed_t d=P_AproxDistance(sectors[i].soundorg.x-post_use_x,
                                              sectors[i].soundorg.y-post_use_y);
                    if (d < best_dist) { best=i; best_dist=d; }
                    if (sectors[i].specialdata && d < best_active_dist) {
                        best_active=i; best_active_dist=d;
                    }
                }
            }
            progress_sector = best_active >= 0 ? best_active : best;
            if (progress_sector >= 0) {
                /* Prefer the sector whose thinker actually fired, rather than
                   the arbitrary first sector sharing this tag. */
                progress_until = leveltime + 1050;
                progress_x = sectors[progress_sector].soundorg.x;
                progress_y = sectors[progress_sector].soundorg.y;
            }
        }

        if (map03_route && progress_sector >= 0 &&
            !Bot_LiftUseSpecial(pending_use_special) &&
            ((li->frontsector && li->frontsector == &sectors[progress_sector]) ||
             (li->backsector && li->backsector == &sectors[progress_sector]))) {
            mobj_t *key=Bot_BlueKey();
            if (key) {
                /* The door around the lowered platform is only an intermediate
                   action. Keep the same exact-radius sector target, now aimed
                   at the blue key that motivated the route. */
                progress_x=key->x;
                progress_y=key->y;
                progress_until=leveltime+1050;
            } else {
                progress_sector = -1;
                progress_until = 0;
            }
        }

        I_Log("Bot: confirmed switch line=%d special=%d\n",
              pending_use_line,pending_use_special);
        pending_use_line = -1;
        return;
    }

    /* Give P_UseLines / thinker spawning a couple of tics.  If nothing changed,
       treat it as a missed click and do NOT poison this switch as already used. */
    if (leveltime >= pending_use_until) pending_use_line = -1;
}

static void Bot_StartActionCommit(int line)
{
    action_commit_line=line;
    action_commit_until=leveltime + TICRATE*5;
}

static boolean Bot_ActionCommitted(void)
{
    return action_commit_until > leveltime;
}

static void Bot_Plan(player_t *p)
{
    int i, n;
    int story_target = -1;
    fixed_t story_x = 0, story_y = 0;
    thinker_t *th;
    boolean forced_seen = false;
    boolean key_progress;
    int key_target_line = -1;
    fixed_t key_target_x = 0, key_target_y = 0;
    int key_target_score = INF;

    if (forced_item_active) {
        boolean exists = false;
        for (th=thinkercap.next; th!=&thinkercap; th=th->next) {
            mobj_t *item;
            if (th->function.acp1 != (actionf_p1)P_MobjThinker) continue;
            item=(mobj_t *)th;
            if ((item->flags & MF_SPECIAL) &&
                P_AproxDistance(item->x-forced_item_x,item->y-forced_item_y) < 48*FRACUNIT &&
                Bot_ItemPriority(p,item) != INF) { exists=true; break; }
        }
        if (!exists) { forced_item_active=false; forced_item_until=0; }
    }
    Bot_Flood(p);
	boolean prefer_forward = (leveltime < commit_forward_until);

    /* A long route or a fight must not erase the reason for finding the key.
       Clear the intent on confirmed use, or when no matching target remains. */
    key_progress = key_progress_mask != 0;

    /* The important part: discover the desired keyed interaction WITHOUT
       requiring it to be reachable yet. Bot_Candidate cannot do that because
       it deliberately searches only the current flood component. */
    if (key_progress) {
        for (i=0; i<numlines; ++i) {
            line_t *li=&lines[i];
            int key=Bot_Key(li->special);
            fixed_t mx, my;
            int score;

            if (key < 0 || !(key_progress_mask & (1<<key)) || !Bot_HasKey(p,key))
                continue;

            /* A confirmed non-manual keyed switch is already finished. */
            if (line_used[i] || line_retry[i] > leveltime)
                continue;

            mx=li->v1->x + li->dx/2;
            my=li->v1->y + li->dy/2;
            score=P_AproxDistance(mx-p->mo->x,my-p->mo->y)/FRACUNIT;

            /* If we actually encountered this keyed object before, prefer it. */
            if (line_key_memory && line_key_memory[i])
                score -= 1200;

            if (score < key_target_score) {
                key_target_score=score;
                key_target_line=i;
                key_target_x=mx;
                key_target_y=my;
            }
        }
    }

    if (key_progress && key_target_line < 0) {
        key_progress_mask = 0;
        key_progress = false;
    }

    goal.type = GO_NONE; goal.score = INF; goal.line = -1; path_len = path_step = 0;

    /* Hints use ordinary flood paths and validated USE approach points. */
    if (forced_progress_line >= 0) {
        if (line_used[forced_progress_line] || leveltime >= forced_progress_until) {
            forced_progress_line = -1;
            forced_progress_until = 0;
        } else story_target = forced_progress_line;
    }

    /* Geometry identity is checked once at level initialization. Restore
       progression from actual position/key/world state when loading a save. */
    if (map03_route) {
        if (p->mo->x > 2780*FRACUNIT && p->mo->z >= 96*FRACUNIT)
            map03_platform_done = true;
        if (!map03_platform_done)
            story_target = 87;
        else if (Bot_HasKey(p,0)) {
            if (line_used[map03_blue_door_left] ||
                line_used[map03_blue_door_right])
                map03_blue_door_done = true;
            if (Bot_HasKey(p,it_redcard)) {
                /* The red key is not the end of MAP03's puzzle. The lower room
                   first needs the left lift transaction; only after the bot is
                   safely on its terminal floor should it operate the red door
                   and continue to the exit switch. */
                if (!map03_final_lift_done && lift_commit_line < 0)
                    story_target = 498;
                else if (!map03_red_door_done)
                    story_target = 458;
                else if (lines[554].special)
                    story_target = 554;
            } else {
                /* The switch is behind the left blue door (split across two
                   linedefs). Finish that doorway before targeting the switch;
                   otherwise both door faces remain equally attractive and the
                   planner can alternate between them forever. */
                if (lines[map03_blue_button].special) {
                    if (!map03_blue_door_done &&
                        !line_used[map03_blue_door_left] &&
                        !line_used[map03_blue_door_right])
                        story_target = map03_blue_door_left;
                    else
                        story_target = map03_blue_button;
                } else {
                    story_target = map03_teleport_done ? -1 : 418;
                }
            }
        }
        if (story_target >= 0) {
            line_t *story = &lines[story_target];
            story_x = story->v1->x + story->dx/2;
            story_y = story->v1->y + story->dy/2;
        }
    }
    if (map04_route && !Bot_HasKey(p,it_yellowcard) && Bot_HasKey(p,it_bluecard)) {
        /* MAP04's final lift is a walk-over trigger on the small box in sector
           78. After the blue door, make that trigger the story destination;
           generic exploration otherwise keeps circling the old blue-door area. */
        if (Bot_HasKey(p,it_redcard)) {
            if (map04_crate_route)
                story_target = 459;
            else if (!line_used[16] && !line_used[19])
                story_target = 16;
            else if (lines[574].special)
                story_target = 574;
        } else if (map04_crate_retry)
            story_target = 408;
        else if (!line_used[302] && !line_used[304])
            story_target = 302;
        else if (!line_used[388] && !line_used[393])
            story_target = 388;
        else if (!map04_lift_triggered && !map04_lift_done)
            story_target = 408;
    }
    if (map04_route && Bot_HasKey(p,it_yellowcard)) {
        int sector = p->mo->subsector->sector-sectors;
        /* Recover from saves using destination sectors and switch state. */
        if (map04_yellow_state == 0) map04_yellow_state = 1;
        if (sector >= 111 && sector <= 118) map04_yellow_state = 2;
        if (map04_yellow_state == 2 &&
            (!lines[map04_yellow_switch].special || line_used[map04_yellow_switch]))
            map04_yellow_state = 3;
        if (!lines[map04_yellow_switch].special && sector == 67)
            map04_yellow_state = 4;
        story_target = map04_yellow_state == 1 ? map04_yellow_entry :
                       map04_yellow_state == 2 ? map04_yellow_switch :
                       map04_yellow_state == 3 ? map04_yellow_return : 546;
    }
    if (story_target >= 0) {
        story_x = lines[story_target].v1->x + lines[story_target].dx/2;
        story_y = lines[story_target].v1->y + lines[story_target].dy/2;
    }
    if (map04_route && story_target == 408) {
        line_t *lift_line=&lines[408];
        int best=-1, best_score=INF;
        fixed_t mx=lift_line->v1->x+lift_line->dx/2;
        fixed_t my=lift_line->v1->y+lift_line->dy/2;
        for (i=0; i<count; ++i) {
            int d;
            if (cells[i].stamp != stamp || cells[i].dist == INF || !cells[i].clear)
                continue;
            d=(int)(P_AproxDistance(Bot_X(i)-mx,Bot_Y(i)-my)/FRACUNIT) +
              cells[i].dist/4;
            if (d < best_score) { best_score=d; best=i; }
        }
        if (best >= 0) {
            goal.type=GO_WALK;
            goal.x=Bot_X(best); goal.y=Bot_Y(best);
            goal.aimx=mx; goal.aimy=my; goal.line=-1;
            goal.cell=best; goal.score=-50000+best_score;
        }
    }

    /* An active lift call is unfinished progression, not ordinary post-switch
       exploration. The very negative score keeps the continuation sticky while
       still allowing combat to own movement at runtime. */
    if (lift_commit_line >= 0 && leveltime < lift_commit_until)
        Bot_LiftCommitGoal(p);

    if (map03_route && progress_sector >= 0 && leveltime < progress_until) {
        int progress_cell=-1, progress_score=INF;
        boolean progress_ready = sectors[progress_sector].floorheight <=
                                 p->mo->z + 24*FRACUNIT;
        boolean progress_action=false;

        /* A lowered sector can still be sealed by a W1 door line. Walk to
           that trigger first; trying to reach the sector centre would stop at
           the grate forever. */
        for (i=0; i<numlines; ++i) {
            line_t *li=&lines[i];
            double dx,dy,len;
            if (!li->special || !Bot_WalkSpecial(li->special) ||
                !(li->frontsector == &sectors[progress_sector] ||
                  li->backsector == &sectors[progress_sector])) continue;
            dx=(double)li->dx; dy=(double)li->dy; len=sqrt(dx*dx+dy*dy);
            if (len < FRACUNIT) continue;
            {
                int n;
                for (n=1; n<=3; ++n) {
                    fixed_t ax=li->v1->x+(fixed_t)((long long)li->dx*n/4);
                    fixed_t ay=li->v1->y+(fixed_t)((long long)li->dy*n/4);
                    fixed_t x=ax-(fixed_t)(P_PointOnLineSide(p->mo->x,p->mo->y,li) ?
                                           li->dy/len*32*FRACUNIT :
                                           -li->dy/len*32*FRACUNIT);
                    fixed_t y=ay+(fixed_t)(P_PointOnLineSide(p->mo->x,p->mo->y,li) ?
                                           li->dx/len*32*FRACUNIT :
                                           -li->dx/len*32*FRACUNIT);
                    if (P_PointOnLineSide(x,y,li) == P_PointOnLineSide(p->mo->x,p->mo->y,li)) {
                        x=2*ax-x; y=2*ay-y;
                    }
                    if (Bot_Candidate(GO_WALK,x,y,ax,ay,i,-100000))
                        progress_action=true;
                }
            }
        }
        if (progress_action)
            progress_ready=false;
        for (i=0; i<count; ++i) {
            int sec, score;
            if (cells[i].stamp != stamp || cells[i].dist == INF || !cells[i].clear)
                continue;
            sec=R_PointInSubsector(Bot_X(i),Bot_Y(i))->sector-sectors;
            score=(int)(P_AproxDistance(Bot_X(i)-progress_x,
                                         Bot_Y(i)-progress_y)/FRACUNIT);
            score += cells[i].dist/4;
            if (sec == progress_sector) score -= 9000;
            if (score < progress_score) {
                progress_score=score;
                progress_cell=i;
            }
        }
        if (progress_cell >= 0 && !progress_action) {
            goal.type=GO_WALK;
            /* Keep the frontier cell for path reconstruction, but do not make
               it the arrival point. Otherwise the bot stops at the lip and
               replans the same one-cell route forever. */
            if (progress_ready) {
                goal.x=progress_x;
                goal.y=progress_y;
            } else {
                goal.x=Bot_X(progress_cell);
                goal.y=Bot_Y(progress_cell);
            }
            goal.aimx=progress_x; goal.aimy=progress_y;
            goal.line=-1;
            goal.cell=progress_cell;
            goal.score=-16000+progress_score;
        }
    }

    for (i = 0; i < numlines; ++i) {
        line_t *li = &lines[i];
        int special = li->special, priority;
        if (map03_route && map03_platform_done &&
            (i == 86 || i == 87 || i == 344 || i == 345)) continue;
        double dx = (double)li->dx, dy = (double)li->dy, len = sqrt(dx*dx + dy*dy);
        if (!special || len < FRACUNIT || line_retry[i] > leveltime || !Bot_HasKey(p,Bot_Key(special))) continue;
        if (Bot_UseSpecial(special)) {
            int used_age = line_used[i] ? leveltime-line_used[i] : INT_MAX;

            /* A successfully crossed manual door is no longer a useful
               destination. Keeping it eligible makes the bot turn back to
               the starting room whenever the key remains in inventory. */
            /* A repeatable door may be the only way back out of a room after
               it closes behind the player. Suppress the immediate turn-back,
               but allow a later re-use instead of trapping the bot inside. */
            if (line_used[i] && Bot_UseDoor(li) && i != story_target &&
                leveltime-line_used[i] < USED_SWITCH_HARD_COOLDOWN) {
                P_LineOpening(li);
                if (openrange >= 56*FRACUNIT || Bot_LineMoving(li)) continue;
                /* A closed return door is useful again immediately. */
            }

            /* A remote/repeatable switch that already fired is not fresh
               progression.  Hard-ignore it for a while, then keep a large
               penalty so unexplored space, keys and new switches win.  Manual
               doors and exits have their own continuation semantics. */
            if (i != story_target && !Bot_UseDoor(li) && !Bot_IsExitSpecial(special) && line_used[i]) {
                if (used_age < USED_SWITCH_HARD_COOLDOWN) continue;
            }

            /* Already-open manual doors need no interaction.  Test the actual
               line opening, not just the back sector's nominal height. */
            if (Bot_UseDoor(li) && li->backsector) {
                P_LineOpening(li);
                if (openrange >= 56*FRACUNIT) {
                    /* An opened door that we already touched is unfinished
                       traversal. Reacquire its far side instead of turning back
                       for ammo when the explicit door commit expires. */
                    if (line_tries[i] > 0 && !line_used[i]) {
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
            if (i == story_target) priority = -30000;
            if (map03_route && progress_sector >= 0 &&
                ((li->frontsector && li->frontsector == &sectors[progress_sector]) ||
                 (li->backsector && li->backsector == &sectors[progress_sector])))
                priority = -14000;

            /* A reachable colored interaction is progression once its key is
               owned. Do not make that depend entirely on having walked within
               the memory radius before pickup: red bars first exposed by a red
               door are a common counterexample. */
            if (Bot_Key(special) >= 0 && !line_used[i]) {
                priority -= 9000;
                if (line_key_memory && line_key_memory[i])
                    priority -= 5000;
            } else if (key_progress && key_target_line >= 0 && Bot_UseDoor(li)) {
                /* The keyed object may be behind an ordinary door. Treat a
                   reachable normal manual door as a possible gateway, with a
                   strong preference for doors spatially close to the keyed
                   target. This is intentionally a heuristic, not omniscient
                   connectivity: the flood still proves that THIS door can be
                   reached from the player's current side. */
                fixed_t mx=li->v1->x + li->dx/2;
                fixed_t my=li->v1->y + li->dy/2;
                int gate_dist=P_AproxDistance(mx-key_target_x,my-key_target_y)/FRACUNIT;

                priority -= 3500; /* any reachable manual frontier matters */
                if (gate_dist < 768) {
                    priority -= 7000;
                    priority += gate_dist * 6;
                }
            }

            if (i != story_target && line_used[i] && !Bot_IsExitSpecial(special) &&
                !Bot_UseDoor(li))
                priority += USED_SWITCH_SOFT_PENALTY;
            if (prefer_forward && special != 11 && special != 51) {
                priority += 3000;   // РІРѕ РІСЂРµРјСЏ commit СЃРёР»СЊРЅРѕ РЅРµ Р»СЋР±РёС‚ РґСЂСѓРіРёРµ РґРІРµСЂРё/СЃРµРєСЂРµС‚РєРё
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
                if (!Bot_CanUsePoint(x,y,a,li) &&
                    !(map03_route && i == story_target)) continue;
                Bot_Candidate(GO_USE,x,y,ax,ay,i,priority);
            }
        } else if (Bot_WalkSpecial(special) && li->backsector) {
            int side = P_PointOnLineSide(p->mo->x,p->mo->y,li) ? 1 : -1;
            /* Teleport triggers are progression, not optional exploration.
               Prefer reaching them over nearby repeatable blue doors. */
            priority = special==52 ? -20000 : special==124 ? -15000 :
                       800+line_tries[i]*1000;
            if (i == story_target) priority = -30000;
            if (map03_route && progress_sector >= 0 &&
                ((li->frontsector && li->frontsector == &sectors[progress_sector]) ||
                 (li->backsector && li->backsector == &sectors[progress_sector])))
                priority = -14000;
            if (Bot_Key(special) >= 0) {
                priority -= 9000;
                if (line_key_memory && line_key_memory[i])
                    priority -= 5000;
            } else if (key_progress && key_target_line >= 0) {
                fixed_t mx=li->v1->x + li->dx/2;
                fixed_t my=li->v1->y + li->dy/2;
                int gate_dist=P_AproxDistance(mx-key_target_x,my-key_target_y)/FRACUNIT;
                if (gate_dist < 768) {
                    priority -= 4500;
                    priority += gate_dist * 5;
                }
            }
            if (map04_route && i == story_target &&
                (i == 235 || i == 408)) {
                /* The MAP04 lift/teleporter triggers are narrow walk-over
                   strips. Try both faces and let the flood choose whichever
                   side is actually reachable from the current room. */
                for (n = 1; n <= 3; ++n) {
                    fixed_t ax = li->v1->x+(fixed_t)((long long)li->dx*n/4);
                    fixed_t ay = li->v1->y+(fixed_t)((long long)li->dy*n/4);
                    fixed_t nx=(fixed_t)(dy/len*32*FRACUNIT);
                    fixed_t ny=(fixed_t)(-dx/len*32*FRACUNIT);
                    Bot_Candidate(GO_WALK,ax+nx,ay+ny,ax,ay,i,priority);
                    Bot_Candidate(GO_WALK,ax-nx,ay-ny,ax,ay,i,priority);
                }
                continue;
            }
            if (special == 39 || special == 97) {
                /* Doom teleports only on a front-to-back crossing. The player
                   may be on the back half-plane in a DIFFERENT room: that is
                   not a reason to reverse the desired crossing direction. */
                fixed_t ax=li->v1->x+li->dx/2, ay=li->v1->y+li->dy/2;
                int side=(p->mo->subsector->sector == li->backsector &&
                          P_PointOnLineSide(p->mo->x,p->mo->y,li)) ? 1 : -1;
                Bot_Candidate(GO_WALK,ax+(fixed_t)(side*dy/len*32*FRACUNIT),
                    ay-(fixed_t)(side*dx/len*32*FRACUNIT),ax,ay,i,priority);
                continue;
            }
            for (n = 1; n <= 3; ++n) {
                fixed_t ax = li->v1->x+(fixed_t)((long long)li->dx*n/4);
                fixed_t ay = li->v1->y+(fixed_t)((long long)li->dy*n/4);
                fixed_t x=ax-(fixed_t)(side*dy/len*32*FRACUNIT);
                fixed_t y=ay+(fixed_t)(side*dx/len*32*FRACUNIT);
                /* A walk-over special fires only after the player origin
                   crosses the line. The previous 40-unit staging target left
                   the bot permanently on the approach side. */
                if (P_PointOnLineSide(x,y,li) == P_PointOnLineSide(p->mo->x,p->mo->y,li)) {
                    x=ax+(fixed_t)(side*dy/len*32*FRACUNIT);
                    y=ay-(fixed_t)(side*dx/len*32*FRACUNIT);
                }
                if (P_PointOnLineSide(x,y,li) != P_PointOnLineSide(p->mo->x,p->mo->y,li))
                    Bot_Candidate(GO_WALK,x,y,ax,ay,i,priority);
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
        if (map04_crate_route && !Bot_HasKey(p,it_redcard) &&
            (mo->sprite == SPR_RKEY || mo->sprite == SPR_RSKU)) priority=-80000;
        else if (map04_crate_route) continue;
        if (map03_route && map03_platform_done && !Bot_HasKey(p,it_bluecard) &&
            (mo->sprite == SPR_BKEY || mo->sprite == SPR_BSKU))
            priority = -30000;
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
            if ((leveltime < post_use_local_until || key_progress ||
                 (lift_commit_line >= 0 && leveltime < lift_commit_until)) &&
                !forced_item_active) {
                /* After a switch OR while finishing newly unlocked key
                   progression, do not abandon the route for ordinary ammo,
                   armor or routine health. Keys, major weapons and critical
                   health may still interrupt. */
                if (priority > -2400)
                    continue;
            } else if (prefer_forward) {
                if (priority > -5000) priority += 2500;
            }
            if (!Bot_ItemCandidate(p, mo, priority)) {
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
        fixed_t from_switch;
        if (cells[i].stamp != stamp || cells[i].dist == INF || cells[i].dist < 96) continue;
        if (i == explore_cooldown_cell && leveltime < explore_cooldown_until) continue;
        sec = R_PointInSubsector(Bot_X(i),Bot_Y(i))->sector-sectors;

        from_switch = P_AproxDistance(Bot_X(i)-post_use_x, Bot_Y(i)-post_use_y);
        if (leveltime < post_use_local_until) {
            /* Stay in the interaction's neighborhood while a platform/door is
               reacting.  384 units is large enough for a nearby lift/room, but
               small enough to prevent a sudden trip back across the level. */
            if (from_switch > 384*FRACUNIT) continue;
        }

        score = 1800 + cells[i].dist + sector_visits[sec]*2000;
        if (map03_route && progress_sector >= 0 && leveltime < progress_until) {
            fixed_t to_progress=P_AproxDistance(Bot_X(i)-progress_x,
                                                 Bot_Y(i)-progress_y);
            score += to_progress / FRACUNIT;
            score -= 14000;
            if (sec == progress_sector) score -= 6000;
        } else if (leveltime < post_use_local_until) {
            score += from_switch / (4*FRACUNIT);
            if (sec == post_use_sector) score -= 600;
        } else if (story_target >= 0) {
            fixed_t to_story=P_AproxDistance(Bot_X(i)-story_x,
                                             Bot_Y(i)-story_y);
            /* Follow the reachable frontier toward the mandatory MAP03
               switch/teleporter instead of selecting unrelated exploration. */
            score += to_story / FRACUNIT;
            score -= 12000;
        } else if (key_progress && key_target_line >= 0) {
            fixed_t to_key=P_AproxDistance(Bot_X(i)-key_target_x,
                                           Bot_Y(i)-key_target_y);
            /* Turn generic exploration into a frontier search toward the
               inaccessible keyed interaction. Reachability is still guaranteed
               by the flood; only the choice among reachable frontier cells is
               biased toward the target. */
            score += to_key / (2*FRACUNIT);
            score -= 1800;
        } else if (prefer_forward) {
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
    /* A failed/transient plan must not freeze the bot for ten seconds.  This
       commonly happens just after an off-mesh landing while the flood is being
       rebuilt around a moving platform / narrow stair entrance. */
    if (lift_commit_line >= 0 && leveltime < lift_commit_until &&
        !lift_commit_boarded)
        next_plan = leveltime + 18; /* moving floor can become reachable without flood-spamming */
    else if (goal.type == GO_NONE)
        next_plan = leveltime + (forced_item_active ? 8 : 18);
    else if (forced_item_active && goal.type != GO_ITEM)
        next_plan = leveltime + 12;
    else
        next_plan = leveltime + 350;

    last_progress = leveltime; progress_dist = INF;
    I_Log("Bot: plan type=%d line=%d goal=(%d,%d) path=%d\n",goal.type,goal.line,goal.x/FRACUNIT,goal.y/FRACUNIT,path_len);
    if (goal.line >= 0 && Bot_Key(lines[goal.line].special) >= 0)
        I_Log("Bot: KEYED goal line=%d special=%d key=%d remembered=%d\n",
              goal.line, lines[goal.line].special, Bot_Key(lines[goal.line].special),
              line_key_memory ? line_key_memory[goal.line] : 0);
    if (key_progress && key_target_line >= 0)
        I_Log("Bot: key target line=%d special=%d at=(%d,%d) chosen_goal=%d line=%d\n",
              key_target_line,lines[key_target_line].special,
              key_target_x/FRACUNIT,key_target_y/FRACUNIT,
              goal.type,goal.line);
}

void Bot_InitLevel(void)
{
    long long total;
    map03_route = false;
    map03_platform_done = false;
    map03_teleport_done = false;
    map03_final_lift_done = false;
    map03_red_door_done = false;
    map03_blue_door_done = false;
    map02_route = Bot_MatchesRoute(&bot_routes[0]);
    map03_route = Bot_MatchesRoute(&bot_routes[1]);
    map04_route = Bot_MatchesRoute(&bot_routes[2]);
    combat_pause_until = 0;
    nearby_attackers = 0;
    perch_enemy = NULL;
    map04_lift_triggered = false;
    map04_lift_done = false;
    map04_crate_route = map04_crate_retry = false;
    map04_yellow_state = 0;
    forced_progress_line = -1;
    forced_progress_until = 0;
    action_commit_until = 0;
    action_commit_line = -1;
    free(cells); free(heap); free(path); free(line_tries); free(line_retry); free(line_used); free(sector_visits); free(run_marks); free(line_key_memory);
    cells = NULL; heap = path = line_tries = line_retry = line_used = sector_visits = run_marks = NULL;
    line_key_memory = NULL;
    orgx = (bmaporgx >> (FRACBITS+3)) * (GRID*FRACUNIT);
    orgy = (bmaporgy >> (FRACBITS+3)) * (GRID*FRACUNIT);
    width = bmapwidth*(128/GRID)+2; height = bmapheight*(128/GRID)+2; total = (long long)width*height;
    count = 0;
    if (total <= 0 || total > MAX_CELLS) { I_Log("Bot: level exceeds navigation grid limit\n"); return; }
    count = (int)total;
    cells = calloc(count,sizeof(*cells)); heap = malloc(count*sizeof(*heap)); path = malloc(count*sizeof(*path));
    line_tries = calloc(numlines,sizeof(int)); line_retry = calloc(numlines,sizeof(int)); line_used = calloc(numlines,sizeof(int)); sector_visits = calloc(numsectors,sizeof(int));
    run_marks = calloc(count,sizeof(*run_marks));
    line_key_memory = calloc(numlines,sizeof(*line_key_memory));
    if (!cells || !heap || !path || !line_tries || !line_retry || !line_used ||
        !sector_visits || !run_marks || !line_key_memory)
        I_Error("Bot: navigation allocation failed");
    stamp = 0; path_len = path_step = 0; goal.type = GO_NONE;
    next_plan = use_until = 0; last_sector = -1; last_keys = 0;
    explore_cooldown_cell = -1; explore_cooldown_until = 0;
    next_key_memory_scan = 0;
    had_ranged_ammo = true;
    key_progress_mask = 0;
    stuck_since = last_progress = 0; last_x = last_y = 0;
    next_ledge_diagnostic = 0;
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
    post_use_hold_until = post_use_local_until = 0;
    post_use_line = -1; post_use_sector = -1;
    post_use_x = post_use_y = post_use_look_x = post_use_look_y = 0;
    progress_sector = -1; progress_until = 0;
    progress_x = progress_y = 0;
    Bot_ClearLiftCommit();
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

/* Overestimate barrel chains: any barrel in blast range may join the chain,
   even if a wall or its current health would actually prevent detonation.
   Explosion distance intentionally ignores Z, matching P_RadiusAttack. */
static mobj_t *Bot_BarrelTarget(player_t *p)
{
    mobj_t *barrels[128];
    thinker_t *th;
    int count=0, root, i, j;
    ammotype_t ammo=weaponinfo[p->readyweapon].ammo;
    int needed=p->readyweapon==wp_bfg ? 40 : p->readyweapon==wp_supershotgun ? 2 : 1;
    if (ammo==am_noammo || p->ammo[ammo]<needed || p->pendingweapon!=wp_nochange) return NULL;
    for (th=thinkercap.next; th!=&thinkercap; th=th->next) {
        mobj_t *m;
        if (th->function.acp1 != (actionf_p1)P_MobjThinker) continue;
        m=(mobj_t*)th;
        if (m->type!=MT_BARREL || !(m->flags & MF_SHOOTABLE) || m->health<=0) continue;
        if (count == 128) return NULL; /* Never truncate a safety calculation. */
        barrels[count++]=m;
    }
    for (root=0; root<count; ++root) {
        boolean chain[128]={false}, changed=true, safe=true, useful=false;
        fixed_t distance=P_AproxDistance(barrels[root]->x-p->mo->x,barrels[root]->y-p->mo->y);
        if (distance>480*FRACUNIT) continue;
        P_AimLineAttack(p->mo,R_PointToAngle2(p->mo->x,p->mo->y,
                        barrels[root]->x,barrels[root]->y),distance+32*FRACUNIT);
        if (linetarget!=barrels[root]) continue;
        chain[root]=true;
        /* Include every barrel the current weapon could hit, not just the
           aimed barrel. Use a conservative horizontal cone with body margin;
           no sight filter, since missed pellets can travel behind the target. */
        {
            double dx=(double)barrels[root]->x-p->mo->x;
            double dy=(double)barrels[root]->y-p->mo->y;
            double len=sqrt(dx*dx+dy*dy);
            double spread=p->readyweapon==wp_bfg ? 1.1 :
                          p->readyweapon==wp_supershotgun ? 0.23 : 0.08;
            if (len<FRACUNIT) continue;
            for (i=0; i<count; ++i) {
                double bx=(double)barrels[i]->x-p->mo->x;
                double by=(double)barrels[i]->y-p->mo->y;
                double along=(bx*dx+by*dy)/len;
                double across=fabs(bx*dy-by*dx)/len;
                if (along>=0.0 && along<=MISSILERANGE &&
                    across<=along*spread+barrels[i]->radius+32*FRACUNIT)
                    chain[i]=true;
                /* Projectile splash can ignite neighbors of the impact too. */
                if (p->readyweapon==wp_missile || p->readyweapon==wp_bfg) {
                    fixed_t sx=abs(barrels[i]->x-barrels[root]->x);
                    fixed_t sy=abs(barrels[i]->y-barrels[root]->y);
                    if ((sx>sy ? sx:sy)<160*FRACUNIT+barrels[i]->radius) chain[i]=true;
                }
            }
        }
        while (changed) {
            changed=false;
            for (i=0; i<count; ++i) if (chain[i]) {
                fixed_t dx=abs(barrels[i]->x-p->mo->x), dy=abs(barrels[i]->y-p->mo->y);
                /* Extra margin for movement while the explosion animates. */
                if ((dx>dy ? dx:dy) < 192*FRACUNIT+p->mo->radius) safe=false;
                for (j=0; j<count; ++j) if (!chain[j]) {
                    dx=abs(barrels[i]->x-barrels[j]->x); dy=abs(barrels[i]->y-barrels[j]->y);
                    if ((dx>dy ? dx:dy) < 128*FRACUNIT+barrels[j]->radius) {
                        chain[j]=true; changed=true;
                    }
                }
            }
        }
        if (!safe) continue;
        for (th=thinkercap.next; th!=&thinkercap && !useful; th=th->next) {
            mobj_t *m;
            if (th->function.acp1 != (actionf_p1)P_MobjThinker) continue;
            m=(mobj_t*)th;
            if (!(m->flags & MF_SHOOTABLE) || m->health<=0 || m->player ||
                m->type==MT_BARREL || m->type==MT_CYBORG || m->type==MT_SPIDER) continue;
            for (i=0; i<count; ++i) if (chain[i]) {
                fixed_t dx=abs(m->x-barrels[i]->x), dy=abs(m->y-barrels[i]->y);
                if ((dx>dy ? dx:dy) < 112*FRACUNIT+m->radius && P_CheckSight(m,barrels[i])) {
                    useful=true; break;
                }
            }
        }
        if (useful) return barrels[root];
    }
    return NULL;
}

static mobj_t *Bot_Threat(player_t *p)
{
    thinker_t *th;
    mobj_t *best = NULL, *current_visible = NULL;
    boolean current_alive = false;
    fixed_t bestdist = 1000*FRACUNIT, bestscore = INT_MAX, current_dist = INT_MAX;

    combat_has_shot = false;
    nearby_attackers = 0;
    perch_enemy = NULL;

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

        if (dist < 640*FRACUNIT && mo->target == p->mo) ++nearby_attackers;
        if (dist < 640*FRACUNIT && p->mo->z >= mo->z + 32*FRACUNIT &&
            (!perch_enemy || dist < P_AproxDistance(perch_enemy->x-p->mo->x,
                                                   perch_enemy->y-p->mo->y)))
            perch_enemy=mo;

        /* A visible sprite is not necessarily shootable through a sill/corner.
           Only a real weapon trace is allowed to acquire or replace a target. */
        angle = R_PointToAngle2(p->mo->x,p->mo->y,mo->x,mo->y);
        P_AimLineAttack(p->mo,angle,dist+64*FRACUNIT);
        if (linetarget != mo) continue;

        if (mo == combat_target) {
            current_visible = mo;
            current_dist = dist;
        }
        /* Chaingunners are the main hitscan threat. Prefer one over an imp or
           shotgunner that is merely a little closer, so combat does not leave
           the dangerous target alive while chasing a low priority monster. */
        {
            fixed_t threat_score = dist;
            if (mo->type == MT_CHAINGUY)
                threat_score -= 800*FRACUNIT;
            else if (mo->type == MT_SHOTGUY)
                threat_score -= 80*FRACUNIT;
            if (threat_score < bestscore) {
                bestscore = threat_score;
                bestdist = dist;
                best = mo;
            }
        }
    }

    /* Once we have started shooting somebody, finish that fight instead of
       snapping 180 degrees because another monster became a little closer.
       Only an immediate point-blank threat may pre-empt the current target. */
    if (current_visible) {
        /* Do not let hysteresis keep firing at an imp while a visible
           chaingunner is actively threatening the player. */
        if (best && best->type == MT_CHAINGUY &&
            current_visible->type != MT_CHAINGUY)
            current_visible = NULL;
    }
    if (current_visible) {
        if (!best || best == current_visible ||
            !(bestdist < 96*FRACUNIT && current_dist > 192*FRACUNIT)) {
            best = current_visible;
            bestdist = current_dist;
        }
    }

    /* A safe explosion can take precedence over a distant monster. Keep
       defending against enemies already in melee range. Recheck every tic. */
    if (!best || bestdist > 96*FRACUNIT) {
        mobj_t *barrel=Bot_BarrelTarget(p);
        if (barrel) {
            best=barrel;
            bestdist=P_AproxDistance(best->x-p->mo->x,best->y-p->mo->y);
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

/* Before leaving high ground, look for a nearby firing position. A visible
   enemy behind a sill is not yet a firing solution: trace from each candidate
   and require a supported path, rather than waiting blindly after a pickup. */
static boolean Bot_PerchApproach(mobj_t *mo, mobj_t *enemy,
                                 fixed_t *outx, fixed_t *outy)
{
    static const int ox[8]={1,-1,0,0,1,1,-1,-1};
    static const int oy[8]={0,0,1,-1,1,-1,1,-1};
    int radius, i;
    for (radius=24; radius<=96; radius+=24) {
        for (i=0; i<8; ++i) {
            mobj_t from=*mo;
            fixed_t distance;
            from.x += ox[i]*radius*FRACUNIT;
            from.y += oy[i]*radius*FRACUNIT;
            if (!Bot_WalkLedgeSafe(mo->x,mo->y,mo->z,from.x,from.y,mo,true)) continue;
            from.z=probe.floor;
            if (from.z < mo->z-8*FRACUNIT) continue;
            distance=P_AproxDistance(enemy->x-from.x,enemy->y-from.y);
            P_AimLineAttack(&from,R_PointToAngle2(from.x,from.y,enemy->x,enemy->y),
                           distance+64*FRACUNIT);
            if (linetarget != enemy) continue;
            *outx=from.x; *outy=from.y;
            return true;
        }
    }
    return false;
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

/* Hitscan enemies need movement even when they are not firing a visible
   projectile. On a bridge, choose only points that preserve the supported
   floor; on ordinary ground, prefer a point behind or to the side of the
   player that increases the distance from the chaingunner. */
static boolean Bot_FindHitscanRetreat(mobj_t *mo, mobj_t *threat,
                                      boolean ledge_safe,
                                      fixed_t *outx, fixed_t *outy)
{
    static const int ox[8] = { 1,-1, 0, 0, 1, 1,-1,-1 };
    static const int oy[8] = { 0, 0, 1,-1, 1,-1, 1,-1 };
    fixed_t bestx=0, besty=0;
    double best=-1e30;
    int i, radius;

    if (!mo || !threat || !outx || !outy) return false;
    for (radius=40; radius<=72; radius+=16) {
        for (i=0; i<8; ++i) {
            fixed_t x=mo->x+ox[i]*radius*FRACUNIT;
            fixed_t y=mo->y+oy[i]*radius*FRACUNIT;
            fixed_t dx=threat->x-mo->x, dy=threat->y-mo->y;
            fixed_t nx=x-mo->x, ny=y-mo->y;
            double score;

            if (ledge_safe) {
                if (!Bot_WalkLedgeSafe(mo->x,mo->y,mo->z,x,y,mo,true)) continue;
            } else if (!Bot_WalkStable(mo->x,mo->y,mo->z,x,y,mo,true)) continue;

            /* Favour moving away from the shooter, then maximize separation.
               A small distance penalty avoids needless long strafes. */
            score=(double)nx*dx+(double)ny*dy;
            score=-score/(FRACUNIT*(double)FRACUNIT);
            score += P_AproxDistance(x-threat->x,y-threat->y)/FRACUNIT*8.0;
            score -= P_AproxDistance(nx,ny)/FRACUNIT;
            if (score > best) {
                best=score; bestx=x; besty=y;
            }
        }
    }
    if (best <= -1e29) return false;
    *outx=bestx; *outy=besty;
    return true;
}

/* Risk score for ALL incoming missiles at a candidate point.  v8 dodged only
   the single nearest predicted impact and then rebuilt a far-away target every
   tic.  Scoring all trajectories lets the bot choose the free side/corridor
   between two fireballs instead of dodging one directly into another. */
static int Bot_MissileRiskAt(mobj_t *mo, fixed_t px, fixed_t py, int *danger_count)
{
    thinker_t *th;
    int risk=0, dangers=0;
    double pathx[23], pathy[23];
    double vx=(double)mo->momx/FRACUNIT, vy=(double)mo->momy/FRACUNIT;
    int step;

    /* Predict travel to the candidate instead of teleporting there. Match the
       movement controller's speed, acceleration limit and ground friction. */
    pathx[0]=(double)mo->x/FRACUNIT;
    pathy[0]=(double)mo->y/FRACUNIT;
    for (step=1; step<=22; ++step) {
        double dx=(double)px/FRACUNIT-pathx[step-1];
        double dy=(double)py/FRACUNIT-pathy[step-1];
        double len=sqrt(dx*dx+dy*dy), speed=fmin(7.0,len*0.35);
        double ax=(len>0.0 ? dx/len*speed : 0.0)-vx;
        double ay=(len>0.0 ? dy/len*speed : 0.0)-vy;
        double limit=mo->z<=mo->floorz ? 1.5625 : 0.390625;
        vx+=fmax(-limit,fmin(limit,ax));
        vy+=fmax(-limit,fmin(limit,ay));
        pathx[step]=pathx[step-1]+vx;
        pathy[step]=pathy[step-1]+vy;
        if (mo->z<=mo->floorz) { vx*=0.90625; vy*=0.90625; }
    }

    for (th=thinkercap.next; th!=&thinkercap; th=th->next) {
        mobj_t *m;
        double rx,ry,vx,vy,v2,t,cx,cy,clear,hit,margin;
        double mz,pz0,pz1;
        int local=0;

        if (th->function.acp1 != (actionf_p1)P_MobjThinker) continue;
        m=(mobj_t*)th;
        if (!(m->flags & MF_MISSILE) || m==mo || m->target==mo) continue;

        rx=(double)(m->x-mo->x)/FRACUNIT;
        ry=(double)(m->y-mo->y)/FRACUNIT;
        if (rx*rx+ry*ry > 576.0*576.0) continue;
        hit=(double)(mo->radius+m->radius)/FRACUNIT+8.0;
        for (step=0; step<22; ++step) {
            double fraction;
            int sample;
            rx=(double)m->x/FRACUNIT+(double)m->momx/FRACUNIT*step-pathx[step];
            ry=(double)m->y/FRACUNIT+(double)m->momy/FRACUNIT*step-pathy[step];
            vx=(double)m->momx/FRACUNIT-(pathx[step+1]-pathx[step]);
            vy=(double)m->momy/FRACUNIT-(pathy[step+1]-pathy[step]);
            v2=vx*vx+vy*vy;
            fraction=v2>0.01 ? fmax(0.0,fmin(1.0,-(rx*vx+ry*vy)/v2)) : 0.0;
            t=step+fraction;
            cx=rx+vx*fraction; cy=ry+vy*fraction;
            clear=fmax(fabs(cx),fabs(cy)); /* Doom uses square XY collision boxes. */
            mz=(double)m->z+(double)m->momz*t;
            pz0=(double)mo->z;
            pz1=(double)(mo->z+mo->height);
            if (mz > pz1+8*FRACUNIT || mz+m->height < pz0-8*FRACUNIT) continue;

            margin=clear-hit;
            if (margin<48.0) {
                if (margin<=0.0)
                    sample=120000+(int)((22.0-t)*2200.0);
                else
                    sample=(int)((48.0-margin)*900.0+(22.0-t)*160.0);
                if (sample>local) local=sample;
            }
        }
        if (local>0) { ++dangers; if (risk<INT_MAX-local) risk+=local; }
    }

    if (danger_count) *danger_count=dangers;
    return risk;
}

/* Multi-projectile tactical dodge.  The result is a FIXED short destination,
   not a point 72 units away from the bot's new position every tic.  A combat
   leash and a brief pause between bursts keep dodging inside the current room. */
static boolean Bot_ProjectileDodge(mobj_t *mo, fixed_t navx, fixed_t navy,
                                   fixed_t *outx, fixed_t *outy)
{
    static const int radii[] = { 24, 36, 48, 64 };
    int current_risk, dangers, bestscore=INT_MAX;
    fixed_t anchorx, anchory, bestx=0, besty=0;
    int r,k;

    if (!mo || !outx || !outy) return false;

    current_risk=Bot_MissileRiskAt(mo,navx,navy,&dangers);
    if (!dangers || current_risk<=0) {
        missile_dodge_until=0;
        return false;
    }

    /* Reuse the current dodge point for the whole short burst.  This is the
       key difference from v8's endless drifting strafe. */
    if (leveltime < missile_dodge_until &&
        P_AproxDistance(mo->x-missile_dodge_tx,mo->y-missile_dodge_ty) > 5*FRACUNIT &&
        Bot_WalkLedgeSafe(mo->x,mo->y,mo->z,missile_dodge_tx,missile_dodge_ty,mo,true) &&
        Bot_MissileRiskAt(mo,missile_dodge_tx,missile_dodge_ty,NULL) < current_risk) {
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
    if (leveltime < missile_dodge_pause_until && current_risk < 120000)
        return false;

    for (r=0; r<(int)(sizeof(radii)/sizeof(radii[0])); ++r) {
        for (k=0; k<16; ++k) {
            int an=(k*FINEANGLES)/16;
            fixed_t x=mo->x+FixedMul(radii[r]*FRACUNIT,finecosine[an]);
            fixed_t y=mo->y+FixedMul(radii[r]*FRACUNIT,finesine[an]);
            int risk, dummy, score, anchordist;
            sector_t *sec;

            if (P_AproxDistance(x-anchorx,y-anchory) > 80*FRACUNIT) continue;
            if (!Bot_WalkStable(mo->x,mo->y,mo->z,x,y,mo,true)) continue;
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
    if (bestscore >= current_risk)
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
    else if (p->weaponowned[wp_bfg] && p->ammo[am_cell]>=40) best = wp_bfg;
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
    /* Ordinary navigation often keeps the view on a monster while the feet
       route around geometry.  Limiting sidemove to 20 made the bot push into a
       crate whenever the navigation vector was mostly sideways relative to its
       combat aim. Keep precision/slow controllers gentle, but restore full
       lateral authority for normal 7+ speed movement. */
    {
        int side_limit = maxspeed >= 5.0 ? 50 : 24;
        cmd->sidemove = Bot_Clamp(
            (int)((ax*finesine[an]-ay*finecosine[an])/FRACUNIT/2048),
            side_limit);
    }
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

/* Produce the fastest ledge-safe command instead of oscillating between
   "too fast" and a complete stop.  This lets precision traversal stay brisk on
   straight stairs while retaining the exact same safety predictor near the pit. */
static void Bot_LedgeMoveAdaptive(ticcmd_t *cmd, mobj_t *mo,
                                  fixed_t tx, fixed_t ty,
                                  boolean stop, double requested_speed)
{
    static const double caps[] = { 3.2, 2.4, 1.8, 1.35, 1.0, 0.70, 0.45 };
    ticcmd_t base, test;
    int i;

    if (!cmd || !mo) return;

    base=*cmd;

    if (stop) {
        Bot_MoveEx(cmd,mo,mo->x,mo->y,true,0.0);
        return;
    }

    /* Try the requested speed first. */
    test=base;
    Bot_MoveEx(&test,mo,tx,ty,false,requested_speed);
    if (Bot_LedgeCommandSafe(mo,&test)) {
        *cmd=test;
        return;
    }

    /* Then progressively reduce only the translational speed. */
    for (i=0; i<(int)(sizeof(caps)/sizeof(caps[0])); ++i) {
        double cap=caps[i];

        if (cap >= requested_speed-0.001) continue;

        test=base;
        Bot_MoveEx(&test,mo,tx,ty,false,cap);
        if (Bot_LedgeCommandSafe(mo,&test)) {
            *cmd=test;
            return;
        }
    }

    /* No forward command is currently safe: brake in place. */
    *cmd=base;
    Bot_MoveEx(cmd,mo,mo->x,mo->y,true,0.0);
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
    if (dist < (map04_crate_route ? 8 : 20)*FRACUNIT && mo->z <= mo->floorz &&
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
    /* A diagonal from a quarter-point approach can clip a door jamb even
       after the door is fully open. Use an explicit two-stage continuation:
       first align with the middle of THIS doorway on the entry side, then drive
       through the already-validated far-side point.

       The previous code always returned sx/sy even after proving sx/sy -> px/py
       was walkable. With two adjacent doors that meant "open door A, stand at
       its threshold, time out, open door B, stand at its threshold..." forever. */
    P_LineOpening(li);
    if (openrange >= mo->height) {
        fixed_t mx=li->v1->x+li->dx/2, my=li->v1->y+li->dy/2;
        fixed_t sx=mx+(fixed_t)(nx*24*FRACUNIT);
        fixed_t sy=my+(fixed_t)(ny*24*FRACUNIT);
        fixed_t sf;
        if (P_PointOnLineSide(sx,sy,li) != door_commit_side) {
            sx=mx-(fixed_t)(nx*24*FRACUNIT);
            sy=my-(fixed_t)(ny*24*FRACUNIT);
        }

        if (P_PointOnLineSide(mo->x,mo->y,li) == door_commit_side &&
            Bot_Position(sx,sy,mo,actors,&sf) &&
            Bot_Walk(mo->x,mo->y,mo->z,sx,sy,mo,actors)) {
            for (a=0; a<(int)(sizeof(across)/sizeof(across[0])); ++a) {
                fixed_t px=mx+(fixed_t)(nx*across[a]*FRACUNIT);
                fixed_t py=my+(fixed_t)(ny*across[a]*FRACUNIT);

                if (P_PointOnLineSide(px,py,li) == door_commit_side) {
                    px=mx-(fixed_t)(nx*across[a]*FRACUNIT);
                    py=my-(fixed_t)(ny*across[a]*FRACUNIT);
                }
                if (P_PointOnLineSide(px,py,li) == door_commit_side) continue;
                if (!Bot_Walk(sx,sy,sf,px,py,mo,actors)) continue;

                /* Not centered yet: finish the alignment first. */
                if (P_AproxDistance(mo->x-sx,mo->y-sy) > 6*FRACUNIT) {
                    if (outx) *outx=sx;
                    if (outy) *outy=sy;
                } else {
                    /* Centered: the next target MUST be across the same door. */
                    if (outx) *outx=px;
                    if (outy) *outy=py;
                }
                return true;
            }
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
    door_commit_until = leveltime + 280; /* ~8 s: open -> center -> cross */
    I_Log("Bot: door commit line=%d side=%d seed=(%d,%d)\n",
          door_commit_line,door_commit_side,door_tx/FRACUNIT,door_ty/FRACUNIT);
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

/* Precision path edges are cardinal, but steering to the exact next grid point
   can still be diagonal when the player is a unit or two off the lane.  On a
   narrow staircase that tiny diagonal is enough to leak momentum toward the
   hole.  First recenter on the edge's lane, then advance along it. */
static void Bot_LedgeRailTarget(mobj_t *mo, int from_cell, int to_cell,
                                fixed_t desired_x, fixed_t desired_y,
                                fixed_t *tx, fixed_t *ty, double *maxspeed)
{
    int dx, dy;
    const fixed_t eps = 2*FRACUNIT;

    if (!mo || !tx || !ty || !maxspeed ||
        from_cell < 0 || from_cell >= count || to_cell < 0 || to_cell >= count) {
        if (tx) *tx=desired_x;
        if (ty) *ty=desired_y;
        return;
    }

    dx = (to_cell%width) - (from_cell%width);
    dy = (to_cell/width) - (from_cell/width);
    *tx=desired_x; *ty=desired_y;

    if (dx && !dy) {
        fixed_t lane=Bot_Y(to_cell);
        if (abs(mo->y-lane) > eps) {
            /* Recenter without making progress along X. */
            *tx=mo->x; *ty=lane;
            if (*maxspeed > 1.80) *maxspeed=1.80;
        } else {
            *ty=lane;
        }
    } else if (dy && !dx) {
        fixed_t lane=Bot_X(to_cell);
        if (abs(mo->x-lane) > eps) {
            /* Recenter without making progress along Y. */
            *tx=lane; *ty=mo->y;
            if (*maxspeed > 1.80) *maxspeed=1.80;
        } else {
            *tx=lane;
        }
    }
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
    boolean door_combat_window = false;
    boolean door_fighting = false;
    boolean lift_riding = false;
    boolean ledge_protect = false;
    boolean perch_move = false;
    boolean combat_route_lock = false;
    double ledge_speed = 4.2;
    static int next_door_combat_log = 0;
    memset(cmd,0,sizeof(*cmd));
    if (!p || !(mo=p->mo) || !usergame || gamestate != GS_LEVEL) return;
    if (p->playerstate == PST_DEAD) { if ((leveltime%35)==0) cmd->buttons = BT_USE; return; }
    if (!count) return;
    if (run_state && Bot_RunCommand(cmd,p)) return;

    if (had_ranged_ammo && !Bot_HasRangedAmmo(p)) next_plan=0;
    had_ranged_ammo=Bot_HasRangedAmmo(p);
    if (map04_route && Bot_HasKey(p,it_bluecard) && !Bot_HasKey(p,it_yellowcard)) {
        int room=mo->subsector->sector-sectors;
        if (room >= 37 && room <= 45 && mo->z >= 40*FRACUNIT) {
            map04_crate_route=true;
            map04_crate_retry=false;
            if (!Bot_HasKey(p,it_redcard)) {
                /* Lift arrivals need the same precision lock as jump arrivals.
                   Keep it across the gaps between the three key crates. */
                if (!forced_item_active) next_plan=0;
                forced_item_active=true;
                forced_item_x=-192*FRACUNIT; forced_item_y=1568*FRACUNIT;
                forced_item_until=leveltime+2100;
            }
        }
        if (room == 80) map04_crate_route=false; /* real teleport destination */
        if (room >= 37 && room <= 50 && mo->z <= mo->floorz &&
            mo->z < 40*FRACUNIT && !Bot_HasKey(p,it_redcard) &&
            (map04_crate_route || map04_lift_done)) {
            map04_crate_route=false;
            map04_crate_retry=true;
            map04_lift_done=false;
            forced_item_active=false; forced_item_until=0;
            if (lift_commit_line == 408) Bot_ClearLiftCommit();
            if (!sectors[38].specialdata) map04_lift_triggered=false;
            line_retry[408]=line_retry[409]=0;
            next_plan=0;
        }
        if (map04_crate_retry && !sectors[38].specialdata)
            map04_lift_triggered=false;
        if (Bot_HasKey(p,it_redcard) && room >= 37 && room <= 45)
            map04_crate_route=true;
    }
    /* A lift can leave the player on the crate lip. Strict ledge-safe
       flood connectors reject that starting point, including every route
       inward. Recenter on this verified rectangular crate before planning. */
    if (map04_crate_route && !Bot_HasKey(p,it_redcard) && mo->z == mo->floorz &&
        (mo->subsector->sector->floorheight < mo->z ||
         Bot_DropClearance(mo->x,mo->y) < 24)) {
        static const int crates[] = {38,42,44};
        int k, best=-1;
        fixed_t bestdist=80*FRACUNIT;
        for (k=0;k<3;++k) {
            sector_t *crate=&sectors[crates[k]];
            fixed_t cx=crate->soundorg.x, cy=crate->soundorg.y;
            fixed_t d=P_AproxDistance(cx-mo->x,cy-mo->y);
            if (mo->z != crate->floorheight || d>=bestdist) continue;
            if (!Bot_WalkStable(mo->x,mo->y,mo->z,cx,cy,mo,true)) continue;
            best=k; bestdist=d;
        }
        if (best>=0) {
            sector_t *crate=&sectors[crates[best]];
            Bot_MoveEx(cmd,mo,crate->soundorg.x,crate->soundorg.y,false,2.0);
            next_plan=0;
            return;
        }
    }
    Bot_CheckPendingUse();
    /* Do not aim at a distant key while retaining the lift footprint. */
    if (map03_route && progress_sector == 14) {
        progress_sector = -1;
        progress_until = 0;
        next_plan = 0;
    }

    if (map03_route && progress_sector >= 0 && progress_sector < numsectors &&
        (int)(mo->subsector->sector-sectors) == progress_sector &&
        !Bot_BlueKey()) {
        progress_sector = -1;
        progress_until = 0;
    }

    if (map03_route) {
        if (mo->subsector->sector == &sectors[7])
            map03_teleport_done = true;
        /* Recover completion from the actual upper floor, including saves and
           stepping off the platform during its last movement tic. */
        if (Bot_HasKey(p,it_redcard) &&
            (mo->subsector->sector == &sectors[40] ||
             mo->subsector->sector == &sectors[49]) &&
            mo->subsector->sector->floorheight == 40*FRACUNIT &&
            mo->z >= 40*FRACUNIT && !sectors[40].specialdata) {
            if (!map03_final_lift_done || lift_commit_line == 498 || progress_sector == 40) {
                map03_final_lift_done = true;
                map03_platform_done = true;
                if (lift_commit_line == 498) Bot_ClearLiftCommit();
                if (progress_sector == 40) { progress_sector=-1; progress_until=0; }
                post_use_hold_until=post_use_local_until=commit_forward_until=0;
                goal.type=GO_NONE; path_len=path_step=0; next_plan=0;
            }
        }
        /* Returning to the initial room means the lift sequence was not
           completed. Make the lift a live progression goal again instead of
           treating the earlier boarding as permanent success. */
        if (map03_platform_done && mo->x < 2500*FRACUNIT &&
            mo->y > 3000*FRACUNIT && mo->y < 4000*FRACUNIT)
            map03_platform_done = false;
        if (!map03_platform_done && mo->subsector->sector == &sectors[13] &&
            mo->z >= 96*FRACUNIT) {
            map03_platform_done = true;
            if (lift_commit_tag == 3) Bot_ClearLiftCommit();
            post_use_hold_until = post_use_local_until = 0;
            commit_forward_until = 0;
            next_plan = 0;
        }
    }
    /* MAP04's lift is started by crossing either face of the repeatable
       walk-over at lines 408/409. The special remains on the linedef, so the
       active tagged sector is the reliable confirmation that the box was
       actually crossed. */
    if (map04_route && !map04_lift_triggered && Bot_HasKey(p,it_bluecard) &&
        P_AproxDistance(mo->x-(lines[408].v1->x+lines[408].dx/2),
                        mo->y-(lines[408].v1->y+lines[408].dy/2)) <= 64*FRACUNIT) {
        /* The box is only 16 map units wide, smaller than a player-sized nav
           cell. Submit the same walk-over trigger once the bot has reached its
           safe approach, then let the real platform thinker drive the timed
           run to the red-key crate. */
        P_CrossSpecialLine(408,0,mo);
    }
    if (map04_route && !map04_lift_triggered && Bot_HasKey(p,it_bluecard) &&
        sectors[38].specialdata) {
        map04_lift_triggered = true;
        Bot_StartLiftCommit(mo,408);
        next_plan = 0;
        I_Log("Bot: MAP04 lift trigger confirmed line=408 tag=15\n");
    }
    /* Lift transaction watchdog and ride completion. */
    if (lift_commit_line >= 0) {
        int cursec=(int)(mo->subsector->sector-sectors);

        if (map04_route && lift_commit_line == 408 &&
            sectors[38].specialdata && lift_commit_target_x &&
            P_AproxDistance(mo->x-lift_commit_target_x,
                            mo->y-lift_commit_target_y) <= 48*FRACUNIT) {
            /* The crate is narrower than a player-sized grid cell. Treat the
               bounded arrival on its cell as boarding, even if the BSP point
               at the player's edge still reports the neighboring sector. */
            lift_commit_boarded=true;
            lift_riding=true;
        }

        if (leveltime >= lift_commit_until) {
            int failed_lift=lift_commit_line;
            I_Log("Bot: lift commit timeout line=%d\n",failed_lift);
            Bot_ClearLiftCommit();
            if (map04_route && failed_lift == 408)
                map04_lift_triggered = false;
            if (failed_lift >= 0 && failed_lift < numlines) {
                line_used[failed_lift]=0;
                line_retry[failed_lift]=leveltime+TICRATE;
            }
            next_plan=0;
        } else if (Bot_LiftSectorMatches(cursec) ||
                   (map04_route && lift_commit_line == 408 &&
                    lift_commit_boarded && lift_commit_target_x &&
                    P_AproxDistance(mo->x-lift_commit_target_x,
                                    mo->y-lift_commit_target_y) <= 48*FRACUNIT)) {
            lift_commit_boarded=true;

            if (sectors[cursec].specialdata ||
                (map04_route && lift_commit_line == 408 && sectors[38].specialdata)) {
                /* Platform is still lowering/waiting/rising. Stay aboard. */
                lift_riding=true;
            } else {
                /* The thinker finished while the player is still on its floor:
                   the ride reached its terminal height. */
                I_Log("Bot: lift ride complete line=%d sector=%d\n",
                      lift_commit_line,cursec);

                /* The thinker is finished and the player is on the terminal
                   floor. Do not leave the MAP03 progression flag behind,
                   otherwise the post-lift guard can keep the bot in
                   lift_riding and combat/evade mode forever. */
                if (map03_route) {
                    /* Finishing the mover on sector 14 is not disembarking.
                       Keep steering to sector 13 before releasing the ride. */
                    if (lift_commit_tag != 3)
                        map03_platform_done = true;
                    lift_riding = false;
                    if (lift_commit_line == 498)
                        map03_final_lift_done = true;
                }
                if (map04_route && lift_commit_tag == 15) {
                    map04_lift_done = true;
                    lift_riding = false;
                }

                Bot_ClearLiftCommit();

                /* The old lift target points back to the platform. Discard it
                   so the next plan can choose the upper room or the way back. */
                goal.type=GO_NONE;
                goal.line=-1;
                path_len=path_step=0;
                next_plan=0;
            }
        }
    }

    /* Merely boarding is not completion. Keep pushing toward the upper exit
       while on the lift; a failed ride retains the call as the next goal. */
    if (map03_route && !map03_platform_done &&
        mo->subsector->sector == &sectors[14])
        lift_riding = true;

    /* Exploration can board without using a switch. Wait for the actual
       platform, including its bottom pause; perpetual lifts release at top. */
    if (!map03_route && !map04_route && mo->z <= mo->floorz + FRACUNIT) {
        for (i=0; i<MAXPLATS; ++i) {
            plat_t *plat=activeplats[i];
            if (!plat || plat->sector != mo->subsector->sector) continue;
            lift_riding = plat->status != in_stasis &&
                          plat->sector->floorheight < plat->high;
            if (!lift_riding && lift_commit_boarded) {
                Bot_ClearLiftCommit();
                next_plan=0;
            }
            break;
        }
    }

    if (door_commit_line >= 0 && leveltime < door_commit_until) {
        /* Once the PLAYER CENTER is on the opposite side of the operated
           linedef, the atomic press->cross action succeeded. Do not require the
           exact temporary cross target as well: that target may have changed
           from the entry-center to the far-side point on the preceding tic. */
        if (P_PointOnLineSide(mo->x,mo->y,&lines[door_commit_line]) != door_commit_side) {
            int crossed_line=door_commit_line;
            I_Log("Bot: crossed door line=%d\n",crossed_line);
            if (map03_route && crossed_line == 458)
                map03_red_door_done = true;
            if (map03_route &&
                (crossed_line == map03_blue_door_left ||
                 crossed_line == map03_blue_door_right))
                map03_blue_door_done = true;

            /* Record a completed traversal, not merely a USE attempt. */
            /* Remember both faces of the same physical door. */
            for (i=0;i<numlines;++i) {
                if (Bot_UseDoor(&lines[i]) &&
                    lines[i].backsector == lines[crossed_line].backsector) {
                    line_used[i]=leveltime ? leveltime : 1;
                    line_key_memory[i]=0;
                    line_tries[i]=0;
                    line_retry[i]=leveltime+2*TICRATE;
                }
            }
            line_used[crossed_line]=leveltime ? leveltime : 1;
            if (line_key_memory)
                line_key_memory[crossed_line]=0;
            /* A blue door may close behind the player. Keep its approach out
               of the planner briefly, otherwise MAP03's cluster of blue doors
               causes immediate door-to-door oscillation. */
            line_retry[crossed_line]=leveltime+2*TICRATE;

            /* Do not clear the whole color progression just because one of
               several same-color doors was crossed. The planner can still find
               another uncompleted keyed interaction later. */
            Bot_ClearDoorCommit();
            next_plan = 0;
        }
    } else if (door_commit_line >= 0) {
        int expired_line=door_commit_line;
        I_Log("Bot: door commit timeout line=%d\n",expired_line);
        Bot_ClearDoorCommit();

        /* A failed crossing may be required later; retry after three seconds. */
        if (expired_line >= 0 && expired_line < numlines)
            line_retry[expired_line]=leveltime+3*TICRATE;
        next_plan=leveltime+1;
    }

    sec = mo->subsector->sector-sectors;
    if (sec != last_sector) { ++sector_visits[sec]; last_sector = sec; }

    Bot_RememberNearbyLockedLines(p);

    for (i=0;i<3;++i) if (Bot_HasKey(p,i)) keys |= 1<<i;
    if (keys != last_keys) {
        int gained = keys & ~last_keys;
        int li;

        /* A newly acquired key should immediately resurrect any matching
           interaction we encountered earlier, even if that line happened to be
           under a retry cooldown from some previous navigation attempt. */
        if (gained) {
            key_progress_mask |= gained;

            if (line_key_memory) {
                for (li=0; li<numlines; ++li) {
                    int key=Bot_Key(lines[li].special);
                    if (key >= 0 && (gained & (1<<key)) && line_key_memory[li]) {
                        line_retry[li]=0;
                        I_Log("Bot: key acquired, recall line=%d key=%d\n",li,key);
                    }
                }
            }

            I_Log("Bot: key progression start mask=%d\n",key_progress_mask);
        }

        next_plan = 0;
        last_keys = keys;
    }
    if (P_AproxDistance(mo->x-last_x,mo->y-last_y) > 96*FRACUNIT) next_plan = 0;
    last_x = mo->x; last_y = mo->y;
    /* Finish the already selected door crossing before doing another flood
       and off-mesh search. Replanning here can pause an otherwise open door. */
    /* Keep the selected route through combat and brief LOS flicker. */
    threat = Bot_Threat(p);
    if (leveltime >= next_plan && door_commit_line < 0 && !lift_riding &&
        leveltime >= combat_pause_until)
        Bot_Plan(p);
    ledge_protect = (forced_item_active && leveltime < forced_item_until &&
                     goal.type == GO_ITEM &&
                     P_AproxDistance(goal.aimx-forced_item_x,goal.aimy-forced_item_y) < 64*FRACUNIT);

    combat_route_lock = map04_crate_route || ledge_protect ||
        (goal.type != GO_NONE && !Bot_LedgeSafe(mo->x,mo->y));
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
                /* Only real XY turns and the final checkpoint require a full
                   stop. Straight stair risers are handled by the rail controller
                   and command-safety predictor; stopping at every height change
                   made an ordinary staircase crawl. */
                boolean checkpoint = Bot_PathTurnsAt(path_step) || path_step == path_len-1;
                if (wpdist >= 2*FRACUNIT) break;
                /* Turns AND stair-height transitions are stop-and-go control
                   points.  A straight-looking riser can still carry lateral
                   momentum toward MAP02's central hole. */
                if (checkpoint && speednow > FRACUNIT/3) break;
                /* Arrival tolerance must not skip a corner while the next
                   edge still cuts the pit from the player's real position.
                   Keep approaching this checkpoint until that edge is safe. */
                if (path_step+1 < path_len &&
                    !Bot_WalkLedgeSafe(mo->x,mo->y,mo->z,
                                      Bot_X(path[path_step+1]),Bot_Y(path[path_step+1]),mo,true))
                    break;
                ++path_step;
                progress_dist = INF;
            }
        } else {
            while (path_step < path_len &&
                   P_AproxDistance(mo->x-Bot_X(path[path_step]),mo->y-Bot_Y(path[path_step])) < 3*FRACUNIT)
                ++path_step;
        }
        /* GO_ITEM stores the reachable pickup checkpoint in x/y; the actual
           item origin lives in aimx/aimy.  Other goals already use x/y as their
           movement destination as well. */
        tx = goal.x;
        ty = goal.y;
        /* Precision edges were validated with BOT_RADIUS. Inflating it here
           can reject the very corridor selected by the flood near a wall. */
        probe_radius = ledge_protect ? BOT_RADIUS :
                       (progress_sector >= 0 && leveltime < progress_until ?
                        PLAYERRADIUS : PLAYERRADIUS+2*FRACUNIT);
        {
            boolean stable_goal = goal.cell >= 0 && goal.cell < count &&
                                  cells[goal.cell].floor >= mo->z-24*FRACUNIT;
            boolean progress_goal =
                map03_route &&
                progress_sector >= 0 &&
                leveltime < progress_until &&
                goal.type == GO_WALK &&
                goal.line < 0 &&
                P_AproxDistance(goal.aimx-progress_x,
                                goal.aimy-progress_y) < 8*FRACUNIT;
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
                    corner = Bot_PathTurnsAt(path_step) || path_step == path_len-1;

                    /* Brake before every geometric turn OR stair transition.
                       The latter matters on MAP02 because the dangerous hole is
                       beside a staircase even where the grid direction itself
                       remains straight. */
                    if (corner && wpdist < 8*FRACUNIT) {
                        if (speednow > FRACUNIT/2) {
                            stop = true;
                        } else {
                            ledge_speed = 2.2;
                        }
                    }

                    /* Even a cardinal graph edge becomes a diagonal command if
                       the player arrived a little off-centre.  Rail the command
                       back onto this edge before allowing forward progress. */
                    if (!stop && path_step > 0) {
                        /* The initial connector was checked from the real
                           position by Bot_NearLedgeStart. It is not the next
                           grid edge in reverse: railing it can replace a safe
                           diagonal connector with a blocked cardinal move. */
                        Bot_LedgeRailTarget(mo,path[path_step-1],path[path_step],tx,ty,
                                            &tx,&ty,&ledge_speed);
                    }
                } else {
                    /* Bot_ItemCandidate already chose goal.cell inside the
                       item's pickup envelope.  Do NOT invent one more approach
                       vector here.  Reaching the final grid checkpoint is enough
                       to collect the item and, more importantly, cannot recreate
                       the diagonal shortcut across MAP02's stairwell. */
                    tx = goal.x;
                    ty = goal.y;
                    ledge_speed = 2.0;
                }
                /* Keep the controller target equal to the arrival checkpoint.
                   Shifting it every tic left the original cell unreached and
                   could turn a cardinal path back into a diagonal pit shortcut. */

                if (!stop && !Bot_WalkLedgeSafe(mo->x,mo->y,mo->z,tx,ty,mo,true)) {
                    stop = true;
                    if (leveltime >= next_ledge_diagnostic) {
                        boolean geometry_ok = Bot_WalkLedgeSafe(mo->x,mo->y,mo->z,tx,ty,mo,false);
                        I_Log("Bot: ledge blocked pos=(%d,%d,%d) target=(%d,%d) path=%d/%d geometry=%d\n",
                              mo->x/FRACUNIT,mo->y/FRACUNIT,mo->z/FRACUNIT,
                              tx/FRACUNIT,ty/FRACUNIT,path_step,path_len,geometry_ok);
                        next_ledge_diagnostic = leveltime+TICRATE;
                    }
                    /* Schedule once; never push a pending retry farther away.
                       Extending it whenever it was two tics away prevented
                       Bot_Plan from ever running during a persistent stop. */
                    if (next_plan > leveltime + 8)
                        next_plan = leveltime + 8;
                }

                /* Slow down based on actual free floor around the player's
                   centre, not just whether the route is technically legal. */
                {
                    int clearance=Bot_DropClearance(mo->x,mo->y);
                    if (clearance<=12 && ledge_speed>1.50) ledge_speed=1.50;
                    else if (clearance<=20 && ledge_speed>2.40) ledge_speed=2.40;
                    else if (clearance<=28 && ledge_speed>3.20) ledge_speed=3.20;
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
                        stop=false; ledge_speed=1.50;
                    } else {
                        stop=true;
                    }
                } else if (!stop && Bot_LedgeMomentumDanger(mo)) {
                    stop=true;
                    ledge_speed=0.0;
                }
            } else {
                direct_ok = progress_goal ?
                    Bot_Walk(mo->x,mo->y,mo->z,tx,ty,mo,true) :
                    stable_goal ?
                    Bot_WalkStable(mo->x,mo->y,mo->z,tx,ty,mo,true) :
                    Bot_Walk(mo->x,mo->y,mo->z,tx,ty,mo,true);

                if (!direct_ok) {
                    if (path_step < path_len) { tx = Bot_X(path[path_step]); ty = Bot_Y(path[path_step]); }
                    for (i=path_step; i<path_len && i<path_step+lookahead; ++i) {
                        fixed_t px=Bot_X(path[i]), py=Bot_Y(path[i]);
                        boolean stable_step = cells[path[i]].floor >= mo->z-24*FRACUNIT;
                        boolean step_ok;
                        if (P_AproxDistance(px-mo->x,py-mo->y)>maxlook) break;
                        step_ok = progress_goal ?
                            Bot_Walk(mo->x,mo->y,mo->z,px,py,mo,true) :
                            stable_step ?
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
            boolean progress_line = map03_route &&
                (goal.line == 498 || goal.line == 500 || goal.line == 458 ||
                 (progress_sector >= 0 &&
                  ((lines[goal.line].frontsector &&
                    lines[goal.line].frontsector == &sectors[progress_sector]) ||
                   (lines[goal.line].backsector &&
                    lines[goal.line].backsector == &sectors[progress_sector]))));
            boolean canuse = linedist <= USERANGE &&
                             (Bot_CanUse(mo,useaim,&lines[goal.line]) || progress_line);

            /* Stop only when Doom can actually reach this linedef.  Reaching
               the abstract staging point alone is not enough: around recessed
               panels/corners that used to freeze the bot nose-first forever. */
            if (canuse) {
                tx=goal.aimx; ty=goal.aimy; aim=useaim;
                stop=true; ready_to_use=true;
                use_fail_line = -1; use_fail_since = 0;
            } else if (dist < 12*FRACUNIT) {
                if (progress_line) {
                    /* Narrow lift doors can make the exact ray test flicker at
                       the threshold. Stay on this approach point and let the
                       actual USE trace decide, instead of abandoning the key
                       route for generic exploration. */
                    tx=goal.aimx; ty=goal.aimy; stop=true;
                    aim=useaim;
                    ready_to_use=true;
                    interaction_lock=true;
                } else {
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
                }
            } else {
                use_fail_line = -1; use_fail_since = 0;
                interaction_lock = linedist < 88*FRACUNIT;
            }
            if (goal.type == GO_USE)
                interaction_lock = interaction_lock || ready_to_use || linedist < 88*FRACUNIT;
        } else if (goal.type == GO_RUN && dist < 7*FRACUNIT) {
            tx=goal.aimx; ty=goal.aimy; stop=true; ready_to_run=true;
        } else if (goal.type == GO_ITEM && dist < 2*FRACUNIT) {
            /* The item checkpoint has a 4-unit pickup margin.  Once we are
               within 2 units of it, stop for this tic and immediately replan.
               If the pickup was collected it disappears from the next plan;
               if not, the next plan continues instead of idling for 350 tics. */
            next_plan=leveltime+1;
            stop=true;
        } else if (goal.type == GO_WALK && lift_commit_line >= 0 &&
                   !lift_commit_boarded) {
            /* Finish boarding, not just reaching the old 12-unit envelope. */
            if (dist < 2*FRACUNIT) {
                if (next_plan > leveltime+8) next_plan=leveltime+8;
                stop=true;
            }
        } else if (dist < 12*FRACUNIT && goal.type == GO_WALK &&
                   ((lift_commit_line >= 0 && !lift_commit_boarded) ||
                    (map03_route && progress_sector >= 0 && leveltime < progress_until))) {
            /* Waiting for a moving lift must not trigger a one-tic replan loop.
               Keep progression goals for a few tics while the floor or route
               settles instead of rebuilding the full flood every tic. */
            if (next_plan > leveltime+8) next_plan=leveltime+8;
            stop=true;
        } else if (dist < 12*FRACUNIT && goal.type != GO_USE &&
                   goal.type != GO_RUN && goal.type != GO_ITEM) {
            if (goal.line>=0) { ++line_tries[goal.line]; line_retry[goal.line]=leveltime+350; }
            if (goal.type == GO_EXPLORE) {
                ++sector_visits[R_PointInSubsector(goal.x,goal.y)->sector-sectors];
                /* Do not select the same reached cell on the next plan. This
                   breaks room-local A/B exploration loops without blocking a
                   later return when the map has genuinely changed. */
                explore_cooldown_cell=goal.cell;
                explore_cooldown_until=leveltime+350;
                /* Do not postpone the replan on every tic while the controller
                   is braking inside the arrival radius. */
                if (next_plan > leveltime+18)
                    next_plan=leveltime+18;
            } else {
                next_plan=leveltime+1;
            }
            stop=true;
        }
    } else stop=true;

    /* Door continuation overrides whatever the normal planner picked (ammo,
       explore, etc.).  Wait at the opening while closed; as soon as the current
       sector heights make the route legal, walk straight through it. */
    if (door_commit_line >= 0 && leveltime < door_commit_until) {
        boolean geometry_open;
        boolean actor_open;
        fixed_t crossx=door_tx, crossy=door_ty;
        probe_radius = BOT_RADIUS;

        /* Once the sector opens, find a reachable point just across this
           particular doorway instead of insisting on one fixed deep target. */
        geometry_open = Bot_FindDoorCrossPoint(mo,door_commit_line,false,&crossx,&crossy);
        actor_open = geometry_open &&
            Bot_FindDoorCrossPoint(mo,door_commit_line,true,&crossx,&crossy);
        if (geometry_open) {
            door_tx=crossx; door_ty=crossy;
        }

        if (actor_open) {
            /* The doorway is physically traversable, but an enemy visible from
               here must still be allowed to steal aim/movement.  The old
               interaction_lock + door_cross combination made the bot ignore a
               monster literally standing in front of the open door. */
            tx=door_tx; ty=door_ty; stop=false; door_cross=true;
            interaction_lock = false;
            door_combat_window = true;
        } else if (!geometry_open) {
            /* Door itself is still shut: face it, wait/re-use.  While the solid
               door blocks the route there is no useful room-side combat trace
               to pursue through it. */
            tx=door_ax; ty=door_ay; stop=true; door_waiting=true;
            interaction_lock = true;
        } else {
            /* Geometry is open but a solid actor blocks the tested crossing.
               Preserve this exact door commit and fight instead of selecting
               another navigation goal. */
            tx=door_ax; ty=door_ay; stop=true;
            interaction_lock = false;
            door_combat_window = true;
        }
        probe_radius = BOT_RADIUS;
    }

    if (leveltime < post_use_hold_until &&
        door_commit_line < 0 && !ledge_protect && !ready_to_run &&
        !lift_riding) {
        tx = post_use_look_x;
        ty = post_use_look_y;
        stop = true;
    }

    if (lift_riding) {
        if (map03_route && !map03_platform_done) {
            tx=2816*FRACUNIT; ty=3360*FRACUNIT; stop=false;
        } else { tx=mo->x; ty=mo->y; stop=true; }
    }

    if (!threat && perch_enemy && Bot_HasRangedAmmo(p) &&
        !map04_crate_route && !ledge_protect && !lift_riding &&
        !ready_to_run && !interaction_lock && door_commit_line < 0 &&
        Bot_PerchApproach(mo,perch_enemy,&tx,&ty)) {
        perch_move=true;
        stop=false;
        combat_pause_until=leveltime+TICRATE;
    }

    aim = R_PointToAngle2(mo->x, mo->y, tx, ty);

    if (goal.line >= 0)
        exit_goal = Bot_IsExitSpecial(lines[goal.line].special);

    /* Closed-door operation stays atomic, but once the doorway is OPEN the bot
       must defend itself.  door_combat_window keeps the same door commit alive
       while allowing a real weapon trace to acquire monsters in the next room. */
    if (ready_to_run || leveltime < exit_commit_until ||
        ((exit_goal || door_waiting || (interaction_lock && !door_combat_window)) &&
         !(threat && (p->health <= 35 || nearby_attackers >= 3))))
        threat = NULL;


    /* A short lost-sight grace holds the previous view direction, but movement
       remains under navigation control until a real weapon trace reacquires the
       monster.  This prevents path-angle / monster-angle ping-pong at corners. */
    boolean combat_view_grace = (!threat && combat_target && leveltime < combat_commit_until);
    boolean prefer_combat = false;
    boolean ranged_ammo = Bot_HasRangedAmmo(p);
    boolean can_fire = false;

    if (threat && combat_has_shot) {
        fixed_t d = P_AproxDistance(threat->x - mo->x, threat->y - mo->y);
        fixed_t mymid = mo->z + mo->height/2;
        fixed_t hismid = threat->z + threat->height/2;
        fixed_t dz = abs(hismid-mymid);
        boolean combat_moved = false;

        boolean urgent = p->health <= 35 || nearby_attackers >= 3;
        boolean high_ground = mo->z >= threat->z + 32*FRACUNIT;
        /* Precision routes forbid strafing, not braking to defend ourselves. */
        prefer_combat = ranged_ammo && d < (urgent ? 800 : 640)*FRACUNIT &&
            (!map04_crate_route || urgent);
        can_fire = d < 800*FRACUNIT;
        /* While approaching a timed platform, combat may still aim and fire,
           but it must not replace the route with an endless dodge/strafe. */
        if (lift_commit_line >= 0 && !lift_commit_boarded && !urgent)
            prefer_combat = false;

        /* When the target is almost overhead, its XY bearing can flip by 90-180
           degrees from a one-unit movement.  Do not let that pathological
           azimuth steal the first-person view; move out from underneath first. */
        if (!(dz > 40*FRACUNIT && d < 96*FRACUNIT)) {
            if (!(map04_crate_route && Bot_HasKey(p,it_redcard)) || prefer_combat)
                aim = combat_last_aim;
            /* Barrel safety was checked for the current weapon. Keep it;
               switching here would invalidate its spread/blast calculation. */
            if (threat->type != MT_BARREL) Bot_Weapon(cmd, p, d);
        } else {
            prefer_combat = false;
        }

        if (!combat_route_lock && !high_ground && !lift_riding && !door_combat_window &&
            (threat->type == MT_CHAINGUY || urgent) && d < 420*FRACUNIT) {
            fixed_t retreatx, retreaty;
            if (Bot_FindHitscanRetreat(mo,threat,ledge_protect,
                                       &retreatx,&retreaty)) {
                /* Keep shooting while moving away. combat_moved prevents the
                   generic combat branch below from replacing this retreat
                   with a stationary firing position. */
                tx=retreatx; ty=retreaty;
                stop=false;
                combat_moved=true;
            }
        }

        if (weaponinfo[p->readyweapon].ammo == am_noammo) prefer_combat=false;
        if (!ranged_ammo && !ledge_protect && !door_waiting &&
            goal.type != GO_ITEM && d < 192*FRACUNIT && dz < 24*FRACUNIT &&
            Bot_WalkStable(mo->x,mo->y,mo->z,threat->x,threat->y,mo,false)) {
            tx=threat->x; ty=threat->y; stop=false;
            combat_moved=true;
        }

        /* Ordinary combat strafing is now a short burst around the position
           where this engagement began.  v8 rebuilt a 48-unit side target from
           the NEW player position every tic, which effectively meant "keep
           walking sideways forever" and could carry the bot into another room. */
        if (prefer_combat && !combat_route_lock && !high_ground && !door_combat_window &&
            !lift_riding && threat->type != MT_BARREL &&
            leveltime >= missile_dodge_pause_until &&
            d < 350*FRACUNIT && d > 96*FRACUNIT && dz < 40*FRACUNIT) {
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
        /* An open doorway is a chokepoint, not permission to body-block our way
           through a monster.  Stop on our current side, shoot the visible threat,
           and retain door_commit_line.  After the fight, the exact same atomic
           press->cross continuation resumes automatically. */
        if (lift_riding && prefer_combat) {
            stop=true;
            combat_strafe_until=0;
            combat_strafe_pause_until=leveltime+8;
        } else if (door_combat_window && prefer_combat) {
            stop = true;
            door_cross = false;
            door_fighting = true;
            combat_strafe_until = 0;
            combat_strafe_pause_until = leveltime + 8;
        } else if (prefer_combat && !combat_moved) {
            stop = true;
        }
        if (prefer_combat) {
            combat_pause_until = leveltime + TICRATE;
            perch_move = high_ground && !lift_riding;
            ready_to_use = false;
            interaction_lock = false;
        }
        if (mlook) lookdir = 0;
    } else if (ranged_ammo && combat_view_grace && !interaction_lock && !ready_to_run &&
               !exit_goal && !door_waiting &&
               !(map04_crate_route && Bot_HasKey(p,it_redcard))) {
        /* Recompute the bearing to the last-known position. A frozen absolute
           angle becomes wrong as the player moves and can itself cause a snap.
           At an open doorway, hold the chokepoint briefly through a LOS flicker
           instead of instantly walking into the room where the monster vanished. */
        aim = R_PointToAngle2(mo->x,mo->y,combat_last_x,combat_last_y);
        if (leveltime < combat_pause_until) stop = true;
        if (door_combat_window) {
            stop = true;
            door_cross = false;
            door_fighting = true;
        }
        if (mlook) lookdir = 0;
    } else if (mlook) {
        lookdir = 0;
    }

    /* Target strafing above only reacts to enemy position.  Missiles need their
       own avoidance pass: predict impact and override movement while preserving
       the aim on the monster.  Do not do this during precision traversal or
       while operating/crossing a door. */
    if (!combat_route_lock && !perch_move && !ready_to_run && !interaction_lock &&
        !door_cross && !door_waiting && !door_fighting && !lift_riding &&
        !(lift_commit_line >= 0 && !lift_commit_boarded) &&
        leveltime >= exit_commit_until) {
        fixed_t dodgex, dodgey;
        if (Bot_ProjectileDodge(mo,stop ? mo->x : tx,stop ? mo->y : ty,&dodgex,&dodgey)) {
            /* A dodge supersedes the old strafe; do not snap back to its stale
               target as soon as the projectile passes. */
            combat_strafe_until=0;
            combat_strafe_pause_until=leveltime+14;
            tx=dodgex; ty=dodgey; stop=false;
        }
    }

    turn = (short)((aim - mo->angle) >> 16);
    cmd->angleturn = Bot_Clamp(turn, 2400);
    if (map04_crate_route && Bot_HasKey(p,it_redcard) &&
        !prefer_combat && abs(turn) > 6000)
        stop=true;

    if (door_fighting && threat && leveltime >= next_door_combat_log) {
        I_Log("Bot: door combat line=%d enemy=%d dist=%d hold crossing\n",
              door_commit_line,threat->type,
              P_AproxDistance(threat->x-mo->x,threat->y-mo->y)/FRACUNIT);
        next_door_combat_log = leveltime + TICRATE;
    }

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
    if (goal.type == GO_USE && ready_to_use && door_commit_line < 0 && leveltime >= use_until &&
        abs(turn) < 650) {
        angle_t useangle = mo->angle + (angle_t)((int)cmd->angleturn * 65536);
        if (Bot_CanUse(mo,useangle,&lines[goal.line]) ||
            (map03_route &&
             (goal.line == 498 || goal.line == 500 || goal.line == 458 ||
              (progress_sector >= 0 &&
               ((lines[goal.line].frontsector &&
                 lines[goal.line].frontsector == &sectors[progress_sector]) ||
                (lines[goal.line].backsector &&
                 lines[goal.line].backsector == &sectors[progress_sector])))))) {

        int sp = lines[goal.line].special;
        cmd->buttons |= BT_USE;
        if (map03_route &&
            (goal.line == 498 || goal.line == 500 || goal.line == 458)) {
            /* The two lower-room panels are narrow one-sided lines.  A
               perfectly aligned visual ray can still lose the intercept after
               fixed-point momentum moves the player a few units on the same
               tic.  Preserve the human USE command, but also submit the exact
               story line once the controller has reached its bounded approach
               point so the puzzle cannot be abandoned on a harmless ray flicker. */
            P_UseSpecialLine(mo,&lines[goal.line],0);
        }
        ++line_tries[goal.line];

        // РџРѕСЃР»Рµ Р»СЋР±РѕРіРѕ РёСЃРїРѕР»СЊР·РѕРІР°РЅРёСЏ СЃРІРёС‚С‡Р°/РґРІРµСЂРё вЂ” РІСЂРµРјРµРЅРЅРѕ Р·Р°РїСЂРµС‰Р°РµРј РІРѕР·РІСЂР°С‚
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
        else if (Bot_UseDoor(&lines[goal.line]) ||
                 (map03_route && goal.line == 458 && sp == 33)) {
            /* Press -> cross is one atomic navigation action.  Do not allow an
               ammo pickup behind us to replace the second half while the door
               is opening. */
            Bot_StartDoorCommit(mo,goal.line,goal.aimx,goal.aimy);
            line_retry[goal.line] = leveltime + 40;
            next_plan = leveltime + 24;
            use_until = leveltime + 35;
        } else if (Bot_LiftUseSpecial(sp)) {
            /* Calling a lift starts a persistent call->board->ride transaction.
               The action may be physically unreachable for several seconds
               while the floor descends, so generic post-use exploration is not
               enough. */
            Bot_StartLiftCommit(mo,goal.line);

            pending_use_line = goal.line;
            pending_use_special = sp;
            pending_use_until = leveltime + 6;
            Bot_StartActionCommit(goal.line);
            line_retry[goal.line] = leveltime + 18;
            next_plan = leveltime + 5;
            stop = true;
            goal.type = GO_NONE;
            use_until = leveltime + 14;
        } else {
            /* Remote switch/platform interaction: briefly stay and watch it,
               then keep decisions local instead of instantly backtracking for
               old ammo in another room. */
            post_use_line = goal.line;
            post_use_sector = mo->subsector->sector-sectors;
            post_use_x = mo->x; post_use_y = mo->y;
            post_use_look_x = goal.aimx; post_use_look_y = goal.aimy;
            post_use_hold_until = leveltime + 18;   /* ~0.5 s */
            post_use_local_until = leveltime + 140; /* ~4 s */
            commit_forward_until = post_use_local_until;

            /* Remember the attempted remote switch, but only mark it as used
               after the next tics prove that its special changed or a tagged
               sector thinker started. */
            pending_use_line = goal.line;
            pending_use_special = sp;
            pending_use_until = leveltime + 4;
            line_retry[goal.line] = leveltime + 18;
            next_plan = leveltime + 8;
            stop = true;
            goal.type = GO_NONE;
            use_until = leveltime + 14;
        }

        I_Log("Bot: use line=%d special=%d\n", goal.line, sp);
        }
    }
    else if (threat && can_fire && abs(turn) < 900 &&
             (threat->type != MT_BARREL || p->pendingweapon == wp_nochange)) {
        angle_t shot = mo->angle + (angle_t)((int)cmd->angleturn * 65536);
        ammotype_t ammo=weaponinfo[p->readyweapon].ammo;
        int needed=p->readyweapon==wp_bfg ? 40 : p->readyweapon==wp_supershotgun ? 2 : 1;
        P_AimLineAttack(mo, shot, ammo==am_noammo ? MELEERANGE : MISSILERANGE);
        if (linetarget == threat && (ammo==am_noammo || p->ammo[ammo]>=needed)) {
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
            /* Replanning every tic rebuilt the exact same GO_ITEM path and
               permanently reset path_step=0. Retry at human-scale cadence;
               if this persists, the new ledge-safe flood start will choose a
               reachable grid centre on the next rebuild. */
            if (next_plan > leveltime + 8)
                next_plan=leveltime+8;
            if (++stuck_since>25) {
                next_plan=leveltime+1;
                stuck_since=0;
            }
        } else {
            /* Runtime obstacle avoidance must follow the NAVIGATION vector, not
               the first-person aim.  During combat aim can point directly at a
               zombie standing on a crate while the feet need to go around it;
               using 'aim' here was exactly why the bot tried to cuddle the box.

               Test useful 45..90 degree side steps on BOTH sides and choose the
               one that leaves us closest to the blocked navigation target. */
            static const int turns[] = { 2, 3, 4 };   /* 45, 67.5, 90 deg */
            static const int radii[] = { 32, 48 };
            fixed_t blocked_tx=tx, blocked_ty=ty;
            fixed_t blocked_dist=P_AproxDistance(blocked_tx-mo->x,
                                                  blocked_ty-mo->y);
            angle_t moveaim=R_PointToAngle2(mo->x,mo->y,blocked_tx,blocked_ty);
            fixed_t bestx=0, besty=0;
            int bestscore=INT_MAX;
            boolean found=false;
            int pass,k,r;

            for (pass=0; pass<2; ++pass) {
                int side=pass ? -1 : 1;
                for (k=0;k<(int)(sizeof(turns)/sizeof(turns[0]));++k) {
                    int an=(moveaim+(angle_t)(side*turns[k]*(ANG45/2)))>>
                           ANGLETOFINESHIFT;
                    for (r=0;r<(int)(sizeof(radii)/sizeof(radii[0]));++r) {
                        fixed_t px=mo->x+FixedMul(radii[r]*FRACUNIT,finecosine[an]);
                        fixed_t py=mo->y+FixedMul(radii[r]*FRACUNIT,finesine[an]);
                        fixed_t remain;
                        int score;

                        if (!Bot_WalkStable(mo->x,mo->y,mo->z,px,py,mo,true))
                            continue;

                        remain=P_AproxDistance(blocked_tx-px,blocked_ty-py);

                        /* Doom geometry often requires backing away first
                           before turning around a pillar/crate. Do not reject
                           every temporary loss of distance or the bot can
                           oscillate in place forever. */
                        if (remain > blocked_dist + 128*FRACUNIT)
                            continue;

                        score=(int)(remain/FRACUNIT);
                        score += turns[k]*3;
                        score += r*4;

                        if (score < bestscore) {
                            bestscore=score;
                            bestx=px; besty=py;
                            found=true;
                        }
                    }
                }
            }

            if (found) {
                tx=bestx; ty=besty; stop=false;
            } else {
                /* Do not freeze the player when local avoidance fails.
                   Replanning is cheaper than a permanent walk animation loop. */
                stop=true;
                if (next_plan > leveltime+12) next_plan=leveltime+12;
            }

            if (++stuck_since>25) {
                next_plan=leveltime+1;
                stuck_since=0;
            }
        }
    } else stuck_since=0;
    if (lift_riding) {
        /* Steer toward the known landing in world coordinates, including while
           looking at a monster. Other lifts retain their stationary ride. */
        if (map03_route && !map03_platform_done)
            Bot_MoveEx(cmd,mo,2816*FRACUNIT,3360*FRACUNIT,false,4.0);
        else if (map04_route && lift_commit_target_x)
            Bot_MoveEx(cmd,mo,lift_commit_target_x,lift_commit_target_y,false,4.0);
        else
            Bot_MoveEx(cmd,mo,mo->x,mo->y,true,0.0);
    } else if (perch_move && !door_cross) {
        Bot_LedgeMoveAdaptive(cmd,mo,tx,ty,stop,3.0);
    } else if (ledge_protect && !door_cross) {
        Bot_LedgeMoveAdaptive(cmd,mo,tx,ty,stop,ledge_speed);
        /* Command prediction can brake even when the geometric segment passes.
           Watch the actual checkpoint, not the temporary rail/retreat target,
           so neither braking nor oscillation can leave this route stuck. */
        {
            fixed_t wx = path_step < path_len ? Bot_X(path[path_step]) : goal.x;
            fixed_t wy = path_step < path_len ? Bot_Y(path[path_step]) : goal.y;
            fixed_t remaining = P_AproxDistance(wx-mo->x,wy-mo->y);
            if (progress_dist == INF || remaining < progress_dist-FRACUNIT/2) {
                progress_dist = remaining;
                last_progress = leveltime;
            } else if (leveltime-last_progress >= TICRATE) {
                next_plan = leveltime+1;
                last_progress = leveltime;
                I_Log("Bot: ledge stalled pos=(%d,%d) target=(%d,%d) path=%d/%d cmd=(%d,%d)\n",
                      mo->x/FRACUNIT,mo->y/FRACUNIT,wx/FRACUNIT,wy/FRACUNIT,
                      path_step,path_len,cmd->forwardmove,cmd->sidemove);
            }
        }
    } else
        /* The first MAP03 lift has a very short boarding window. Approach its
           walk-over trigger at full steering speed, then the lift_riding branch
           above takes over and keeps moving forward. */
        if (map03_route && progress_sector >= 0 && leveltime < progress_until &&
            sectors[progress_sector].floorheight <= mo->z + 24*FRACUNIT &&
            goal.type == GO_WALK && goal.line < 0 &&
            P_AproxDistance(goal.aimx-progress_x,
                            goal.aimy-progress_y) < 8*FRACUNIT)
            /* Keep the speed-up, but honor combat/local-avoidance overrides. */
            Bot_MoveEx(cmd,mo,tx,ty,stop,7.0);
        else if (map03_route && !map03_platform_done &&
            (goal.line == 86 || goal.line == 87))
            Bot_MoveEx(cmd,mo,tx,ty,stop,10.0);
        else
            Bot_Move(cmd,mo,tx,ty,stop);
}
