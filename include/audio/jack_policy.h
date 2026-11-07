#pragma once

/* Service policy over the kernel's effective endpoint snapshot. There is no
 * hardware access here: unknown presence is never interpreted as unplugged. */
#include "abi/savanxp_abi.h"

struct savanxp_jack_policy {
    uint64_t device_id;
    uint64_t fallback_id;
    uint64_t attempted_id;
    uint32_t enabled;
};

static inline int savanxp_jack_policy_enable(struct savanxp_jack_policy* policy,
    const struct savanxp_audio_endpoint* endpoints, uint32_t count) {
    uint64_t device = 0, fallback = 0;
    uint32_t i;
    int detectable = 0;
    for (i = 0; i < count; ++i) {
        if ((endpoints[i].flags & (SAVANXP_AUDIO_PLAYBACK|SAVANXP_AUDIO_SELECTED)) ==
            (SAVANXP_AUDIO_PLAYBACK|SAVANXP_AUDIO_SELECTED)) {
            if ((endpoints[i].flags & SAVANXP_AUDIO_DIGITAL) || !(endpoints[i].flags & SAVANXP_AUDIO_AVAILABLE))
                return -SX_ENODEV;
            device = endpoints[i].device_id;
            if (!(endpoints[i].flags & SAVANXP_AUDIO_HEADPHONES)) fallback = endpoints[i].id;
            break;
        }
    }
    if (!device) return -SX_ENODEV;
    for (i = 0; i < count; ++i) {
        const struct savanxp_audio_endpoint* ep = &endpoints[i];
        if (ep->device_id != device || (ep->flags & (SAVANXP_AUDIO_PLAYBACK|SAVANXP_AUDIO_AVAILABLE)) !=
            (SAVANXP_AUDIO_PLAYBACK|SAVANXP_AUDIO_AVAILABLE) || (ep->flags & SAVANXP_AUDIO_DIGITAL)) continue;
        if (ep->flags & SAVANXP_AUDIO_HEADPHONES) {
            if (ep->flags & SAVANXP_AUDIO_PRESENCE_CAPABLE) detectable = 1;
        } else if (!fallback) fallback = ep->id;
    }
    if (!detectable || !fallback) return -SX_ENODEV;
    policy->device_id = device; policy->fallback_id = fallback;
    policy->attempted_id = 0; policy->enabled = 1;
    return 0;
}

/* A nonzero target is attempted once per plug-state transition. A failed
 * route change therefore cannot produce a busy retry loop. An explicit manual
 * selection disables the policy in the service before the next decision. */
static inline uint64_t savanxp_jack_policy_target(struct savanxp_jack_policy* policy,
    const struct savanxp_audio_endpoint* endpoints, uint32_t count) {
    uint64_t selected = 0, desired = 0;
    uint32_t i;
    int unknown = 0, fallback_available = 0;
    if (!policy->enabled) return 0;
    for (i = 0; i < count; ++i) {
        const struct savanxp_audio_endpoint* ep = &endpoints[i];
        if (ep->device_id != policy->device_id || !(ep->flags & SAVANXP_AUDIO_PLAYBACK) ||
            !(ep->flags & SAVANXP_AUDIO_AVAILABLE) || (ep->flags & SAVANXP_AUDIO_DIGITAL)) continue;
        if (ep->flags & SAVANXP_AUDIO_SELECTED) selected = ep->id;
        if (ep->id == policy->fallback_id) fallback_available = 1;
        if (!(ep->flags & SAVANXP_AUDIO_HEADPHONES) || !(ep->flags & SAVANXP_AUDIO_PRESENCE_CAPABLE)) continue;
        if (!(ep->flags & SAVANXP_AUDIO_PRESENCE_VALID)) unknown = 1;
        else if (!desired && (ep->flags & SAVANXP_AUDIO_CONNECTED)) desired = ep->id;
    }
    if (!selected) return 0;
    if (!desired) {
        if (unknown || !fallback_available) { policy->attempted_id = 0; return 0; }
        desired = policy->fallback_id;
    }
    if (desired == selected) { policy->attempted_id = 0; return 0; }
    if (desired == policy->attempted_id) return 0;
    policy->attempted_id = desired;
    return desired;
}
