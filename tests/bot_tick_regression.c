/* Exercise the real command queue and G_Ticker with a deterministic angle
   controller. No gameplay AI, window, input injection, or save-file writes. */
#undef main
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "doomstat.h"
#include "p_local.h"
#include "p_setup.h"
#include "p_bot.h"
#include "g_game.h"
#include "m_argv.h"
#include "w_wad.h"
#include "v_video.h"
#include "z_zone.h"
#include "hu_stuff.h"
#include "st_stuff.h"

extern int screenblocks, uncapped_fps;
extern boolean sendpause;
void G_BuildTiccmd(ticcmd_t *cmd);
void R_ExecuteSetViewSize(void);
boolean bot_active = true;
static int calls;
static angle_t desired;
void Bot_Init(void) {}
void Bot_InitLevel(void) {}
void Bot_BuildTiccmd(ticcmd_t *cmd, player_t *p)
{
    int delta = (short)((desired-p->mo->angle)>>16);
    memset(cmd,0,sizeof(*cmd));
    cmd->angleturn = delta > 2400 ? 2400 : delta < -2400 ? -2400 : delta;
    ++calls;
}
#define CHECK(x) do { if (!(x)) { fprintf(stderr,"FAIL line %d: %s\n",__LINE__,#x); return 1; } } while (0)

int main(int argc, char **argv)
{
    char *wads[2];
    int batch, i, before;
    if (argc != 2) return 2;
    myargc=argc; myargv=argv;
    wads[0]=argv[1]; wads[1]=NULL;
    Z_Init(); W_InitMultipleFiles(wads);
    gamemode=commercial; playeringame[0]=true; precache=false;
    nomonsters=true; screenblocks=10; uncapped_fps=0; ticdup=1;
    V_Init(); R_Init(); P_Init(); HU_Init(); ST_Init();
    G_InitNew(sk_medium,1,3); R_ExecuteSetViewSize();
    for (batch=1;batch<=6;++batch) {
        angle_t start=players[0].mo->angle;
        int previous=0;
        desired=start+((angle_t)1800<<16);
        before=calls;
        maketic=gametic;
        for (i=0;i<batch;++i) {
            G_BuildTiccmd(&netcmds[0][maketic%BACKUPTICS]);
            ++maketic;
        }
        CHECK(calls==before); /* No control updates against a stale world. */
        for (i=0;i<batch;++i) {
            int rotated;
            G_Ticker(); ++gametic;
            rotated=(short)((players[0].mo->angle-start)>>16);
            CHECK(rotated>=previous && rotated<=1800);
            previous=rotated;
        }
        CHECK(calls==before+batch && previous==1800);
    }
    before=calls;
    sendpause=true;
    G_BuildTiccmd(&netcmds[0][gametic%BACKUPTICS]);
    G_Ticker(); ++gametic;
    CHECK(paused && calls==before);
    sendpause=true;
    G_BuildTiccmd(&netcmds[0][gametic%BACKUPTICS]);
    G_Ticker(); ++gametic;
    CHECK(!paused && calls==before);
    menuactive=true;
    players[0].viewz=41*FRACUNIT;
    G_BuildTiccmd(&netcmds[0][gametic%BACKUPTICS]);
    G_Ticker(); ++gametic;
    CHECK(calls==before);
    menuactive=false;
    G_SaveGame(0,"QUEUE TEST");
    G_BuildTiccmd(&netcmds[0][gametic%BACKUPTICS]);
    G_Ticker(); ++gametic;
    CHECK(gameaction==ga_savegame && calls==before);
    gameaction=ga_nothing; /* Deliberately do not execute the disk write. */
    puts("PASS: batches 1..6 have no angle overshoot; pause/menu/save preserved");
    return 0;
}
