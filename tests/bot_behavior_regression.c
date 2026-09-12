/* Focused production-controller regressions; no rendered game or input injection. */
#undef main
#include <stdio.h>
#include <stdlib.h>
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
    memset(&cmd,0,sizeof(cmd));Bot_Weapon(&cmd,p,512*FRACUNIT);
    CHECK((cmd.buttons&BT_CHANGE) && ((cmd.buttons&BT_WEAPONMASK)>>BT_WEAPONSHIFT)==wp_chaingun);
    p->readyweapon=wp_chaingun;memset(&cmd,0,sizeof(cmd));Bot_Weapon(&cmd,p,128*FRACUNIT);
    CHECK((cmd.buttons&BT_CHANGE) && ((cmd.buttons&BT_WEAPONMASK)>>BT_WEAPONSHIFT)==wp_shotgun);
    puts("PASS: long-range chaingun and close-range SSG");
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
    return 0;
}
