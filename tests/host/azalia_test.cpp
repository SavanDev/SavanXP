#include <array>
#include <cstdio>
#include <cstdlib>
#include <map>
#include <memory>
#include <tuple>
#include <vector>
#include "azalia/azalia_port.hpp"
using namespace azalia_port;
namespace {
void check(bool value, const char* message) {
    if (!value) { std::fprintf(stderr, "azalia-test: FAIL %s\n", message); std::exit(1); }
}
struct Hardware {
    azalia_t az;
    std::array<uint32_t, 256> corb = {};
    std::array<rirb_entry_t, 256> rirb = {};
    std::map<unsigned, uint32_t> regs;
    std::map<std::tuple<unsigned,unsigned,unsigned>,uint32_t> replies;
    std::vector<std::tuple<unsigned,unsigned,unsigned>> commands;
    unsigned wp = 0;
    bool answer = true, saw_full_bdl = false;
    bdlist_entry_t bdl[32] = {};
    unsigned char pcm[32 * 4096] = {};
    Hardware(uint8_t sizes = 0x40) {
        az.io = {this, read, write, delay};
        az.corb_dma = {corb.data(), 0x10000};
        az.rirb_dma = {rirb.data(), 0x11000};
        regs[0x4e] = sizes; regs[0x5e] = sizes;
    }
    static uint32_t read(void* p, uint16_t r, unsigned) { return static_cast<Hardware*>(p)->regs[r]; }
    static void delay(void*, unsigned) {}
    static void write(void* p, uint16_t r, uint32_t v, unsigned) {
        auto& h = *static_cast<Hardware*>(p);
        if (r == 0x4e || r == 0x5e) { h.regs[r] = (h.regs[r] & 0xf0) | (v & 3); return; }
        if (r == 0x4a) { h.regs[r] = v & 0x8000; return; }
        if (r == 0x58 && (v & 0x8000)) { h.regs[r] = 0; h.wp = 0; return; }
        h.regs[r] = v;
        if (r == 0x48 && h.answer) {
            const uint32_t word = h.corb[v & 0xff];
            unsigned nid = (word >> 20) & 0xff, verb = (word >> 8) & 0xfff, payload = word & 0xff;
            if (((word >> 16) & 0xf) < 7) { verb &= 0xf00; payload = word & 0xffff; }
            h.commands.emplace_back(nid,verb,payload);
            const unsigned entries = h.az.rirb_entries;
            h.wp = (h.wp + 1) % entries;
            h.rirb[h.wp] = {h.replies[{nid,verb,payload}], word >> 28};
            h.regs[0x58] = h.wp;
        }
        if (r == 0x100 && (v & 2)) {
            h.saw_full_bdl = h.regs[0x10c] == 31 && h.regs[0x108] == sizeof(h.pcm);
            for (unsigned i = 0; i < 32; ++i)
                h.saw_full_bdl &= h.bdl[i].length == 4096 && h.bdl[i].flags == 0 &&
                    h.bdl[i].low == 0x20000 + i * 4096;
        }
    }
    bool sent(unsigned nid, unsigned verb, unsigned payload) const {
        for (auto x : commands) if (x == std::tuple{nid,verb,payload}) return true;
        return false;
    }
    void param(unsigned nid, unsigned id, uint32_t value) { replies[{nid,0xf00,id}] = value; }
};
void alc892(Hardware& h) {
    // Relevant nodes read from the host ALC892 Linux codec dump, 2026-11-07.
    // Rear green 0x14 -> mixer 0x0c -> DAC 0x02. 0x15 is disconnected despite
    // its firmware "Speaker" device type, and must not outrank that line-out.
    h.param(0,0,0x10ec0892); h.param(0,4,0x00010001); h.param(1,5,1);
    h.param(1,4,0x0002001a); h.param(1,0x0b,1); h.param(1,0x0a,0x00020040);
    h.param(2,9,0x41d); h.param(2,0x0b,1); h.param(2,0x0a,0x00020040);
    h.param(2,0x12,0x00034040);
    h.param(0x0c,9,0x20010b); h.param(0x0c,0x0d,0x80000000);
    h.param(0x0c,0x0e,2); h.replies[{0x0c,0xf02,0}] = 0x00000b02;
    h.param(0x14,9,0x40058d); h.param(0x14,0x0c,0x1003e);
    h.param(0x14,0x12,0x80000000); h.replies[{0x14,0xf1c,0}] = 0x01014010;
    h.param(0x14,0x0e,1); h.replies[{0x14,0xf02,0}] = 0x0c;
    h.param(0x15,9,0x40058d); h.param(0x15,0x0c,0x36);
    h.replies[{0x15,0xf1c,0}] = 0x411111f0;
    h.param(0x15,0x0e,1); h.replies[{0x15,0xf02,0}] = 0x0c;
}
}
int main() {
    for (unsigned size : {0x10,0x20,0x40}) {
        Hardware h(size);
        check(azalia_reset(&h.az) == 0, "controller reset handshake");
        check(azalia_init_rings(&h.az) == 0, "ring setup");
        check(h.regs[0x4e] == size + (size == 0x40 ? 2 : size == 0x20 ? 1 : 0), "CORB size lives in bits 1:0");
        codec_t c; c.az = &h.az;
        h.param(0,0,0x12345678);
        for (unsigned i = 0; i < 600; ++i) {
            uint32_t response = 0;
            check(azalia_comresp(&c,0,0xf00,0,&response) == 0 && response == 0x12345678,
                  "command/response survives ring wrapping at negotiated size");
        }
    }
    Hardware invalid(0); check(azalia_init_rings(&invalid.az) != 0, "reject missing ring capability");
    Hardware h; check(azalia_init_rings(&h.az) == 0, "codec ring init");
    auto c = std::make_unique<codec_t>(); c->az = &h.az;
    alc892(h);
    check(probe_codec(c.get()) == 0, "enumerate ALC892-like graph");
    Path path;
    check(find_output(c.get(), path), "find analog path");
    check(path.count == 3 && path.nodes[0] == 0x14 && path.nodes[1] == 0x0c && path.nodes[2] == 2,
          "choose connected rear line-out instead of disconnected speaker");
    check(configure_path(c.get(),path) == 0, "open analog path");
    check(h.sent(0x14,0x300,0xb000), "unmute both channels of mute-only pin amp");
    check(h.sent(0x0c,0x300,0x7000), "unmute selected input of mute-only mixer amp");
    check(h.sent(0x0c,0x300,0x7180), "mute unused analog-loopback input");
    check(h.sent(2,0x300,0xb040), "DAC 0dB uses offset 0x40, not offset plus steps");
    check(h.sent(0x14,0x70c,2), "enable external amplifier");
    // Select a non-default input, proving graph discovery is applied to hardware.
    c->w[0x14].nconnections = 2;
    c->w[0x14].connections[0] = 0x0b; c->w[0x14].connections[1] = 0x0c;
    check(find_output(c.get(),path) && configure_path(c.get(),path) == 0 && h.sent(0x14,0x701,1),
          "program the selected connection instead of relying on firmware defaults");
    c->w[0x14].widgetcap |= 0x200;
    check(!find_output(c.get(),path), "HDMI/digital codec cannot claim analog output");
    // Exercise both wire encodings, range expansion, and malformed ranges.
    widget_t w; w.nid = 0x22; w.widgetcap = 0x100;
    h.param(0x22,0x0e,2); h.replies[{0x22,0xf02,0}] = 0x00008502;
    check(read_connections(&w,c.get()) == 0 && w.nconnections == 4 && w.connections[3] == 5,
          "short range expands 2..5");
    h.param(0x22,0x0e,0x82); h.replies[{0x22,0xf02,0}] = 0x80050002;
    check(read_connections(&w,c.get()) == 0 && w.nconnections == 4 && w.connections[3] == 5,
          "long range expands 2..5");
    h.param(0x22,0x0e,1); h.replies[{0x22,0xf02,0}] = 0x85;
    check(read_connections(&w,c.get()) != 0, "reject range without preceding node");
    h.param(0x22,0x0e,2); h.replies[{0x22,0xf02,0}] = 0xfe01;
    check(read_connections(&w,c.get()) != 0, "reject expanded list beyond bounded storage");
    stream_t stream = {&h.az,c.get(),0x100,1,2,{h.bdl,0x18000},{h.pcm,0x20000},sizeof(h.pcm),4096,0x11};
    check(azalia_stream_start(&stream) == 0 && h.saw_full_bdl, "complete cyclic BDL and CBL before RUN");
    check(h.sent(2,0x200,0x11) && h.sent(2,0x706,0x10), "attach converter to 48kHz stereo stream tag 1");
    h.answer = false;
    check(azalia_comresp(c.get(),0,0xf00,0,nullptr) != 0, "unresponsive codec fails instead of binding");
    std::puts("azalia-test: PASS (transport, ALC892 routing, amplifiers, cyclic DMA, negative cases)");
}
