/* Focused production-controller regressions; no rendered game or input injection. */
#undef main
#include <stdio.h>
#include <stdlib.h>
#include <time.h>
#include "p_tick.h"
#include "doomstat.h"
#include "m_argv.h"
#include "p_local.h"
#include "g_game.h"
#include "w_wad.h"
#include "v_video.h"
#include "z_zone.h"
#include "hu_stuff.h"
#include "st_stuff.h"
#include "../linuxdoom-1.10/p_bot.c"
extern int screenblocks,uncapped_fps;
void R_ExecuteSetViewSize(void);
#define CHECK(x) do { if (!(x)) { printf("FAIL %d: %s\n",__LINE__,#x); return 1; } } while(0)
int main(int argc,char **argv)
{
    char *wads[2]; player_t *p; ticcmd_t cmd; int t,attacks=0;
    if(argc!=2)return 2;
    myargc=argc;myargv=argv;wads[0]=argv[1];wads[1]=NULL;
    Z_Init();W_InitMultipleFiles(wads);gamemode=commercial;
    playeringame[0]=true;nomonsters=true;precache=false;
    screenblocks=10;uncapped_fps=0;bot_active=true;
    V_Init();R_Init();P_Init();HU_Init();ST_Init();
    G_InitNew(sk_medium,1,1);R_ExecuteSetViewSize();p=&players[0];
    p->weaponowned[wp_chaingun]=p->weaponowned[wp_supershotgun]=true;
    p->readyweapon=wp_supershotgun;p->pendingweapon=wp_nochange;
    p->ammo[am_clip]=50;p->ammo[am_shell]=24;
    memset(&cmd,0,sizeof(cmd));Bot_Weapon(&cmd,p,512*FRACUNIT,NULL);
    CHECK((cmd.buttons&BT_CHANGE) && ((cmd.buttons&BT_WEAPONMASK)>>BT_WEAPONSHIFT)==wp_chaingun);
    weapon_switch_until=0;
    p->readyweapon=wp_chaingun;memset(&cmd,0,sizeof(cmd));Bot_Weapon(&cmd,p,128*FRACUNIT,NULL);
    CHECK((cmd.buttons&BT_CHANGE) && ((cmd.buttons&BT_WEAPONMASK)>>BT_WEAPONSHIFT)==wp_shotgun);
    puts("PASS: long-range chaingun and close-range SSG");
    {
        mobj_t weak;memset(&weak,0,sizeof(weak));weak.health=20;weak.type=MT_POSSESSED;
        p->readyweapon=wp_pistol;p->pendingweapon=wp_nochange;weapon_switch_until=0;
        for(t=0;t<8;++t) {
            memset(&cmd,0,sizeof(cmd));Bot_Weapon(&cmd,p,(200+t*50)*FRACUNIT,&weak);
            CHECK(!(cmd.buttons&BT_CHANGE));
        }
        p->readyweapon=wp_supershotgun;weapon_switch_until=leveltime+100;
        memset(&cmd,0,sizeof(cmd));Bot_Weapon(&cmd,p,600*FRACUNIT,&weak);
        CHECK(cmd.buttons&BT_CHANGE);
        p->readyweapon=wp_chaingun;p->ammo[am_clip]=0;
        memset(&cmd,0,sizeof(cmd));Bot_Weapon(&cmd,p,128*FRACUNIT,&weak);
        CHECK(cmd.buttons&BT_CHANGE);
    }
    puts("PASS: weak targets retain the current gun; empty/ineffective weapons override cooldown");
    p->weaponowned[wp_chainsaw]=p->weaponowned[wp_shotgun]=true;
    p->readyweapon=wp_chainsaw;p->pendingweapon=wp_nochange;
    p->ammo[am_shell]=12;weapon_switch_until=0;
    memset(&cmd,0,sizeof(cmd));Bot_Weapon(&cmd,p,400*FRACUNIT,NULL);
    CHECK(cmd.buttons&BT_CHANGE);
    CHECK(((cmd.buttons&BT_WEAPONMASK)>>BT_WEAPONSHIFT)==wp_shotgun);
    puts("PASS: shells make the bot put away an idle chainsaw");
    G_InitNew(sk_medium,1,1);p=&players[0];
    for(t=0;t<NUMAMMO;++t)p->ammo[t]=0;
    p->readyweapon=wp_fist;p->pendingweapon=wp_nochange;
    {
        mobj_t *enemy=P_SpawnMobj(p->mo->x+48*FRACUNIT,p->mo->y,ONFLOORZ,MT_TROOP);
        enemy->target=p->mo;
        for(t=0;t<35;++t) {
            Bot_BuildTiccmd(&cmd,p);p->cmd=cmd;
            if(cmd.buttons&BT_ATTACK)++attacks;
            P_Ticker();++gametic;
        }
        CHECK(attacks>0);
    }
    puts("PASS: empty-ammo bot punches a close monster");
    G_InitNew(sk_medium,1,1);p=&players[0];
    {
        mobj_t owner;
        mobj_t *shot;
        fixed_t dodgex=0,dodgey=0;
        memset(&owner,0,sizeof(owner));
        shot=P_SpawnMobj(p->mo->x+180*FRACUNIT,p->mo->y,
                         p->mo->z+24*FRACUNIT,MT_HEADSHOT);
        shot->momx=-10*FRACUNIT;shot->momy=shot->momz=0;shot->target=&owner;
        missile_dodge_until=0;missile_dodge_pause_until=leveltime+20;
        CHECK(Bot_ProjectileDodge(p->mo,p->mo->x,p->mo->y,&dodgex,&dodgey));
        CHECK(P_AproxDistance(dodgex-p->mo->x,dodgey-p->mo->y)>=20*FRACUNIT);
    }
    puts("PASS: incoming cacodemon shot overrides pause and starts a sidestep");
    G_InitNew(sk_medium,1,6);p=&players[0];
    {
        plat_t plat;fixed_t x=0,y=0;sector_t *sec=&sectors[98];
        memset(&plat,0,sizeof(plat));plat.sector=sec;plat.high=184*FRACUNIT;
        sec->floorheight=40*FRACUNIT;
        P_UnsetThingPosition(p->mo);p->mo->x=1041*FRACUNIT;p->mo->y=620*FRACUNIT;
        P_SetThingPosition(p->mo);p->mo->z=p->mo->floorz=40*FRACUNIT;
        CHECK(Bot_PlatformCenter(p->mo,&plat,&x,&y));
        CHECK(Bot_PlatformInterior(sec,x,y));
        CHECK(Bot_Position(x,y,p->mo,false,NULL));
        CHECK(probe.ceiling>=plat.high+p->mo->height);
    }
    puts("PASS: platform boarding leaves headroom for full ascent");
    {
        unsigned int version;int cell;
        Bot_Flood(p);version=geometry_stamp;
        cell=Bot_NearCell(1056*FRACUNIT,608*FRACUNIT,40*FRACUNIT,false);
        CHECK(cell>=0 && cells[cell].floor==40*FRACUNIT);
        Bot_Flood(p);CHECK(geometry_stamp==version);
        sectors[98].floorheight+=FRACUNIT;
        Bot_Flood(p);CHECK(geometry_stamp!=version);version=geometry_stamp;
        Bot_Cell(cell);CHECK(cells[cell].floor==41*FRACUNIT);
        sectors[98].floorheight-=FRACUNIT;sectors[98].ceilingheight-=FRACUNIT;
        Bot_Flood(p);CHECK(geometry_stamp!=version);
        sectors[98].ceilingheight+=FRACUNIT;
    }
    puts("PASS: navigation cache survives static replans and invalidates floor/ceiling changes");
    G_InitNew(sk_medium,1,5);p=&players[0];
    {
        clock_t start,worst=0;
        mobj_t far_enemy;
        memset(&far_enemy,0,sizeof(far_enemy));
        P_UnsetThingPosition(p->mo);p->mo->x=1776*FRACUNIT;p->mo->y=-288*FRACUNIT;
        P_SetThingPosition(p->mo);p->mo->z=p->mo->floorz=p->mo->subsector->sector->floorheight;
        p->mo->ceilingz=p->mo->subsector->sector->ceilingheight;
        far_enemy.x=1776*FRACUNIT;far_enemy.y=400*FRACUNIT;
        p->weaponowned[wp_missile]=p->weaponowned[wp_shotgun]=true;
        p->readyweapon=wp_missile;p->pendingweapon=wp_nochange;
        p->ammo[am_misl]=4;p->ammo[am_shell]=12;weapon_switch_until=leveltime+100;
        CHECK(!Bot_RocketLaneSafe(p->mo,&far_enemy,688*FRACUNIT));
        memset(&cmd,0,sizeof(cmd));Bot_Weapon(&cmd,p,688*FRACUNIT,&far_enemy);
        CHECK(cmd.buttons&BT_CHANGE);
        puts("PASS: nearby MAP05 window/door geometry rejects the rocket launcher");

        p->cards[it_redcard]=true;
        p->weaponowned[wp_shotgun]=true;p->ammo[am_shell]=0;p->ammo[am_clip]=26;
        p->readyweapon=wp_pistol;p->pendingweapon=wp_nochange;
        P_UnsetThingPosition(p->mo);p->mo->x=1240*FRACUNIT;p->mo->y=-608*FRACUNIT;
        P_SetThingPosition(p->mo);p->mo->z=p->mo->floorz=p->mo->subsector->sector->floorheight;
        p->mo->ceilingz=p->mo->subsector->sector->ceilingheight;
        line_used[137]=line_used[185]=line_used[142]=line_used[201]=1;
        /* A stale ride used to suppress the key until its watchdog expired. */
        lift_commit_boarded=true;lift_commit_line=51;lift_commit_tag=4;
        lift_commit_until=leveltime+15*TICRATE;
        start=clock();Bot_BuildTiccmd(&cmd,p);worst=clock()-start;
        CHECK(!lift_commit_boarded);
        CHECK(goal.type==GO_ITEM && goal.aimx==976*FRACUNIT && goal.aimy==-640*FRACUNIT);
        for(t=0;t<5*TICRATE && !Bot_HasKey(p,it_bluecard);++t) {
            start=clock();Bot_BuildTiccmd(&cmd,p);
            if(clock()-start>worst)worst=clock()-start;
            p->cmd=cmd;P_Ticker();++gametic;
        }
        CHECK(Bot_HasKey(p,it_bluecard));
        printf("PASS: MAP05 blue key collected in %d tics; worst command %.2f ms\n",
               t,worst*1000.0/CLOCKS_PER_SEC);
    }
    G_InitNew(sk_medium,1,5);p=&players[0];
    {
        mobj_t *enemy;
        p->cards[it_redcard]=true;
        p->weaponowned[wp_shotgun]=true;p->ammo[am_shell]=40;
        p->readyweapon=wp_shotgun;p->pendingweapon=wp_nochange;
        P_UnsetThingPosition(p->mo);p->mo->x=1776*FRACUNIT;p->mo->y=-288*FRACUNIT;
        P_SetThingPosition(p->mo);p->mo->z=p->mo->floorz=p->mo->subsector->sector->floorheight;
        p->mo->ceilingz=p->mo->subsector->sector->ceilingheight;p->mo->angle=ANG90;
        enemy=P_SpawnMobj(1792*FRACUNIT,-160*FRACUNIT,ONFLOORZ,MT_TROOP);
        enemy->target=p->mo;
        for(t=0;t<8*TICRATE && !line_used[185];++t) {
            Bot_BuildTiccmd(&cmd,p);p->cmd=cmd;P_Ticker();++gametic;
        }
        printf("DOOR regression tics=%d crossed=%d switch=%d enemyhp=%d goal=%d/%d\n",
               t,line_used[137],line_used[185],enemy->health,goal.type,goal.line);
        CHECK(line_used[137] && line_used[185]);
    }
    puts("PASS: MAP05 imp-room door and button complete without waiting for the door watchdog");
    return 0;
}
