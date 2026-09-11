/* No window, cursor capture, audio device, or music worker in simulation tests. */
#include "doomstat.h"
#include "s_sound.h"
int snd_SfxVolume, snd_MusicVolume, numChannels;
void I_CaptureMouse(boolean capture) { (void)capture; }
void S_Init(int sfx, int music) { (void)sfx; (void)music; }
void S_Start(void) {}
void S_StartSound(void *origin, int sound) { (void)origin; (void)sound; }
void S_StartSoundAtVolume(void *origin, int sound, int volume) { (void)origin; (void)sound; (void)volume; }
void S_StopSound(void *origin) { (void)origin; }
void S_UpdateSounds(void *listener) { (void)listener; }
void S_StartMusic(int music) { (void)music; }
void S_ChangeMusic(int music, int looping) { (void)music; (void)looping; }
void S_StopMusic(void) {}
void S_PauseSound(void) {}
void S_ResumeSound(void) {}
void S_SetMusicVolume(int volume) { snd_MusicVolume = volume; }
void S_SetSfxVolume(int volume) { snd_SfxVolume = volume; }
