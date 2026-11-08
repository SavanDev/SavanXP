#include <cstdio>
#include <cstdlib>
#include "audio/peak_meter.h"
static void check(bool ok, const char* why) {
    if (!ok) { std::fprintf(stderr,"audio-peak-meter-test: FAIL %s\n",why); std::exit(1); }
}
int main() {
    savanxp_audio_peak_meter meter = {};
    uint16_t left, right;
    const int16_t full[] = {-32768,6000,12000,-16000}, silence[] = {0,0};
    savanxp_audio_peak_observe(&meter,full,2,1000000);
    savanxp_audio_peak_read(&meter,2000000,&left,&right);
    check(left == 32768 && right == 16000,"full-scale negative PCM and independent stereo peaks");
    savanxp_audio_peak_observe(&meter,full,2,95000000);
    savanxp_audio_peak_read(&meter,110000000,&left,&right);
    check(left == 32768 && right == 16000,"a fresh packet preserves held peaks past the window boundary");
    savanxp_audio_peak_read(&meter,111000000,&left,&right);
    check(left == 32768 && right == 16000,"reading a level never consumes or resets another reader's hold");
    savanxp_audio_peak_observe(&meter,silence,1,120000000);
    savanxp_audio_peak_read(&meter,121000000,&left,&right);
    check(!left && !right,"silence in the next measurement window clears an old peak");
    savanxp_audio_peak_observe(&meter,full,2,130000000);
    savanxp_audio_peak_read(&meter,230000000,&left,&right);
    check(!left && !right,"a stream with no fresh samples expires after 100 ms");
    savanxp_audio_peak_read(&meter,129000000,&left,&right);
    check(!left && !right,"a reversed clock cannot expose stale peak data");
    savanxp_audio_peak_reset(&meter);
    savanxp_audio_peak_read(&meter,131000000,&left,&right);
    check(!left && !right,"mute/gain changes clear held state immediately");
    std::puts("audio-peak-meter-test: PASS (window boundary, freshness, stereo, silence and independent readers)");
}
