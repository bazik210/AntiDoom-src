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
    if (msg == WOM_DONE) { header = (WAVEHDR*)p1; InterlockedExchange(&busy[header->dwUser], 0); }
}
static int sound_lump(sfxinfo_t *sfx)
{
    char name[16]; sprintf(name, "ds%s", sfx->name);
    return W_CheckNumForName(name) >= 0 ? W_GetNumForName(name) : W_GetNumForName("dspistol");
}
void I_InitSound(void)
{
    WAVEFORMATEX f; int i, b, lump, size; byte *raw;
    memset(&f,0,sizeof(f)); f.wFormatTag=WAVE_FORMAT_PCM; f.nChannels=1; f.nSamplesPerSec=MIXRATE; f.wBitsPerSample=8; f.nBlockAlign=1; f.nAvgBytesPerSec=MIXRATE;
    if (waveOutOpen(&wave,WAVE_MAPPER,&f,(DWORD_PTR)wave_callback,0,CALLBACK_FUNCTION)!=MMSYSERR_NOERROR) { wave=NULL; fprintf(stderr,"Sound: waveOut unavailable\n"); return; }
    for (b = 0; b < MIXBUFFERS; b++) {
        memset(&headers[b], 0, sizeof(headers[b]));
        headers[b].lpData = (LPSTR)audio[b];
        headers[b].dwBufferLength = MIXSAMPLES;
        headers[b].dwUser = b;
        waveOutPrepareHeader(wave, &headers[b], sizeof(headers[b]));
        busy[b] = 0;
    }
    for (i = 1; i < NUMSFX; i++) {
        if (!S_sfx[i].link) {
            lump = sound_lump(&S_sfx[i]);
            size = W_LumpLength(lump);
            raw = (byte*)W_CacheLumpNum(lump, PU_STATIC);
            if (size > 8) {
                S_sfx[i].data = malloc(size - 8);
                memcpy(S_sfx[i].data, raw + 8, size - 8);
            }
        }
    }
    for (i = 1; i < NUMSFX; i++) if (S_sfx[i].link) S_sfx[i].data = S_sfx[i].link->data;
    I_InitMusic();
    I_Log("Sound: Windows waveOut initialized (11025 Hz 8-bit mono)\n");
    I_Log("Music: Windows MIDI output ready\n");
}
void I_ShutdownSound(void)
{
    int i, b;
    if (wave) {
        HWAVEOUT w = wave;
        wave = NULL;
        waveOutReset(w);
        for (b = 0; b < MIXBUFFERS; b++) {
            waveOutUnprepareHeader(w, &headers[b], sizeof(headers[b]));
        }
        waveOutClose(w);
    }
    for (i = 1; i < NUMSFX; i++) {
        if (!S_sfx[i].link && S_sfx[i].data) {
            free(S_sfx[i].data);
            S_sfx[i].data = NULL;
        }
    }
}
void I_UpdateSound(void)
{
    // Music is scheduled by the dedicated MIDI output worker thread.
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
    h=&headers[b];
    if(waveOutWrite(wave,h,sizeof(*h))!=MMSYSERR_NOERROR)InterlockedExchange(&busy[b],0);
}
static HMIDIOUT midi_device = NULL;
static HANDLE music_thread = NULL, music_wake_event = NULL;
static volatile LONG shutdown_worker = 0, desired_music_handle = 0;
static volatile LONG desired_looping = 0, desired_paused = 0;
static volatile LONG music_generation = 0;
static const unsigned char *desired_song = NULL;
static int desired_song_len = 0;
static const void *current_song_data = NULL;
static int current_song_len = 0;
static int current_music_handle = 0, next_music_handle = 1;
static const unsigned char mus2midi_ctrl[15] = { 0, 0, 1, 7, 10, 11, 91, 93, 64, 67, 120, 123, 126, 127, 121 };
static CRITICAL_SECTION midi_lock;
static boolean midi_lock_ready = false;
static boolean midi_timer_ready = false;

static int mus_channel(int ch)
{
    static const int map[16] = { 0,1,2,3,4,5,6,7,8,10,11,12,13,14,15,9 };
    return map[ch & 15];
}

static void midi_short(unsigned int msg)
{
    if (!midi_device) return;
    EnterCriticalSection(&midi_lock);
    if (midi_device) midiOutShortMsg(midi_device, msg);
    LeaveCriticalSection(&midi_lock);
}

static void midi_all_notes_off(void)
{
    int ch;
    for (ch = 0; ch < 16; ch++) midi_short(0x00007bb0 | ch);
}

static int music_cancelled(LONG generation)
{
    return InterlockedCompareExchange(&music_generation, 0, 0) != generation ||
           InterlockedCompareExchange(&shutdown_worker, 0, 0) != 0;
}

static int music_wait(LONG generation, unsigned int ms)
{
    return WaitForSingleObject(music_wake_event, ms) == WAIT_OBJECT_0 || music_cancelled(generation);
}

static void send_music_event(int type, int channel, int a, int b, int volume)
{
    unsigned int msg;
    int midi_ch = mus_channel(channel);
    switch (type) {
    case 0: msg = (unsigned int)((0x80 | midi_ch) | ((a & 127) << 8) | (64 << 16)); break;
    case 1: msg = (unsigned int)(0x90 | midi_ch | ((a & 127) << 8) | (((b * volume) / 15) << 16)); break;
    case 2: { int bend = (a & 255) << 6; msg = (unsigned int)(0xe0 | midi_ch | ((bend & 127) << 8) | (((bend >> 7) & 127) << 16)); break; }
    default: return;
    }
    midi_short(msg);
}

static int play_mus(const unsigned char *mus, int len, LONG generation)
{
    int p, ch, type, last, b, ctrl, val, note;
    unsigned char volume[16];
    if (!mus || len < 16 || memcmp(mus, "MUS\x1a", 4) != 0) return 0;
    p = *(const unsigned short *)(mus + 6);
    if (p >= len) return 0;
    memset(volume, 127, sizeof(volume));
    for (ch = 0; ch < 16; ch++) midi_short((unsigned int)(0xb0 | mus_channel(ch) | (7 << 8) | (((100 * snd_MusicVolume) / 15) << 16)));
    while (p < len && !music_cancelled(generation)) {
        unsigned int delay = 0;
        do {
            if (p >= len) return 1;
            b = mus[p++]; last = b & 0x80; type = (b >> 4) & 7; ch = b & 15;
            switch (type) {
            case 0: if (p >= len) return 1; send_music_event(0, ch, mus[p++], 64, snd_MusicVolume); break;
            case 1:
                if (p >= len) return 1; b = mus[p++]; note = b & 127;
                if (b & 128) { if (p >= len) return 1; volume[ch] = mus[p++] & 127; }
                send_music_event(1, ch, note, volume[ch], snd_MusicVolume); break;
            case 2: if (p >= len) return 1; send_music_event(2, ch, mus[p++], 0, snd_MusicVolume); break;
            case 3: if (p >= len) return 1; b = mus[p++]; if (b >= 10 && b <= 14) midi_short(0x000000b0 | mus_channel(ch) | (mus2midi_ctrl[b] << 8)); break;
            case 4:
                if (p + 1 >= len) return 1; ctrl = mus[p++]; val = mus[p++];
                if (ctrl == 0) midi_short(0x000000c0 | mus_channel(ch) | ((val & 127) << 8));
                else if (ctrl < 10) midi_short(0x000000b0 | mus_channel(ch) | (mus2midi_ctrl[ctrl] << 8) | (((ctrl == 3 ? (val * snd_MusicVolume) / 15 : val) & 127) << 16));
                break;
            case 5: return 1;
            default: break;
            }
            if (last) {
                do { if (p >= len) return 1; b = mus[p++]; delay = (delay << 7) | (b & 127); } while (b & 128);
            }
        } while (!last);
        while (delay > 0 && !music_cancelled(generation)) {
            unsigned int ms = (delay * 1000 + 139) / 140;
            unsigned int elapsed_ticks;
            if (ms == 0) ms = 1;
            if (ms > 50) ms = 50;
            if (music_wait(generation, ms)) return 0;
            elapsed_ticks = (ms * 140 + 999) / 1000;
            if (elapsed_ticks > delay) elapsed_ticks = delay;
            delay -= elapsed_ticks;
        }
    }
    return 1;
}

static DWORD WINAPI music_worker(LPVOID param)
{
    const unsigned char *song = NULL; int song_len = 0; LONG generation = 0; int handle = 0;
    (void)param;
    while (!InterlockedCompareExchange(&shutdown_worker, 0, 0)) {
        WaitForSingleObject(music_wake_event, INFINITE);
        if (InterlockedCompareExchange(&shutdown_worker, 0, 0)) break;
        if (InterlockedCompareExchange(&desired_music_handle, 0, 0) == 0) { midi_all_notes_off(); song = NULL; handle = 0; continue; }
        song = desired_song; song_len = desired_song_len; handle = (int)desired_music_handle; generation = InterlockedCompareExchange(&music_generation, 0, 0);
        if (!desired_paused && snd_MusicVolume > 0) {
            do { if (!play_mus(song, song_len, generation)) break; } while (desired_looping && !music_cancelled(generation));
        }
        midi_all_notes_off();
        if (!music_cancelled(generation) && !desired_looping) handle = 0;
    }
    midi_all_notes_off();
    return 0;
}

void I_SetMusicVolume(int v)
{
    if (v > 15) return;
    if (v < 0) v = 0;
    if (v == snd_MusicVolume && music_playing && !music_paused) return;
    snd_MusicVolume = v;
    if (v == 0) {
        desired_paused = 1;
        midi_all_notes_off();
        music_paused = true;
        if (music_wake_event) SetEvent(music_wake_event);
        return;
    }
    desired_paused = 0;
    music_paused = false;
    if (music_wake_event) SetEvent(music_wake_event);
}

void I_InitMusic(void)
{
    if (!midi_lock_ready) { InitializeCriticalSection(&midi_lock); midi_lock_ready = true; }
    if (timeBeginPeriod(1) == TIMERR_NOERROR) midi_timer_ready = true;
    if (midiOutOpen(&midi_device, MIDI_MAPPER, 0, 0, CALLBACK_NULL) != MMSYSERR_NOERROR) {
        midi_device = NULL;
        I_Log("Music: no Windows MIDI output device available\n");
    }
    if (!music_wake_event) music_wake_event = CreateEventA(NULL, FALSE, FALSE, NULL);
    shutdown_worker = 0;
    if (!music_thread)
        music_thread = CreateThread(NULL, 0, music_worker, NULL, 0, NULL);
}

void I_WaitForMusic(int max_ms) { (void)max_ms; }

void I_ShutdownMusic(void)
{
    I_StopSong(0);
    shutdown_worker = 1;
    if (music_wake_event)
        SetEvent(music_wake_event);
    if (music_thread) {
        WaitForSingleObject(music_thread, 1000);
        CloseHandle(music_thread);
        music_thread = NULL;
    }
    if (music_wake_event) {
        CloseHandle(music_wake_event);
        music_wake_event = NULL;
    }
    if (midi_device) { midiOutReset(midi_device); midiOutClose(midi_device); midi_device = NULL; }
    if (midi_timer_ready) { timeEndPeriod(1); midi_timer_ready = false; }
    if (midi_lock_ready) { DeleteCriticalSection(&midi_lock); midi_lock_ready = false; }
}

void I_PauseSong(int h)
{
    (void)h;
    desired_paused = 1;
    music_paused = true;
    if (music_wake_event) SetEvent(music_wake_event);
}

void I_ResumeSong(int h)
{
    (void)h;
    if (snd_MusicVolume > 0) {
        desired_paused = 0;
        music_paused = false;
        if (music_wake_event) SetEvent(music_wake_event);
    }
}

int I_RegisterSong(void *data)
{
    unsigned short slen, sstart;
    if (!data || memcmp(data, "MUS\x1a", 4) != 0) return 0;
    slen = *(unsigned short *)((char *)data + 4); sstart = *(unsigned short *)((char *)data + 6);
    if ((int)slen + sstart < sstart) return 0;
    current_song_data = data; current_song_len = slen + sstart;
    current_music_handle = next_music_handle++;
    return current_music_handle;
}

void I_PlaySong(int h, int looping)
{
    if (!h || !current_song_data || current_song_len <= 0) return;
    music_playing = true;
    music_looping = (looping != 0);
    desired_music_handle = h;
    desired_looping = (looping != 0);
    desired_paused = (snd_MusicVolume == 0);
    desired_song = (const unsigned char *)current_song_data;
    desired_song_len = current_song_len;
    InterlockedIncrement(&music_generation);
    if (music_wake_event) SetEvent(music_wake_event);
}

void I_StopSong(int h)
{
    (void)h;
    desired_music_handle = 0;
    desired_song = NULL;
    desired_song_len = 0;
    InterlockedIncrement(&music_generation);
    music_playing = false;
    music_looping = false;
    music_paused = false;
    if (music_wake_event) SetEvent(music_wake_event);
}

void I_UnRegisterSong(int h)
{
    (void)h;
    current_song_data = NULL;
    current_song_len = 0;
    current_music_handle = 0;
}
