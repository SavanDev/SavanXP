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
volatile uint8_t* g_mmio = nullptr;
azalia_t g_controller;
codec_t g_codec;
Path g_path;
stream_t g_stream = {};
memory::PageAllocation g_rings = {}, g_pcm = {};
bool g_ready = false, g_prepared = false;
uint64_t g_played = 0, g_written = 0, g_last_poll_ns = 0;
uint32_t g_last_position = 0, g_dropped = 0, g_underruns = 0;
uint8_t g_scratch[kPeriod];

uint32_t mmio_read(void*, uint16_t offset, unsigned width) {
    if (width == 1) return *reinterpret_cast<volatile uint8_t*>(g_mmio + offset);
    if (width == 2) return *reinterpret_cast<volatile uint16_t*>(g_mmio + offset);
    return *reinterpret_cast<volatile uint32_t*>(g_mmio + offset);
}
void mmio_write(void*, uint16_t offset, uint32_t value, unsigned width) {
    if (width == 1) *reinterpret_cast<volatile uint8_t*>(g_mmio + offset) = value;
    else if (width == 2) *reinterpret_cast<volatile uint16_t*>(g_mmio + offset) = value;
    else *reinterpret_cast<volatile uint32_t*>(g_mmio + offset) = value;
}
void delay_us(void*, unsigned us) {
    const uint64_t start = timer::monotonic_ns(), duration = static_cast<uint64_t>(us) * 1000;
    while (timer::monotonic_ns() - start < duration) asm volatile("pause");
}
void stop_dma() {
    if (!g_mmio) return;
    mmio_write(nullptr, g_stream.regbase + HDA_SD_CTL,
               mmio_read(nullptr, g_stream.regbase + HDA_SD_CTL, 2) & ~HDA_SD_CTL_RUN, 2);
    (void)azalia_stream_reset(&g_stream);
    g_prepared = false;
}
bool abandon_controller() {
    // Stop DMA before shared rings are reused by another PCI function.
    mmio_write(nullptr, HDA_CORBCTL, 0, 1);
    mmio_write(nullptr, HDA_RIRBCTL, 0, 1);
    for (unsigned i = 0; i < 5000; ++i) {
        if (!(mmio_read(nullptr, HDA_CORBCTL, 1) & HDA_CORBCTL_CORBRUN) &&
            !(mmio_read(nullptr, HDA_RIRBCTL, 1) & HDA_RIRBCTL_RIRBDMAEN)) return true;
        delay_us(nullptr, 10);
    }
    // Keep DMA allocations alive if broken hardware does not acknowledge stop.
    console::write_line("azalia: command DMA stop timed out; buffers retained");
    return false;
}

bool probe() {
    if (g_ready) return true;
    if (!pci::ready() || timer::monotonic_ns() == 0) return false;
    for (size_t index = 0; index < pci::device_count(); ++index) {
        pci::DeviceInfo dev;
        if (!pci::device_info(index, dev) || dev.class_code != 4 || dev.subclass != 3) continue;
        pci::BarInfo bar;
        if (!pci::bar_info(dev, 0, bar) || !bar.present || bar.io_space || !bar.base) continue;
        void* mapped = nullptr;
        if (!vm::map_kernel_mmio(bar.base, bar.size ? bar.size : 0x4000,
            vm::kPageWriteThrough | vm::kPageCacheDisable, &mapped)) continue;
        g_mmio = static_cast<volatile uint8_t*>(mapped);
        const auto bus = dev.bus, slot = dev.slot, function = dev.function;
        const uint16_t command = pci::read_config_u16(bus, slot, function, 4);
        pci::write_config_u16(bus, slot, function, 4, command | 6);
        // azalia_configure_pci: AMD Family 17h needs coherent DMA snooping.
        if (dev.vendor_id == 0x1022 && dev.device_id == 0x1457) {
            const uint8_t value = pci::read_config_u8(bus, slot, function, 0x42);
            pci::write_config_u8(bus, slot, function, 0x42, (value & 0xf8) | 0x02);
        }
        g_controller = {};
        g_controller.io = {nullptr, mmio_read, mmio_write, delay_us};
        if (azalia_reset(&g_controller)) {
            console::write_line("azalia: controller reset failed"); continue;
        }
        const uint16_t cap = mmio_read(nullptr, HDA_GCAP, 2);
        if (!HDA_GCAP_OSS(cap)) continue;
        g_stream = {};
        g_stream.az = &g_controller;
        g_stream.codec = &g_codec;
        g_stream.number = 1;
        g_stream.regbase = HDA_SD_BASE + HDA_GCAP_ISS(cap) * HDA_SD_SIZE;
        if (!g_rings.virtual_address && !memory::allocate_contiguous_pages(2, g_rings)) return false;
        if (!(cap & HDA_GCAP_64OK) && g_rings.physical_address + 2 * memory::kPageSize > 0x100000000ull) return false;
        memset(g_rings.virtual_address, 0, 2 * memory::kPageSize);
        g_controller.corb_dma = {g_rings.virtual_address, g_rings.physical_address};
        g_controller.rirb_dma = {static_cast<uint8_t*>(g_rings.virtual_address) + memory::kPageSize,
                                 g_rings.physical_address + memory::kPageSize};
        const uint16_t codecs = mmio_read(nullptr, HDA_STATESTS, 2);
        mmio_write(nullptr, HDA_STATESTS, codecs, 2);
        if (azalia_init_rings(&g_controller)) {
            if (!abandon_controller()) return false;
            continue;
        }
        bool found = false;
        for (unsigned address = 0; address < HDA_MAX_CODECS; ++address) {
            if (!(codecs & (1u << address))) continue;
            // Reset widgets individually: a codec-sized temporary would exceed
            // the kernel stack on a physical machine.
            for (auto& widget : g_codec.w) widget = {};
            g_codec.audiofunc = -1; g_codec.wstart = 0; g_codec.wend = 0;
            g_codec.vid = 0; g_codec.qrks = 0;
            g_codec.az = &g_controller;
            g_codec.address = address;
            const int codec_error = probe_codec(&g_codec);
            if (codec_error) {
                console::printf("azalia: codec %u probe failed (%d)\n", address, codec_error);
                continue;
            }
            if (!find_output(&g_codec, g_path)) continue;
            const int path_error = configure_path(&g_codec, g_path);
            if (path_error) {
                console::printf("azalia: codec %u path setup failed (%d)\n", address, path_error);
                continue;
            }
            found = true; break;
        }
        if (!found) {
            console::printf("azalia: %x:%x.%u no supported analog path, skipped\n", bus, slot, function);
            if (!abandon_controller()) return false;
            continue;
        }
        if (!memory::allocate_contiguous_pages(1 + kSlots, g_pcm)) {
            (void)abandon_controller(); return false;
        }
        if (!(cap & HDA_GCAP_64OK) && g_pcm.physical_address + (1 + kSlots) * memory::kPageSize > 0x100000000ull) {
            (void)abandon_controller(); return false;
        }
        g_stream.bdlist = {g_pcm.virtual_address, g_pcm.physical_address};
        g_stream.buffer = {static_cast<uint8_t*>(g_pcm.virtual_address) + memory::kPageSize,
                           g_pcm.physical_address + memory::kPageSize};
        g_stream.bufsize = kBytes; g_stream.blk = kPeriod; g_stream.fmt = kFormat;
        g_stream.converter = g_path.nodes[g_path.count - 1];
        g_ready = true;
        console::printf("azalia: ready %x:%x.%u codec=0x%x pin=0x%x dac=0x%x rings=%u/%u\n",
            bus, slot, function, g_codec.vid, g_path.nodes[0], g_stream.converter,
            g_controller.corb_entries, g_controller.rirb_entries);
        return true;
    }
    if (g_rings.virtual_address) {
        // The unsuccessful probe may leave DMA pointed here; retain allocations
        // for this boot instead of freeing memory still reachable by hardware.
        console::write_line("azalia: no supported controller; ring allocation retained");
    }
    return false;
}

bool ready() { return g_ready; }
bool get_info(savanxp_audio_info& info) {
    if (!g_ready) return false;
    info = {48000, 2, 16, 4, kPeriod, kLead, SAVANXP_AUDIO_BACKEND_HDA, 0};
    return true;
}
bool configure() {
    if (!g_ready) return false;
    if (g_prepared) return true;
    memset(g_pcm.virtual_address, 0, (1 + kSlots) * memory::kPageSize);
    g_played = 0; g_written = kLead; g_last_position = 0;
    g_dropped = 0; g_underruns = 0;
    if (azalia_stream_start(&g_stream)) {
        stop_dma(); return false;
    }
    g_last_poll_ns = timer::monotonic_ns();
    g_prepared = true;
    return true;
}
int submit(uint64_t user_buffer, uint32_t byte_count) {
    if (!g_ready || !g_prepared) return -SX_ENODEV;
    if (!byte_count || byte_count > kPeriod || byte_count % 4) return -SX_EINVAL;
    if (!process::copy_from_user(g_scratch, user_buffer, byte_count)) return -SX_EINVAL;
    audio::apply_playback_gain(g_scratch, byte_count);
    azalia::poll();
    if (!g_prepared) return -SX_EIO;
    // Leave at least two periods ahead of DMA (including its FIFO prefetch).
    // Never wait in a syscall: the ABI's existing audio backends discard when full.
    if (g_written < g_played + 2 * kPeriod) {
        g_written = ((g_played + 3 * kPeriod - 1) / kPeriod) * kPeriod;
        ++g_underruns;
    }
    if (g_written + byte_count > g_played + kMaxQueued) { ++g_dropped; return 0; }
    const uint32_t offset = g_written % kBytes;
    const uint32_t first = byte_count < kBytes - offset ? byte_count : kBytes - offset;
    auto* pcm = static_cast<uint8_t*>(g_stream.buffer.addr);
    memcpy(pcm + offset, g_scratch, first);
    if (first < byte_count) memcpy(pcm, g_scratch + first, byte_count - first);
    __atomic_thread_fence(__ATOMIC_RELEASE);
    g_written += byte_count;
    return 0;
}
void stop() {
    if (!g_ready || !g_prepared) return;
    stop_dma();
    console::printf("azalia: stop played=%llu written=%llu drops=%u underruns=%u\n",
        static_cast<unsigned long long>(g_played), static_cast<unsigned long long>(g_written),
        g_dropped, g_underruns);
    (void)azalia_comresp(&g_codec, g_stream.converter, CORB_SET_CONVERTER_STREAM_CHANNEL, 0, nullptr);
}
const audio::Backend g_backend = {ready, get_info, configure, submit, stop, nullptr, nullptr, nullptr, nullptr};
const audio::Backend& backend() { return g_backend; }
const audio::Driver g_driver = {"azalia", 60, probe, backend};
} // namespace

namespace azalia {
const audio::Driver& driver() { return g_driver; }
void poll() {
    if (!g_prepared) return;
    const uint64_t now = timer::monotonic_ns();
    if (now - g_last_poll_ns >= static_cast<uint64_t>(kBytes) * 1000000000 / kBytesPerSecond) {
        // LPIB alone cannot count complete unobserved laps. Fail instead of
        // silently treating a stale cursor as a valid playback position.
        stop_dma();
        console::write_line("azalia: DMA polling deadline missed; stream stopped");
        return;
    }
    g_last_poll_ns = now;
    const uint32_t position = mmio_read(nullptr, g_stream.regbase + HDA_SD_LPIB, 4);
    if (position >= kBytes) { stop_dma(); return; }
    const uint32_t delta = (position + kBytes - g_last_position) % kBytes;
    // Clear only complete periods behind the hardware. Neither the current
    // DMA period nor future queued samples are modified by this timer callback.
    const uint64_t old_period = g_played / kPeriod;
    g_played += delta;
    auto* pcm = static_cast<uint8_t*>(g_stream.buffer.addr);
    for (uint64_t period = old_period; period < g_played / kPeriod; ++period)
        memset(pcm + (period % kSlots) * kPeriod, 0, kPeriod);
    g_last_position = position;
    const uint8_t status = mmio_read(nullptr, g_stream.regbase + HDA_SD_STS, 1);
    if (status & (HDA_SD_STS_DESE | HDA_SD_STS_FIFOE)) {
        stop_dma();
        console::printf("azalia: stream DMA error status=0x%x\n", status);
    }
}
} // namespace azalia
