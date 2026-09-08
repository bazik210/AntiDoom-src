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
static boolean music_playing = false;
static boolean music_looping = false;
static boolean music_paused = false;

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
    I_InitMusic();
    I_Log("Sound: Windows waveOut initialized (11025 Hz 8-bit mono)\n");
    I_Log("Music: Windows MCI MIDI Sequencer ready\n");
}
void I_ShutdownSound(void) { int i; if(wave){waveOutReset(wave);waveOutClose(wave);wave=NULL;} for(i=0;i<NUMSFX;i++){if(!S_sfx[i].link)free(S_sfx[i].data);S_sfx[i].data=NULL;} }
void I_UpdateSound(void)
{
    static DWORD last_loop_check = 0;
    DWORD now = GetTickCount();
    if (music_playing && music_looping && !music_paused && (now - last_loop_check >= 500)) {
        last_loop_check = now;
        char mode[32];
        if (mciSendStringA("status doom_bgm mode", mode, sizeof(mode), NULL) == 0) {
            if (strcmp(mode, "stopped") == 0) mciSendStringA("play doom_bgm from 0", NULL, 0, NULL);
        }
    }
}
void I_SetChannels(void) { }
void I_SetSfxVolume(int v) { snd_SfxVolume=v; }
void I_SetMusicVolume(int v);
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
    for(s=0;s<MIXSAMPLES;s++){mixed=128;for(c=0;c<MIXCHANNELS;c++)if(channels[c].data){sample=((int)channels[c].data[channels[c].pos]-128)*channels[c].volume/15;sample=sample*(256-abs(channels[c].sep-128))/256;mixed+=sample;if(++channels[c].pos>=channels[c].length)channels[c].data=NULL;}if(mixed<0)mixed=0;if(mixed>255)mixed=255;audio[b][s]=(byte)mixed;}
    h=&headers[b];memset(h,0,sizeof(*h));h->lpData=(LPSTR)audio[b];h->dwBufferLength=MIXSAMPLES;h->dwUser=b;
    if(waveOutPrepareHeader(wave,h,sizeof(*h))!=MMSYSERR_NOERROR||waveOutWrite(wave,h,sizeof(*h))!=MMSYSERR_NOERROR)InterlockedExchange(&busy[b],0);
}
static char current_mid_path[MAX_PATH] = {0};
static const void *current_song_data = NULL;
static int current_song_len = 0;
static int current_music_handle = 0;
static int next_music_handle = 1;
static const unsigned char mus2midi_ctrl[15] = { 0, 0, 1, 7, 10, 11, 91, 93, 64, 67, 120, 123, 126, 127, 121 };

static unsigned char* mus2midi(const unsigned char *mus, int mus_len, int *out_len)
{
    if (!mus || mus_len < 16 || memcmp(mus, "MUS\x1a", 4) != 0) return NULL;
    unsigned short score_start = *(unsigned short*)(mus + 6);
    if (score_start >= mus_len) return NULL;
    int max_midi = mus_len * 4 + 2048;
    unsigned char *midi = (unsigned char*)malloc(max_midi);
    if (!midi) return NULL;
    memcpy(midi, "MThd\0\0\0\x06\0\0\0\x01\0\x46", 14);
    memcpy(midi + 14, "MTrk\0\0\0\0", 8);
    int mpos = 22, p = score_start;
    unsigned int cur_delay = 0;
    static const unsigned char chan_map[16] = { 0,1,2,3,4,5,6,7,8,10,11,12,13,14,15,9 };
    unsigned char vol_map[16];
    memset(vol_map, 127, sizeof(vol_map));

    /* Initialize volume for each channel */
    int vol_val = (100 * snd_MusicVolume) / 15;
    for (int ch = 0; ch < 16; ch++) {
        midi[mpos++] = 0;
        midi[mpos++] = 0xb0 | ch;
        midi[mpos++] = 7;
        midi[mpos++] = (unsigned char)(vol_val > 127 ? 127 : vol_val);
    }

    while (p < mus_len && mpos < max_midi - 32) {
        unsigned char desc = mus[p++];
        int last = (desc & 0x80);
        int etype = (desc >> 4) & 0x07;
        int ch = desc & 0x0f;
        int mch = chan_map[ch];
        unsigned int val = cur_delay;
        unsigned char buf[8]; int bi = 0;
        buf[bi++] = val & 0x7f; val >>= 7;
        while (val > 0) { buf[bi++] = (val & 0x7f) | 0x80; val >>= 7; }
        while (bi > 0) midi[mpos++] = buf[--bi];
        cur_delay = 0;
        if (etype == 0) {
            if (p >= mus_len) break;
            midi[mpos++] = 0x80 | mch; midi[mpos++] = mus[p++] & 0x7f; midi[mpos++] = 64;
        } else if (etype == 1) {
            if (p >= mus_len) break;
            unsigned char b = mus[p++]; int note = b & 0x7f;
            if (b & 0x80) { if (p >= mus_len) break; vol_map[ch] = mus[p++] & 0x7f; }
            int nvol = (vol_map[ch] * snd_MusicVolume) / 15;
            midi[mpos++] = 0x90 | mch; midi[mpos++] = note; midi[mpos++] = (nvol > 127 ? 127 : nvol);
        } else if (etype == 2) {
            if (p >= mus_len) break;
            int v = mus[p++] << 6;
            midi[mpos++] = 0xe0 | mch; midi[mpos++] = v & 0x7f; midi[mpos++] = (v >> 7) & 0x7f;
        } else if (etype == 3) {
            if (p >= mus_len) break;
            unsigned char b = mus[p++];
            if (b >= 10 && b <= 14) {
                midi[mpos++] = 0xb0 | mch; midi[mpos++] = mus2midi_ctrl[b]; midi[mpos++] = 0;
            }
        } else if (etype == 4) {
            if (p + 1 >= mus_len) break;
            unsigned char ctrl = mus[p++], val2 = mus[p++];
            if (ctrl == 0) { midi[mpos++] = 0xc0 | mch; midi[mpos++] = val2 & 0x7f; }
            else if (ctrl < 10) {
                unsigned char mc = mus2midi_ctrl[ctrl];
                unsigned char mv = val2 & 0x7f;
                if (mc == 7) mv = (mv * snd_MusicVolume) / 15;
                midi[mpos++] = 0xb0 | mch;
                midi[mpos++] = mc;
                midi[mpos++] = mv;
            }
        } else if (etype == 5) break;
        if (last) {
            unsigned int ticks = 0;
            for (;;) {
                if (p >= mus_len) break;
                unsigned char b = mus[p++]; ticks = (ticks << 7) | (b & 0x7f);
                if (!(b & 0x80)) break;
            }
            cur_delay = ticks;
        }
    }
    midi[mpos++] = 0; midi[mpos++] = 0xff; midi[mpos++] = 0x2f; midi[mpos++] = 0x00;
    int track_len = mpos - 22;
    midi[18] = (track_len >> 24) & 0xff; midi[19] = (track_len >> 16) & 0xff;
    midi[20] = (track_len >> 8) & 0xff; midi[21] = track_len & 0xff;
    *out_len = mpos; return midi;
}

static void reconvert_current_song(void)
{
    if (!current_song_data || current_mid_path[0] == '\0') return;
    int out_len = 0;
    unsigned char *midi = NULL;
    if (current_song_len > 0) {
        midi = mus2midi((const unsigned char*)current_song_data, current_song_len, &out_len);
    }
    if (midi) {
        FILE *f = fopen(current_mid_path, "wb");
        if (f) { fwrite(midi, 1, out_len, f); fclose(f); }
        free(midi);
    }
}

void I_SetMusicVolume(int v)
{
    if (v > 15) return;
    if (v < 0) v = 0;
    if (v == snd_MusicVolume && music_playing && !music_paused) return;
    snd_MusicVolume = v;
    if (v == 0) {
        if (music_playing && !music_paused) {
            mciSendStringA("pause doom_bgm", NULL, 0, NULL);
            music_paused = true;
        }
        return;
    }
    if (music_playing && current_song_data && current_mid_path[0]) {
        char pos[32] = "0";
        if (music_paused) {
            pos[0] = '0'; pos[1] = '\0';
        } else {
            mciSendStringA("status doom_bgm position", pos, sizeof(pos), NULL);
        }
        mciSendStringA("stop doom_bgm", NULL, 0, NULL);
        mciSendStringA("close doom_bgm", NULL, 0, NULL);
        reconvert_current_song();
        char cmd[512];
        snprintf(cmd, sizeof(cmd), "open \"%s\" type sequencer alias doom_bgm", current_mid_path);
        if (mciSendStringA(cmd, NULL, 0, NULL) == 0) {
            snprintf(cmd, sizeof(cmd), "play doom_bgm from %s", pos);
            mciSendStringA(cmd, NULL, 0, NULL);
            music_paused = false;
        }
    }
}

void I_InitMusic(void) { }
void I_ShutdownMusic(void) { I_StopSong(0); I_UnRegisterSong(0); }
void I_PauseSong(int h) { (void)h; mciSendStringA("pause doom_bgm", NULL, 0, NULL); music_paused = true; }
void I_ResumeSong(int h) { (void)h; if (snd_MusicVolume > 0) { mciSendStringA("resume doom_bgm", NULL, 0, NULL); music_paused = false; } }

int I_RegisterSong(void *data)
{
    if (!data) return 0;
    current_song_data = data;
    current_song_len = 0;
    if (memcmp(data, "MUS\x1a", 4) == 0) {
        unsigned short slen = *(unsigned short*)((char*)data + 4);
        unsigned short sstart = *(unsigned short*)((char*)data + 6);
        current_song_len = slen + sstart + 16;
    }
    int out_len = 0; unsigned char *midi = NULL;
    if (current_song_len > 0) {
        midi = mus2midi((const unsigned char*)data, current_song_len, &out_len);
    } else if (memcmp(data, "MThd", 4) == 0) {
        midi = (unsigned char*)data;
    }
    if (!midi) return 0;
    char tmppath[MAX_PATH];
    GetTempPathA(MAX_PATH, tmppath);
    current_music_handle = next_music_handle++;
    snprintf(current_mid_path, sizeof(current_mid_path), "%sdoom_bgm_%d.mid", tmppath, current_music_handle);
    FILE *f = fopen(current_mid_path, "wb");
    if (f) { fwrite(midi, 1, out_len, f); fclose(f); }
    if (midi != (unsigned char*)data) free(midi);
    return current_music_handle;
}

void I_PlaySong(int h, int looping)
{
    if (!h || current_mid_path[0] == '\0') return;
    mciSendStringA("stop doom_bgm", NULL, 0, NULL);
    mciSendStringA("close doom_bgm", NULL, 0, NULL);
    music_playing = true;
    music_looping = (looping != 0);
    if (snd_MusicVolume == 0) {
        music_paused = true;
        return;
    }
    char cmd[512];
    snprintf(cmd, sizeof(cmd), "open \"%s\" type sequencer alias doom_bgm", current_mid_path);
    MCIERROR oerr = mciSendStringA(cmd, NULL, 0, NULL);
    MCIERROR perr = oerr == 0 ? mciSendStringA("play doom_bgm from 0", NULL, 0, NULL) : 9999;
    if (oerr == 0 && perr == 0) {
        music_paused = false;
    }
}

void I_StopSong(int h)
{
    (void)h;
    mciSendStringA("stop doom_bgm", NULL, 0, NULL);
    mciSendStringA("close doom_bgm", NULL, 0, NULL);
    music_playing = false;
    music_looping = false;
    music_paused = false;
}

void I_UnRegisterSong(int h)
{
    (void)h;
    if (current_mid_path[0]) {
        DeleteFileA(current_mid_path);
        current_mid_path[0] = '\0';
    }
    current_song_data = NULL;
    current_song_len = 0;
    current_music_handle = 0;
}
