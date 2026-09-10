// Emacs style mode select   -*- C++ -*- 
//-----------------------------------------------------------------------------
//
// $Id:$
//
// Copyright (C) 1993-1996 by id Software, Inc.
//
// This source is available for distribution and/or modification
// only under the terms of the DOOM Source Code License as
// published by id Software. All rights reserved.
//
// The source is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// FITNESS FOR A PARTICULAR PURPOSE. See the DOOM Source Code License
// for more details.
//
// $Log:$
//
// DESCRIPTION:
//	Archiving: SaveGame I/O.
//	Thinker, Ticker.
//
//-----------------------------------------------------------------------------

static const char
rcsid[] = "$Id: p_tick.c,v 1.4 1997/02/03 16:47:55 b1 Exp $";

#include "z_zone.h"
#include "p_local.h"

#include "doomstat.h"
#include "p_mobj.h"

extern void P_MobjThinker(mobj_t*);


int	leveltime;

//
// THINKERS
// All thinkers should be allocated by Z_Malloc
// so they can be operated on uniformly.
// The actual structures will vary in size,
// but the first element must be thinker_t.
//



// Both the head and tail of the thinker list.
thinker_t	thinkercap = { { NULL }, &thinkercap, &thinkercap };


//
// P_InitThinkers
//
void P_InitThinkers (void)
{
    thinkercap.prev = thinkercap.next  = &thinkercap;
}




//
// P_AddThinker
// Adds a new thinker at the end of the list.
//
void P_AddThinker (thinker_t* thinker)
{
    thinkercap.prev->next = thinker;
    thinker->next = &thinkercap;
    thinker->prev = thinkercap.prev;
    thinkercap.prev = thinker;
}



//
// P_RemoveThinker
// Deallocation is lazy -- it will not actually be freed
// until its thinking turn comes up.
//
void P_RemoveThinker (thinker_t* thinker)
{
  // FIXME: NOP.
  thinker->function.acv = (actionf_v)(-1);
}



//
// P_AllocateThinker
// Allocates memory and adds a new thinker at the end of the list.
//
void P_AllocateThinker (thinker_t*	thinker)
{
}



//
// P_RunThinkers
//
void P_RunThinkers (void)
{
    thinker_t*	currentthinker;

    currentthinker = thinkercap.next;
    while (currentthinker != &thinkercap)
    {
	if ( currentthinker->function.acv == (actionf_v)(-1) )
	{
	    // time to remove it
	    currentthinker->next->prev = currentthinker->prev;
	    currentthinker->prev->next = currentthinker->next;
	    Z_Free (currentthinker);
	}
	else
	{
	    if (currentthinker->function.acp1)
		currentthinker->function.acp1 (currentthinker);
	}
	currentthinker = currentthinker->next;
    }
}



//
// Interpolation state storage for smooth uncapped rendering
// without modifying struct sizes (100% savegame compatibility)
//
#include "p_interp.h"
#include <stdint.h>
#include <string.h>

extern int uncapped_fps;
extern int lookdir;

#define MOBJ_INTERP_SIZE 4096
#define MOBJ_INTERP_MASK (MOBJ_INTERP_SIZE - 1)

typedef struct {
    mobj_t* mobj;
    fixed_t oldx;
    fixed_t oldy;
    fixed_t oldz;
    angle_t oldangle;
} mobj_interp_entry_t;

static mobj_interp_entry_t mobj_interp_table[MOBJ_INTERP_SIZE];

typedef struct {
    fixed_t oldviewz;
    int oldlookdir;
} player_interp_entry_t;

static player_interp_entry_t player_interp_table[MAXPLAYERS];

void P_ClearInterpolation(void)
{
    int i;
    memset(mobj_interp_table, 0, sizeof(mobj_interp_table));
    memset(player_interp_table, 0, sizeof(player_interp_table));
    for (i = 0; i < MAXPLAYERS; i++)
    {
        if (playeringame[i] && players[i].mo && players[i].viewz <= 1)
            player_interp_table[i].oldviewz = players[i].mo->z + players[i].viewheight;
        else
            player_interp_table[i].oldviewz = players[i].viewz;
        player_interp_table[i].oldlookdir = 0;
    }
}

void P_SaveInterpolationState(void)
{
    thinker_t* th;
    int i;
    memset(mobj_interp_table, 0, sizeof(mobj_interp_table));
    for (th = thinkercap.next; th && th != &thinkercap; th = th->next)
    {
        if (th->function.acp1 == (actionf_p1)P_MobjThinker)
        {
            mobj_t* mo = (mobj_t*)th;
            uintptr_t h = (((uintptr_t)mo) >> 4) & MOBJ_INTERP_MASK;
            int step = 0;
            while (mobj_interp_table[h].mobj != NULL && mobj_interp_table[h].mobj != mo && step < 64)
            {
                h = (h + 1) & MOBJ_INTERP_MASK;
                step++;
            }
            mobj_interp_table[h].mobj = mo;
            mobj_interp_table[h].oldx = mo->x;
            mobj_interp_table[h].oldy = mo->y;
            mobj_interp_table[h].oldz = mo->z;
            mobj_interp_table[h].oldangle = mo->angle;
        }
    }
    for (i = 0; i < MAXPLAYERS; i++)
    {
        if (playeringame[i])
        {
            if (players[i].viewz <= 1 && players[i].mo)
                player_interp_table[i].oldviewz = players[i].mo->z + players[i].viewheight;
            else
                player_interp_table[i].oldviewz = players[i].viewz;
            player_interp_table[i].oldlookdir = lookdir;
        }
    }
}

void P_GetMobjInterp(mobj_t* mo, fixed_t* oldx, fixed_t* oldy, fixed_t* oldz, angle_t* oldangle)
{
    if (mo)
    {
        uintptr_t h = (((uintptr_t)mo) >> 4) & MOBJ_INTERP_MASK;
        int step = 0;
        while (mobj_interp_table[h].mobj != NULL && step < 64)
        {
            if (mobj_interp_table[h].mobj == mo)
            {
                if (oldx) *oldx = mobj_interp_table[h].oldx;
                if (oldy) *oldy = mobj_interp_table[h].oldy;
                if (oldz) *oldz = mobj_interp_table[h].oldz;
                if (oldangle) *oldangle = mobj_interp_table[h].oldangle;
                return;
            }
            h = (h + 1) & MOBJ_INTERP_MASK;
            step++;
        }
        if (oldx) *oldx = mo->x;
        if (oldy) *oldy = mo->y;
        if (oldz) *oldz = mo->z;
        if (oldangle) *oldangle = mo->angle;
    }
}

void P_GetPlayerInterp(player_t* player, fixed_t* oldviewz, int* oldlookdir)
{
    if (player)
    {
        int pnum = player - players;
        if (pnum >= 0 && pnum < MAXPLAYERS)
        {
            if (oldviewz) *oldviewz = player_interp_table[pnum].oldviewz;
            if (oldlookdir) *oldlookdir = player_interp_table[pnum].oldlookdir;
            return;
        }
        if (oldviewz) *oldviewz = player->viewz;
    }
}


//
// P_Ticker
//

void P_Ticker (void)
{
    int		i;
    
    // run the tic
    if (paused)
    {
	if (uncapped_fps)
	    P_SaveInterpolationState();
	return;
    }
		
    // pause if in menu and at least one tic has been run
    if ( !netgame
	 && menuactive
	 && !demoplayback
	 && players[consoleplayer].viewz != 1)
    {
	if (uncapped_fps)
	    P_SaveInterpolationState();
	return;
    }
    
		
    // Snapshot positions for interpolation before simulation step
    if (uncapped_fps)
        P_SaveInterpolationState();

    for (i=0 ; i<MAXPLAYERS ; i++)
	if (playeringame[i])
	    P_PlayerThink (&players[i]);
			
    P_RunThinkers ();
    P_UpdateSpecials ();
    P_RespawnSpecials ();

    // for par times
    leveltime++;	
}
