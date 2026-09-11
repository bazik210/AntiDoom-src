/* Native ABI used by migrate_saves.py; compiled with the port's GCC. */
#include <stdio.h>
#include <stddef.h>
#include "doomstat.h"
#include "p_local.h"
#include "p_spec.h"
#define SIZE(t) printf("\"" #t "\":%zu,",sizeof(t))
#define OFF(t,f) printf("\"" #t "." #f "\":%zu,",offsetof(t,f))
int main(void) {
 puts("{");
 SIZE(player_t); SIZE(mobj_t); SIZE(ceiling_t); SIZE(vldoor_t); SIZE(floormove_t);
 SIZE(plat_t); SIZE(lightflash_t); SIZE(strobe_t); SIZE(glow_t); SIZE(pspdef_t);
 OFF(player_t,psprites); OFF(player_t,health); OFF(player_t,armorpoints); OFF(player_t,readyweapon);
 OFF(mobj_t,state); OFF(mobj_t,type); OFF(mobj_t,player); OFF(mobj_t,x); OFF(mobj_t,y); OFF(mobj_t,z); OFF(mobj_t,health);
 OFF(ceiling_t,sector); OFF(vldoor_t,sector); OFF(floormove_t,sector); OFF(plat_t,sector);
 OFF(lightflash_t,sector); OFF(strobe_t,sector); OFF(glow_t,sector);
 printf("\"NUMSTATES\":%d,\"NUMMOBJTYPES\":%d,\"pointer\":%zu}\n",NUMSTATES,NUMMOBJTYPES,sizeof(void*));
 return 0;
}
