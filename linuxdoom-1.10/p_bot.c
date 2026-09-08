//-----------------------------------------------------------------------------
//
// Autonomous DOOM II First-Person Bot AI
//
// Features:
// - Dynamic level waypoint graph generation from map geometry (portals, doors, keys, exit)
// - BFS / Dijkstra pathfinding to items, keys, doors, and the level exit
// - Threat detection using engine BSP line-of-sight (P_CheckSight)
// - Smooth horizontal camera tracking and full 3D vertical pitch aiming (lookdir)
// - Intelligent weapon selection (SSG, Shotgun, Chaingun, Rockets, Plasma)
// - Tactical combat strafing, circle-strafing, and backpedaling from melee threats
// - Key collection logic (locates keys, unlocks doors, resumes exit path)
// - Interactive doors, switches, and exit line activation (BT_USE)
// - Obstacle avoidance feelers and anti-stuck recovery
//
//-----------------------------------------------------------------------------

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

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

#define MAX_BOT_WAYPOINTS 512
#define MAX_BOT_EDGES     16
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
static int bot_attack_delay = 0;

// Stuck recovery
static fixed_t bot_last_x = 0;
static fixed_t bot_last_y = 0;
static int bot_stuck_tics = 0;
static int bot_unstuck_phase = 0;

// Path cache
static int bot_current_target_wp = -1;
static int bot_path_cache[MAX_BOT_WAYPOINTS];
static int bot_path_len = 0;
static int bot_path_step = 0;

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

// Check if a line is an exit trigger/switch
static boolean Bot_IsExitLine(line_t* line)
{
    if (!line) return false;
    switch (line->special)
    {
        case 11:  // S1 Exit
        case 51:  // W1 Exit
        case 52:  // WR Exit
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

// Check if player has the required key
static boolean Bot_HasKey(player_t* player, int key)
{
    if (key < 0) return true;
    if (key == it_bluecard) return player->cards[it_bluecard] || player->cards[it_blueskull];
    if (key == it_redcard) return player->cards[it_redcard] || player->cards[it_redskull];
    if (key == it_yellowcard) return player->cards[it_yellowcard] || player->cards[it_yellowskull];
    return true;
}

// Check if door is currently closed
static boolean Bot_IsDoorClosed(line_t* line)
{
    if (!line || !line->backsector) return false;
    fixed_t h1 = line->frontsector->ceilingheight - line->frontsector->floorheight;
    fixed_t h2 = line->backsector->ceilingheight - line->backsector->floorheight;
    if (h1 < 56*FRACUNIT || h2 < 56*FRACUNIT) return true;
    return false;
}

// Add a waypoint if not too close to existing ones
static int Bot_AddWaypoint(fixed_t x, fixed_t y, fixed_t z, wptype_t type, line_t* line, int req_key)
{
    if (bot_num_wp >= MAX_BOT_WAYPOINTS) return -1;

    for (int i = 0; i < bot_num_wp; i++)
    {
        fixed_t d = P_AproxDistance(bot_wp[i].x - x, bot_wp[i].y - y);
        if (d < 48*FRACUNIT)
        {
            if (type == WP_EXIT) { bot_wp[i].type = WP_EXIT; bot_wp[i].line = line; bot_exit_wp = i; }
            return i;
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
            if (dist > 1600*FRACUNIT) continue;

            fixed_t f1 = bot_wp[i].z;
            fixed_t f2 = bot_wp[j].z;
            if (abs(f1 - f2) > 36*FRACUNIT) continue;

            if (Bot_CheckSightCoords(bot_wp[i].x, bot_wp[i].y, f1 + 24*FRACUNIT,
                                     bot_wp[j].x, bot_wp[j].y, f2 + 24*FRACUNIT))
            {
                if (bot_wp[i].num_edges < MAX_BOT_EDGES && bot_wp[j].num_edges < MAX_BOT_EDGES)
                {
                    int e1 = bot_wp[i].num_edges++;
                    bot_wp[i].edges[e1] = j;
                    bot_wp[i].edge_dist[e1] = (int)(dist >> FRACBITS);

                    int e2 = bot_wp[j].num_edges++;
                    bot_wp[j].edges[e2] = i;
                    bot_wp[j].edge_dist[e2] = (int)(dist >> FRACBITS);
                }
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

    I_Log("Bot: Initializing navigation graph for level...\n");

    // 1. Add Player Spawn
    Bot_AddWaypoint(playerstarts[0].x * FRACUNIT, playerstarts[0].y * FRACUNIT, 0, WP_NORMAL, NULL, -1);

    // 2. Scan Lines: portals, doors, exit lines
    for (int i = 0; i < numlines; i++)
    {
        line_t* li = &lines[i];
        fixed_t mx = (li->v1->x + li->v2->x) / 2;
        fixed_t my = (li->v1->y + li->v2->y) / 2;
        fixed_t mz = li->frontsector ? li->frontsector->floorheight : 0;

        if (Bot_IsExitLine(li))
        {
            // Exit line
            Bot_AddWaypoint(mx, my, mz, WP_EXIT, li, -1);
        }
        else if (li->flags & ML_TWOSIDED)
        {
            int req_key = Bot_DoorRequiredKey(li);
            if (li->special != 0 || req_key >= 0)
            {
                // Door or passage with special
                Bot_AddWaypoint(mx, my, mz, WP_DOOR, li, req_key);
            }
            else
            {
                // Regular room-connecting portal
                fixed_t hdiff = abs(li->frontsector->floorheight - li->backsector->floorheight);
                if (hdiff <= 28*FRACUNIT)
                {
                    Bot_AddWaypoint(mx, my, mz, WP_NORMAL, li, -1);
                }
            }
        }
    }

    // 3. Scan Items: Keys and Super Weapons
    thinker_t* th;
    for (th = thinkercap.next; th != &thinkercap; th = th->next)
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

    // Fallback if LOS fails
    if (best == -1 && check_los) return Bot_FindNearestWaypoint(x, y, z, false);
    return best;
}

// BFS shortest path on waypoint graph
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
    int queue[MAX_BOT_WAYPOINTS];
    int qhead = 0, qtail = 0;

    for (int i = 0; i < bot_num_wp; i++)
    {
        dist[i] = 999999;
        prev[i] = -1;
    }

    dist[start_wp] = 0;
    queue[qtail++] = start_wp;

    while (qhead < qtail)
    {
        int u = queue[qhead++];
        if (u == goal_wp) break;

        for (int e = 0; e < bot_wp[u].num_edges; e++)
        {
            int v = bot_wp[u].edges[e];
            int cost = bot_wp[u].edge_dist[e];

            // If waypoint is locked door and player has no key, cannot pass
            if (bot_wp[v].type == WP_DOOR && !Bot_HasKey(player, bot_wp[v].required_key))
                continue;

            // Prefer less visited nodes
            cost += bot_wp[v].visits * 50;

            if (dist[u] + cost < dist[v])
            {
                dist[v] = dist[u] + cost;
                prev[v] = u;
                queue[qtail++] = v;
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

// 3D vertical pitch aim
static void Bot_AimPitch(player_t* player, mobj_t* target, fixed_t dist)
{
    if (!mlook) return;
    fixed_t dz = (target->z + (target->height >> 1)) - player->viewz;
    int dxy = (int)(dist >> FRACBITS);
    if (dxy <= 0) dxy = 1;

    int target_pitch = (int)(((long long)dz * 160) / ((long long)dxy * FRACUNIT));
    if (target_pitch > 90) target_pitch = 90;
    if (target_pitch < -100) target_pitch = -100;

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

// Check lines immediately in front of the bot and press USE if door/switch
static boolean Bot_CheckUseLines(player_t* player, ticcmd_t* cmd)
{
    int angle = player->mo->angle >> ANGLETOFINESHIFT;
    fixed_t fx = player->mo->x + (64 * finecosine[angle]);
    fixed_t fy = player->mo->y + (64 * finesine[angle]);

    for (int i = 0; i < numlines; i++)
    {
        line_t* li = &lines[i];
        if (li->special == 0) continue;

        fixed_t mx = (li->v1->x + li->v2->x) / 2;
        fixed_t my = (li->v1->y + li->v2->y) / 2;
        fixed_t d = P_AproxDistance(player->mo->x - mx, player->mo->y - my);

        if (d < 80*FRACUNIT)
        {
            if (Bot_IsExitLine(li) || Bot_IsDoorClosed(li) || li->special == 1 || li->special == 11)
            {
                angle_t face_ang = R_PointToAngle2(player->mo->x, player->mo->y, mx, my);
                cmd->angleturn = (short)((face_ang - player->mo->angle) >> 16);
                cmd->buttons |= BT_USE;
                return true;
            }
        }
    }
    return false;
}

// Find uncollected key that is needed
static int Bot_FindNeededKeyWaypoint(player_t* player)
{
    for (int i = 0; i < bot_num_wp; i++)
    {
        if (bot_wp[i].type == WP_KEY)
        {
            if (!Bot_HasKey(player, bot_wp[i].required_key))
            {
                return i;
            }
        }
    }
    return -1;
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
        cmd->buttons |= BT_USE;
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

        // Tactical Movement: Strafe & Distance Control
        if (++bot_strafe_timer > 30)
        {
            bot_strafe_timer = 0;
            bot_strafe_dir = -bot_strafe_dir;
        }

        cmd->sidemove = (signed char)(bot_strafe_dir * 35);

        if (dist < 180*FRACUNIT)
        {
            // Backpedal from close melee enemies
            cmd->forwardmove = -BOT_MAXMOVE;
        }
        else if (dist > 280*FRACUNIT)
        {
            // Close the gap to attack
            cmd->forwardmove = 35;
        }

        return;
    }

    // Reset pitch to level when out of combat
    if (mlook && lookdir != 0)
    {
        if (lookdir > 0) lookdir = (lookdir > 4) ? lookdir - 4 : 0;
        else if (lookdir < 0) lookdir = (lookdir < -4) ? lookdir + 4 : 0;
    }

    // 3. Check for doors/switches to activate directly in front
    if (Bot_CheckUseLines(player, cmd))
    {
        return;
    }

    // 4. Determine High-Level Goal Waypoint
    int goal_wp = -1;

    // Check if we need an uncollected key
    int key_wp = Bot_FindNeededKeyWaypoint(player);
    if (key_wp >= 0)
    {
        goal_wp = key_wp;
    }
    else if (bot_exit_wp >= 0)
    {
        // Head for Exit!
        goal_wp = bot_exit_wp;
    }
    else
    {
        // Fallback: search for least visited node
        int min_vis = 999999;
        for (int i = 0; i < bot_num_wp; i++)
        {
            if (bot_wp[i].visits < min_vis)
            {
                min_vis = bot_wp[i].visits;
                goal_wp = i;
            }
        }
    }

    if (goal_wp < 0 && bot_num_wp > 0) goal_wp = 0;
    if (goal_wp < 0) return;

    // 5. Navigate Path to Goal
    int cur_nearest = Bot_FindNearestWaypoint(player->mo->x, player->mo->y, player->mo->z, true);

    if (goal_wp != bot_current_target_wp || bot_path_len == 0 || bot_path_step >= bot_path_len)
    {
        bot_current_target_wp = goal_wp;
        Bot_FindPath(cur_nearest, goal_wp, player);
    }

    // Identify next waypoint coordinate
    fixed_t tx = bot_wp[goal_wp].x;
    fixed_t ty = bot_wp[goal_wp].y;

    if (bot_path_len > 0 && bot_path_step < bot_path_len)
    {
        int wp_idx = bot_path_cache[bot_path_step];
        tx = bot_wp[wp_idx].x;
        ty = bot_wp[wp_idx].y;

        fixed_t d_wp = P_AproxDistance(player->mo->x - tx, player->mo->y - ty);
        if (d_wp < 56*FRACUNIT)
        {
            bot_wp[wp_idx].visits++;
            bot_path_step++;
            if (bot_path_step < bot_path_len)
            {
                wp_idx = bot_path_cache[bot_path_step];
                tx = bot_wp[wp_idx].x;
                ty = bot_wp[wp_idx].y;
            }
        }

        // If approaching a door or exit waypoint, prepare to USE
        if ((bot_wp[wp_idx].type == WP_DOOR || bot_wp[wp_idx].type == WP_EXIT) && d_wp < 80*FRACUNIT)
        {
            cmd->buttons |= BT_USE;
        }
    }

    // 6. Steering & Movement towards target
    angle_t desired_ang = R_PointToAngle2(player->mo->x, player->mo->y, tx, ty);
    int diff = (int)(desired_ang - player->mo->angle);

    short turn = (short)(diff >> 16);
    if (turn > 1200) turn = 1200;
    if (turn < -1200) turn = -1200;
    cmd->angleturn = turn;
    cmd->forwardmove = BOT_MAXMOVE;

    // 7. Stuck Detection & Obstacle Avoidance
    fixed_t dist_moved = P_AproxDistance(player->mo->x - bot_last_x, player->mo->y - bot_last_y);
    bot_last_x = player->mo->x;
    bot_last_y = player->mo->y;

    if (dist_moved < 12*FRACUNIT)
    {
        bot_stuck_tics++;
        if (bot_stuck_tics > 15)
        {
            // Unstick maneuver: back up, strafe, try pressing use
            cmd->forwardmove = -BOT_MAXMOVE;
            cmd->sidemove = (signed char)(bot_strafe_dir * BOT_MAXMOVE);
            cmd->buttons |= BT_USE;

            if (bot_stuck_tics > 35)
            {
                bot_strafe_dir = -bot_strafe_dir;
                cmd->angleturn = 1600;
                bot_stuck_tics = 0;
                bot_path_step++; // skip blocked node
            }
        }
    }
    else
    {
        bot_stuck_tics = 0;
    }
}
