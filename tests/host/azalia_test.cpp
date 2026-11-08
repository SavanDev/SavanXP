#include <array>
#include <cstdio>
#include <cstdlib>
#include <map>
#include <memory>
#include <tuple>
#include <vector>
#include "azalia/azalia_port.hpp"
#include "azalia_hardware.hpp"
using namespace azalia_port;
namespace {
void check(bool value, const char* message) {
    if (!value) { std::fprintf(stderr, "azalia-test: FAIL %s\n", message); std::exit(1); }
}
using azalia_test::Hardware;
using azalia_test::alc892;
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
    {
        Hardware routes;
        check(azalia_init_rings(&routes.az) == 0, "route transport setup");
        alc892(routes);
        // ALC892 front headphone path: pin 0x1b -> mixer 0x0f -> DAC 0x05.
        routes.param(5,9,0x41d); routes.param(5,0x0b,1); routes.param(5,0x0a,0x00020040);
        routes.param(5,0x12,0x00034040);
        routes.param(0x0f,9,0x20010b); routes.param(0x0f,0x0d,0x80000000);
        routes.param(0x0f,0x0e,2); routes.replies[{0x0f,0xf02,0}] = 0x00000b05;
        routes.param(0x1b,9,0x40058f); routes.param(0x1b,0x0c,0x1373e);
        routes.param(0x1b,0x0d,0x00270300);
        routes.param(0x1b,0x12,0x80000000); routes.replies[{0x1b,0xf1c,0}] = 0x02214020;
        routes.replies[{0x1b,0xf01,0}] = 3;
        routes.param(0x1b,0x0e,5); routes.replies[{0x1b,0xf02,0}] = 0x0f0e0d0c;
        routes.replies[{0x1b,0xf02,4}] = 0x26;
        auto codec = std::make_unique<codec_t>(); codec->az = &routes.az;
        check(probe_codec(codec.get()) == 0, "enumerate rear and front connectors");
        Path rear, front, disabled;
        check(output_path(codec.get(),0x14,rear) && output_path(codec.get(),0x1b,front),
              "both usable analog pins are enumerable");
        check(!output_path(codec.get(),0x15,disabled), "disconnected firmware speaker is not selectable");
        check(front.nodes[front.count-1] == 5 && front.inputs[0] == 3,
              "headphone route records its distinct DAC and fourth connection");
        Path current;
        routes.commands.clear();
        check(switch_output(codec.get(),current,rear) == 0 && routes.sent(0x1b,0x707,0),
              "initial selection disables other valid connectors before enabling rear output");
        routes.commands.clear();
        check(switch_output(codec.get(),current,front) == 0 && current.nodes[0] == 0x1b,
              "commit manual selection only after configuring the new path");
        check(routes.sent(0x14,0x707,0) && routes.sent(2,0x706,0),
              "silence old connector and detach old DAC");
        check(routes.sent(0x1b,0x701,3) && routes.sent(0x1b,0x707,0xc0) &&
              routes.sent(0x1b,0x70c,2) && routes.sent(5,0x300,0xb040),
              "front path selects connection, headphone mode, EAPD and DAC unity gain");
        for (const auto& [nid,verb,payload] : routes.commands)
            check(nid != 0x1b || verb != 0x300 || (payload & 0x8000),
                  "headphone playback never programs the bidirectional pin's microphone boost amp");
        check(switch_output(codec.get(),current,rear) == 0, "manual switch back to line-out");
        routes.commands.clear(); routes.fail_once = {0x1b,0x707,0xc0};
        check(switch_output(codec.get(),current,front) != 0 && !current.count && routes.az.transport_failed,
              "a timed-out command invalidates the route until controller recovery");
        check(!routes.sent(0x14,0x707,0x40) && !routes.sent(0x0c,0x300,0x7000),
              "poisoned transport cannot falsely report successful rollback");
        check(!azalia_reset(&routes.az) && !azalia_init_rings(&routes.az) &&
              !switch_output(codec.get(),current,rear),"verified reset permits restoring the previous route");
        routes.commands.clear();
        routes.replies[{0x1b,0xf09,0}] = 0x80000000;
        check(pin_presence(codec.get(),0x1b) == PinPresence::Present &&
              routes.commands == std::vector<std::tuple<unsigned,unsigned,unsigned>>{{0x1b,0x709,0},{0x1b,0xf09,0}},
              "presence sensing triggers measurement before reading the high presence bit");
        routes.replies[{0x1b,0xf09,0}] = 0;
        check(pin_presence(codec.get(),0x1b) == PinPresence::Absent, "unplugged jack is a valid absent state");
        routes.fail_once = {0x1b,0xf09,0};
        check(pin_presence(codec.get(),0x1b) == PinPresence::Unknown, "sense timeout is unknown, never unplugged");
        check(!azalia_reset(&routes.az) && !azalia_init_rings(&routes.az),"reset poisoned sense transport before another command");
        routes.fail_once = {0x1b,0x709,0};
        check(pin_presence(codec.get(),0x1b) == PinPresence::Unknown, "trigger failure is unknown");
        routes.commands.clear();
        codec->w[0x1b].d.pin.config |= 0x100;
        check(pin_presence(codec.get(),0x1b) == PinPresence::Unsupported && routes.commands.empty(),
              "firmware no-presence override suppresses sensing verbs");
        codec->w[0x1b].d.pin.config &= ~0x100u;
        codec->w[0x1b].d.pin.cap &= ~4u;
        check(pin_presence(codec.get(),0x1b) == PinPresence::Unsupported && routes.commands.empty(),
              "missing presence capability is distinct from unplugged");
        check(pin_presence(codec.get(),0x15) == PinPresence::Unsupported && routes.commands.empty(),
              "disconnected firmware pins are never sensed");
        routes.answer = false;
        check(switch_output(codec.get(),current,front) != 0 && !current.count,
              "failed rollback reports no usable effective route");
    }
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
    {
        Hardware late;
        codec_t codec; codec.az = &late.az;
        check(!azalia_init_rings(&late.az),"late response transport setup");
        late.answer = false;
        check(azalia_comresp(&codec,0,0xf00,0,nullptr) == 110 && late.az.transport_failed,
              "timeout latches a transport failure");
        late.wp = (late.wp+1)%late.az.rirb_entries;
        late.rirb[late.wp] = {0xdeadbeef,0}; late.regs[HDA_RIRBWP] = late.wp;
        const auto old_wp = late.regs[HDA_CORBWP];
        uint32_t result = 0x1234;
        check(azalia_comresp(&codec,0,0xf00,0,&result) == 5 && result == 0x1234 && late.regs[HDA_CORBWP] == old_wp,
              "late same-codec reply cannot satisfy a new command or enqueue another verb");
        late.answer = true; late.param(0,0,0x12345678);
        check(!azalia_reset(&late.az) && !azalia_init_rings(&late.az) &&
              !azalia_comresp(&codec,0,0xf00,0,&result) && result == 0x12345678,
              "full reset and ring initialization discard stale responses");
        late.regs[HDA_RIRBSTS] = HDA_RIRBSTS_RIRBOIS;
        check(azalia_comresp(&codec,0,0xf00,0,nullptr) == 5 && late.az.transport_failed,
              "RIRB overflow loses response attribution and requires recovery");
    }
    {
        Hardware transport;
        check(azalia_init_rings(&transport.az) == 0,"shared controller command transport");
        codec_t one, two; one.az = two.az = &transport.az; one.address = 0; two.address = 2;
        check(azalia_comresp(&one,0,0xf00,0,nullptr) == 0 && azalia_comresp(&two,0,0xf00,0,nullptr) == 0,
              "serialized commands accept each codec's own response address");
        transport.answer = false;
        check(azalia_set_command(&transport.az,2,0,0xf00,0) == 0,"issue command to second codec");
        transport.wp = (transport.wp+1)%transport.az.rirb_entries;
        transport.rirb[transport.wp] = {0xdeadbeef,0}; transport.regs[0x58] = transport.wp;
        uint32_t result = 0x1234;
        check(azalia_get_response(&transport.az,&result) != 0 && result == 0x1234,
              "another codec's response never satisfies the outstanding command");
    }
    std::puts("azalia-test: PASS (transport, ALC892 routing, amplifiers, cyclic DMA, negative cases)");
}
