#define boolean win_boolean
#include <windows.h>
#include <mmsystem.h>
#undef boolean
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "i_sound.h"
#include "w_wad.h"
#include "z_zone.h"

#define MIXRATE 11025
#define MIXSAMPLES 512
#define MIXBUFFERS 4
#define MIXCHANNELS 8
typedef struct { byte *data; int length, pos, volume, sep, handle; } win_channel_t;
static HWAVEOUT wave;
static WAVEHDR headers[MIXBUFFERS];
static byte audio[MIXBUFFERS][MIXSAMPLES];
static volatile LONG busy[MIXBUFFERS];
static win_channel_t channels[MIXCHANNELS];
static int next_handle = 1;

static void CALLBACK wave_callback(HWAVEOUT h, UINT msg, DWORD_PTR instance, DWORD_PTR p1, DWORD_PTR p2)
{
    WAVEHDR *header;
    (void)h; (void)instance; (void)p2;
    if (msg == WOM_DONE) { header = (WAVEHDR*)p1; InterlockedExchange(&busy[header->dwUser], 0); waveOutUnprepareHeader(wave, header, sizeof(*header)); }
}
static int sound_lump(sfxinfo_t *sfx)
{
    char name[16]; sprintf(name, "ds%s", sfx->name);
    return W_CheckNumForName(name) >= 0 ? W_GetNumForName(name) : W_GetNumForName("dspistol");
}
void I_InitSound(void)
{
    WAVEFORMATEX f; int i, lump, size; byte *raw;
    memset(&f,0,sizeof(f)); f.wFormatTag=WAVE_FORMAT_PCM; f.nChannels=1; f.nSamplesPerSec=MIXRATE; f.wBitsPerSample=8; f.nBlockAlign=1; f.nAvgBytesPerSec=MIXRATE;
    if (waveOutOpen(&wave,WAVE_MAPPER,&f,(DWORD_PTR)wave_callback,0,CALLBACK_FUNCTION)!=MMSYSERR_NOERROR) { wave=NULL; fprintf(stderr,"Sound: waveOut unavailable\n"); return; }
    for (i=1;i<NUMSFX;i++) { lump=sound_lump(&S_sfx[i]); size=W_LumpLength(lump); raw=(byte*)W_CacheLumpNum(lump,PU_STATIC); if(size>8){S_sfx[i].data=malloc(size-8);memcpy(S_sfx[i].data,raw+8,size-8);} }
    for (i=1;i<NUMSFX;i++) if (S_sfx[i].link) S_sfx[i].data = S_sfx[i].link->data;
    fprintf(stderr,"Sound: Windows waveOut initialized\n");
}
void I_ShutdownSound(void) { int i; if(wave){waveOutReset(wave);waveOutClose(wave);wave=NULL;} for(i=0;i<NUMSFX;i++){if(!S_sfx[i].link)free(S_sfx[i].data);S_sfx[i].data=NULL;} }
void I_UpdateSound(void) { }
void I_SetChannels(void) { }
void I_SetSfxVolume(int v) { snd_SfxVolume=v; }
void I_SetMusicVolume(int v) { snd_MusicVolume=v; }
int I_GetSfxLumpNum(sfxinfo_t *s) { return sound_lump(s); }
int I_StartSound(int id,int vol,int sep,int pitch,int priority)
{
    int i,slot=0; (void)pitch;(void)priority;
    if(!wave||id<=0||id>=NUMSFX||!S_sfx[id].data)return 0;
    for(i=0;i<MIXCHANNELS;i++)if(!channels[i].data){slot=i;break;}
    channels[slot].data=(byte*)S_sfx[id].data; channels[slot].length=W_LumpLength(sound_lump(&S_sfx[id]))-8; channels[slot].pos=0; channels[slot].volume=vol; channels[slot].sep=sep; channels[slot].handle=next_handle++;
    return channels[slot].handle;
}
void I_StopSound(int h) { int i;for(i=0;i<MIXCHANNELS;i++)if(channels[i].handle==h)channels[i].data=NULL; }
int I_SoundIsPlaying(int h) { int i;for(i=0;i<MIXCHANNELS;i++)if(channels[i].handle==h&&channels[i].data)return 1;return 0; }
void I_UpdateSoundParams(int h,int v,int s,int p) { int i;(void)p;for(i=0;i<MIXCHANNELS;i++)if(channels[i].handle==h){channels[i].volume=v;channels[i].sep=s;} }
void I_SubmitSound(void)
{
    int b,s,c,mixed,sample; WAVEHDR *h;
    if(!wave)return;
    for(b=0;b<MIXBUFFERS;b++)if(InterlockedCompareExchange(&busy[b],1,0)==0)break;
    if(b==MIXBUFFERS)return;
    for(s=0;s<MIXSAMPLES;s++){mixed=128;for(c=0;c<MIXCHANNELS;c++)if(channels[c].data){sample=((int)channels[c].data[channels[c].pos]-128)*channels[c].volume/127;sample=sample*(256-abs(channels[c].sep-128))/256;mixed+=sample;if(++channels[c].pos>=channels[c].length)channels[c].data=NULL;}if(mixed<0)mixed=0;if(mixed>255)mixed=255;audio[b][s]=(byte)mixed;}
    h=&headers[b];memset(h,0,sizeof(*h));h->lpData=(LPSTR)audio[b];h->dwBufferLength=MIXSAMPLES;h->dwUser=b;
    if(waveOutPrepareHeader(wave,h,sizeof(*h))!=MMSYSERR_NOERROR||waveOutWrite(wave,h,sizeof(*h))!=MMSYSERR_NOERROR)InterlockedExchange(&busy[b],0);
}
void I_InitMusic(void) { } void I_ShutdownMusic(void) { } void I_PauseSong(int h){(void)h;} void I_ResumeSong(int h){(void)h;} int I_RegisterSong(void*d){(void)d;return 0;} void I_PlaySong(int h,int l){(void)h;(void)l;} void I_StopSong(int h){(void)h;} void I_UnRegisterSong(int h){(void)h;}
