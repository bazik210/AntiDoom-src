// Emacs style mode select   -*- C++ -*- 
//-----------------------------------------------------------------------------
//
// DESCRIPTION:
//	Interpolation state storage for uncapped FPS without modifying
//	mobj_t or player_t binary layouts (preserving savegame compatibility).
//
//-----------------------------------------------------------------------------

#ifndef __P_INTERP__
#define __P_INTERP__

#include "doomtype.h"
#include "m_fixed.h"
#include "tables.h"
#include "p_mobj.h"
#include "d_player.h"

void P_ClearInterpolation(void);
void P_SaveInterpolationState(void);
void P_GetMobjInterp(mobj_t* mo, fixed_t* oldx, fixed_t* oldy, fixed_t* oldz, angle_t* oldangle);
void P_GetPlayerInterp(player_t* player, fixed_t* oldviewz, int* oldlookdir);

#endif
