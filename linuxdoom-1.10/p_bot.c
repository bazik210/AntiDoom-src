//-----------------------------------------------------------------------------
//
// Autonomous DOOM II First-Person Bot AI
//
// Features:
// - Dynamic level waypoint graph generation from map geometry (portals, doors, keys, exit)
// - Robust doorway connectivity (front/mid/back nodes linked through door threshold)
// - Walkable exit switch approach nodes (placed in front of 1-sided wall switches)
// - Smart key progression: identifies locked doors, prioritizes finding required
//   keys (Red, Blue, Yellow), and avoids pressing locked doors without keys
// - Pulsed BT_USE interaction (prevents usedown lock and wall grunting)
// - Decoupled path execution from replanning (advances step-by-step to exit)
// - Multi-whisker collision detection (P_CheckPosition) for smooth cornering
// - Threat detection using engine BSP line-of-sight (P_CheckSight)
// - Smooth horizontal camera tracking and full 3D vertical pitch aiming (lookdir)
// - Intelligent weapon selection (SSG, Shotgun, Chaingun, Rockets, Plasma)
// - Tactical combat strafing, circle-strafing, and backpedaling from melee threats
//
//-----------------------------------------------------------------------------

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

#include "doomdef.h"
#include "doomstat.h"
#include "d_player.h"
#include "d_ticcmd.h"
#include "d_event.h"
#include "p_local.h"
#include "p_spec.h"
#include "p_inter.h"
#include "r_local.h"
#include "r_main.h"
#include "s_sound.h"
#include "sounds.h"
#include "i_system.h"
#include "p_bot.h"

boolean bot_active = false;

extern int lookdir;
extern int mlook;

#define MAX_BOT_WAYPOINTS 640
#define MAX_BOT_EDGES     24
#define ANG10             (ANG45 / 4)
#define BOT_MAXMOVE       50

typedef enum {
    WP_NORMAL = 0,
    WP_DOOR,
    WP_SWITCH,
    WP_EXIT,
    WP_KEY,
    WP_ITEM
} wptype_t;

typedef struct {
    fixed_t x, y, z;
    wptype_t type;
    line_t* line;
    int required_key; // it_bluecard, it_redcard, it_yellowcard or -1
    int visits;
    int num_edges;
    int edges[MAX_BOT_EDGES];
    int edge_dist[MAX_BOT_EDGES];
} bot_wp_t;

static bot_wp_t bot_wp[MAX_BOT_WAYPOINTS];
static int bot_num_wp = 0;
static int bot_exit_wp = -1;

// Combat state
static int bot_strafe_dir = 1;
static int bot_strafe_timer = 0;

// Stuck recovery
static fixed_t bot_last_x = 0;
static fixed_t bot_last_y = 0;
static int bot_stuck_tics = 0;
static int bot_last_keys = 0;

// Path cache
static int bot_current_target_wp = -1;
static int bot_path_cache[MAX_BOT_WAYPOINTS];
static int bot_path_len = 0;
static int bot_path_step = 0;
static int bot_replan_timer = 0;

// Check if a line is an exit trigger or switch
static boolean Bot_IsExitLine(line_t* line)
{
    if (!line) return false;
    switch (line->special)
    {
        case 11:  // S1 Exit
        case 51:  // S1 Secret Exit
        case 52:  // W1 Exit
        case 124: // W1 Secret Exit
        case 197: // G1 Exit
        case 198: // GR Exit
            return true;
        default:
            return false;
    }
}

// Check what key a door requires
static int Bot_DoorRequiredKey(line_t* line)
{
    if (!line) return -1;
    switch (line->special)
    {
        case 26: case 32: case 99: case 133:
            return it_bluecard;
        case 28: case 33: case 134: case 135:
            return it_redcard;
        case 27: case 34: case 136: case 137:
            return it_yellowcard;
        default:
            return -1;
    }
}

// Check if player has the required key (cards or skulls both satisfy)
static boolean Bot_HasKey(player_t* player, int key)
{
    if (key < 0) return true;
    if (key == it_bluecard) return player->cards[it_bluecard] || player->cards[it_blueskull];
    if (key == it_redcard) return player->cards[it_redcard] || player->cards[it_redskull];
    if (key == it_yellowcard) return player->cards[it_yellowcard] || player->cards[it_yellowskull];
    return true;
}

// Check if a line is any door type
static boolean Bot_IsDoor(line_t* line)
{
    if (!line) return false;
    switch (line->special)
    {
        case 1: case 2: case 4: case 10:
        case 26: case 27: case 28: case 29:
        case 31: case 32: case 33: case 34:
        case 46: case 63: case 86: case 99:
        case 103: case 114: case 115: case 116:
        case 117: case 118:
        case 133: case 134: case 135: case 136: case 137:
            return true;
        default:
            return false;
    }
}

// Check if door is currently closed
static boolean Bot_IsDoorClosed(line_t* line)
{
    if (!line || !line->backsector) return false;
    fixed_t h = line->backsector->ceilingheight - line->backsector->floorheight;
    if (h < 56*FRACUNIT) return true;
    return false;
}

// Helper: Check sight between arbitrary coordinates via BSP
static boolean Bot_CheckSightCoords(fixed_t x1, fixed_t y1, fixed_t z1, fixed_t x2, fixed_t y2, fixed_t z2)
{
    static mobj_t d1, d2;
    subsector_t* ss1 = R_PointInSubsector(x1, y1);
    subsector_t* ss2 = R_PointInSubsector(x2, y2);
    if (!ss1 || !ss2) return false;

    memset(&d1, 0, sizeof(d1));
    memset(&d2, 0, sizeof(d2));

    d1.x = x1; d1.y = y1; d1.z = z1; d1.height = 56*FRACUNIT; d1.subsector = ss1;
    d2.x = x2; d2.y = y2; d2.z = z2; d2.height = 56*FRACUNIT; d2.subsector = ss2;

    return P_CheckSight(&d1, &d2);
}

// Add a directed edge from u to v
static void Bot_AddDirectedEdge(int u, int v, int dist)
{
    if (u < 0 || v < 0 || u >= bot_num_wp || v >= bot_num_wp || u == v) return;

    for (int i = 0; i < bot_wp[u].num_edges; i++)
    {
        if (bot_wp[u].edges[i] == v) return;
    }

    if (bot_wp[u].num_edges < MAX_BOT_EDGES)
    {
        int e = bot_wp[u].num_edges++;
        bot_wp[u].edges[e] = v;
        bot_wp[u].edge_dist[e] = dist;
    }
}

// Add a bidirectional edge between waypoints
static void Bot_AddEdge(int u, int v, int dist)
{
    Bot_AddDirectedEdge(u, v, dist);
    Bot_AddDirectedEdge(v, u, dist);
}

// Add a waypoint
static int Bot_AddWaypoint(fixed_t x, fixed_t y, fixed_t z, wptype_t type, line_t* line, int req_key)
{
    if (bot_num_wp >= MAX_BOT_WAYPOINTS) return -1;

    // Check proximity to existing waypoints (don't merge doors/exits/keys)
    if (type == WP_NORMAL)
    {
        for (int i = 0; i < bot_num_wp; i++)
        {
            fixed_t d = P_AproxDistance(bot_wp[i].x - x, bot_wp[i].y - y);
            if (d < 36*FRACUNIT)
            {
                return i;
            }
        }
    }

    int idx = bot_num_wp++;
    bot_wp[idx].x = x;
    bot_wp[idx].y = y;
    bot_wp[idx].z = z;
    bot_wp[idx].type = type;
    bot_wp[idx].line = line;
    bot_wp[idx].required_key = req_key;
    bot_wp[idx].visits = 0;
    bot_wp[idx].num_edges = 0;

    if (type == WP_EXIT) bot_exit_wp = idx;
    return idx;
}

// Connect waypoints with visible Line of Sight
static void Bot_BuildGraph(void)
{
    for (int i = 0; i < bot_num_wp; i++)
    {
        for (int j = i + 1; j < bot_num_wp; j++)
        {
            fixed_t dist = P_AproxDistance(bot_wp[i].x - bot_wp[j].x, bot_wp[i].y - bot_wp[j].y);
            if (dist > 1200*FRACUNIT) continue;

            fixed_t f1 = bot_wp[i].z;
            fixed_t f2 = bot_wp[j].z;

            // In DOOM, players can drop down up to 128 units, but only step UP 24 units
            boolean can_ij = ((f2 - f1) <= 24*FRACUNIT) && ((f1 - f2) <= 128*FRACUNIT);
            boolean can_ji = ((f1 - f2) <= 24*FRACUNIT) && ((f2 - f1) <= 128*FRACUNIT);
            if (!can_ij && !can_ji) continue;

            if (Bot_CheckSightCoords(bot_wp[i].x, bot_wp[i].y, f1 + 24*FRACUNIT,
                                     bot_wp[j].x, bot_wp[j].y, f2 + 24*FRACUNIT))
            {
                int idist = (int)(dist >> FRACBITS);
                if (can_ij) Bot_AddDirectedEdge(i, j, idist);
                if (can_ji) Bot_AddDirectedEdge(j, i, idist);
            }
        }
    }
}

// Initialize bot for the current level
void Bot_InitLevel(void)
{
    bot_num_wp = 0;
    bot_exit_wp = -1;
    bot_stuck_tics = 0;
    bot_current_target_wp = -1;
    bot_path_len = 0;
    bot_path_step = 0;
    bot_replan_timer = 0;

    I_Log("Bot: Initializing navigation graph for level...\n");

    // 1. Add Player Spawn
    subsector_t* sp_ss = R_PointInSubsector(playerstarts[0].x * FRACUNIT, playerstarts[0].y * FRACUNIT);
    fixed_t sp_z = sp_ss ? sp_ss->sector->floorheight : 0;
    Bot_AddWaypoint(playerstarts[0].x * FRACUNIT, playerstarts[0].y * FRACUNIT, sp_z, WP_NORMAL, NULL, -1);

    // 2. Scan Lines: doors, portals, exit switches
    for (int i = 0; i < numlines; i++)
    {
        line_t* li = &lines[i];
        fixed_t mx = (li->v1->x + li->v2->x) / 2;
        fixed_t my = (li->v1->y + li->v2->y) / 2;
        fixed_t mz = li->frontsector ? li->frontsector->floorheight : 0;

        fixed_t dx = li->v2->x - li->v1->x;
        fixed_t dy = li->v2->y - li->v1->y;
        fixed_t len = P_AproxDistance(dx, dy);
        if (len == 0) continue;

        // Normal perpendicular to line
        fixed_t nx = FixedDiv(-dy, len);
        fixed_t ny = FixedDiv(dx, len);

        if (Bot_IsExitLine(li))
        {
            if (!(li->flags & ML_TWOSIDED))
            {
                // One-sided wall exit switch: place waypoint 36 units out in front room
                fixed_t fnx = FixedDiv(dy, len);
                fixed_t fny = FixedDiv(-dx, len);
                fixed_t ex = mx + FixedMul(fnx, 36*FRACUNIT);
                fixed_t ey = my + FixedMul(fny, 36*FRACUNIT);
                Bot_AddWaypoint(ex, ey, mz, WP_EXIT, li, -1);
            }
            else
            {
                // Two-sided exit walk-over trigger
                Bot_AddWaypoint(mx, my, mz, WP_EXIT, li, -1);
            }
        }
        else if (li->flags & ML_TWOSIDED)
        {
            int req_key = Bot_DoorRequiredKey(li);
            if (Bot_IsDoor(li) || req_key >= 0)
            {
                // Door: add front, middle, and back waypoints with explicit edge link
                int w_mid = Bot_AddWaypoint(mx, my, mz, WP_DOOR, li, req_key);
                int w_front = Bot_AddWaypoint(mx + FixedMul(nx, 44*FRACUNIT), my + FixedMul(ny, 44*FRACUNIT), mz, WP_NORMAL, NULL, -1);
                int w_back = Bot_AddWaypoint(mx - FixedMul(nx, 44*FRACUNIT), my - FixedMul(ny, 44*FRACUNIT), mz, WP_NORMAL, NULL, -1);

                Bot_AddEdge(w_front, w_mid, 44);
                Bot_AddEdge(w_mid, w_back, 44);
            }
            else
            {
                // Regular room portal or step-down ledge
                fixed_t f1 = li->frontsector->floorheight;
                fixed_t f2 = li->backsector->floorheight;
                fixed_t hdiff = abs(f1 - f2);
                if (hdiff <= 128*FRACUNIT)
                {
                    fixed_t pz = (f1 > f2) ? f1 : f2;
                    Bot_AddWaypoint(mx, my, pz, WP_NORMAL, li, -1);
                }
            }
        }
    }

    // 3. Scan Items: Keys and Weapons
    thinker_t* th;
    if (thinkercap.next)
    {
        for (th = thinkercap.next; th && th != &thinkercap; th = th->next)
        {
            if (th->function.acp1 != (actionf_p1)P_MobjThinker) continue;
            mobj_t* mo = (mobj_t*)th;
            if (!(mo->flags & MF_SPECIAL)) continue;

            switch (mo->sprite)
            {
                case SPR_BKEY: case SPR_BSKU:
                    Bot_AddWaypoint(mo->x, mo->y, mo->z, WP_KEY, NULL, it_bluecard);
                    break;
                case SPR_RKEY: case SPR_RSKU:
                    Bot_AddWaypoint(mo->x, mo->y, mo->z, WP_KEY, NULL, it_redcard);
                    break;
                case SPR_YKEY: case SPR_YSKU:
                    Bot_AddWaypoint(mo->x, mo->y, mo->z, WP_KEY, NULL, it_yellowcard);
                    break;
                case SPR_SGN2: case SPR_MGUN: case SPR_LAUN: case SPR_PLAS: case SPR_BFUG:
                case SPR_SOUL: case SPR_MEGA:
                    Bot_AddWaypoint(mo->x, mo->y, mo->z, WP_ITEM, NULL, -1);
                    break;
                default:
                    break;
            }
        }
    }

    // Connect graph
    Bot_BuildGraph();

    I_Log("Bot: Graph created with %d waypoints (Exit WP: %d)\n", bot_num_wp, bot_exit_wp);
}

void Bot_Init(void)
{
    bot_active = false;
}

// Find nearest visible waypoint to position
static int Bot_FindNearestWaypoint(fixed_t x, fixed_t y, fixed_t z, boolean check_los)
{
    int best = -1;
    fixed_t best_dist = 4000 * FRACUNIT;

    for (int i = 0; i < bot_num_wp; i++)
    {
        fixed_t d = P_AproxDistance(bot_wp[i].x - x, bot_wp[i].y - y);
        if (d < best_dist)
        {
            if (!check_los || Bot_CheckSightCoords(x, y, z + 24*FRACUNIT, bot_wp[i].x, bot_wp[i].y, bot_wp[i].z + 24*FRACUNIT))
            {
                best_dist = d;
                best = i;
            }
        }
    }

    if (best == -1 && check_los) return Bot_FindNearestWaypoint(x, y, z, false);
    return best;
}

// Dijkstra path solver that checks if a path exists between two nodes without altering bot state
static boolean Bot_QueryPath(int start_wp, int goal_wp, player_t* player, int* out_dist)
{
    if (start_wp < 0 || goal_wp < 0 || start_wp >= bot_num_wp || goal_wp >= bot_num_wp)
        return false;
    if (start_wp == goal_wp)
    {
        if (out_dist) *out_dist = 0;
        return true;
    }

    int dist[MAX_BOT_WAYPOINTS];
    boolean visited[MAX_BOT_WAYPOINTS];

    for (int i = 0; i < bot_num_wp; i++)
    {
        dist[i] = 999999;
        visited[i] = false;
    }

    dist[start_wp] = 0;

    for (int count = 0; count < bot_num_wp; count++)
    {
        int u = -1;
        int min_d = 999999;
        for (int i = 0; i < bot_num_wp; i++)
        {
            if (!visited[i] && dist[i] < min_d)
            {
                min_d = dist[i];
                u = i;
            }
        }

        if (u == -1 || u == goal_wp) break;
        visited[u] = true;

        for (int e = 0; e < bot_wp[u].num_edges; e++)
        {
            int v = bot_wp[u].edges[e];
            if (visited[v]) continue;

            if (bot_wp[v].type == WP_DOOR && !Bot_HasKey(player, bot_wp[v].required_key))
                continue;

            int cost = bot_wp[u].edge_dist[e] + bot_wp[v].visits * 30;
            if (dist[u] + cost < dist[v])
            {
                dist[v] = dist[u] + cost;
            }
        }
    }

    if (dist[goal_wp] >= 999999) return false;
    if (out_dist) *out_dist = dist[goal_wp];
    return true;
}

// Dijkstra shortest path on waypoint graph; updates bot_path_cache and resets step to 0
static boolean Bot_FindPath(int start_wp, int goal_wp, player_t* player)
{
    if (start_wp < 0 || goal_wp < 0 || start_wp >= bot_num_wp || goal_wp >= bot_num_wp)
        return false;
    if (start_wp == goal_wp)
    {
        bot_path_len = 1;
        bot_path_cache[0] = start_wp;
        bot_path_step = 0;
        return true;
    }

    int dist[MAX_BOT_WAYPOINTS];
    int prev[MAX_BOT_WAYPOINTS];
    boolean visited[MAX_BOT_WAYPOINTS];

    for (int i = 0; i < bot_num_wp; i++)
    {
        dist[i] = 999999;
        prev[i] = -1;
        visited[i] = false;
    }

    dist[start_wp] = 0;

    for (int count = 0; count < bot_num_wp; count++)
    {
        int u = -1;
        int min_d = 999999;
        for (int i = 0; i < bot_num_wp; i++)
        {
            if (!visited[i] && dist[i] < min_d)
            {
                min_d = dist[i];
                u = i;
            }
        }

        if (u == -1 || u == goal_wp) break;
        visited[u] = true;

        for (int e = 0; e < bot_wp[u].num_edges; e++)
        {
            int v = bot_wp[u].edges[e];
            if (visited[v]) continue;

            // Check if destination is a locked door player cannot open
            if (bot_wp[v].type == WP_DOOR && !Bot_HasKey(player, bot_wp[v].required_key))
                continue;

            int cost = bot_wp[u].edge_dist[e] + bot_wp[v].visits * 30;
            if (dist[u] + cost < dist[v])
            {
                dist[v] = dist[u] + cost;
                prev[v] = u;
            }
        }
    }

    if (prev[goal_wp] == -1) return false;

    // Reconstruct path
    int curr = goal_wp;
    int temp[MAX_BOT_WAYPOINTS];
    int count = 0;

    while (curr != -1 && count < MAX_BOT_WAYPOINTS)
    {
        temp[count++] = curr;
        curr = prev[curr];
    }

    bot_path_len = 0;
    for (int i = count - 1; i >= 0; i--)
    {
        bot_path_cache[bot_path_len++] = temp[i];
    }
    bot_path_step = 0;
    if (bot_path_len > 1 && player && player->mo)
    {
        fixed_t d0 = P_AproxDistance(player->mo->x - bot_wp[bot_path_cache[0]].x, player->mo->y - bot_wp[bot_path_cache[0]].y);
        if (d0 < 56*FRACUNIT)
        {
            bot_path_step = 1;
        }
    }
    return true;
}

// Find closest visible living threat
static mobj_t* Bot_FindThreat(player_t* player)
{
    mobj_t* best = NULL;
    fixed_t best_dist = 1800 * FRACUNIT;
    thinker_t* th;

    if (!player || !player->mo || !thinkercap.next)
        return NULL;

    for (th = thinkercap.next; th && th != &thinkercap; th = th->next)
    {
        if (th->function.acp1 != (actionf_p1)P_MobjThinker) continue;
        mobj_t* mo = (mobj_t*)th;

        if (!(mo->flags & MF_SHOOTABLE) || mo->health <= 0 || mo == player->mo)
            continue;

        if (!P_CheckSight(player->mo, mo))
            continue;

        fixed_t d = P_AproxDistance(player->mo->x - mo->x, player->mo->y - mo->y);
        if (d < best_dist)
        {
            best_dist = d;
            best = mo;
        }
    }
    return best;
}

// Intelligent weapon switcher
static void Bot_SelectWeapon(ticcmd_t* cmd, player_t* player, fixed_t dist)
{
    weapontype_t best = wp_pistol;

    if (player->weaponowned[wp_chainsaw]) best = wp_chainsaw;
    if (player->weaponowned[wp_pistol] && player->ammo[am_clip] > 0) best = wp_pistol;
    if (player->weaponowned[wp_shotgun] && player->ammo[am_shell] >= 1) best = wp_shotgun;
    if (player->weaponowned[wp_chaingun] && player->ammo[am_clip] >= 1) best = wp_chaingun;
    if (player->weaponowned[wp_supershotgun] && player->ammo[am_shell] >= 2 && dist < 550*FRACUNIT) best = wp_supershotgun;
    if (player->weaponowned[wp_missile] && player->ammo[am_misl] >= 1 && dist > 350*FRACUNIT) best = wp_missile;
    if (player->weaponowned[wp_plasma] && player->ammo[am_cell] >= 1) best = wp_plasma;
    if (player->weaponowned[wp_bfg] && player->ammo[am_cell] >= 40 && dist > 250*FRACUNIT) best = wp_bfg;

    if (best != player->readyweapon && best != player->pendingweapon)
    {
        cmd->buttons |= BT_CHANGE;
        cmd->buttons |= (best << BT_WEAPONSHIFT);
    }
}

// Vertical 3D freelook pitch aiming
static void Bot_AimPitch(player_t* player, mobj_t* target, fixed_t dist_horiz)
{
    if (!mlook || dist_horiz < 32*FRACUNIT) return;

    fixed_t eye_z = player->mo->z + (player->viewheight);
    fixed_t target_z = target->z + (target->height / 2);
    fixed_t dz = target_z - eye_z;

    int target_pitch = (int)(dz / (dist_horiz >> 6));
    if (target_pitch > 60) target_pitch = 60;
    if (target_pitch < -60) target_pitch = -60;

    if (lookdir < target_pitch)
    {
        lookdir += 8;
        if (lookdir > target_pitch) lookdir = target_pitch;
    }
    else if (lookdir > target_pitch)
    {
        lookdir -= 8;
        if (lookdir < target_pitch) lookdir = target_pitch;
    }
}

// Main bot decision maker called every tic
void Bot_BuildTiccmd(ticcmd_t* cmd, player_t* player)
{
    memset(cmd, 0, sizeof(*cmd));

    if (!player || !player->mo || !usergame || gamestate != GS_LEVEL)
        return;

    // 1. Respawn if dead
    if (player->playerstate == PST_DEAD)
    {
        if ((gametic & 2) == 0) cmd->buttons |= BT_USE;
        return;
    }

    static int bot_log_tic = 0;
    if (++bot_log_tic > 70)
    {
        bot_log_tic = 0;
        I_Log("Bot: HP=%d, Pos=(%d, %d), TargetWP=%d (step %d/%d)\n",
              player->health,
              (int)(player->mo->x >> FRACBITS), (int)(player->mo->y >> FRACBITS),
              bot_current_target_wp, bot_path_step, bot_path_len);
    }

    // 2. Combat Evaluation (Priority 1)
    mobj_t* threat = Bot_FindThreat(player);
    if (threat)
    {
        fixed_t dist = P_AproxDistance(player->mo->x - threat->x, player->mo->y - threat->y);
        angle_t target_ang = R_PointToAngle2(player->mo->x, player->mo->y, threat->x, threat->y);
        int adiff = (int)(target_ang - player->mo->angle);

        // Smooth camera turning towards threat
        short turn = (short)(adiff >> 16);
        if (turn > 1400) turn = 1400;
        if (turn < -1400) turn = -1400;
        cmd->angleturn = turn;

        // Vertical 3D aim
        Bot_AimPitch(player, threat, dist);

        // Weapon Selection
        Bot_SelectWeapon(cmd, player, dist);

        // Attack if roughly facing enemy
        if (abs(adiff) < (ANG10))
        {
            cmd->buttons |= BT_ATTACK;
        }

        // Tactical Movement: Strafe & Distance Control with Whisker Wall Detection
        if (++bot_strafe_timer > 30)
        {
            bot_strafe_timer = 0;
            bot_strafe_dir = -bot_strafe_dir;
        }

        int s_an = (player->mo->angle + (bot_strafe_dir > 0 ? -ANG90 : ANG90)) >> ANGLETOFINESHIFT;
        fixed_t s_x = player->mo->x + FixedMul(32*FRACUNIT, finecosine[s_an]);
        fixed_t s_y = player->mo->y + FixedMul(32*FRACUNIT, finesine[s_an]);
        if (!P_CheckPosition(player->mo, s_x, s_y))
        {
            bot_strafe_dir = -bot_strafe_dir;
            bot_strafe_timer = 0;
            cmd->sidemove = 0;
        }
        else
        {
            cmd->sidemove = (signed char)(bot_strafe_dir * 30);
        }

        if (dist < 180*FRACUNIT)
        {
            // Backpedal from close melee enemies
            cmd->forwardmove = -BOT_MAXMOVE;
        }
        else
        {
            // Advance towards threat if clear ahead
            int f_an = player->mo->angle >> ANGLETOFINESHIFT;
            fixed_t f_x = player->mo->x + FixedMul(36*FRACUNIT, finecosine[f_an]);
            fixed_t f_y = player->mo->y + FixedMul(36*FRACUNIT, finesine[f_an]);
            if (P_CheckPosition(player->mo, f_x, f_y))
            {
                cmd->forwardmove = (dist > 280*FRACUNIT) ? 35 : 15;
            }
            else
            {
                cmd->sidemove = (signed char)(bot_strafe_dir * 35);
            }
        }

        return;
    }

    // Reset pitch to level when out of combat
    if (mlook && lookdir != 0)
    {
        if (lookdir > 0) lookdir = (lookdir > 4) ? lookdir - 4 : 0;
        else if (lookdir < 0) lookdir = (lookdir < -4) ? lookdir + 4 : 0;
    }

    // 3. High-Level Path Planning (Decoupled from per-frame execution)
    int cur_nearest = Bot_FindNearestWaypoint(player->mo->x, player->mo->y, player->mo->z, true);

    int cur_keys = (player->cards[it_bluecard] ? 1 : 0) | (player->cards[it_redcard] ? 2 : 0) | (player->cards[it_yellowcard] ? 4 : 0);
    boolean keys_changed = (cur_keys != bot_last_keys);
    bot_last_keys = cur_keys;

    boolean need_replan = (bot_path_len == 0 ||
                           bot_path_step >= bot_path_len ||
                           keys_changed ||
                           bot_stuck_tics > 30 ||
                           ++bot_replan_timer > 150);

    if (need_replan)
    {
        bot_replan_timer = 0;
        int goal_wp = -1;

        // Strategy A: Check if exit is currently reachable
        if (bot_exit_wp >= 0 && Bot_QueryPath(cur_nearest, bot_exit_wp, player, NULL))
        {
            goal_wp = bot_exit_wp;
        }
        else
        {
            // Strategy B: Exit blocked (likely locked door). Find nearest reachable uncollected key!
            int best_key = -1;
            int best_key_dist = 999999;

            for (int k = 0; k < bot_num_wp; k++)
            {
                if (bot_wp[k].type == WP_KEY && !Bot_HasKey(player, bot_wp[k].required_key))
                {
                    int k_dist = 0;
                    if (Bot_QueryPath(cur_nearest, k, player, &k_dist))
                    {
                        if (k_dist < best_key_dist)
                        {
                            best_key_dist = k_dist;
                            best_key = k;
                        }
                    }
                }
            }

            if (best_key >= 0)
            {
                goal_wp = best_key;
            }
            else
            {
                // Strategy C: Explore least-visited reachable node
                int min_vis = 999999;
                for (int i = 0; i < bot_num_wp; i++)
                {
                    if (bot_wp[i].type == WP_DOOR && !Bot_HasKey(player, bot_wp[i].required_key))
                        continue;

                    if (bot_wp[i].visits < min_vis && Bot_QueryPath(cur_nearest, i, player, NULL))
                    {
                        min_vis = bot_wp[i].visits;
                        goal_wp = i;
                    }
                }
            }
        }

        if (goal_wp < 0 && bot_num_wp > 0) goal_wp = 0;

        if (goal_wp >= 0)
        {
            bot_current_target_wp = goal_wp;
            Bot_FindPath(cur_nearest, goal_wp, player);
        }
    }

    // 4. Identify Next Waypoint in Path
    fixed_t tx = player->mo->x;
    fixed_t ty = player->mo->y;
    int cur_target_wp = -1;

    if (bot_path_len > 0 && bot_path_step < bot_path_len)
    {
        cur_target_wp = bot_path_cache[bot_path_step];
        tx = bot_wp[cur_target_wp].x;
        ty = bot_wp[cur_target_wp].y;

        fixed_t d_wp = P_AproxDistance(player->mo->x - tx, player->mo->y - ty);

        // Check if current waypoint reached
        fixed_t reach_dist = (bot_wp[cur_target_wp].type == WP_DOOR) ? 64*FRACUNIT : 48*FRACUNIT;
        if (d_wp < reach_dist)
        {
            bot_wp[cur_target_wp].visits++;
            bot_path_step++;
            if (bot_path_step < bot_path_len)
            {
                cur_target_wp = bot_path_cache[bot_path_step];
                tx = bot_wp[cur_target_wp].x;
                ty = bot_wp[cur_target_wp].y;
            }
        }
    }
    else if (bot_current_target_wp >= 0 && bot_current_target_wp < bot_num_wp)
    {
        tx = bot_wp[bot_current_target_wp].x;
        ty = bot_wp[bot_current_target_wp].y;
    }

    boolean facing_interaction = false;

    // 5. Door and Exit Switch Handling
    if (cur_target_wp >= 0)
    {
        fixed_t d_targ = P_AproxDistance(player->mo->x - bot_wp[cur_target_wp].x, player->mo->y - bot_wp[cur_target_wp].y);

        if (bot_wp[cur_target_wp].type == WP_DOOR && d_targ < 96*FRACUNIT)
        {
            if (Bot_HasKey(player, bot_wp[cur_target_wp].required_key))
            {
                // Face door and pulse USE
                angle_t door_ang = R_PointToAngle2(player->mo->x, player->mo->y, bot_wp[cur_target_wp].x, bot_wp[cur_target_wp].y);
                cmd->angleturn = (short)((door_ang - player->mo->angle) >> 16);
                if ((gametic & 2) == 0) cmd->buttons |= BT_USE;
                facing_interaction = true;
            }
        }
        else if (bot_wp[cur_target_wp].type == WP_EXIT && d_targ < 72*FRACUNIT)
        {
            // Face exit switch line center and pulse USE
            if (bot_wp[cur_target_wp].line)
            {
                line_t* el = bot_wp[cur_target_wp].line;
                fixed_t ex = (el->v1->x + el->v2->x) / 2;
                fixed_t ey = (el->v1->y + el->v2->y) / 2;
                angle_t exit_ang = R_PointToAngle2(player->mo->x, player->mo->y, ex, ey);
                cmd->angleturn = (short)((exit_ang - player->mo->angle) >> 16);
                facing_interaction = true;
            }
            if ((gametic & 2) == 0) cmd->buttons |= BT_USE;
        }
    }

    // Also check immediate proximity (64 units) for any closed unlocked doors to tap
    for (int i = 0; i < numlines; i++)
    {
        line_t* li = &lines[i];
        if (li->special == 0) continue;

        fixed_t lmx = (li->v1->x + li->v2->x) / 2;
        fixed_t lmy = (li->v1->y + li->v2->y) / 2;
        fixed_t ld = P_AproxDistance(player->mo->x - lmx, player->mo->y - lmy);

        if (ld < 68*FRACUNIT)
        {
            if (Bot_IsDoor(li) && Bot_IsDoorClosed(li) && Bot_HasKey(player, Bot_DoorRequiredKey(li)))
            {
                if ((gametic & 2) == 0) cmd->buttons |= BT_USE;
            }
            else if (Bot_IsExitLine(li))
            {
                if ((gametic & 2) == 0) cmd->buttons |= BT_USE;
            }
        }
    }

    // 6. Steering towards target coordinate
    if (!facing_interaction)
    {
        angle_t desired_ang = R_PointToAngle2(player->mo->x, player->mo->y, tx, ty);
        int diff = (int)(desired_ang - player->mo->angle);

        short turn = (short)(diff >> 16);
        if (turn > 1400) turn = 1400;
        if (turn < -1400) turn = -1400;
        cmd->angleturn = turn;
    }

    // 7. Multi-Whisker Obstacle Avoidance (P_CheckPosition)
    int an = player->mo->angle >> ANGLETOFINESHIFT;
    fixed_t fwd_x = player->mo->x + FixedMul(36*FRACUNIT, finecosine[an]);
    fixed_t fwd_y = player->mo->y + FixedMul(36*FRACUNIT, finesine[an]);

    boolean front_clear = P_CheckPosition(player->mo, fwd_x, fwd_y);

    if (front_clear)
    {
        // Clear ahead: run full speed forward
        cmd->forwardmove = BOT_MAXMOVE;
    }
    else
    {
        // Obstacle ahead: test diagonal whisker feelers
        int an_l = (player->mo->angle + ANG45) >> ANGLETOFINESHIFT;
        int an_r = (player->mo->angle - ANG45) >> ANGLETOFINESHIFT;

        fixed_t left_x = player->mo->x + FixedMul(32*FRACUNIT, finecosine[an_l]);
        fixed_t left_y = player->mo->y + FixedMul(32*FRACUNIT, finesine[an_l]);

        fixed_t right_x = player->mo->x + FixedMul(32*FRACUNIT, finecosine[an_r]);
        fixed_t right_y = player->mo->y + FixedMul(32*FRACUNIT, finesine[an_r]);

        boolean left_clear = P_CheckPosition(player->mo, left_x, left_y);
        boolean right_clear = P_CheckPosition(player->mo, right_x, right_y);

        if (left_clear && !right_clear)
        {
            // Steer & strafe left around obstacle
            cmd->forwardmove = 30;
            cmd->sidemove = -35;
            cmd->angleturn += 600;
        }
        else if (right_clear && !left_clear)
        {
            // Steer & strafe right around obstacle
            cmd->forwardmove = 30;
            cmd->sidemove = 35;
            cmd->angleturn -= 600;
        }
        else if (left_clear && right_clear)
        {
            // Both sides open: choose side closer to target
            fixed_t dl = P_AproxDistance(left_x - tx, left_y - ty);
            fixed_t dr = P_AproxDistance(right_x - tx, right_y - ty);
            if (dl < dr)
            {
                cmd->forwardmove = 25;
                cmd->sidemove = -35;
                cmd->angleturn += 500;
            }
            else
            {
                cmd->forwardmove = 25;
                cmd->sidemove = 35;
                cmd->angleturn -= 500;
            }
        }
        else
        {
            // Corner or door in front:
            if (cur_target_wp >= 0 && bot_wp[cur_target_wp].type == WP_DOOR)
            {
                cmd->forwardmove = 30; // Keep pushing through opening door
            }
            else
            {
                cmd->forwardmove = -15;
                cmd->sidemove = (signed char)(bot_strafe_dir * 35);
                cmd->angleturn = 800;
            }
        }
    }

    // 8. Anti-Stuck Maneuver (no generic wall grunting)
    fixed_t dist_moved = P_AproxDistance(player->mo->x - bot_last_x, player->mo->y - bot_last_y);
    bot_last_x = player->mo->x;
    bot_last_y = player->mo->y;

    if (dist_moved < 8*FRACUNIT)
    {
        if (++bot_stuck_tics > 15)
        {
            // Back up and strafe away from obstacle
            cmd->forwardmove = -BOT_MAXMOVE;
            cmd->sidemove = (signed char)(bot_strafe_dir * BOT_MAXMOVE);

            if (bot_stuck_tics > 35)
            {
                bot_strafe_dir = -bot_strafe_dir;
                cmd->angleturn = 2200;
                bot_path_step++; // Advance past stuck node
                bot_stuck_tics = 0;
            }
        }
    }
    else
    {
        bot_stuck_tics = 0;
    }
}
