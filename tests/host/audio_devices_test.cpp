#include <cstdio>
#include <cstdlib>
#include "kernel/audio.hpp"
static void check(bool ok, const char* why) {
    if (!ok) { std::fprintf(stderr,"audio-devices-test: FAIL %s\n",why); std::exit(1); }
}
struct Hardware {
    uint64_t id;
    bool available = true, input = true, fail_start = false;
    unsigned starts = 0, stops = 0, input_starts = 0, input_stops = 0, writes = 0, reads = 0;
    uint32_t rate = 48000;
    audio::Backend backend;
    Hardware(uint64_t identity) : id(identity) {
        backend = {
            .ready = [](void* p) { return static_cast<Hardware*>(p)->available; },
            .get_info = [](void* p, savanxp_audio_info& i) {
                i = {static_cast<Hardware*>(p)->rate,2,16,4,4096,16384,SAVANXP_AUDIO_BACKEND_HDA,0}; return true;
            },
            .configure = [](void* p) { auto& h = *static_cast<Hardware*>(p); ++h.starts; return !h.fail_start; },
            .submit_period = [](void* p, uint64_t, uint32_t) { ++static_cast<Hardware*>(p)->writes; return 0; },
            .stop = [](void* p) { ++static_cast<Hardware*>(p)->stops; },
            .capture_ready = [](void* p) { const auto& h = *static_cast<Hardware*>(p); return h.available && h.input; },
            .capture_configure = [](void* p) { auto& h = *static_cast<Hardware*>(p); ++h.input_starts; return !h.fail_start; },
            .capture_read_period = [](void* p, void*, uint32_t) { ++static_cast<Hardware*>(p)->reads; return 0; },
            .capture_stop = [](void* p) { ++static_cast<Hardware*>(p)->input_stops; },
            .endpoint = [](void* p, uint32_t index, savanxp_audio_endpoint& i) {
                const auto& h = *static_cast<Hardware*>(p);
                if (index > 1 || (index && !h.input)) return false;
                i = {}; i.device_id = h.id; i.id = h.id+index+1;
                i.flags = (index ? SAVANXP_AUDIO_CAPTURE : SAVANXP_AUDIO_PLAYBACK)|SAVANXP_AUDIO_SELECTED;
                if (h.available) i.flags |= SAVANXP_AUDIO_AVAILABLE;
                return true;
            },
            .capture_available = [](void*) { return 4096; },
            .context = this,
        };
    }
};
static uint64_t selected(uint32_t direction) {
    savanxp_audio_endpoint e; uint64_t result = 0;
    for (uint32_t i = 0; audio::endpoint(i,e); ++i) {
        if ((e.flags&(direction|SAVANXP_AUDIO_SELECTED)) != (direction|SAVANXP_AUDIO_SELECTED)) continue;
        check(!result,"each direction has only one effective default"); result = e.id;
    }
    return result;
}
Hardware first(0x300000000010000), second(0x300000000010200), third(0x300000000020000);
unsigned probes = 0;
const audio::Backend& driver_first() { return first.backend; }
const audio::Backend* driver_extra(uint32_t i) { return i == 1 ? &second.backend : nullptr; }
const audio::Backend& driver_third() { return third.backend; }
bool probe_ok() { ++probes; return true; }
bool probe_absent() { ++probes; return false; }
int main() {
    const audio::Driver high = {"high",100,probe_ok,driver_first,driver_extra};
    const audio::Driver low = {"low",50,probe_ok,driver_third};
    const audio::Driver absent = {"absent",200,probe_absent,driver_first};
    check(audio::register_driver(low) && audio::register_driver(high) && audio::register_driver(absent),"register independent drivers");
    check(audio::bind_best() == &high && probes == 3,"probe every driver by priority, including lower-priority devices");
    savanxp_audio_endpoint end;
    check(audio::endpoint(5,end) && !audio::endpoint(6,end),"catalog includes both instances and the other driver");
    check(selected(SAVANXP_AUDIO_PLAYBACK) == first.id+1 && selected(SAVANXP_AUDIO_CAPTURE) == first.id+2,"initial independent defaults honor priority");
    check(audio::configure() && audio::capture_configure(),"start both directions on the initial device");
    check(audio::select_endpoint(second.id+1,SAVANXP_AUDIO_PLAYBACK) == 0,"switch output to a second codec");
    check(first.stops == 1 && !first.input_stops && second.starts == 1 && !second.input_starts,"output switch leaves the input DMA untouched");
    check(selected(SAVANXP_AUDIO_CAPTURE) == first.id+2,"input default stays on the first codec");
    check(audio::select_endpoint(third.id+2,SAVANXP_AUDIO_CAPTURE) == 0,"switch input to another controller");
    check(first.input_stops == 1 && third.input_starts == 1 && !second.stops,"input switch leaves playback untouched");
    check(audio::submit_period(0,4) == 0 && audio::capture_read_period(nullptr,4) == 0 && second.writes == 1 && third.reads == 1,
          "PCM dispatch uses each direction's own instance context");
    check(selected(SAVANXP_AUDIO_PLAYBACK) == second.id+1 && selected(SAVANXP_AUDIO_CAPTURE) == third.id+2,"selected flags describe effective defaults, not idle local routes");
    const unsigned starts = second.starts, input_starts = third.input_starts;
    check(!audio::select_endpoint(second.id+1,SAVANXP_AUDIO_PLAYBACK) && !audio::select_endpoint(third.id+2,SAVANXP_AUDIO_CAPTURE) &&
          second.starts == starts && third.input_starts == input_starts,"effective selections are idempotent");
    first.fail_start = true;
    check(audio::select_endpoint(first.id+1,SAVANXP_AUDIO_PLAYBACK) == -SX_EIO && selected(SAVANXP_AUDIO_PLAYBACK) == second.id+1 && !second.stops,
          "failed new output preparation preserves the old DMA and default");
    check(audio::select_endpoint(first.id+2,SAVANXP_AUDIO_CAPTURE) == -SX_EIO && selected(SAVANXP_AUDIO_CAPTURE) == third.id+2 && !third.input_stops,
          "failed new input preparation preserves existing capture");
    first.fail_start = false; first.available = false;
    check(audio::select_endpoint(first.id+1,SAVANXP_AUDIO_PLAYBACK) == -SX_ENODEV,"unavailable endpoint remains visible and cannot become default");
    check(audio::select_endpoint(second.id+1,SAVANXP_AUDIO_CAPTURE) == -SX_ENOENT &&
          audio::select_endpoint(0x123,SAVANXP_AUDIO_PLAYBACK) == -SX_ENOENT && audio::select_endpoint(first.id+1,3) == -SX_EINVAL,
          "unknown ids and wrong directions cannot affect another device");
    first.available = true; first.rate = 44100;
    check(audio::select_endpoint(first.id+1,SAVANXP_AUDIO_PLAYBACK) == -SX_ENOSYS && selected(SAVANXP_AUDIO_PLAYBACK) == second.id+1,
          "an active format mismatch is refused rather than playing at the wrong rate");
    audio::stop();
    check(second.stops == 1 && !third.input_stops,"closing playback stops only its selected backend");
    audio::capture_stop();
    check(third.input_stops == 1,"closing capture stops its independently selected backend");
    auto capture_only = first.backend;
    capture_only.ready = [](void*) { return false; };
    capture_only.endpoint = [](void* p, uint32_t index, savanxp_audio_endpoint& e) {
        if (index) return false;
        const auto& h = *static_cast<Hardware*>(p);
        e = {}; e.id = h.id+2; e.device_id = h.id;
        e.flags = SAVANXP_AUDIO_CAPTURE|SAVANXP_AUDIO_AVAILABLE|SAVANXP_AUDIO_SELECTED;
        return true;
    };
    audio::set_backend(capture_only);
    check(!audio::ready() && audio::capture_ready() && !selected(SAVANXP_AUDIO_PLAYBACK) &&
          selected(SAVANXP_AUDIO_CAPTURE) == first.id+2,"a capture-only device does not require a playback default");
    std::puts("audio-devices-test: PASS (all drivers, instances, independent defaults, DMA and failed transitions)");
}
