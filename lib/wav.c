#include "lib/wav.h"
#include "drivers/sb16.h"
#include "fs/fs.h"
#include "lib/memory.h"
#include "lib/string.h"

static uint8_t *s_file; /* kept while the card plays from it */

static uint32_t rd32(const uint8_t *p) {
    return p[0] | (p[1] << 8) | (p[2] << 16) | ((uint32_t)p[3] << 24);
}
static uint16_t rd16(const uint8_t *p) {
    return (uint16_t)(p[0] | (p[1] << 8));
}

const char *wav_decode(uint8_t *f, uint32_t size, uint8_t **pcm, uint32_t *len,
                       uint32_t *rate) {
    if (size < 12 || memcmp(f, "RIFF", 4) || memcmp(f + 8, "WAVE", 4))
        return "not a WAV file";
    uint16_t fmt = 0, ch = 0, bits = 0;
    uint32_t sr = 0;
    uint8_t *data = NULL;
    uint32_t dlen = 0;
    for (uint32_t p = 12; p + 8 <= size;) {
        uint32_t clen = rd32(f + p + 4);
        uint8_t *body = f + p + 8;
        if (clen > size - p - 8)
            clen = size - p - 8; /* truncated file: use what is there */
        if (!memcmp(f + p, "fmt ", 4) && clen >= 16) {
            fmt = rd16(body);
            ch = rd16(body + 2);
            sr = rd32(body + 4);
            bits = rd16(body + 14);
        } else if (!memcmp(f + p, "data", 4)) {
            data = body;
            dlen = clen;
        }
        p += 8 + clen + (clen & 1); /* chunks are word aligned */
    }
    if (!sr || !data)
        return "missing fmt or data chunk";
    if (fmt != 1)
        return "compressed WAV (only PCM)";
    if ((bits != 8 && bits != 16) || ch < 1 || ch > 2)
        return "only 8/16-bit mono/stereo";

    uint32_t frame = (uint32_t)ch * (bits / 8), n = dlen / frame;
    for (uint32_t i = 0; i < n; i++) { /* in place: output <= input */
        const uint8_t *s = data + i * frame;
        int v; /* -128..127 */
        if (bits == 8)
            v = ch == 2 ? ((s[0] + s[1]) >> 1) - 128 : s[0] - 128;
        else {
            int l = (int16_t)rd16(s), r = ch == 2 ? (int16_t)rd16(s + 2) : l;
            v = ((l + r) / 2) >> 8;
        }
        data[i] = (uint8_t)(v + 128);
    }
    *pcm = data;
    *len = n;
    *rate = sr;
    return NULL;
}

void wav_stop(void) {
    sb16_stop();
    if (s_file)
        kfree(s_file);
    s_file = NULL;
}

bool wav_is_playing(void) { return sb16_is_playing(); }

const char *wav_play_file(const char *path, bool loop) {
    if (!sb16_detected())
        return "no Sound Blaster";
    fat_file_t f;
    if (!fs_open(path, &f))
        return "file not found";
    uint32_t size = f.entry.file_size;
    if (size > WAV_MAX_BYTES) {
        fs_close(&f);
        return "file too big (max 1 MB)";
    }
    wav_stop();
    uint8_t *buf = (uint8_t *)kmalloc(size ? size : 1);
    if (!buf) {
        fs_close(&f);
        return "out of memory";
    }
    int got = fs_read(&f, buf, (int)size);
    fs_close(&f);
    uint8_t *pcm;
    uint32_t len, rate;
    const char *err = got > 0
                          ? wav_decode(buf, (uint32_t)got, &pcm, &len, &rate)
                          : "read error";
    if (err) {
        kfree(buf);
        return err;
    }
    s_file = buf;
    sb16_play_pcm(pcm, len, rate, false);
    sb16_set_loop(loop);
    return sb16_is_playing() ? NULL : "card did not start";
}
