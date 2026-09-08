#define boolean win_boolean
#include <windows.h>
#undef boolean
#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <string.h>
#include "doomdef.h"
#include "m_misc.h"
#include "i_video.h"
#include "i_sound.h"
#include "d_net.h"
#include "g_game.h"
#include "i_system.h"

int mb_used = 6;
ticcmd_t emptycmd;
ticcmd_t *I_BaseTiccmd(void) { return &emptycmd; }
int I_GetHeapSize(void) { return mb_used * 1024 * 1024; }
byte *I_ZoneBase(int *size) { *size = I_GetHeapSize(); return (byte*)malloc(*size); }
int I_GetTime(void) { return (int)(GetTickCount64() * TICRATE / 1000); }
void I_Tactile(int on, int off, int total) { (void)on;(void)off;(void)total; }
void I_Init(void) { I_InitSound(); }
void I_WaitVBL(int count) { Sleep((DWORD)(count * 1000 / 70)); }
void I_BeginRead(void) { }
void I_EndRead(void) { }
byte *I_AllocLow(int length) { byte *p=(byte*)calloc(1,length); return p; }
void I_Quit(void) { D_QuitNetGame(); I_ShutdownSound(); I_ShutdownMusic(); M_SaveDefaults(); I_ShutdownGraphics(); exit(0); }
void I_Error(char *error, ...)
{
    va_list ap; char msg[1024]; va_start(ap,error); vsnprintf(msg,sizeof(msg),error,ap); va_end(ap);
    fprintf(stderr, "DOOM error: %s\n", msg);
    MessageBoxA(NULL, msg, "DOOM error", MB_OK | MB_ICONERROR);
    I_ShutdownGraphics(); exit(-1);
}
