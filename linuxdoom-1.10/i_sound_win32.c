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
    I_Log("Music: Windows MIDI backend initialized\n");
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
static HANDLE music_thread = NULL, music_wake_event = NULL, music_idle_event = NULL;
static volatile LONG shutdown_worker = 0, desired_music_handle = 0;
static volatile LONG desired_looping = 0, desired_paused = 0;
static volatile LONG music_generation = 0;
static const unsigned char *desired_song = NULL;
static int desired_song_len = 0, desired_song_format = 0;
static const void *current_song_data = NULL;
static int current_song_len = 0, current_song_format = 0;
static int current_music_handle = 0, next_music_handle = 1;
static const unsigned char mus2midi_ctrl[15] = { 0, 0, 1, 7, 10, 11, 91, 93, 64, 67, 120, 123, 126, 127, 121 };
static CRITICAL_SECTION midi_lock;
static CRITICAL_SECTION music_state_lock;
static boolean midi_lock_ready = false, music_state_lock_ready = false;
static boolean midi_timer_ready = false;

#define MUSIC_FORMAT_MUS 1
#define MUSIC_FORMAT_MIDI 2

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

static void midi_set_channel_volume(int volume)
{
    int ch;
    int value = (100 * volume) / 15;
    for (ch = 0; ch < 16; ch++)
        midi_short((unsigned int)(0x000000b0 | ch | (7 << 8) | ((value & 127) << 16)));
}

static int music_cancelled(LONG generation)
{
    return InterlockedCompareExchange(&music_generation, 0, 0) != generation ||
           InterlockedCompareExchange(&shutdown_worker, 0, 0) != 0;
}

static int music_pause_wait(LONG generation)
{
    while (InterlockedCompareExchange(&desired_paused, 0, 0)) {
        midi_all_notes_off();
        if (WaitForSingleObject(music_wake_event, INFINITE) == WAIT_FAILED) return 1;
        if (music_cancelled(generation)) return 1;
    }
    return music_cancelled(generation);
}

static int music_wait(LONG generation, unsigned int ms)
{
    WaitForSingleObject(music_wake_event, ms);
    if (music_cancelled(generation)) return 1;
    return music_pause_wait(generation);
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
        if (music_pause_wait(generation)) return 0;
        do {
            if (p >= len) return 1;
            b = mus[p++]; last = b & 0x80; type = (b >> 4) & 7; ch = b & 15;
            switch (type) {
            case 0: if (p >= len) return 1; send_music_event(0, ch, mus[p++], 64, snd_MusicVolume); break;
            case 1:
                if (p >= len) return 1;
                b = mus[p++];
                note = b & 127;
                if (b & 128) { if (p >= len) return 1; volume[ch] = mus[p++] & 127; }
                send_music_event(1, ch, note, volume[ch], snd_MusicVolume); break;
            case 2: if (p >= len) return 1; send_music_event(2, ch, mus[p++], 0, snd_MusicVolume); break;
            case 3: if (p >= len) return 1; b = mus[p++]; if (b >= 10 && b <= 14) midi_short(0x000000b0 | mus_channel(ch) | (mus2midi_ctrl[b] << 8)); break;
            case 4:
                if (p + 1 >= len) return 1;
                ctrl = mus[p++];
                val = mus[p++];
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

typedef struct {
    unsigned long long tick;
    unsigned int msg;
    unsigned int tempo;
    int is_tempo;
} midi_event_t;

static unsigned int midi_be16(const unsigned char *p)
{
    return ((unsigned int)p[0] << 8) | p[1];
}

static unsigned int midi_be32(const unsigned char *p)
{
    return ((unsigned int)p[0] << 24) | ((unsigned int)p[1] << 16) |
           ((unsigned int)p[2] << 8) | p[3];
}

static int midi_vlq(const unsigned char **cursor, const unsigned char *end, unsigned int *value)
{
    const unsigned char *p = *cursor;
    unsigned int result = 0;
    int count = 0;
    do {
        if (p >= end || count++ == 4) return 0;
        result = (result << 7) | (*p & 127);
    } while (*p++ & 128);
    *cursor = p;
    *value = result;
    return 1;
}

static int midi_file_length(const unsigned char *data, int *length)
{
    unsigned int header_len, tracks, i, pos;
    if (!data || memcmp(data, "MThd", 4) != 0) return 0;
    header_len = midi_be32(data + 4);
    if (header_len < 6 || header_len > 1024) return 0;
    tracks = midi_be16(data + 10);
    pos = 8 + header_len;
    for (i = 0; i < tracks; i++) {
        unsigned int track_len;
        if (memcmp(data + pos, "MTrk", 4) != 0) return 0;
        track_len = midi_be32(data + pos + 4);
        pos += 8 + track_len;
    }
    if (pos == 0 || pos > 0x7fffffff) return 0;
    *length = (int)pos;
    return 1;
}

static int midi_add_event(midi_event_t **events, int *count, int *capacity, midi_event_t event)
{
    midi_event_t *grown;
    if (*count == *capacity) {
        int new_capacity = *capacity ? *capacity * 2 : 256;
        grown = (midi_event_t *)realloc(*events, new_capacity * sizeof(**events));
        if (!grown) return 0;
        *events = grown;
        *capacity = new_capacity;
    }
    (*events)[(*count)++] = event;
    return 1;
}

static int midi_event_compare(const void *a, const void *b)
{
    const midi_event_t *ea = (const midi_event_t *)a;
    const midi_event_t *eb = (const midi_event_t *)b;
    if (ea->tick < eb->tick) return -1;
    if (ea->tick > eb->tick) return 1;
    return ea->is_tempo - eb->is_tempo;
}

static int midi_parse(const unsigned char *data, int len, midi_event_t **events, int *count, unsigned int *division)
{
    unsigned int header_len, tracks, track, pos;
    int capacity = 0;
    if (!midi_file_length(data, &len)) return 0;
    header_len = midi_be32(data + 4);
    tracks = midi_be16(data + 10);
    *division = midi_be16(data + 12);
    if (*division & 0x8000) *division = 70;
    pos = 8 + header_len;
    *events = NULL;
    *count = 0;
    for (track = 0; track < tracks; track++) {
        const unsigned char *p;
        const unsigned char *end;
        unsigned long long tick = 0;
        int running = 0;
        unsigned int track_len = midi_be32(data + pos + 4);
        if (memcmp(data + pos, "MTrk", 4) != 0 || pos + 8 + track_len > (unsigned int)len) goto fail;
        p = data + pos + 8;
        end = p + track_len;
        while (p < end) {
            unsigned int delta, value;
            int status, data_bytes, meta;
            midi_event_t event;
            if (!midi_vlq(&p, end, &delta)) goto fail;
            tick += delta;
            if (p >= end) goto fail;
            status = *p++;
            if (status < 0x80) {
                if (!running) goto fail;
                p--;
                status = running;
            } else if (status < 0xf0) {
                running = status;
            }
            if (status >= 0x80 && status < 0xf0) {
                data_bytes = ((status & 0xe0) == 0xc0) ? 1 : 2;
                if (p + data_bytes > end) goto fail;
                event.tick = tick;
                event.msg = (unsigned int)status | ((unsigned int)p[0] << 8);
                if (data_bytes == 2) event.msg |= (unsigned int)p[1] << 16;
                event.tempo = 0;
                event.is_tempo = 0;
                if (!midi_add_event(events, count, &capacity, event)) goto fail;
                p += data_bytes;
            } else if (status == 0xff) {
                if (p >= end) goto fail;
                meta = *p++;
                if (!midi_vlq(&p, end, &value) || p + value > end) goto fail;
                if (meta == 0x51 && value == 3) {
                    event.tick = tick;
                    event.msg = 0;
                    event.tempo = ((unsigned int)p[0] << 16) | ((unsigned int)p[1] << 8) | p[2];
                    event.is_tempo = 1;
                    if (!midi_add_event(events, count, &capacity, event)) goto fail;
                }
                p += value;
            } else if (status == 0xf0 || status == 0xf7) {
                if (!midi_vlq(&p, end, &value) || p + value > end) goto fail;
                p += value;
            } else {
                data_bytes = (status == 0xf1 || status == 0xf3) ? 1 : (status == 0xf2 ? 2 : 0);
                if (p + data_bytes > end) goto fail;
                p += data_bytes;
            }
        }
        pos += 8 + track_len;
    }
    qsort(*events, *count, sizeof(**events), midi_event_compare);
    return 1;
fail:
    free(*events);
    *events = NULL;
    *count = 0;
    return 0;
}

static int play_midi(const unsigned char *data, int len, LONG generation)
{
    midi_event_t *events;
    unsigned int division, tempo = 500000;
    unsigned long long last_tick = 0;
    int count, i;
    if (!midi_parse(data, len, &events, &count, &division)) return 1;
    midi_set_channel_volume(snd_MusicVolume);
    for (i = 0; i < count && !music_cancelled(generation); i++) {
        unsigned long long delta = events[i].tick - last_tick;
        unsigned long long duration = (delta * tempo) / division;
        unsigned int ms;
        if (duration > 0) {
            ms = (unsigned int)((duration + 999) / 1000);
            if (ms > 50) ms = 50;
            while (duration > 0 && !music_cancelled(generation)) {
                unsigned long long slice = ms * 1000ULL;
                if (slice > duration) slice = duration;
                if (music_wait(generation, ms)) { free(events); return 0; }
                duration -= slice;
                ms = (unsigned int)((duration + 999) / 1000);
                if (ms > 50) ms = 50;
            }
        }
        if (events[i].is_tempo) tempo = events[i].tempo;
        else midi_short(events[i].msg);
        last_tick = events[i].tick;
    }
    free(events);
    return !music_cancelled(generation);
}

static DWORD WINAPI music_worker(LPVOID param)
{
    const unsigned char *song = NULL; int song_len = 0, song_format = 0;
    LONG generation = 0;
    (void)param;
    while (!InterlockedCompareExchange(&shutdown_worker, 0, 0)) {
        WaitForSingleObject(music_wake_event, INFINITE);
        if (InterlockedCompareExchange(&shutdown_worker, 0, 0)) break;
        if (InterlockedCompareExchange(&desired_music_handle, 0, 0) == 0) {
            midi_all_notes_off();
            song = NULL;
            if (music_idle_event) SetEvent(music_idle_event);
            continue;
        }
        EnterCriticalSection(&music_state_lock);
        song = desired_song;
        song_len = desired_song_len;
        song_format = desired_song_format;
        generation = InterlockedCompareExchange(&music_generation, 0, 0);
        LeaveCriticalSection(&music_state_lock);
        if (!InterlockedCompareExchange(&desired_paused, 0, 0) && snd_MusicVolume > 0) {
            do {
                if (song_format == MUSIC_FORMAT_MUS) {
                    if (!play_mus(song, song_len, generation)) break;
                } else if (song_format == MUSIC_FORMAT_MIDI) {
                    if (!play_midi(song, song_len, generation)) break;
                } else break;
            } while (InterlockedCompareExchange(&desired_looping, 0, 0) && !music_cancelled(generation));
        }
        midi_all_notes_off();
        if (!music_cancelled(generation) && !InterlockedCompareExchange(&desired_looping, 0, 0)) {
            EnterCriticalSection(&music_state_lock);
            if (InterlockedCompareExchange(&music_generation, 0, 0) == generation) {
                InterlockedExchange(&desired_music_handle, 0);
                desired_song = NULL;
                desired_song_len = 0;
                desired_song_format = 0;
            }
            LeaveCriticalSection(&music_state_lock);
        }
        if (music_idle_event) SetEvent(music_idle_event);
    }
    midi_all_notes_off();
    if (music_idle_event) SetEvent(music_idle_event);
    return 0;
}

void I_SetMusicVolume(int v)
{
    if (v > 15) return;
    if (v < 0) v = 0;
    if (v == snd_MusicVolume && music_playing && !music_paused) return;
    snd_MusicVolume = v;
    if (v == 0) {
        InterlockedExchange(&desired_paused, 1);
        midi_all_notes_off();
        music_paused = true;
        if (music_wake_event) SetEvent(music_wake_event);
        return;
    }
    InterlockedExchange(&desired_paused, 0);
    music_paused = false;
    if (music_wake_event) SetEvent(music_wake_event);
}

void I_InitMusic(void)
{
    if (!midi_lock_ready) { InitializeCriticalSection(&midi_lock); midi_lock_ready = true; }
    if (!music_state_lock_ready) { InitializeCriticalSection(&music_state_lock); music_state_lock_ready = true; }
    if (timeBeginPeriod(1) == TIMERR_NOERROR) midi_timer_ready = true;
    if (midiOutOpen(&midi_device, MIDI_MAPPER, 0, 0, CALLBACK_NULL) != MMSYSERR_NOERROR) {
        midi_device = NULL;
        I_Log("Music: no Windows MIDI output device available\n");
    }
    if (!music_wake_event) music_wake_event = CreateEventA(NULL, FALSE, FALSE, NULL);
    if (!music_idle_event) music_idle_event = CreateEventA(NULL, TRUE, TRUE, NULL);
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
    if (music_idle_event) {
        CloseHandle(music_idle_event);
        music_idle_event = NULL;
    }
    if (midi_device) { midiOutReset(midi_device); midiOutClose(midi_device); midi_device = NULL; }
    if (midi_timer_ready) { timeEndPeriod(1); midi_timer_ready = false; }
    if (music_state_lock_ready) { DeleteCriticalSection(&music_state_lock); music_state_lock_ready = false; }
    if (midi_lock_ready) { DeleteCriticalSection(&midi_lock); midi_lock_ready = false; }
}

void I_PauseSong(int h)
{
    (void)h;
    InterlockedExchange(&desired_paused, 1);
    music_paused = true;
    if (music_wake_event) SetEvent(music_wake_event);
}

void I_ResumeSong(int h)
{
    (void)h;
    if (snd_MusicVolume > 0) {
        InterlockedExchange(&desired_paused, 0);
        music_paused = false;
        if (music_wake_event) SetEvent(music_wake_event);
    }
}

int I_RegisterSong(void *data)
{
    unsigned short slen, sstart;
    int midi_len;
    if (!data) return 0;
    if (memcmp(data, "MUS\x1a", 4) == 0) {
        slen = *(unsigned short *)((char *)data + 4);
        sstart = *(unsigned short *)((char *)data + 6);
        if ((int)slen + sstart < sstart) return 0;
        current_song_len = slen + sstart;
        current_song_format = MUSIC_FORMAT_MUS;
    } else if (memcmp(data, "MThd", 4) == 0 && midi_file_length((const unsigned char *)data, &midi_len)) {
        current_song_len = midi_len;
        current_song_format = MUSIC_FORMAT_MIDI;
    } else {
        return 0;
    }
    current_song_data = data;
    current_music_handle = next_music_handle++;
    return current_music_handle;
}

void I_PlaySong(int h, int looping)
{
    if (!h || !current_song_data || current_song_len <= 0) return;
    music_playing = true;
    music_looping = (looping != 0);
    EnterCriticalSection(&music_state_lock);
    InterlockedExchange(&desired_music_handle, h);
    InterlockedExchange(&desired_looping, (looping != 0));
    InterlockedExchange(&desired_paused, (snd_MusicVolume == 0));
    desired_song = (const unsigned char *)current_song_data;
    desired_song_len = current_song_len;
    desired_song_format = current_song_format;
    InterlockedIncrement(&music_generation);
    if (music_idle_event) ResetEvent(music_idle_event);
    LeaveCriticalSection(&music_state_lock);
    if (music_wake_event) SetEvent(music_wake_event);
}

void I_StopSong(int h)
{
    (void)h;
    EnterCriticalSection(&music_state_lock);
    InterlockedExchange(&desired_music_handle, 0);
    desired_song = NULL;
    desired_song_len = 0;
    desired_song_format = 0;
    InterlockedIncrement(&music_generation);
    LeaveCriticalSection(&music_state_lock);
    music_playing = false;
    music_looping = false;
    music_paused = false;
    if (music_wake_event) SetEvent(music_wake_event);
    if (music_idle_event && music_thread) WaitForSingleObject(music_idle_event, 1000);
}

void I_UnRegisterSong(int h)
{
    (void)h;
    if (music_idle_event && music_thread) WaitForSingleObject(music_idle_event, 1000);
    EnterCriticalSection(&music_state_lock);
    current_song_data = NULL;
    current_song_len = 0;
    current_song_format = 0;
    current_music_handle = 0;
    LeaveCriticalSection(&music_state_lock);
}
