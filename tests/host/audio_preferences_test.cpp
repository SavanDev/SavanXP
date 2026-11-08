#include <cstdio>
#include <cstdlib>
#include <string>
#include <cstring>
#include "audio/preferences.h"
static void check(bool ok, const char* why) {
    if (!ok) { std::fprintf(stderr,"audio-preferences-test: FAIL %s\n",why); std::exit(1); }
}
static void rejected(const std::string& text) {
    savanxp_audio_preferences parsed;
    if (savanxp_audio_preferences_decode(&parsed,text.data(),text.size())) {
        std::fprintf(stderr,"unexpectedly accepted %zu bytes: %s\n",text.size(),text.c_str());
        check(false,"reject malformed preference text");
    }
}
int main() {
    savanxp_audio_preferences p, parsed;
    savanxp_audio_preferences_init(&p);
    const char empty[] = "SAVANXP_AUDIO_PREFS 1\nD 0000000000000000 0000000000000000 0000000000000000 0 100 0\n";
    char text[SAVANXP_AUDIO_PREFERENCE_TEXT];
    size_t bytes = savanxp_audio_preferences_encode(&p,text,sizeof(text));
    check(bytes == sizeof(empty)-1 && !memcmp(text,empty,bytes),"default file format has an independent literal fixture");
    check(savanxp_audio_preferences_decode(&parsed,empty,sizeof(empty)-1) && parsed.input_volume == 100 && !parsed.applications,"parse literal defaults");
    p.output = 0x030000000b030014; p.input = 0x030000000b030018; p.fallback = p.output;
    p.headphones = 1; p.input_volume = 73; p.input_muted = 1; p.applications = 2;
    p.app[0] = {{1000,"doomgeneric","Doom music"},45,1};
    p.app[1] = {{1000,"sound.sxe","Sound recording test"},80,0};
    bytes = savanxp_audio_preferences_encode(&p,text,sizeof(text));
    check(bytes && savanxp_audio_preferences_decode(&parsed,text,bytes),"parse persisted devices, policy, capture and application state");
    check(parsed.output == p.output && parsed.input == p.input && parsed.fallback == p.fallback &&
          parsed.headphones == 1 && parsed.input_volume == 73 && parsed.input_muted == 1 && parsed.applications == 2,
          "route ids and levels retain every bit");
    check(savanxp_audio_key_equal(&p.app[0].key,&parsed.app[0].key) && parsed.app[0].volume == 45 && parsed.app[0].muted == 1 &&
          !savanxp_audio_key_equal(&p.app[0].key,&parsed.app[1].key),"application identity and controls survive serialization");
    auto changed = p.app[0].key; ++changed.uid;
    check(!savanxp_audio_key_equal(&changed,&p.app[0].key),"identical image/role under another uid is a different application key");
    const std::string valid(text,bytes);
    for (size_t i = 0; i < bytes; ++i) {
        // A complete prefix ending at a record boundary is a valid smaller file.
        if (i && valid[i-1] == '\n' && i > valid.find("D ")) continue;
        rejected(valid.substr(0,i));
    }
    rejected(valid+"garbage\n");
    auto bad = valid; bad[19] = '9'; rejected(bad);
    bad = valid; bad[23] = 'g'; rejected(bad);
    bad = valid; bad.replace(bad.find(" 73 1\n"),6," 101 1\n"); rejected(bad);
    bad = valid; bad.replace(bad.find("A 1000"),6,"A 4294967296"); rejected(bad);
    bad = valid; bad.replace(bad.find(" 45 1 "),6," 100000000000000000000 1 "); rejected(bad);
    const auto a = valid.find("A "), end = valid.find('\n',a)+1;
    rejected(valid+valid.substr(a,end-a));
    p.app[1] = p.app[0];
    check(!savanxp_audio_preferences_encode(&p,text,sizeof(text)),"refuse duplicate application identities on write");
    p.applications = 1;
    p.app[0].key.role[31] = 'x';
    check(!savanxp_audio_preferences_encode(&p,text,sizeof(text)),"refuse noncanonical name padding");
    savanxp_audio_preferences_init(&p); p.applications = SAVANXP_AUDIO_PREFERENCE_APPS;
    for (unsigned i = 0; i < p.applications; ++i) {
        p.app[i] = {{UINT32_MAX,"application.sxe","role"},100,1};
        std::snprintf(p.app[i].key.role,32,"role%u",i);
    }
    bytes = savanxp_audio_preferences_encode(&p,text,sizeof(text));
    check(bytes && bytes < sizeof(text) && savanxp_audio_preferences_decode(&parsed,text,bytes) && parsed.applications == 32,
          "full bounded table fits one helper pipe message");
    check(!savanxp_audio_preferences_encode(&p,text,bytes-1),"short output buffers never produce a successful truncated file");
    rejected(std::string(SAVANXP_AUDIO_PREFERENCE_TEXT+1,'0'));
    std::puts("audio-preferences-test: PASS (literal format, stable keys, maximum table, truncation, overflow and malformed input)");
}
