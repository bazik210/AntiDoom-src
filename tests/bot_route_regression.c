/* Usage: bot_run.exe WAD map seconds [nomonsters] [skill 1..5] [episode].
   Runs actual 35 Hz player/monster physics and exits only on a real exit action. */
#undef main
#include <stdio.h>
#include <stdlib.h>
#include "doomstat.h"
#include "m_argv.h"
#include "p_local.h"
#include "p_setup.h"
#include "p_tick.h"
#include "p_bot.h"
#include "g_game.h"
#include "w_wad.h"
#include "v_video.h"
#include "z_zone.h"
#include "hu_stuff.h"
#include "st_stuff.h"
extern int mlook, uncapped_fps, screenblocks;
void R_ExecuteSetViewSize(void);
/* White-box diagnostics of the exact production navigation implementation. */
#include "../linuxdoom-1.10/p_bot.c"
void D_ProcessEvents(void);
#define CHECK(x) do { if (!(x)) { printf("FAIL line %d: %s\n",__LINE__,#x); return 1; } } while (0)
int main(int argc, char **argv)
{
    char *wads[2];
    int limit, t, map, episode, skill;
    if (argc < 4) return 2;
    setbuf(stdout, NULL);
    myargc = argc; myargv = argv;
    wads[0] = argv[1]; wads[1] = NULL;
    map = atoi(argv[2]); limit = atoi(argv[3]) * TICRATE;
    nomonsters = argc > 4 ? atoi(argv[4]) : 0;
    skill = argc > 5 ? atoi(argv[5]) - 1 : sk_medium;
    episode = argc > 6 ? atoi(argv[6]) : 1;
    Z_Init(); W_InitMultipleFiles(wads);
    gamemode = W_CheckNumForName("MAP01") >= 0 ? commercial : retail;
    playeringame[0] = true; precache = false;
    mlook = 1; uncapped_fps = 0; bot_active = true; screenblocks = 10;
    V_Init(); R_Init(); P_Init(); HU_Init(); ST_Init();
    {
        event_t ev={ev_keydown,KEY_CONSOLE,0,0};
        gamestate=GS_DEMOSCREEN; menuactive=true;
        D_PostEvent(&ev); D_ProcessEvents(); CHECK(console_on && !menuactive);
        ev.data1=KEY_ESCAPE;
        CHECK(G_Responder(&ev)); CHECK(!console_on);
    }
    G_InitNew(skill, episode, map);
    R_ExecuteSetViewSize();

    {
        player_t *p=&players[0]; mobj_t *mo=p->mo;
        CHECK(map04_route);
        P_UnsetThingPosition(mo);
        mo->x=-1504*FRACUNIT; mo->y=1312*FRACUNIT;
        P_SetThingPosition(mo);
        mo->z=mo->floorz=mo->subsector->sector->floorheight;
        mo->ceilingz=mo->subsector->sector->ceilingheight;
        p->cards[it_yellowcard]=true;
        for (t=0; t<35*45 && gameaction==ga_nothing; ++t) {
            Bot_BuildTiccmd(&p->cmd,p); P_Ticker(); ++gametic;
            if (!(t%175)) printf("STEP %d sector=%d xy=%d,%d z=%d state=%d goal=%d/%d path=%d/%d\n",
                t,(int)(mo->subsector->sector-sectors),mo->x/FRACUNIT,mo->y/FRACUNIT,
                mo->z/FRACUNIT,map04_yellow_state,goal.type,goal.line,path_step,path_len);
            if (map04_yellow_state==4) break;
        }
        CHECK(map04_yellow_state==4);
        CHECK(!lines[map04_yellow_switch].special);
        CHECK(mo->subsector->sector==&sectors[67]);
        for (t=0; t<35*25 && gameaction==ga_nothing; ++t) {
            Bot_BuildTiccmd(&p->cmd,p); P_Ticker(); ++gametic;
            if (!(t%175)) printf("EXIT STEP %d xy=%d,%d sector=%d floor98=%d goal=%d/%d\n",
                t,mo->x/FRACUNIT,mo->y/FRACUNIT,(int)(mo->subsector->sector-sectors),
                sectors[98].floorheight/FRACUNIT,goal.type,goal.line);
        }
        CHECK(gameaction==ga_completed);
        memset(p->ammo,0,sizeof(p->ammo));
        CHECK(!Bot_HasRangedAmmo(p));
        { mobj_t pickup; memset(&pickup,0,sizeof(pickup)); pickup.sprite=SPR_CLIP;
          CHECK(Bot_ItemPriority(p,&pickup)<-30000); }
        puts("PASS: yellow platform -> lower teleport -> switch -> return; rearm priority");
    }
    G_InitNew(skill,1,2);
    {
        player_t *p=&players[0]; mobj_t *mo=p->mo;
        CHECK(map02_route && !map04_route);
        CHECK(!Bot_UseDoor(&lines[111]));
        P_UnsetThingPosition(mo); mo->x=792*FRACUNIT; mo->y=1344*FRACUNIT;
        P_SetThingPosition(mo); mo->z=mo->floorz=mo->subsector->sector->floorheight;
        mo->ceilingz=mo->subsector->sector->ceilingheight;
        p->cards[it_redcard]=true;
        printf("MAP02 BARS pos=%d,%d z=%d distkey=%d\n",mo->x/FRACUNIT,mo->y/FRACUNIT,
               mo->z/FRACUNIT,P_AproxDistance(mo->x-map02_key_house_x*FRACUNIT,
                                               mo->y-map02_key_house_y*FRACUNIT)/FRACUNIT);
        for (t=0; t<35*8 && lines[311].special; ++t) {
            Bot_BuildTiccmd(&p->cmd,p); P_Ticker(); ++gametic;
        }
        printf("BARS t=%d xy=%d,%d goal=%d/%d\n",t,mo->x/FRACUNIT,mo->y/FRACUNIT,goal.type,goal.line);
        CHECK(!lines[311].special);
        puts("PASS: red bars -> follow-up button within eight seconds");
        {
            mobj_t *enemy=NULL;
            int k;
            /* Find a real clear firing lane in the loaded map, then verify
               fists cannot acquire a distant enemy as a melee hit. */
            for (k=0;k<8;++k) {
                int an=k*FINEANGLES/8;
                fixed_t x=mo->x+FixedMul(128*FRACUNIT,finecosine[an]);
                fixed_t y=mo->y+FixedMul(128*FRACUNIT,finesine[an]);
                if (!Bot_WalkStable(mo->x,mo->y,mo->z,x,y,mo,false)) continue;
                enemy=P_SpawnMobj(x,y,ONFLOORZ,MT_TROOP);
                mo->angle=R_PointToAngle2(mo->x,mo->y,x,y);
                P_AimLineAttack(mo,mo->angle,MISSILERANGE);
                if (linetarget==enemy) break;
                P_RemoveMobj(enemy); enemy=NULL;
            }
            CHECK(enemy);
            P_SpawnMobj(mo->x+(enemy->x-mo->x)/2,
                        mo->y+(enemy->y-mo->y)/2,ONFLOORZ,MT_CLIP);
            memset(p->ammo,0,sizeof(p->ammo));
            p->readyweapon=wp_fist; p->pendingweapon=wp_nochange;
            use_until=post_use_hold_until=post_use_local_until=0;
            forced_progress_line=-1;
            Bot_BuildTiccmd(&p->cmd,p);
            CHECK(!(p->cmd.buttons&BT_ATTACK));
            CHECK(goal.type==GO_ITEM);
            CHECK(p->cmd.forwardmove || p->cmd.sidemove);
            puts("PASS: empty fists do not fire at distant enemy; bot moves toward supplies");
        }
        {
            bot_route_id_t other=bot_routes[0];
            other.geometry ^= 1;
            CHECK(!Bot_MatchesRoute(&other));
            gamemode=retail;
            CHECK(!Bot_MatchesRoute(&bot_routes[0]));
            gamemode=commercial;
        }
    }
    { int start_case;
    static const int starts[][2]={{-224,1760},{-240,1784},{-260,1784},{-240,1796}};
    for (start_case=0;start_case<4;++start_case) {
    G_InitNew(skill,1,4);
    {
        player_t *p=&players[0]; mobj_t *mo=p->mo;
        p->cards[it_bluecard]=true;
        p->cheats |= CF_GODMODE;
        P_UnsetThingPosition(mo); mo->x=starts[start_case][0]*FRACUNIT; mo->y=starts[start_case][1]*FRACUNIT;
        P_SetThingPosition(mo); mo->z=mo->floorz=64*FRACUNIT;
        mo->ceilingz=mo->subsector->sector->ceilingheight;
        map04_lift_done=map04_lift_triggered=true;
        P_SpawnMobj(-304*FRACUNIT,1760*FRACUNIT,ONFLOORZ,MT_TROOP);
        P_SpawnMobj(-112*FRACUNIT,1800*FRACUNIT,ONFLOORZ,MT_TROOP);
        for (t=0; t<35*30 && mo->subsector->sector!=&sectors[80]; ++t) {
            Bot_BuildTiccmd(&p->cmd,p); P_Ticker(); ++gametic;
            if (!p->cards[it_redcard]) CHECK(mo->z>=40*FRACUNIT);
            if (!(t%175)) printf("CRATES t=%d xy=%d,%d z=%d goal=%d/%d key=%d\n",
                t,mo->x/FRACUNIT,mo->y/FRACUNIT,mo->z/FRACUNIT,goal.type,goal.line,p->cards[it_redcard]);
        }
        CHECK(p->cards[it_redcard]);
        CHECK(mo->subsector->sector==&sectors[80]);
        puts("PASS: crate route under imp fire -> red key -> teleport");
        G_InitNew(skill,1,4); p=&players[0]; mo=p->mo;
        p->cards[it_bluecard]=true;
        P_UnsetThingPosition(mo); mo->x=-304*FRACUNIT; mo->y=1760*FRACUNIT;
        P_SetThingPosition(mo); mo->z=mo->floorz=mo->subsector->sector->floorheight;
        mo->ceilingz=mo->subsector->sector->ceilingheight;
        map04_lift_done=map04_lift_triggered=map04_crate_route=true;
        Bot_BuildTiccmd(&p->cmd,p);
        CHECK(map04_crate_retry && !map04_lift_done && !map04_lift_triggered);
        CHECK(!forced_item_active);
        CHECK(goal.aimx==lines[408].v1->x+lines[408].dx/2);
        puts("PASS: falling without red key restores lift objective");
    }
    } }
    G_InitNew(skill,1,3);
    {
        player_t *p=&players[0]; mobj_t *mo=p->mo;
        p->cards[it_bluecard]=true;
        P_UnsetThingPosition(mo); mo->x=4608*FRACUNIT; mo->y=3360*FRACUNIT;
        P_SetThingPosition(mo); mo->z=mo->floorz=mo->subsector->sector->floorheight;
        mo->ceilingz=mo->subsector->sector->ceilingheight;
        map03_platform_done=true; map03_blue_door_done=false;
        Bot_BuildTiccmd(&p->cmd,p);
        printf("MAP03 BLUE goal=%d line=%d xy=%d,%d\n",goal.type,goal.line,
               goal.x/FRACUNIT,goal.y/FRACUNIT);
        CHECK(goal.line==map03_blue_door_left || goal.line==map03_blue_door_right ||
              goal.line==map03_blue_button);
        for (t=0; t<35*20 && !line_used[map03_blue_door_left] &&
             !line_used[map03_blue_door_right]; ++t) {
            Bot_BuildTiccmd(&p->cmd,p); P_Ticker(); ++gametic;
        }
        CHECK(line_used[map03_blue_door_left] || line_used[map03_blue_door_right]);
        Bot_BuildTiccmd(&p->cmd,p);
        CHECK(goal.line==map03_blue_button);
        puts("PASS: MAP03 left blue door -> corridor button");
    }
    G_InitNew(skill,1,2);
    printf("MAP02 KEY HOUSE floor=%d\n",
           R_PointInSubsector(1168*FRACUNIT,528*FRACUNIT)->sector->floorheight/FRACUNIT);
    {
        player_t *p=&players[0]; mobj_t *mo=p->mo;
        sector_t *house=R_PointInSubsector(1168*FRACUNIT,528*FRACUNIT)->sector;
        P_UnsetThingPosition(mo); mo->x=1168*FRACUNIT; mo->y=528*FRACUNIT;
        P_SetThingPosition(mo); mo->z=mo->floorz=house->floorheight;
        mo->ceilingz=house->ceilingheight;
        p->cards[it_bluecard]=p->cards[it_yellowcard]=p->cards[it_redcard]=false;
        P_SpawnMobj(mo->x,mo->y,ONFLOORZ,MT_MISC5);
        Bot_BuildTiccmd(&p->cmd,p); P_Ticker(); ++gametic;
        Bot_BuildTiccmd(&p->cmd,p);
        CHECK(map02_key_perch_hold);
        CHECK(goal.type != GO_USE && map02_key_perch_until > leveltime);
        puts("PASS: MAP02 elevated key holds perch before route descent");
    }
    return 0;
}
