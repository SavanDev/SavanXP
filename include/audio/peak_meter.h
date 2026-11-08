#pragma once
#include <stdint.h>

/* Held stereo S16 peaks. A fixed measurement window bounds how long a loud
 * sample is retained; freshness is measured from the latest packet, so reads
 * between adjacent windows cannot fabricate silence on a continuous stream. */
struct savanxp_audio_peak_meter {
    uint64_t window_ns, sample_ns;
    uint16_t left, right;
};
static inline void savanxp_audio_peak_reset(struct savanxp_audio_peak_meter* meter) {
    meter->window_ns = meter->sample_ns = 0;
    meter->left = meter->right = 0;
}
static inline void savanxp_audio_peak_observe(struct savanxp_audio_peak_meter* meter,
                                              const int16_t* samples, uint32_t frames, uint64_t now) {
    uint32_t i;
    if (!frames) return;
    if (!meter->sample_ns || now < meter->window_ns || now-meter->window_ns >= 100000000) {
        meter->left = meter->right = 0; meter->window_ns = now;
    }
    for (i = 0; i < frames; ++i) {
        int32_t left = samples[i*2], right = samples[i*2+1];
        uint16_t l = left < 0 ? -left : left, r = right < 0 ? -right : right;
        if (l > meter->left) meter->left = l;
        if (r > meter->right) meter->right = r;
    }
    meter->sample_ns = now;
}
static inline void savanxp_audio_peak_read(const struct savanxp_audio_peak_meter* meter,
                                           uint64_t now, uint16_t* left, uint16_t* right) {
    const int fresh = meter->sample_ns && now >= meter->sample_ns && now-meter->sample_ns < 100000000;
    *left = fresh ? meter->left : 0; *right = fresh ? meter->right : 0;
}
