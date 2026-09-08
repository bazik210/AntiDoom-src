#include <stdio.h>
#include "i_sound.h"
#include "w_wad.h"

void I_InitSound(void) { fprintf(stderr, "Sound: disabled in this first Win32 port\n"); }
void I_ShutdownSound(void) { }
void I_UpdateSound(void) { }
void I_SubmitSound(void) { }
void I_SetChannels(void) { }
void I_SetSfxVolume(int volume) { snd_SfxVolume = volume; }
void I_SetMusicVolume(int volume) { snd_MusicVolume = volume; }
int I_GetSfxLumpNum(sfxinfo_t *sfx) { char n[16]; sprintf(n, "ds%s", sfx->name); return W_CheckNumForName(n); }
int I_StartSound(int id, int vol, int sep, int pitch, int priority) { (void)id;(void)vol;(void)sep;(void)pitch;(void)priority; return 0; }
void I_StopSound(int handle) { (void)handle; }
int I_SoundIsPlaying(int handle) { (void)handle; return 0; }
void I_UpdateSoundParams(int handle, int vol, int sep, int pitch) { (void)handle;(void)vol;(void)sep;(void)pitch; }
void I_InitMusic(void) { }
void I_ShutdownMusic(void) { }
void I_PauseSong(int handle) { (void)handle; }
void I_ResumeSong(int handle) { (void)handle; }
int I_RegisterSong(void *data) { (void)data; return 0; }
void I_PlaySong(int handle, int looping) { (void)handle;(void)looping; }
void I_StopSong(int handle) { (void)handle; }
void I_UnRegisterSong(int handle) { (void)handle; }
