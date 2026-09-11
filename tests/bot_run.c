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
    G_InitNew(skill, episode, map);
    R_ExecuteSetViewSize();
    for (t = 0; t < limit && gameaction == ga_nothing; ++t) {
        Bot_BuildTiccmd(&players[0].cmd, &players[0]);
        P_Ticker(); ++gametic;
        if (!(t % (35 * 30))) printf("PROGRESS tic=%d hp=%d pos=%d,%d kills=%d/%d\n", t, players[0].health, players[0].mo->x/FRACUNIT, players[0].mo->y/FRACUNIT, players[0].killcount, totalkills);
        if (players[0].playerstate == PST_DEAD) break;
    }
    printf("RESULT map=%d episode=%d nomonsters=%d skill=%d tics=%d hp=%d action=%d kills=%d/%d pos=%d,%d\n", map, episode, nomonsters, skill+1, t, players[0].health, gameaction, players[0].killcount, totalkills, players[0].mo->x/FRACUNIT, players[0].mo->y/FRACUNIT);
    printf("CARDS %d %d %d %d %d %d\n", players[0].cards[0],players[0].cards[1],players[0].cards[2],players[0].cards[3],players[0].cards[4],players[0].cards[5]);
    printf("STATE z=%d floor=%d ceil=%d cmd=%d,%d,%d,%d path=%d/%d\n",players[0].mo->z/FRACUNIT,players[0].mo->floorz/FRACUNIT,players[0].mo->ceilingz/FRACUNIT,players[0].cmd.forwardmove,players[0].cmd.sidemove,players[0].cmd.angleturn,players[0].cmd.buttons,path_step,path_len);
    { extern fixed_t tmfloorz,tmceilingz; mobj_t *mo=players[0].mo; fixed_t x=mo->x,y=mo->y+2*FRACUNIT; int ok=P_CheckPosition(mo,x,y); printf("PHYS angle=%u mom=%d,%d flags=%x check=%d floor=%d ceil=%d bot=%d height=%d\n",mo->angle,mo->momx,mo->momy,mo->flags,ok,tmfloorz/FRACUNIT,tmceilingz/FRACUNIT,Bot_Walk(mo->x,mo->y,mo->z,x,y,mo,true),mo->height/FRACUNIT); }
    for(t=path_step;t<path_len && t<path_step+5;++t) printf("PATH %d %d,%d floor=%d walk=%d\n",t,Bot_X(path[t])/FRACUNIT,Bot_Y(path[t])/FRACUNIT,cells[path[t]].floor/FRACUNIT,Bot_Walk(players[0].mo->x,players[0].mo->y,players[0].mo->z,Bot_X(path[t]),Bot_Y(path[t]),players[0].mo,true));
    if (argc>7) {
        FILE *f=fopen(argv[7],"w"); int c;
        Bot_Flood(&players[0]);
        { thinker_t *th; for(th=thinkercap.next;th!=&thinkercap;th=th->next) if(th->function.acp1==(actionf_p1)P_MobjThinker) {
            mobj_t *mo=(mobj_t*)th;
            if(mo->sprite==SPR_RSKU || mo->sprite==SPR_RKEY || mo->sprite==SPR_BKEY) printf("KEY %d,%d z=%d sectorfloor=%d ceiling=%d cell=%d position=%d\n",mo->x/FRACUNIT,mo->y/FRACUNIT,mo->z/FRACUNIT,mo->subsector->sector->floorheight/FRACUNIT,mo->subsector->sector->ceilingheight/FRACUNIT,Bot_NearCell(mo->x,mo->y,mo->z,true),Bot_Position(mo->x,mo->y,NULL,false,NULL));
        }}
        for(c=0;c<count;++c) if(cells[c].stamp==stamp && cells[c].dist<INF)
            fprintf(f,"%d %d\n",Bot_X(c)/FRACUNIT,Bot_Y(c)/FRACUNIT);
        fclose(f);
        if(map==2) { int x,y; for(y=320;y<=560;y+=16) { printf("GRID y=%d ",y); for(x=1088;x<=1200;x+=16) { int c=((y*FRACUNIT-orgy)/(GRID*FRACUNIT))*width+(x*FRACUNIT-orgx)/(GRID*FRACUNIT); printf("%d:%d/%d/%d ",x,Bot_Cell(c),cells[c].floor/FRACUNIT,cells[c].dist==INF?-1:cells[c].dist); } puts(""); } }
    }
    return gameaction == ga_completed || gameaction == ga_victory ? 0 : 1;
}
