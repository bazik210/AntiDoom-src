/* Load-only verification: no window, audio, simulation tics, or save writes. */
#undef main
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include "doomstat.h"
#include "m_argv.h"
#include "p_local.h"
#include "p_setup.h"
#include "p_bot.h"
#include "g_game.h"
#include "w_wad.h"
#include "v_video.h"
#include "z_zone.h"
#include "hu_stuff.h"
#include "st_stuff.h"
extern int screenblocks;
extern int ticdup;
void G_DoLoadGame(void);
int main(int argc,char **argv)
{
    char *wads[2]; int i,failures=0;
    if(argc<3) return 2;
    myargc=argc; myargv=argv; setbuf(stdout,NULL);
    wads[0]=argv[1]; wads[1]=NULL;
    Z_Init(); W_InitMultipleFiles(wads);
    gamemode=commercial; playeringame[0]=true; precache=false;
    screenblocks=10; bot_active=false; ticdup=1;
    V_Init(); R_Init(); P_Init(); HU_Init(); ST_Init();
    for(i=2;i<argc;++i) {
        unsigned char header[56]; player_t expected;
        FILE *f=fopen(argv[i],"rb");
        if(!f || fread(header,1,56,f)!=56 || fread(&expected,1,sizeof(expected),f)!=sizeof(expected)) return 2;
        fclose(f);
        G_LoadGame(argv[i]); G_DoLoadGame();
        if(gamestate!=GS_LEVEL || gamemap!=header[42] || !players[0].mo ||
           players[0].health!=expected.health || players[0].armorpoints!=expected.armorpoints ||
           players[0].killcount!=expected.killcount || players[0].itemcount!=expected.itemcount ||
           players[0].secretcount!=expected.secretcount || players[0].readyweapon!=expected.readyweapon ||
           memcmp(players[0].ammo,expected.ammo,sizeof(expected.ammo)) ||
           memcmp(players[0].cards,expected.cards,sizeof(expected.cards)) ||
           memcmp(players[0].weaponowned,expected.weaponowned,sizeof(expected.weaponowned))) {
            printf("FAIL %s\n",argv[i]); ++failures;
        } else printf("LOAD_OK %s MAP%02d health=%d armor=%d position=%d,%d,%d\n",argv[i],gamemap,
            players[0].health,players[0].armorpoints,players[0].mo->x,players[0].mo->y,players[0].mo->z);
    }
    return failures ? 1 : 0;
}
