#ifndef P_BOT_H
#define P_BOT_H

#include "doomdef.h"
#include "d_player.h"
#include "d_ticcmd.h"

extern boolean bot_active;

void Bot_Init(void);
void Bot_InitLevel(void);
void Bot_BuildTiccmd(ticcmd_t* cmd, player_t* player);

#endif
