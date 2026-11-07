#pragma once

#include <stddef.h>
#include <stdint.h>
#include "azaliareg.hpp"

// SavanXP's narrow replacement for OpenBSD bus_space/bus_dma and audio(9).
// The ported routines retain their upstream names to make review against the
// pinned vendor copy possible. They run under the kernel's existing lock.
namespace azalia_port {

struct Io {
    void* context;
    uint32_t (*read)(void*, uint16_t, unsigned);
    void (*write)(void*, uint16_t, uint32_t, unsigned);
    void (*delay)(void*, unsigned);
};
struct Dma { void* addr; uint64_t physical; };
struct azalia_t;
using nid_t = int;
constexpr unsigned kNodes = 256;
constexpr unsigned kConnections = 32;
enum class PinPresence : uint8_t { Unknown, Absent, Present, Unsupported };
struct widget_t {
    int nid = 0, type = 0, enable = 0, selected = -1, nconnections = 0;
    uint32_t widgetcap = 0, inamp_cap = 0, outamp_cap = 0;
    int connections[kConnections] = {};
    const char* name = "widget";
    struct {
        struct { uint32_t encodings = 0, bits_rates = 0; } audio;
        struct {
            uint32_t config = 0, cap = 0;
            PinPresence presence = PinPresence::Unknown;
            unsigned sequence = 0, association = 0, color = 0, device = 0;
        } pin;
    } d;
};
struct codec_t {
    azalia_t* az = nullptr;
    int address = 0, audiofunc = -1, wstart = 0, wend = 0;
    uint32_t vid = 0, qrks = 0;
    widget_t w[kNodes] = {};
};
struct azalia_t {
    Io io = {};
    Dma corb_dma = {}, rirb_dma = {};
    unsigned corb_entries = 0, rirb_entries = 0, rirb_rp = 0;
    uint8_t corbsize = 0, rirbsize = 0;
    // SavanXP polls and disables unsolicited responses; no timeout queue.
};
struct stream_t {
    azalia_t* az;
    codec_t* codec;
    int regbase, number, converter;
    Dma bdlist, buffer;
    uint32_t bufsize, blk;
    uint16_t fmt;
};

int azalia_reset(azalia_t*);
int azalia_init_rings(azalia_t*);
int azalia_set_command(azalia_t*, int, nid_t, uint32_t, uint32_t);
int azalia_get_response(azalia_t*, uint32_t*);
int azalia_comresp(const codec_t*, nid_t, uint32_t, uint32_t, uint32_t*);
int azalia_widget_init_audio(widget_t*, const codec_t*);
int azalia_widget_init_pin(widget_t*, const codec_t*);
int azalia_widget_enabled(const codec_t*, nid_t);
int azalia_stream_reset(stream_t*);
int azalia_stream_start(stream_t*);
int azalia_codec_connect_stream(stream_t*);

// Codec initialization and bounded analog routing/mixer setup.
struct Path { int nodes[10] = {}, inputs[10] = {}; unsigned count = 0; };
int probe_codec(codec_t*);
bool output_path(codec_t*, int pin, Path&);
bool find_output(codec_t*, Path&);
// Configuration is transactional: update current only after success. A failed
// rollback clears current, so callers cannot report a route that is unusable.
int switch_output(codec_t*, Path& current, const Path& next);
int configure_path(codec_t*, const Path&);
int read_connections(widget_t*, const codec_t*);
bool pin_detectable(const widget_t&);
PinPresence pin_presence(codec_t*, int pin);

} // namespace azalia_port
