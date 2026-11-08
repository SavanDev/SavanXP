#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include "azalia_hardware.hpp"
#include "azalia/capture.hpp"
#include "azalia/playback.hpp"
#include "azalia/recovery.hpp"
using namespace azalia_port;
using azalia_test::Hardware;
static void check(bool ok, const char* reason) {
    if (!ok) { std::fprintf(stderr,"azalia-capture-test: FAIL %s\n",reason); std::exit(1); }
}
static void capture_codec(Hardware& h) {
    azalia_test::alc892(h);
    h.param(1,4,0x00020026);
    h.param(8,9,0x10051b); h.param(8,0xb,1); h.param(8,0xa,0x20040);
    h.param(8,0xd,0x80032e10); h.param(8,0xe,1); h.replies[{8,0xf02,0}] = 0x23;
    h.param(0x23,9,0x20010b); h.param(0x23,0xd,0x80000000);
    h.param(0x23,0xe,3); h.replies[{0x23,0xf02,0}] = 0x001a1918;
    for (unsigned pin : {0x18u,0x19u,0x1au}) {
        h.param(pin,9,0x40058f); h.param(pin,0xc,0x3736);
        h.param(pin,0xd,0x00270300); h.param(pin,0x12,0x80000000);
        h.replies[{pin,0xf1c,0}] = pin == 0x18 ? 0x01a19030 : pin == 0x19 ? 0x02a19040 : 0x0181303f;
        h.param(pin,0xe,5); h.replies[{pin,0xf02,0}] = 0x0f0e0d0c; h.replies[{pin,0xf02,4}] = 0x26;
    }
}
int main() {
    check(playback_write_position(40960,45960,256,4096) == 45960,
          "safe queued playback survives scheduling jitter instead of inserting silence");
    check(playback_write_position(40960,45056,256,4096) == 49152,
          "a cursor inside the FIFO plus software guard resumes at a safe boundary");
    check(playback_write_position(40960,40960,8192,4096) == 53248,
          "larger hardware FIFOs increase the protected playback area");
    check(playback_write_position(40960ull*1000000,40960ull*1000000+5000,256,4096) == 40960ull*1000000+5000,
          "playback safety survives repeated DMA laps without narrowing the absolute cursor");
    Hardware h;
    check(azalia_init_rings(&h.az) == 0,"capture command rings");
    capture_codec(h);
    auto c = std::make_unique<codec_t>(); c->az = &h.az;
    check(probe_codec(c.get()) == 0,"enumerate PCM capabilities on input converters");
    Path rear, front, line, invalid;
    check(find_input(c.get(),rear) && rear.count == 3 && rear.nodes[0] == 8 &&
          rear.nodes[1] == 0x23 && rear.nodes[2] == 0x18,"default rear mic traces back from ADC through input mixer");
    check(input_path(c.get(),0x19,front) && front.inputs[1] == 1,"front mic selects second mixer input");
    check(input_path(c.get(),0x1a,line) && line.inputs[1] == 2,"rear line-in selects third mixer input");
    check(!input_path(c.get(),0x14,invalid),"output jack cannot be silently retasked as recording input");
    check(configure_input_path(c.get(),front) == 0,"configure front microphone");
    check(h.sent(8,0x300,0x7010),"ADC input gain uses unity offset 0x10 on both channels");
    check(h.sent(0x23,0x300,0x7080) && h.sent(0x23,0x300,0x7100) && h.sent(0x23,0x300,0x7280),
          "capture mixer opens only the selected input");
    check(h.sent(0x19,0x300,0x7000) && h.sent(0x19,0x300,0xb080) && h.sent(0x19,0x707,0x24),
          "microphone unity boost, output mute and 80 percent bias");
    h.commands.clear();
    check(configure_input_path(c.get(),line) == 0 && h.sent(0x1a,0x707,0x20),"line-in has no microphone bias");
    Path current = rear;
    h.fail_once = {0x19,0x707,0x24};
    check(switch_input(c.get(),current,front) != 0 && !current.count && h.az.transport_failed,
          "timed-out input change cannot claim a restored microphone over poisoned transport");
    Path restored_output;
    RecoveryTarget target = {c.get(),&restored_output,&current,0x14,0x18,true,true};
    h.regs[HDA_STATESTS] = 1;
    check(recover_controller(&h.az,&target,1) && target.available && restored_output.nodes[0] == 0x14 &&
          current.nodes[current.count-1] == 0x18,"controller recovery reprobes codec and restores both desired analog routes");
    h.stuck_reset = true; h.regs[HDA_GCTL] = HDA_GCTL_CRST;
    h.bdl[0].low = 0xfeedface; h.pcm[0] = 0xa5;
    check(!recover_controller(&h.az,&target,1) && !target.available && h.bdl[0].low == 0xfeedface && h.pcm[0] == 0xa5,
          "failed controller reset quarantines DMA buffers without modifying BDL or PCM");
    h.stuck_reset = false; h.regs[HDA_STATESTS] = 0;
    check(recover_controller(&h.az,&target,1) && !target.available,
          "a lost codec remains unavailable after a successful controller reset");
    h.regs[HDA_STATESTS] = 1;
    check(recover_controller(&h.az,&target,1) && target.available,"returning expected codec can restore its routes");
    stream_t stream = {&h.az,c.get(),0x80,2,8,{h.bdl,0x18000},{h.pcm,0x20000},sizeof(h.pcm),4096,0x11};
    h.stuck_run = true; h.regs[0x80] = HDA_SD_CTL_RUN;
    check(azalia_stream_start(&stream) == 110 && h.bdl[0].low == 0xfeedface && h.pcm[0] == 0xa5,
          "RUN that will not clear prevents BDL reuse even if SRST could be written");
    h.stuck_run = false;
    check(azalia_stream_start(&stream) == 0 && h.sent(8,0x200,0x11) && h.sent(8,0x706,0x20),
          "input descriptor uses independent stream tag 2 and 48kHz stereo converter format");
    CaptureCursor partial;
    h.regs[0x84] = 2;
    check(capture_poll(&stream,partial,1000) == 0 && !partial.produced,
          "a partial link frame is withheld until both stereo samples arrive");
    CaptureCursor cursor;
    unsigned char out[4096];
    for (unsigned period = 1; period <= 70; ++period) {
        const unsigned slot = (period-1)%32;
        for (unsigned i = 0; i < 4096; ++i) h.pcm[slot*4096+i] = static_cast<unsigned char>((period-1)*17+i*3);
        h.regs[0x84] = period%32*4096;
        const uint64_t now = static_cast<uint64_t>(period)*21333333;
        const int result = capture_copy(&stream,cursor,now,out,sizeof(out));
        if (period == 1) check(result == 11,"first DMA period is withheld for the capture FIFO guard");
        else {
            check(result == 0,"read a completed input period across repeated DMA wraps");
            for (unsigned i = 0; i < sizeof(out); ++i)
                check(out[i] == static_cast<unsigned char>((period-2)*17+i*3),"captured bytes match DMA, including stereo order and wrap");
        }
    }
    check(capture_copy(&stream,cursor,cursor.last_poll_ns,out,4095) == 22,"refuse an incomplete stereo frame");
    auto deadline = cursor;
    check(capture_poll(&stream,deadline,deadline.last_poll_ns+700000000) == 5 && deadline.failed,
          "an unobserved complete DMA lap fails instead of replaying stale recording data");
    CaptureCursor overflow;
    for (unsigned period = 1; period <= 32; ++period) {
        h.regs[0x84] = period%32*4096;
        const int result = capture_poll(&stream,overflow,static_cast<uint64_t>(period)*21333333);
        check(result == (period >= 31 ? 5 : 0),"an unread block is never returned after hardware overwrites it");
    }
    CaptureCursor error;
    h.regs[0x83] = 8;
    check(capture_poll(&stream,error,1000) == 5,"capture FIFO error is a real error");
    h.answer = false;
    check(switch_input(c.get(),current,front) != 0 && !current.count,"failed capture rollback clears the effective input");
    std::puts("azalia-capture-test: PASS (ADC routing, bias/amps, DMA contents, wraps, failures)");
}
