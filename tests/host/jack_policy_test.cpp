#include <cstdio>
#include <cstdlib>
#include "audio/jack_policy.h"
static void check(bool ok, const char* reason) {
    if (!ok) { std::fprintf(stderr,"jack-policy-test: FAIL %s\n",reason); std::exit(1); }
}
int main() {
    constexpr uint32_t output = SAVANXP_AUDIO_PLAYBACK | SAVANXP_AUDIO_AVAILABLE;
    constexpr uint32_t headphones = output | SAVANXP_AUDIO_HEADPHONES | SAVANXP_AUDIO_PRESENCE_CAPABLE;
    savanxp_audio_endpoint ep[3] = {};
    ep[0].id = 101; ep[0].device_id = 100; ep[0].flags = output | SAVANXP_AUDIO_SELECTED;
    ep[1].id = 102; ep[1].device_id = 100; ep[1].flags = headphones | SAVANXP_AUDIO_PRESENCE_VALID;
    ep[2].id = 201; ep[2].device_id = 200;
    ep[2].flags = headphones | SAVANXP_AUDIO_PRESENCE_VALID | SAVANXP_AUDIO_CONNECTED;
    savanxp_jack_policy policy = {};
    check(!savanxp_jack_policy_target(&policy,ep,3),"manual is the default even with headphones connected elsewhere");
    check(!savanxp_jack_policy_enable(&policy,ep,3),"enable for one device with detectable headphones and a fallback");
    check(!savanxp_jack_policy_target(&policy,ep,3),"ignore another controller's plugged connector");
    ep[1].flags |= SAVANXP_AUDIO_CONNECTED;
    check(savanxp_jack_policy_target(&policy,ep,3) == 102,"plug selects headphones on the chosen device");
    check(!savanxp_jack_policy_target(&policy,ep,3),"failed transition is not retried continuously");
    ep[0].flags &= ~SAVANXP_AUDIO_SELECTED; ep[1].flags |= SAVANXP_AUDIO_SELECTED;
    check(!savanxp_jack_policy_target(&policy,ep,3),"effective selection acknowledges a successful switch");
    ep[1].flags &= ~SAVANXP_AUDIO_PRESENCE_VALID;
    check(!savanxp_jack_policy_target(&policy,ep,3),"unknown detection never switches away from headphones");
    ep[1].flags = headphones | SAVANXP_AUDIO_PRESENCE_VALID | SAVANXP_AUDIO_SELECTED;
    check(savanxp_jack_policy_target(&policy,ep,3) == 101,"unplug returns to the previous manual output");
    check(!savanxp_jack_policy_target(&policy,ep,3),"failed fallback is also latched");
    ep[0].flags |= SAVANXP_AUDIO_SELECTED; ep[1].flags &= ~SAVANXP_AUDIO_SELECTED;
    check(!savanxp_jack_policy_target(&policy,ep,3),"restored rear output remains stable");
    ep[1].flags |= SAVANXP_AUDIO_CONNECTED;
    check(savanxp_jack_policy_target(&policy,ep,3) == 102,"a later plug event permits another attempt");
    policy.enabled = 0;
    check(!savanxp_jack_policy_target(&policy,ep,3),"a successful manual choice disables automatic switching");
    ep[1].flags = headphones; ep[0].flags |= SAVANXP_AUDIO_SELECTED;
    check(!savanxp_jack_policy_enable(&policy,ep,3) && !savanxp_jack_policy_target(&policy,ep,3),
          "enabling while presence is unknown does not fabricate an unplug");
    ep[1].flags |= SAVANXP_AUDIO_DIGITAL;
    check(savanxp_jack_policy_enable(&policy,ep,3) == -SX_ENODEV,"digital receivers never become analog jack policy targets");
    ep[1].flags = output;
    check(savanxp_jack_policy_enable(&policy,ep,3) == -SX_ENODEV,"unsupported detection cannot enable automation");
    ep[0].flags = output | SAVANXP_AUDIO_SELECTED | SAVANXP_AUDIO_DIGITAL;
    ep[1].flags = headphones;
    check(savanxp_jack_policy_enable(&policy,ep,3) == -SX_ENODEV,"a selected digital endpoint cannot become an analog fallback");
    ep[0].flags = SAVANXP_AUDIO_PLAYBACK | SAVANXP_AUDIO_SELECTED;
    check(savanxp_jack_policy_enable(&policy,ep,3) == -SX_ENODEV,"an unavailable selected route cannot enable automation");
    ep[1].flags = headphones | SAVANXP_AUDIO_SELECTED | SAVANXP_AUDIO_PRESENCE_VALID | SAVANXP_AUDIO_CONNECTED;
    ep[0].flags = output;
    check(!savanxp_jack_policy_enable(&policy,ep,3) && policy.fallback_id == 101,
          "enabling on headphones discovers a usable fallback");
    ep[0].flags = 0;
    ep[1].flags &= ~SAVANXP_AUDIO_CONNECTED;
    check(!savanxp_jack_policy_target(&policy,ep,3),"a missing fallback cannot be selected after unplug");
    ep[1].flags &= ~SAVANXP_AUDIO_SELECTED;
    check(!savanxp_jack_policy_target(&policy,ep,3),"failed rollback with no effective selection cannot invent one");
    check(savanxp_jack_policy_enable(&policy,ep,3) == -SX_ENODEV,"refuse automatic switching without a usable fallback");
    std::puts("jack-policy-test: PASS (manual, plug/unplug, unknown, isolation, failures)");
}
