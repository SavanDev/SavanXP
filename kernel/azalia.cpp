/*-
 * Copyright (c) 2005 The NetBSD Foundation, Inc.
 * All rights reserved.
 *
 * This code is derived from software contributed to The NetBSD Foundation
 * by TAMURA Kent
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY THE NETBSD FOUNDATION, INC. AND CONTRIBUTORS
 * ``AS IS'' AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED
 * TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR
 * PURPOSE ARE DISCLAIMED.  IN NO EVENT SHALL THE FOUNDATION OR CONTRIBUTORS
 * BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
 * CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
 * SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
 * INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
 * CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
 * ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
 * POSSIBILITY OF SUCH DAMAGE.
 */

// SavanXP bus/DMA/audio adapter for the selective OpenBSD port.
#include "kernel/azalia.hpp"
#include "azalia/azalia_port.hpp"
#include "azalia/capture.hpp"
#include "kernel/console.hpp"
#include "kernel/pci.hpp"
#include "kernel/physical_memory.hpp"
#include "kernel/process.hpp"
#include "kernel/string.hpp"
#include "kernel/timer.hpp"
#include "kernel/vmm.hpp"

namespace {
using namespace azalia_port;
constexpr uint32_t kPeriod = 4096, kSlots = 32, kBytes = kPeriod * kSlots;
constexpr uint32_t kLead = 4 * kPeriod, kMaxQueued = 8 * kPeriod;
constexpr uint16_t kFormat = HDA_SD_FMT_BITS_16_16 | 1;
constexpr uint64_t kBytesPerSecond = 48000 * 4;
constexpr size_t kMaxControllers = 4, kMaxCodecs = 8, kCodecsPerController = 4;
struct Controller {
    volatile uint8_t* mmio = nullptr;
    azalia_t az;
    memory::PageAllocation rings = {};
    uint16_t capability = 0, bdf = 0;
    unsigned outputs = 0, inputs = 0, tags = 1, codecs = 0;
};
uint32_t mmio_read(void* p, uint16_t offset, unsigned width) {
    const auto* c = static_cast<Controller*>(p);
    if (width == 1) return *reinterpret_cast<volatile uint8_t*>(c->mmio+offset);
    if (width == 2) return *reinterpret_cast<volatile uint16_t*>(c->mmio+offset);
    return *reinterpret_cast<volatile uint32_t*>(c->mmio+offset);
}
void mmio_write(void* p, uint16_t offset, uint32_t value, unsigned width) {
    auto* c = static_cast<Controller*>(p);
    if (width == 1) *reinterpret_cast<volatile uint8_t*>(c->mmio+offset) = value;
    else if (width == 2) *reinterpret_cast<volatile uint16_t*>(c->mmio+offset) = value;
    else *reinterpret_cast<volatile uint32_t*>(c->mmio+offset) = value;
}
void delay_us(void*, unsigned us) {
    const uint64_t start = timer::monotonic_ns(), duration = static_cast<uint64_t>(us) * 1000;
    while (timer::monotonic_ns() - start < duration) asm volatile("pause");
}

bool stop_commands(Controller& c) {
    mmio_write(&c,HDA_CORBCTL,0,1); mmio_write(&c,HDA_RIRBCTL,0,1);
    for (unsigned i = 0; i < 5000; ++i) {
        if (!(mmio_read(&c,HDA_CORBCTL,1)&HDA_CORBCTL_CORBRUN) &&
            !(mmio_read(&c,HDA_RIRBCTL,1)&HDA_RIRBCTL_RIRBDMAEN)) return true;
        delay_us(&c,10);
    }
    console::write_line("azalia: command DMA stop timed out; buffers retained");
    return false;
}
struct Device {
    Controller* ctrl = nullptr;
    codec_t codec;
    Path playback_path, input_route;
    stream_t stream = {}, record_stream = {};
    CaptureCursor record_cursor;
    memory::PageAllocation pcm = {}, record_pcm = {};
    bool online = false, prepared = false, playback_supported = false;
    bool capture_supported = false, capture_prepared = false, capture_fault = false, capture_dma_stopped = true;
    uint64_t played = 0, written = 0, last_poll_ns = 0;
    uint32_t last_position = 0, dropped = 0, underruns = 0;
    uint8_t scratch[kPeriod];
    uint64_t device_id = 0, next_presence_ns = 0;
    char device_name[48] = {};
    audio::Backend adapter = {};

    void stop_dma() {
        if (!ctrl->mmio) return;
        mmio_write(ctrl, stream.regbase + HDA_SD_CTL,
                   mmio_read(ctrl, stream.regbase + HDA_SD_CTL, 2) & ~HDA_SD_CTL_RUN, 2);
        (void)azalia_stream_reset(&stream);
        prepared = false;
    }
    bool ready() { return online && playback_supported && playback_path.count; }
    bool get_info(savanxp_audio_info& info) {
        if (!online) return false;
        info = {48000, 2, 16, 4, kPeriod, kLead, SAVANXP_AUDIO_BACKEND_HDA, 0};
        return true;
    }
    bool configure() {
        if (!ready()) return false;
        if (prepared) return true;
        memset(pcm.virtual_address, 0, (1 + kSlots) * memory::kPageSize);
        played = 0; written = kLead; last_position = 0;
        dropped = 0; underruns = 0;
        if (azalia_stream_start(&stream)) {
            stop_dma(); return false;
        }
        last_poll_ns = timer::monotonic_ns();
        prepared = true;
        return true;
    }
    int submit(uint64_t user_buffer, uint32_t byte_count) {
        if (!online || !prepared) return -SX_ENODEV;
        if (!byte_count || byte_count > kPeriod || byte_count % 4) return -SX_EINVAL;
        if (!process::copy_from_user(scratch, user_buffer, byte_count)) return -SX_EINVAL;
        audio::apply_playback_gain(scratch, byte_count);
        poll();
        if (!prepared) return -SX_EIO;
        // Leave at least two periods ahead of DMA (including its FIFO prefetch).
        // Never wait in a syscall: the ABI's existing audio backends discard when full.
        if (written < played + 2 * kPeriod) {
            written = ((played + 3 * kPeriod - 1) / kPeriod) * kPeriod;
            ++underruns;
        }
        if (written + byte_count > played + kMaxQueued) { ++dropped; return 0; }
        const uint32_t offset = written % kBytes;
        const uint32_t first = byte_count < kBytes - offset ? byte_count : kBytes - offset;
        auto* pcm = static_cast<uint8_t*>(stream.buffer.addr);
        memcpy(pcm + offset, scratch, first);
        if (first < byte_count) memcpy(pcm, scratch + first, byte_count - first);
        __atomic_thread_fence(__ATOMIC_RELEASE);
        written += byte_count;
        return 0;
    }
    void stop() {
        if (!online || !prepared) return;
        stop_dma();
        console::printf("azalia: stop played=%llu written=%llu drops=%u underruns=%u\n",
            static_cast<unsigned long long>(played), static_cast<unsigned long long>(written),
            dropped, underruns);
        (void)azalia_comresp(&codec, stream.converter, CORB_SET_CONVERTER_STREAM_CHANNEL, 0, nullptr);
    }
    bool stop_record_dma(bool fault) {
        if (!ctrl->mmio || !capture_supported) return true;
        const int reg = record_stream.regbase;
        mmio_write(ctrl,reg+HDA_SD_CTL,mmio_read(ctrl,reg+HDA_SD_CTL,2)&~HDA_SD_CTL_RUN,2);
        capture_dma_stopped = !azalia_stream_reset(&record_stream) &&
            !(mmio_read(ctrl,reg+HDA_SD_CTL,2)&HDA_SD_CTL_RUN);
        capture_prepared = false;
        capture_fault = fault || !capture_dma_stopped;
        return capture_dma_stopped;
    }
    bool capture_ready() { return online && capture_supported && input_route.count; }
    void capture_stop() {
        if (!online || !capture_supported) return;
        (void)stop_record_dma(false);
        (void)azalia_comresp(&codec,record_stream.converter,CORB_SET_CONVERTER_STREAM_CHANNEL,0,nullptr);
        if (input_route.count)
            (void)azalia_comresp(&codec,input_route.nodes[input_route.count-1],CORB_SET_PIN_WIDGET_CONTROL,0,nullptr);
    }
    bool capture_configure() {
        if (!capture_ready()) return false;
        if (capture_prepared) return true;
        if (!capture_dma_stopped && !stop_record_dma(false)) return false;
        if (!record_pcm.virtual_address && !memory::allocate_contiguous_pages(1+kSlots,record_pcm)) return false;
        if (!(ctrl->capability & HDA_GCAP_64OK) &&
            record_pcm.physical_address+(1+kSlots)*memory::kPageSize > 0x100000000ull) return false;
        record_stream.bdlist = {record_pcm.virtual_address,record_pcm.physical_address};
        record_stream.buffer = {static_cast<uint8_t*>(record_pcm.virtual_address)+memory::kPageSize,
            record_pcm.physical_address+memory::kPageSize};
        record_stream.converter = input_route.nodes[0];
        memset(record_pcm.virtual_address,0,(1+kSlots)*memory::kPageSize);
        if (configure_input_path(&codec,input_route) || azalia_stream_start(&record_stream)) {
            capture_stop(); return false;
        }
        record_cursor = {};
        record_cursor.last_poll_ns = timer::monotonic_ns();
        capture_prepared = true; capture_dma_stopped = false; capture_fault = false;
        return true;
    }
    int capture_available() {
        if (capture_fault) return -SX_EIO;
        if (!capture_prepared) return 0;
        const int error = capture_poll(&record_stream,record_cursor,timer::monotonic_ns());
        if (error) { (void)stop_record_dma(true); return -SX_EIO; }
        return static_cast<int>(azalia_port::capture_available(&record_stream,record_cursor));
    }
    int capture_read(void* buffer, uint32_t bytes) {
        if (!buffer || !bytes || bytes > kPeriod || bytes%4) return -SX_EINVAL;
        const uint64_t deadline = timer::monotonic_ns()+250000000;
        // Preserve legacy blocking device reads. The native nonblocking ioctl
        // checks capture_available first, so the mixer never enters this wait.
        for (;;) {
            const int available = capture_available();
            if (available < 0) return available;
            if (static_cast<uint32_t>(available) >= bytes) break;
            if (!capture_prepared || timer::monotonic_ns() >= deadline) return -SX_EIO;
            poll(); delay_us(nullptr,100);
        }
        const int error = capture_copy(&record_stream,record_cursor,timer::monotonic_ns(),buffer,bytes);
        return error ? -error : 0;
    }
    int select_input(uint64_t id) {
        if (!capture_supported) return -SX_ENODEV;
        Path candidate;
        if (!input_path(&codec,id&0xff,candidate)) return -SX_ENOENT;
        const Path previous = input_route;
        const bool running = capture_prepared;
        capture_stop();
        if (!capture_dma_stopped) return -SX_EIO;
        const int changed = switch_input(&codec,input_route,candidate);
        if (input_route.count) record_stream.converter = input_route.nodes[0];
        if (!changed && (!running || capture_configure())) return 0;
        capture_stop();
        if (!changed && previous.count) {
            (void)switch_input(&codec,input_route,previous);
            if (input_route.count) record_stream.converter = input_route.nodes[0];
        }
        if (running && input_route.count && !capture_configure()) input_route = {};
        return -SX_EIO;
    }

    bool endpoint(uint32_t index, savanxp_audio_endpoint& info) {
        if (!online) return false;
        const uint64_t now = timer::monotonic_ns();
        if (now >= next_presence_ns) {
            next_presence_ns = now + 250000000;
            for (int nid = codec.wstart; nid < codec.wend; ++nid) {
                Path route;
                if ((playback_supported && output_path(&codec,nid,route)) ||
                    (capture_supported && input_path(&codec,nid,route)))
                    codec.w[nid].d.pin.presence = pin_presence(&codec,nid);
            }
        }
        for (int nid = codec.wstart; nid < codec.wend; ++nid) {
            Path path;
            const bool output = playback_supported && output_path(&codec,nid,path);
            if (!output && (!capture_supported || !input_path(&codec,nid,path))) continue;
            if (index--) continue;
            const auto& pin = codec.w[nid];
            info = {};
            info.id = device_id | static_cast<unsigned>(nid);
            info.device_id = device_id;
            info.backend = SAVANXP_AUDIO_BACKEND_HDA;
            info.flags = (output ? SAVANXP_AUDIO_PLAYBACK : SAVANXP_AUDIO_CAPTURE) | SAVANXP_AUDIO_AVAILABLE;
            if ((output && playback_path.count && playback_path.nodes[0] == nid) ||
                (!output && input_route.count && input_route.nodes[input_route.count-1] == nid))
                info.flags |= SAVANXP_AUDIO_SELECTED;
            if (CORB_CD_PORT(pin.d.pin.config) == CORB_CD_JACK) info.flags |= SAVANXP_AUDIO_JACK;
            if (pin.d.pin.device == CORB_CD_HEADPHONE) info.flags |= SAVANXP_AUDIO_HEADPHONES;
            if (pin_detectable(pin)) info.flags |= SAVANXP_AUDIO_PRESENCE_CAPABLE;
            if (pin.d.pin.presence == PinPresence::Present || pin.d.pin.presence == PinPresence::Absent)
                info.flags |= SAVANXP_AUDIO_PRESENCE_VALID;
            if (pin.d.pin.presence == PinPresence::Present) info.flags |= SAVANXP_AUDIO_CONNECTED;
            const unsigned location = CORB_CD_LOC_GEO(pin.d.pin.config);
            const char* position = location == CORB_CD_FRONT ? "Front " : location == CORB_CD_REAR ? "Rear " : "";
            const char* name = pin.d.pin.device == CORB_CD_HEADPHONE ? "headphones" :
                pin.d.pin.device == CORB_CD_SPEAKER ? "speakers" : pin.d.pin.device == CORB_CD_MICIN ? "microphone" :
                pin.d.pin.device == CORB_CD_LINEIN ? "line-in" : output ? "line-out" : "auxiliary input";
            strcpy(info.name, position);
            strcpy(info.name + strlen(info.name), name);
            strcpy(info.device_name, device_name);
            return true;
        }
        return false;
    }
    int select_endpoint(uint64_t id, uint32_t direction) {
        if (!online || (id & ~uint64_t{0xff}) != device_id) return -SX_ENOENT;
        if (direction == SAVANXP_AUDIO_CAPTURE) return select_input(id);
        if (direction != SAVANXP_AUDIO_PLAYBACK) return -SX_EINVAL;
        if (!playback_supported) return -SX_ENODEV;
        Path candidate;
        if (!output_path(&codec, id & 0xff, candidate)) return -SX_ENOENT;
        const Path previous = playback_path;
        const bool running = prepared;
        stop();
        const int changed = switch_output(&codec, playback_path, candidate);
        if (playback_path.count) stream.converter = playback_path.nodes[playback_path.count - 1];
        if (!changed && (!running || configure())) return 0;
        // Route setup or restarting DMA failed. Restore the old route and resume
        // it if possible; never report the requested connector as effective.
        stop();
        if (!changed && previous.count) {
            (void)switch_output(&codec, playback_path, previous);
            if (playback_path.count) stream.converter = playback_path.nodes[playback_path.count - 1];
        }
        if (running && playback_path.count && !configure()) playback_path = {};
        return -SX_EIO;
    }
    void poll() {
        if (capture_prepared) (void)capture_available();
        if (!prepared) return;
        const uint64_t now = timer::monotonic_ns();
        if (now - last_poll_ns >= static_cast<uint64_t>(kBytes) * 1000000000 / kBytesPerSecond) {
            // LPIB alone cannot count complete unobserved laps. Fail instead of
            // silently treating a stale cursor as a valid playback position.
            stop_dma();
            console::write_line("azalia: DMA polling deadline missed; stream stopped");
            return;
        }
        last_poll_ns = now;
        const uint32_t position = mmio_read(ctrl, stream.regbase + HDA_SD_LPIB, 4);
        if (position >= kBytes) { stop_dma(); return; }
        const uint32_t delta = (position + kBytes - last_position) % kBytes;
        // Clear only complete periods behind the hardware. Neither the current
        // DMA period nor future queued samples are modified by this timer callback.
        const uint64_t old_period = played / kPeriod;
        played += delta;
        auto* pcm = static_cast<uint8_t*>(stream.buffer.addr);
        for (uint64_t period = old_period; period < played / kPeriod; ++period)
            memset(pcm + (period % kSlots) * kPeriod, 0, kPeriod);
        last_position = position;
        const uint8_t status = mmio_read(ctrl, stream.regbase + HDA_SD_STS, 1);
        if (status & (HDA_SD_STS_DESE | HDA_SD_STS_FIFOE)) {
            stop_dma();
            console::printf("azalia: stream DMA error status=0x%x\n", status);
        }
    }

    void setup_adapter() {
        adapter = {
            .ready = [](void* p) { return static_cast<Device*>(p)->ready(); },
            .get_info = [](void* p, savanxp_audio_info& i) { return static_cast<Device*>(p)->get_info(i); },
            .configure = [](void* p) { return static_cast<Device*>(p)->configure(); },
            .submit_period = [](void* p, uint64_t u, uint32_t n) { return static_cast<Device*>(p)->submit(u,n); },
            .stop = [](void* p) { static_cast<Device*>(p)->stop(); },
            .capture_ready = [](void* p) { return static_cast<Device*>(p)->capture_ready(); },
            .capture_configure = [](void* p) { return static_cast<Device*>(p)->capture_configure(); },
            .capture_read_period = [](void* p, void* b, uint32_t n) { return static_cast<Device*>(p)->capture_read(b,n); },
            .capture_stop = [](void* p) { static_cast<Device*>(p)->capture_stop(); },
            .endpoint = [](void* p, uint32_t n, savanxp_audio_endpoint& i) { return static_cast<Device*>(p)->endpoint(n,i); },
            .select_endpoint = [](void* p, uint64_t id, uint32_t dir) { return static_cast<Device*>(p)->select_endpoint(id,dir); },
            .capture_available = [](void* p) { return static_cast<Device*>(p)->capture_available(); },
            .context = this,
        };
    }
};
Controller g_controllers[kMaxControllers];
Device g_devices[kMaxCodecs];
size_t g_controller_count = 0, g_device_count = 0;
bool g_probed = false;

bool probe() {
    if (g_probed) return g_device_count != 0;
    g_probed = true;
    if (!pci::ready() || !timer::monotonic_ns()) return false;
    for (size_t index = 0; index < pci::device_count() && g_controller_count < kMaxControllers && g_device_count < kMaxCodecs; ++index) {
        pci::DeviceInfo dev;
        if (!pci::device_info(index,dev) || dev.class_code != 4 || dev.subclass != 3) continue;
        pci::BarInfo bar;
        if (!pci::bar_info(dev,0,bar) || !bar.present || bar.io_space || !bar.base) continue;
        void* mapped = nullptr;
        if (!vm::map_kernel_mmio(bar.base,bar.size ? bar.size : 0x4000,
            vm::kPageWriteThrough|vm::kPageCacheDisable,&mapped)) continue;
        Controller& c = g_controllers[g_controller_count++];
        c.mmio = static_cast<volatile uint8_t*>(mapped);
        c.bdf = (dev.bus<<8)|(dev.slot<<3)|dev.function;
        const uint16_t command = pci::read_config_u16(dev.bus,dev.slot,dev.function,4);
        pci::write_config_u16(dev.bus,dev.slot,dev.function,4,command|6);
        // OpenBSD azalia_configure_pci: AMD Family 17h DMA snooping.
        if (dev.vendor_id == 0x1022 && dev.device_id == 0x1457) {
            const uint8_t value = pci::read_config_u8(dev.bus,dev.slot,dev.function,0x42);
            pci::write_config_u8(dev.bus,dev.slot,dev.function,0x42,(value&0xf8)|0x02);
        }
        c.az.io = {&c,mmio_read,mmio_write,delay_us};
        if (azalia_reset(&c.az)) { console::write_line("azalia: controller reset failed"); continue; }
        c.capability = mmio_read(&c,HDA_GCAP,2);
        if (!HDA_GCAP_OSS(c.capability) && !HDA_GCAP_ISS(c.capability)) continue;
        if (!memory::allocate_contiguous_pages(2,c.rings)) continue;
        if (!(c.capability&HDA_GCAP_64OK) && c.rings.physical_address+2*memory::kPageSize > 0x100000000ull) continue;
        memset(c.rings.virtual_address,0,2*memory::kPageSize);
        c.az.corb_dma = {c.rings.virtual_address,c.rings.physical_address};
        c.az.rirb_dma = {static_cast<uint8_t*>(c.rings.virtual_address)+memory::kPageSize,c.rings.physical_address+memory::kPageSize};
        const uint16_t codecs = mmio_read(&c,HDA_STATESTS,2);
        mmio_write(&c,HDA_STATESTS,codecs,2);
        if (azalia_init_rings(&c.az)) { (void)stop_commands(c); continue; }
        for (unsigned address = 0; address < HDA_MAX_CODECS && c.codecs < kCodecsPerController && g_device_count < kMaxCodecs; ++address) {
            if (!(codecs&(1u<<address))) continue;
            Device& d = g_devices[g_device_count];
            // Reset individually; a codec-sized stack temporary exceeds the
            // physical machine's kernel stack. Unpublished slots can be reused.
            for (auto& w : d.codec.w) w = {};
            d.codec.audiofunc = -1; d.codec.wstart = d.codec.wend = 0;
            d.codec.vid = d.codec.qrks = 0; d.codec.az = &c.az; d.codec.address = address;
            const int error = probe_codec(&d.codec);
            if (error) { console::printf("azalia: codec %u probe failed (%d)\n",address,error); continue; }
            Path output, input;
            const bool playback = c.outputs < HDA_GCAP_OSS(c.capability) && find_output(&d.codec,output);
            const bool capture = c.inputs < HDA_GCAP_ISS(c.capability) && find_input(&d.codec,input);
            if (!playback && !capture) continue;
            d.ctrl = &c; d.playback_path = {}; d.input_route = {};
            if (playback && switch_output(&d.codec,d.playback_path,output)) continue;
            if (playback) {
                if (!memory::allocate_contiguous_pages(1+kSlots,d.pcm)) continue;
                if (!(c.capability&HDA_GCAP_64OK) && d.pcm.physical_address+(1+kSlots)*memory::kPageSize > 0x100000000ull) continue;
                d.stream = {&c.az,&d.codec,HDA_SD_BASE+static_cast<int>(HDA_GCAP_ISS(c.capability)+c.outputs)*HDA_SD_SIZE,
                    static_cast<int>(c.tags++),d.playback_path.nodes[d.playback_path.count-1],
                    {d.pcm.virtual_address,d.pcm.physical_address},
                    {static_cast<uint8_t*>(d.pcm.virtual_address)+memory::kPageSize,d.pcm.physical_address+memory::kPageSize},
                    kBytes,kPeriod,kFormat};
                ++c.outputs;
            }
            d.playback_supported = playback; d.capture_supported = capture;
            if (capture) {
                d.input_route = input;
                d.record_stream = {&c.az,&d.codec,HDA_SD_BASE+static_cast<int>(c.inputs)*HDA_SD_SIZE,
                    static_cast<int>(c.tags++),input.nodes[0],{},{},kBytes,kPeriod,kFormat};
                ++c.inputs;
                console::printf("azalia: capture pin=0x%x adc=0x%x descriptor=%u tag=%u\n",
                    input.nodes[input.count-1],input.nodes[0],c.inputs-1,d.record_stream.number);
            }
            d.device_id = (uint64_t{SAVANXP_AUDIO_BACKEND_HDA}<<56)|(static_cast<uint64_t>(c.bdf)<<16)|(static_cast<uint64_t>(address)<<8);
            strcpy(d.device_name,d.codec.vid == 0x10ec0892 ? "Realtek ALC892 (HD Audio)" : "HD Audio analog codec");
            char suffix[] = " [00:00.0/0]";
            constexpr char digits[] = "0123456789abcdef";
            suffix[2] = digits[dev.bus>>4]; suffix[3] = digits[dev.bus&15];
            suffix[5] = digits[dev.slot>>4]; suffix[6] = digits[dev.slot&15];
            suffix[8] = digits[dev.function]; suffix[10] = digits[address];
            strcpy(d.device_name+strlen(d.device_name),suffix);
            d.online = true; d.setup_adapter();
            ++g_device_count; ++c.codecs;
            console::printf("azalia: ready %x:%x.%u codec=0x%x address=%u pin=0x%x dac=0x%x rings=%u/%u\n",
                dev.bus,dev.slot,dev.function,d.codec.vid,address,playback ? d.playback_path.nodes[0] : 0,
                playback ? d.stream.converter : 0,c.az.corb_entries,c.az.rirb_entries);
        }
        if (!c.codecs) {
            console::printf("azalia: %x:%x.%u no supported analog path, skipped\n",dev.bus,dev.slot,dev.function);
            (void)stop_commands(c);
        }
        // MMIO has no unmap yet; command/stream allocations remain reserved for
        // this controller for the boot, including failed-stop hardware.
    }
    return g_device_count != 0;
}
const audio::Backend& backend() { return g_devices[0].adapter; }
const audio::Backend* backend_at(uint32_t index) { return index < g_device_count ? &g_devices[index].adapter : nullptr; }
const audio::Driver g_driver = {"azalia",60,probe,backend,backend_at};
} // namespace
namespace azalia {
const audio::Driver& driver() { return g_driver; }
void poll() { for (size_t i = 0; i < g_device_count; ++i) g_devices[i].poll(); }
} // namespace azalia
