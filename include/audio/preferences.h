#pragma once
#include <stdint.h>
#include <stddef.h>
#include <string.h>

/* Versioned, bounded service preferences. Text has no native structure layout
 * dependency; image and role bytes are hex encoded, including canonical NUL
 * padding, so application names cannot inject another record. */
#define SAVANXP_AUDIO_PREFERENCE_APPS 32u
#define SAVANXP_AUDIO_PREFERENCE_TEXT 6144u
struct savanxp_audio_application_key {
    uint32_t uid;
    char image[32];
    char role[32];
};
struct savanxp_audio_application_preference {
    struct savanxp_audio_application_key key;
    uint32_t volume, muted;
};
struct savanxp_audio_preferences {
    uint64_t output, input, fallback;
    uint32_t headphones, input_volume, input_muted, applications;
    struct savanxp_audio_application_preference app[SAVANXP_AUDIO_PREFERENCE_APPS];
};
static inline void savanxp_audio_preferences_init(struct savanxp_audio_preferences* p) {
    memset(p,0,sizeof(*p)); p->input_volume = 100;
}
static inline int savanxp_audio_key_equal(const struct savanxp_audio_application_key* a,
                                        const struct savanxp_audio_application_key* b) {
    return a->uid == b->uid && !memcmp(a->image,b->image,32) && !memcmp(a->role,b->role,32);
}
static inline int savanxp_audio_key_name(const char* text) {
    unsigned i;
    int ended = 0;
    if (!text[0]) return 0;
    for (i = 0; i < 32; ++i) {
        if (!text[i]) ended = 1;
        else if (ended) return 0;
    }
    return ended;
}
struct savanxp_audio_preference_cursor { const char* text; size_t size, at; };
static inline int savanxp_audio_preference_char(struct savanxp_audio_preference_cursor* c, char value) {
    if (c->at == c->size || c->text[c->at] != value) return 0;
    ++c->at; return 1;
}
static inline int savanxp_audio_preference_number(struct savanxp_audio_preference_cursor* c,
                                                  uint64_t maximum, unsigned base, unsigned digits,
                                                  uint64_t* value) {
    uint64_t result = 0;
    unsigned count = 0;
    while (c->at < c->size && (!digits || count < digits)) {
        const char ch = c->text[c->at];
        unsigned digit;
        if (ch >= '0' && ch <= '9') digit = ch-'0';
        else if (base == 16 && ch >= 'a' && ch <= 'f') digit = ch-'a'+10;
        else break;
        if (digit >= base || digit > maximum || result > (maximum-digit)/base) return 0;
        result = result*base+digit; ++c->at; ++count;
    }
    if (!count || (digits && count != digits)) return 0;
    *value = result; return 1;
}
static inline int savanxp_audio_preference_read_name(struct savanxp_audio_preference_cursor* c, char* out) {
    unsigned i;
    for (i = 0; i < 32; ++i) {
        uint64_t value;
        if (!savanxp_audio_preference_number(c,255,16,2,&value)) return 0;
        out[i] = (char)value;
    }
    return savanxp_audio_key_name(out);
}
static inline int savanxp_audio_preference_space(char ch) {
    return ch == ' ' || ch == '\t' || ch == '\r' || ch == '\n';
}
static inline int savanxp_audio_master_decode(const char* text, size_t bytes, uint32_t* volume, uint32_t* muted) {
    struct savanxp_audio_preference_cursor c = {text,bytes,0};
    uint64_t v, m;
    if (!bytes || bytes > 15) return 0;
    while (c.at < c.size && savanxp_audio_preference_space(c.text[c.at])) ++c.at;
    if (!savanxp_audio_preference_number(&c,100,10,0,&v) || c.at == c.size ||
        !savanxp_audio_preference_space(c.text[c.at])) return 0;
    while (c.at < c.size && savanxp_audio_preference_space(c.text[c.at])) ++c.at;
    if (!savanxp_audio_preference_number(&c,1,10,0,&m)) return 0;
    while (c.at < c.size && savanxp_audio_preference_space(c.text[c.at])) ++c.at;
    if (c.at != c.size) return 0;
    *volume = (uint32_t)v; *muted = (uint32_t)m; return 1;
}
static inline int savanxp_audio_preferences_decode(struct savanxp_audio_preferences* p,
                                                    const char* text, size_t bytes) {
    static const char header[] = "SAVANXP_AUDIO_PREFS 1\nD ";
    struct savanxp_audio_preference_cursor c = {text,bytes,0};
    uint64_t number;
    unsigned i;
    savanxp_audio_preferences_init(p);
    if (bytes > SAVANXP_AUDIO_PREFERENCE_TEXT || bytes < sizeof(header)-1 ||
        memcmp(text,header,sizeof(header)-1)) return 0;
    c.at = sizeof(header)-1;
    if (!savanxp_audio_preference_number(&c,UINT64_MAX,16,16,&p->output) || !savanxp_audio_preference_char(&c,' ') ||
        !savanxp_audio_preference_number(&c,UINT64_MAX,16,16,&p->input) || !savanxp_audio_preference_char(&c,' ') ||
        !savanxp_audio_preference_number(&c,UINT64_MAX,16,16,&p->fallback) || !savanxp_audio_preference_char(&c,' ') ||
        !savanxp_audio_preference_number(&c,1,10,0,&number)) return 0;
    p->headphones = (uint32_t)number;
    if (!savanxp_audio_preference_char(&c,' ') || !savanxp_audio_preference_number(&c,100,10,0,&number)) return 0;
    p->input_volume = (uint32_t)number;
    if (!savanxp_audio_preference_char(&c,' ') || !savanxp_audio_preference_number(&c,1,10,0,&number) ||
        !savanxp_audio_preference_char(&c,'\n')) return 0;
    p->input_muted = (uint32_t)number;
    while (c.at < c.size) {
        struct savanxp_audio_application_preference* a;
        if (p->applications == SAVANXP_AUDIO_PREFERENCE_APPS || !savanxp_audio_preference_char(&c,'A') ||
            !savanxp_audio_preference_char(&c,' ')) return 0;
        a = &p->app[p->applications];
        if (!savanxp_audio_preference_number(&c,UINT32_MAX,10,0,&number) || !savanxp_audio_preference_char(&c,' ')) return 0;
        a->key.uid = (uint32_t)number;
        if (!savanxp_audio_preference_number(&c,100,10,0,&number) || !savanxp_audio_preference_char(&c,' ')) return 0;
        a->volume = (uint32_t)number;
        if (!savanxp_audio_preference_number(&c,1,10,0,&number) || !savanxp_audio_preference_char(&c,' ')) return 0;
        a->muted = (uint32_t)number;
        if (!savanxp_audio_preference_read_name(&c,a->key.image) || !savanxp_audio_preference_char(&c,' ') ||
            !savanxp_audio_preference_read_name(&c,a->key.role) || !savanxp_audio_preference_char(&c,'\n')) return 0;
        for (i = 0; i < p->applications; ++i) if (savanxp_audio_key_equal(&p->app[i].key,&a->key)) return 0;
        ++p->applications;
    }
    return 1;
}
struct savanxp_audio_preference_output { char* text; size_t size, used; int failed; };
static inline void savanxp_audio_preference_put(struct savanxp_audio_preference_output* out, char ch) {
    if (out->used == out->size) { out->failed = 1; return; }
    out->text[out->used++] = ch;
}
static inline void savanxp_audio_preference_write_number(struct savanxp_audio_preference_output* out,
                                                         uint64_t value, unsigned base, unsigned digits) {
    static const char hex[] = "0123456789abcdef";
    char buffer[20]; unsigned used = 0;
    do { buffer[used++] = hex[value%base]; value /= base; } while (value);
    while (used < digits) buffer[used++] = '0';
    while (used) savanxp_audio_preference_put(out,buffer[--used]);
}
static inline void savanxp_audio_preference_write_name(struct savanxp_audio_preference_output* out, const char* name) {
    unsigned i;
    for (i = 0; i < 32; ++i) savanxp_audio_preference_write_number(out,(unsigned char)name[i],16,2);
}
static inline size_t savanxp_audio_preferences_encode(const struct savanxp_audio_preferences* p, char* text, size_t capacity) {
    static const char header[] = "SAVANXP_AUDIO_PREFS 1\nD ";
    struct savanxp_audio_preference_output out = {text,capacity,0,0};
    unsigned i;
    if (p->applications > SAVANXP_AUDIO_PREFERENCE_APPS || p->headphones > 1 || p->input_volume > 100 || p->input_muted > 1) return 0;
    for (i = 0; i < sizeof(header)-1; ++i) savanxp_audio_preference_put(&out,header[i]);
    savanxp_audio_preference_write_number(&out,p->output,16,16); savanxp_audio_preference_put(&out,' ');
    savanxp_audio_preference_write_number(&out,p->input,16,16); savanxp_audio_preference_put(&out,' ');
    savanxp_audio_preference_write_number(&out,p->fallback,16,16); savanxp_audio_preference_put(&out,' ');
    savanxp_audio_preference_write_number(&out,p->headphones,10,0); savanxp_audio_preference_put(&out,' ');
    savanxp_audio_preference_write_number(&out,p->input_volume,10,0); savanxp_audio_preference_put(&out,' ');
    savanxp_audio_preference_write_number(&out,p->input_muted,10,0); savanxp_audio_preference_put(&out,'\n');
    for (i = 0; i < p->applications; ++i) {
        const struct savanxp_audio_application_preference* a = &p->app[i];
        unsigned previous;
        if (!savanxp_audio_key_name(a->key.image) || !savanxp_audio_key_name(a->key.role) || a->volume > 100 || a->muted > 1) return 0;
        for (previous = 0; previous < i; ++previous) if (savanxp_audio_key_equal(&p->app[previous].key,&a->key)) return 0;
        savanxp_audio_preference_put(&out,'A'); savanxp_audio_preference_put(&out,' ');
        savanxp_audio_preference_write_number(&out,a->key.uid,10,0); savanxp_audio_preference_put(&out,' ');
        savanxp_audio_preference_write_number(&out,a->volume,10,0); savanxp_audio_preference_put(&out,' ');
        savanxp_audio_preference_write_number(&out,a->muted,10,0); savanxp_audio_preference_put(&out,' ');
        savanxp_audio_preference_write_name(&out,a->key.image); savanxp_audio_preference_put(&out,' ');
        savanxp_audio_preference_write_name(&out,a->key.role); savanxp_audio_preference_put(&out,'\n');
    }
    return out.failed ? 0 : out.used;
}
