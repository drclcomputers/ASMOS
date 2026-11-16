#include "os/prefs.h"

#include "fs/fs.h"
#include "lib/mem.h"
#include "lib/string.h"

#define PREFS_DIR "/PREFS"
#define PREFS_MAX 8192

static char s_buf[PREFS_MAX]; /* file text, '\0'-terminated */
static int s_len;
static char s_app[9]; /* app whose file s_buf holds */
static bool s_batch;  /* inside begin/end */

/* "/PREFS/ASMDRAW.INI": first 8 name characters, upper case, safe ones */
static void file_of(const char *app, char *out, int sz) {
    char n[9];
    int k = 0;
    for (const char *p = app; *p && k < 8; p++) {
        char c = (char)toupper((unsigned char)*p);
        if ((c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_' ||
            c == '-')
            n[k++] = c;
    }
    if (!k)
        n[k++] = 'X';
    n[k] = 0;
    snprintf(out, sz, PREFS_DIR "/%s.INI", n);
}

static void load(const char *app) {
    char path[32];
    fat_file_t f;
    file_of(app, path, sizeof(path));
    strncpy(s_app, app, sizeof(s_app) - 1);
    s_app[sizeof(s_app) - 1] = 0;
    s_len = 0;
    if (fs_open(path, &f)) {
        int n = fs_read(&f, s_buf, PREFS_MAX - 1);
        fs_close(&f);
        s_len = n > 0 ? n : 0;
    }
    s_buf[s_len] = 0;
}

static bool save(void) {
    char path[32];
    fat_file_t f;
    dir_entry_t de;
    file_of(s_app, path, sizeof(path));
    if (!fs_find(PREFS_DIR, &de)) {
        if (!fs_mkdir(PREFS_DIR))
            return false;
        fs_set_hidden(PREFS_DIR, true); /* settings, not documents */
    }
    if (fs_find(path, &de))
        fs_delete(path);
    if (!fs_create(path, &f))
        return false;
    bool ok = fs_write(&f, s_buf, s_len) == s_len;
    fs_close(&f);
    return ok;
}

/* start of the "key=" line, or NULL; *vlen = value length */
static char *find_key(const char *key, int *vlen) {
    int kl = (int)strlen(key);
    char *p = s_buf;
    while (*p) {
        char *e = p;
        while (*e && *e != '\n')
            e++;
        if (e - p > kl && p[kl] == '=' && !strncmp(p, key, kl)) {
            *vlen = (int)(e - p) - kl - 1;
            if (*vlen > 0 && p[kl + 1 + *vlen - 1] == '\r')
                (*vlen)--;
            return p;
        }
        p = *e ? e + 1 : e;
    }
    return NULL;
}

static bool key_ok(const char *key) {
    if (!key || !key[0] || strlen(key) > 23)
        return false;
    for (const char *p = key; *p; p++)
        if (*p == '=' || *p == '\n' || *p == '\r')
            return false;
    return true;
}

bool app_prefs_get(const char *app, const char *key, char *out, int sz) {
    if (!app || !key_ok(key) || !out || sz <= 0)
        return false;
    if (!s_batch)
        load(app);
    int vl;
    char *p = find_key(key, &vl);
    if (!p)
        return false;
    if (vl > sz - 1)
        vl = sz - 1;
    memcpy(out, p + strlen(key) + 1, vl);
    out[vl] = 0;
    return true;
}

int app_prefs_get_int(const char *app, const char *key, int def) {
    char b[16];
    if (!app_prefs_get(app, key, b, sizeof(b)))
        return def;
    const char *p = b;
    bool neg = *p == '-';
    if (neg)
        p++;
    if (*p < '0' || *p > '9')
        return def;
    int v = 0;
    while (*p >= '0' && *p <= '9')
        v = v * 10 + (*p++ - '0');
    return neg ? -v : v;
}

static bool put(const char *key, const char *value) {
    int vl;
    char *p = find_key(key, &vl);
    if (p) { /* drop the old line */
        char *e = p;
        while (*e && *e != '\n')
            e++;
        if (*e)
            e++;
        memmove(p, e, (size_t)(s_buf + s_len - e) + 1);
        s_len -= (int)(e - p);
    }
    if (!value[0]) /* empty: the key goes away */
        return true;
    int need = (int)strlen(key) + 1 + (int)strlen(value) + 1;
    if (s_len + need >= PREFS_MAX)
        return false;
    s_len += snprintf(s_buf + s_len, PREFS_MAX - s_len, "%s=%s\n", key, value);
    return true;
}

bool app_prefs_set(const char *app, const char *key, const char *value) {
    if (!app || !key_ok(key) || !value || strlen(value) > FS_PATH_MAX)
        return false;
    for (const char *v = value; *v; v++)
        if (*v == '\n' || *v == '\r')
            return false;
    if (s_batch) {
        if (strncmp(app, s_app, 8))
            return false; /* begin() was for another app */
        return put(key, value);
    }
    load(app);
    return put(key, value) && save();
}

bool app_prefs_set_int(const char *app, const char *key, int value) {
    char b[16];
    snprintf(b, sizeof(b), "%d", value);
    return app_prefs_set(app, key, b);
}

void app_prefs_begin(const char *app) {
    if (!app)
        return;
    load(app);
    s_batch = true;
}

bool app_prefs_end(void) {
    if (!s_batch)
        return false;
    s_batch = false;
    return save();
}
