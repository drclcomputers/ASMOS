/* One sound at a time, in any format a codec knows (lib/audio.h). */
#include "lib/audio.h"
#include "drivers/cdrom.h"
#include "drivers/opl2.h"
#include "drivers/sb16.h"
#include "fs/fs.h"
#include "fs/iso9660.h"
#include "lib/memory.h"
#include "lib/string.h"
#include "lib/time.h"
#include "lib/wav.h"

typedef struct {
    const char *name;
    const char *const *ext;
    bool (*probe)(const uint8_t *d, uint32_t n);
    /* takes `d` (kfree'd by close); fills info, length */
    const char *(*open)(uint8_t *d, uint32_t n);
    void (*play)(bool loop);
    void (*pause)(bool p);
    void (*stop)(void);
    void (*close)(void);
    bool (*busy)(void); /* still sounding */
    uint32_t (*pos_ms)(void);
    bool (*seek_ms)(uint32_t ms); /* NULL: can't */
    int (*level)(void);
} codec_t;

static uint8_t *s_data;
static uint32_t s_len_ms;
static char s_info[48], s_path[FS_PATH_SZ];
static const codec_t *s_codec;
static int s_state = AUDIO_EMPTY;
static bool s_loop, s_finished;

// WAV

static uint8_t *w_pcm;
static uint32_t w_len, w_rate;

static bool wav_probe(const uint8_t *d, uint32_t n) {
    return n >= 12 && !memcmp(d, "RIFF", 4) && !memcmp(d + 8, "WAVE", 4);
}

static const char *wav_open(uint8_t *d, uint32_t n) {
    if (!sb16_detected())
        return "WAV needs a Sound Blaster";
    /* the format, before wav_decode turns it into 8-bit mono */
    uint16_t ch = 0, bits = 0;
    for (uint32_t p = 12; p + 24 <= n;) {
        uint32_t cl = d[p + 4] | d[p + 5] << 8 | d[p + 6] << 16 |
                      (uint32_t)d[p + 7] << 24;
        if (!memcmp(d + p, "fmt ", 4)) {
            ch = (uint16_t)(d[p + 10] | d[p + 11] << 8);
            bits = (uint16_t)(d[p + 22] | d[p + 23] << 8);
            break;
        }
        if (cl > n)
            break;
        p += 8 + cl + (cl & 1);
    }
    const char *err = wav_decode(d, n, &w_pcm, &w_len, &w_rate);
    if (err)
        return err;
    snprintf(s_info, sizeof(s_info), "%u Hz, %d-bit %s", w_rate, bits,
             ch == 2 ? "stereo" : "mono");
    s_len_ms = w_len / w_rate * 1000 + w_len % w_rate * 1000 / w_rate;
    return NULL;
}

static void wav_play(bool loop) {
    sb16_play_pcm(w_pcm, w_len, w_rate, false);
    sb16_set_loop(loop);
}
static void wav_pause(bool p) { sb16_pause(p); }
static void wav_stop_(void) { sb16_stop(); }
static void wav_close(void) { sb16_stop(); }
static bool wav_busy(void) { return sb16_is_playing(); }
static uint32_t wav_pos(void) {
    uint32_t p = sb16_position();
    return p / w_rate * 1000 + p % w_rate * 1000 / w_rate;
}
static bool wav_seek(uint32_t ms) {
    sb16_seek(ms / 1000 * w_rate + ms % 1000 * w_rate / 1000);
    return true;
}
/* loudness around the play position: the largest swing from silence */
static int wav_level(void) {
    if (!sb16_is_playing() || sb16_paused())
        return 0;
    uint32_t p = sb16_position(), m = 0;
    for (uint32_t i = 0; i < 256 && p + i < w_len; i += 4) {
        int v = (int)w_pcm[p + i] - 128;
        uint32_t a = (uint32_t)(v < 0 ? -v : v);
        if (a > m)
            m = a;
    }
    return (int)(m * 2 > 255 ? 255 : m * 2);
}

// MIDI

static uint32_t m_pos, m_last; /* ms played, pit at the last update */

static bool mid_probe(const uint8_t *d, uint32_t n) {
    return n >= 14 && !memcmp(d, "MThd", 4);
}

static uint32_t rd32(const uint8_t *p) {
    return (uint32_t)p[0] << 24 | p[1] << 16 | p[2] << 8 | p[3];
}
static uint32_t vlq(const uint8_t **p, const uint8_t *end) {
    uint32_t v = 0;
    for (int i = 0; i < 4 && *p < end; i++) {
        uint8_t b = *(*p)++;
        v = v << 7 | (b & 0x7F);
        if (!(b & 0x80))
            break;
    }
    return v;
}

/* ticks at `tempo` (us a beat) in ms, without 64-bit maths */
static uint32_t ticks_ms(uint32_t ticks, uint32_t tempo, uint32_t ppq) {
    uint32_t beats = ticks / ppq, rest = ticks % ppq, tms = tempo / 1000;
    return beats * tms + beats * (tempo % 1000) / 1000 + rest * tms / ppq;
}

/* The length: every track walked for its last tick, and the tempo
 * changes (FF 51) turned into time. */
#define MAX_TEMPOS 128
static uint32_t midi_length_ms(const uint8_t *d, uint32_t n, int *ntracks) {
    uint32_t ppq = (uint32_t)(d[12] << 8 | d[13]);
    if (!ppq || ppq & 0x8000)
        return 0;
    static uint32_t t_tick[MAX_TEMPOS], t_val[MAX_TEMPOS];
    int nt = 0, tracks = 0;
    uint32_t last = 0;
    const uint8_t *p = d + 14, *end = d + n;
    while (p + 8 <= end && !memcmp(p, "MTrk", 4)) {
        uint32_t cl = rd32(p + 4);
        const uint8_t *q = p + 8, *te = q + cl > end ? end : q + cl;
        uint32_t tick = 0;
        uint8_t rs = 0;
        tracks++;
        while (q < te) {
            tick += vlq(&q, te);
            if (q >= te)
                break;
            uint8_t st = *q;
            if (st & 0x80)
                q++;
            else
                st = rs; /* running status */
            if (st == 0xFF) {
                if (q >= te)
                    break;
                uint8_t type = *q++;
                uint32_t len = vlq(&q, te);
                if (type == 0x51 && len == 3 && q + 3 <= te &&
                    nt < MAX_TEMPOS) {
                    t_tick[nt] = tick;
                    t_val[nt++] = (uint32_t)q[0] << 16 | q[1] << 8 | q[2];
                }
                q += len;
                if (type == 0x2F)
                    break;
            } else if (st == 0xF0 || st == 0xF7) {
                q += vlq(&q, te);
            } else {
                rs = st;
                uint8_t hi = st & 0xF0;
                q += (hi == 0xC0 || hi == 0xD0) ? 1 : 2;
            }
        }
        if (tick > last)
            last = tick;
        p = te;
    }
    /* sort the tempo changes by tick (insertion: there are few) */
    for (int i = 1; i < nt; i++)
        for (int k = i; k > 0 && t_tick[k] < t_tick[k - 1]; k--) {
            uint32_t a = t_tick[k], b = t_val[k];
            t_tick[k] = t_tick[k - 1], t_val[k] = t_val[k - 1];
            t_tick[k - 1] = a, t_val[k - 1] = b;
        }
    uint32_t ms = 0, at = 0, tempo = 500000;
    for (int i = 0; i <= nt; i++) {
        uint32_t upto = i < nt && t_tick[i] < last ? t_tick[i] : last;
        ms += ticks_ms(upto - at, tempo, ppq);
        at = upto;
        if (i < nt)
            tempo = t_val[i];
        if (upto == last)
            break;
    }
    if (ntracks)
        *ntracks = tracks;
    return ms;
}

static const char *mid_open(uint8_t *d, uint32_t n) {
    if (!opl2_detected())
        return "MIDI needs an AdLib or Sound Blaster (OPL2)";
    if (!midi_player_load(d, n))
        return "not a MIDI file this player knows";
    int tracks = 0;
    s_len_ms = midi_length_ms(d, n, &tracks);
    snprintf(s_info, sizeof(s_info), "type %d, %d track%s, FM synth", d[9],
             tracks, tracks == 1 ? "" : "s");
    return NULL;
}

static void mid_play(bool loop) {
    midi_player_play(loop);
    m_pos = 0;
    m_last = pit_ticks;
}
static void mid_pause(bool p) {
    (void)p;
    midi_player_pause(); /* toggles */
    m_last = pit_ticks;
}
static void mid_stop(void) { midi_player_stop(); }
static void mid_close(void) { midi_player_stop(); }
static bool mid_busy(void) { return midi_player_is_playing(); }
static uint32_t mid_pos(void) { return m_pos; }

// CD audio
/* A .cda file (Track01.cda) names a track; the drive plays it through its
 * audio cable to the sound card. */

static uint32_t c_start, c_len, c_at; /* sectors; c_at: last position seen */
static uint32_t c_seen;               /* pit_ticks then */
static bool c_loop, c_paused, c_on;   /* c_on: a play was started */
static int c_status;

#define CD_POLL_MS 250

static bool cda_probe(const uint8_t *d, uint32_t n) {
    return n >= 44 && !memcmp(d, "RIFF", 4) && !memcmp(d + 8, "CDDA", 4);
}

static uint32_t le32(const uint8_t *p) {
    return p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 |
           (uint32_t)p[3] << 24;
}

static const char *cda_open(uint8_t *d, uint32_t n) {
    (void)n;
    if (!cd_present())
        return "there is no CD drive";
    uint32_t serial = le32(d + 24);
    c_start = le32(d + 28);
    c_len = le32(d + 32);
    if (!iso_is_audio() || !iso_cda_check(serial, c_start, c_len))
        return "put its audio CD in the drive";
    c_on = c_paused = false;
    c_at = 0;
    s_len_ms = c_len / CD_FPS * 1000 + c_len % CD_FPS * 1000 / CD_FPS;
    snprintf(s_info, sizeof(s_info), "track %d, from the drive", d[22]);
    return NULL;
}

static void cda_from(uint32_t at) { /* play from `at` sectors in */
    c_at = at;
    c_seen = pit_ticks;
    c_on = cd_play(c_start + at, c_start + c_len);
    c_status = c_on ? CD_AUDIO_PLAYING : CD_AUDIO_ERROR;
    if (c_on && c_paused)
        cd_pause(true);
}

static void cda_play(bool loop) {
    c_loop = loop;
    c_paused = false;
    cda_from(0);
}
static void cda_pause(bool p) {
    c_paused = p;
    if (c_on)
        cd_pause(p);
    c_seen = pit_ticks;
}
static void cda_stop(void) {
    if (c_on)
        cd_stop_audio();
    c_on = c_paused = false;
    c_at = 0;
}

/* where the drive is: asked 4 times a second, counted on in between */
static void cda_poll(void) {
    if (!c_on || pit_ticks - c_seen < CD_POLL_MS)
        return;
    uint32_t lba = 0;
    c_status = cd_audio_status(&lba);
    c_seen = pit_ticks;
    if (c_status == CD_AUDIO_PLAYING || c_status == CD_AUDIO_PAUSED) {
        if (lba >= c_start && lba < c_start + c_len)
            c_at = lba - c_start;
    } else if (c_status == CD_AUDIO_DONE && c_loop) {
        cda_from(0); /* again */
    }
}

static bool cda_busy(void) {
    cda_poll();
    return c_on &&
           (c_status == CD_AUDIO_PLAYING || c_status == CD_AUDIO_PAUSED);
}
static uint32_t cda_pos(void) {
    uint32_t ms = c_at / CD_FPS * 1000 + c_at % CD_FPS * 1000 / CD_FPS;
    if (c_on && !c_paused && c_status == CD_AUDIO_PLAYING)
        ms += pit_ticks - c_seen; /* (since the last look) */
    return ms;
}
static bool cda_seek(uint32_t ms) {
    uint32_t at = ms / 1000 * CD_FPS + ms % 1000 * CD_FPS / 1000;
    if (at >= c_len)
        at = c_len ? c_len - 1 : 0;
    cda_from(at);
    return c_on;
}

// the codec table

static const char *const WAV_EXT[] = {"WAV", NULL};
static const char *const MID_EXT[] = {"MID", "MIDI", NULL};
static const char *const CDA_EXT[] = {"CDA", NULL};

static const codec_t CODECS[] = {
    {"WAV", WAV_EXT, wav_probe, wav_open, wav_play, wav_pause, wav_stop_,
     wav_close, wav_busy, wav_pos, wav_seek, wav_level},
    {"MIDI", MID_EXT, mid_probe, mid_open, mid_play, mid_pause, mid_stop,
     mid_close, mid_busy, mid_pos, NULL, NULL},
    {"CD audio", CDA_EXT, cda_probe, cda_open, cda_play, cda_pause, cda_stop,
     cda_stop, cda_busy, cda_pos, cda_seek, NULL},
};
#define NCODECS ((int)(sizeof(CODECS) / sizeof(CODECS[0])))

const char *const *audio_extensions(void) {
    static const char *all[8];
    int n = 0;
    for (int i = 0; i < NCODECS; i++)
        for (int k = 0; CODECS[i].ext[k] && n < 7; k++)
            all[n++] = CODECS[i].ext[k];
    all[n] = NULL;
    return all;
}

// the player

void audio_close(void) {
    if (s_codec)
        s_codec->close();
    s_codec = NULL;
    kfree(s_data);
    s_data = NULL;
    s_state = AUDIO_EMPTY;
    s_len_ms = 0;
    s_info[0] = s_path[0] = 0;
}

#define AUDIO_MAX_BYTES (1024u * 1024u)

const char *audio_open(const char *path) {
    audio_close();
    fat_file_t f;
    if (!fs_open(path, &f))
        return "file not found";
    uint32_t size = f.entry.file_size;
    if (!size || size > AUDIO_MAX_BYTES) {
        fs_close(&f);
        return size ? "file too big (1 MB at most)" : "empty file";
    }
    uint8_t *d = kmalloc(size);
    if (!d) {
        fs_close(&f);
        return "not enough memory for this file";
    }
    int got = fs_read(&f, d, (int)size);
    fs_close(&f);
    if (got <= 0) {
        kfree(d);
        return "read error";
    }
    for (int i = 0; i < NCODECS; i++)
        if (CODECS[i].probe(d, (uint32_t)got)) {
            const char *err = CODECS[i].open(d, (uint32_t)got);
            if (err) {
                kfree(d);
                s_info[0] = 0;
                return err;
            }
            s_codec = &CODECS[i];
            s_data = d;
            s_state = AUDIO_STOPPED;
            strncpy(s_path, path, sizeof(s_path) - 1);
            return NULL;
        }
    kfree(d);
    return "not a sound file this player knows";
}

void audio_play(void) {
    if (!s_codec)
        return;
    if (s_state == AUDIO_PAUSED) {
        s_codec->pause(false);
    } else {
        s_codec->play(s_loop);
        s_finished = false;
    }
    s_state = AUDIO_PLAYING;
}

void audio_pause(void) {
    if (s_state == AUDIO_PLAYING) {
        s_codec->pause(true);
        s_state = AUDIO_PAUSED;
    } else if (s_state == AUDIO_PAUSED) {
        audio_play();
    }
}

void audio_stop(void) {
    if (s_codec && s_state != AUDIO_EMPTY) {
        s_codec->stop();
        s_state = AUDIO_STOPPED;
        m_pos = 0;
    }
}

void audio_set_loop(bool loop) {
    s_loop = loop;
    if (s_codec && s_state == AUDIO_PLAYING && s_codec == &CODECS[0])
        sb16_set_loop(loop);
    else if (s_codec == &CODECS[2])
        c_loop = loop;
    else if (s_state == AUDIO_PLAYING)
        midi_player_set_loop(loop);
}

bool audio_can_seek(void) { return s_codec && s_codec->seek_ms; }

bool audio_seek_ms(uint32_t ms) {
    if (!audio_can_seek() || s_state == AUDIO_EMPTY)
        return false;
    if (s_state == AUDIO_STOPPED) { /* start there, paused */
        s_codec->play(s_loop);
        s_codec->pause(true);
        s_state = AUDIO_PAUSED;
        s_finished = false;
    }
    return s_codec->seek_ms(ms > s_len_ms ? s_len_ms : ms);
}

int audio_state(void) { return s_state; }
bool audio_finished(void) { /* once per ending */
    bool f = s_finished;
    s_finished = false;
    return f;
}
uint32_t audio_len_ms(void) { return s_len_ms; }
const char *audio_codec_name(void) { return s_codec ? s_codec->name : ""; }
const char *audio_info(void) { return s_info; }
const char *audio_file(void) { return s_path; }
int audio_level(void) {
    return s_codec && s_codec->level && s_state == AUDIO_PLAYING
               ? s_codec->level()
               : 0;
}

uint32_t audio_pos_ms(void) {
    if (!s_codec || s_state == AUDIO_STOPPED || s_state == AUDIO_EMPTY)
        return 0;
    uint32_t p = s_codec->pos_ms();
    if (s_len_ms && p > s_len_ms)
        p = s_loop ? p % s_len_ms : s_len_ms;
    return p;
}

void audio_update(void) {
    midi_player_update();
    if (s_state != AUDIO_PLAYING || !s_codec)
        return;
    if (s_codec == &CODECS[1]) { /* MIDI time runs while it plays */
        m_pos += pit_ticks - m_last;
        if (s_loop && s_len_ms && m_pos >= s_len_ms)
            m_pos -= s_len_ms;
    }
    m_last = pit_ticks;
    if (!s_codec->busy()) { /* ran out */
        s_state = AUDIO_STOPPED;
        s_finished = true;
        m_pos = 0;
    }
}
