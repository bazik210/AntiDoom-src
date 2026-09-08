#include <stdlib.h>
#include "i_system.h"
#include "d_net.h"
#include "m_argv.h"
#include "doomstat.h"
#include "i_net.h"

void I_InitNetwork(void)
{
    doomcom = (doomcom_t*)calloc(1, sizeof(*doomcom));
    doomcom->id = DOOMCOM_ID;
    doomcom->ticdup = 1;
    doomcom->numplayers = doomcom->numnodes = 1;
    doomcom->consoleplayer = 0;
    doomcom->deathmatch = false;
    netgame = false;
    if (M_CheckParm("-net"))
        I_Error("Network play is not implemented in this first Win32 port");
}

void I_NetCmd(void) { }
