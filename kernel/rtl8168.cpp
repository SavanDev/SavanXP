/*
 * Realtek RTL8111/8168 family driver for SavanXP.
 *
 * Adapted from OpenBSD re(4): sys/dev/ic/re.c, sys/dev/ic/rtl81x9reg.h and
 * sys/dev/mii/rgephy.c at 287e5b695b75cb0c9c794575ed99e90305f680e8. The
 * revision table, the per-revision flags and the bring-up order come from
 * there; the rings, the polling and the nic:: glue are SavanXP's. As a
 * derivative work this file carries the original license, not the
 * repository's MIT.
 *
 * Copyright (c) 1997, 1998-2003
 *	Bill Paul <wpaul@windriver.com>.  All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 * 3. All advertising materials mentioning features or use of this software
 *    must display the following acknowledgement:
 *	This product includes software developed by Bill Paul.
 * 4. Neither the name of the author nor the names of any co-contributors
 *    may be used to endorse or promote products derived from this software
 *    without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY Bill Paul AND CONTRIBUTORS ``AS IS'' AND
 * ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED.  IN NO EVENT SHALL Bill Paul OR THE VOICES IN HIS HEAD
 * BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
 * CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
 * SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
 * INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
 * CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
 * ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF
 * THE POSSIBILITY OF SUCH DAMAGE.
 */

#include "kernel/rtl8168.hpp"

#include <stddef.h>
#include <stdint.h>

#include "abi/savanxp_abi.h"
#include "arch/cpu.hpp"
#include "arch/x86/io.hpp"
#include "kernel/console.hpp"
#include "kernel/pci.hpp"
#include "kernel/physical_memory.hpp"
#include "kernel/string.hpp"
#include "kernel/timer.hpp"
#include "kernel/uacpi_glue.hpp"
#include "kernel/vmm.hpp"

// The Realtek gigabit family in C+ mode, reduced to what the nic:: layer
// needs: no offloads, no VLAN, no jumbo frames, no interrupt moderation, and
// a fixed 1500-byte MTU. Provenance in docs/third-party/THIRD_PARTY_PROVENANCE.md.

namespace {

using namespace arch::x86::port_io;

constexpr int kDriverPriority = 100;

constexpr uint16_t kVendorRealtek = 0x10ecu;
constexpr uint16_t kSupportedDevices[] = {
    0x8168u, // RTL8111/8168, PCIe gigabit
    0x8161u, // RTL8168 variant
    0x8136u, // RTL810x, PCIe 10/100
    0x8169u, // RTL8169/8110, PCI gigabit
    0x8167u, // RTL8169SC/8110SC
};

constexpr uint8_t kPciCommandOffset = 0x04;
constexpr uint16_t kPciCommandIo = 1u << 0;
constexpr uint16_t kPciCommandMemory = 1u << 1;
constexpr uint16_t kPciCommandBusMaster = 1u << 2;
constexpr uint16_t kPciCommandInterruptDisable = 1u << 10;
constexpr uint8_t kPciCapPowerManagement = 0x01;
constexpr uint8_t kPciCapExpress = 0x10;
constexpr uint8_t kPciPmControlStatus = 0x04;
constexpr uint8_t kPciExpressLinkControl = 0x10;
constexpr uint16_t kLinkControlAspmMask = 0x0003;
constexpr uint16_t kLinkControlClockPm = 0x0100;

// Registers.
constexpr uint16_t kRegMac0 = 0x00;
constexpr uint16_t kRegMulticast0 = 0x08;
constexpr uint16_t kRegMulticast4 = 0x0c;
constexpr uint16_t kRegTxRingLow = 0x20;
constexpr uint16_t kRegTxRingHigh = 0x24;
constexpr uint16_t kRegCommand = 0x37;
constexpr uint16_t kRegTxPoll = 0x38;
constexpr uint16_t kRegInterruptMask = 0x3c;
constexpr uint16_t kRegInterruptStatus = 0x3e;
constexpr uint16_t kRegTxConfig = 0x40;
constexpr uint16_t kRegRxConfig = 0x44;
constexpr uint16_t kRegMissedPackets = 0x4c;
constexpr uint16_t kRegEepromCommand = 0x50;
constexpr uint16_t kRegConfig1 = 0x52;
constexpr uint16_t kRegPhyAccess = 0x60;
constexpr uint16_t kRegPhyStatus = 0x6c;
constexpr uint16_t kRegPowerManagement = 0x6f;
constexpr uint16_t kRegLinkDownPowerSaving = 0x82;
constexpr uint16_t kRegMaxRxPacket = 0xda;
constexpr uint16_t kRegCPlusCommand = 0xe0;
constexpr uint16_t kRegRxRingLow = 0xe4;
constexpr uint16_t kRegRxRingHigh = 0xe8;
constexpr uint16_t kRegEarlyTxThreshold = 0xec;
constexpr uint16_t kRegMisc = 0xf0;

constexpr uint8_t kCommandRxEnable = 0x08;
constexpr uint8_t kCommandTxEnable = 0x04;
constexpr uint8_t kCommandReset = 0x10;
constexpr uint8_t kTxPollStart = 0x40;

constexpr uint16_t kIntRxOk = 0x0001;
constexpr uint16_t kIntRxError = 0x0002;
constexpr uint16_t kIntTxOk = 0x0004;
constexpr uint16_t kIntTxError = 0x0008;
constexpr uint16_t kIntRxUnavailable = 0x0010;
constexpr uint16_t kIntRxFifoOverflow = 0x0040;
constexpr uint16_t kIntSystemError = 0x8000;
constexpr uint16_t kInterruptMask = kIntRxOk | kIntRxError | kIntTxOk | kIntTxError |
    kIntRxUnavailable | kIntRxFifoOverflow | kIntSystemError;

constexpr uint32_t kTxConfigRevisionMask = 0x7c800000u;
constexpr uint32_t kTxConfigDefault = 0x03000700u;  // standard IFG, 2048-byte DMA bursts
constexpr uint32_t kRxConfigDefault = 0x0000ff00u;  // no FIFO threshold, unlimited DMA, 64K
constexpr uint32_t kRxConfigEarlyOff = 0x00003800u;
constexpr uint32_t kRxConfigEarlyOffV2 = 0x00000800u;
constexpr uint32_t kRxAcceptPhysical = 0x00000002u;
constexpr uint32_t kRxAcceptMulticast = 0x00000004u;
constexpr uint32_t kRxAcceptBroadcast = 0x00000008u;
constexpr uint32_t kMiscRxdvGate = 0x00080000u;

constexpr uint16_t kCPlusTxEnable = 0x0001;
constexpr uint16_t kCPlusRxEnable = 0x0002;
constexpr uint16_t kCPlusPciMultiRw = 0x0008;
constexpr uint16_t kCPlusMacStatDisable = 0x0080;

constexpr uint8_t kEepromWriteConfig = 0xc0;
constexpr uint8_t kEepromOff = 0x00;
constexpr uint8_t kConfig1DriverLoaded = 0x20;

constexpr uint32_t kPhyAccessFlag = 0x80000000u;
constexpr uint32_t kPhyDataMask = 0x0000ffffu;
constexpr uint8_t kPhyStatusLink = 0x02;
constexpr uint8_t kPhyStatusFullDuplex = 0x01;
constexpr uint8_t kPhyStatus10 = 0x04;
constexpr uint8_t kPhyStatus100 = 0x08;
constexpr uint8_t kPhyStatus1000 = 0x10;

// MII registers of the internal PHY.
constexpr int kMiiControl = 0x00;
constexpr int kMiiAdvertise = 0x04;
constexpr int kMiiGigabitControl = 0x09;
constexpr int kMiiPage = 0x1f;
constexpr int kMiiPowerSaving = 0x0e;
constexpr uint16_t kMiiControlReset = 0x8000;
constexpr uint16_t kMiiControlAutoNeg = 0x1000;
constexpr uint16_t kMiiControlRestartAutoNeg = 0x0200;
constexpr uint16_t kMiiAdvertiseAll = 0x0de1;  // 10/100 half and full, pause, CSMA
constexpr uint16_t kMiiAdvertiseGigabit = 0x0300;  // 1000BASE-T half and full

// Descriptors. RX length is 14 bits on the gigabit chips; the gigabit status
// layout puts the error summary one bit above the 8139C+ one.
constexpr uint32_t kDescOwn = 0x80000000u;
constexpr uint32_t kDescEndOfRing = 0x40000000u;
constexpr uint32_t kDescFirst = 0x20000000u;
constexpr uint32_t kDescLast = 0x10000000u;
constexpr uint32_t kTxStatusErrorSummary = 0x00800000u;
constexpr uint32_t kRxStatusErrorSummary = 0x00200000u;
constexpr uint32_t kRxLengthMask = 0x00003fffu;

struct Descriptor {
    volatile uint32_t command_status;
    volatile uint32_t vlan;
    volatile uint32_t buffer_low;
    volatile uint32_t buffer_high;
};
static_assert(sizeof(Descriptor) == 16, "the chip reads 16-byte descriptors");

constexpr uint32_t kRingSize = 64;
constexpr uint32_t kBufferBytes = 2048;
constexpr uint32_t kMinFrameBytes = 60;
constexpr uint32_t kMaxFrameBytes = 1514;
constexpr uint32_t kCrcBytes = 4;
constexpr uint64_t kBufferPages = (kRingSize * kBufferBytes) / memory::kPageSize;

// Per-revision behaviour, the subset of re(4)'s flags this driver acts on.
enum Flags : uint32_t {
    kFlagMacReset = 1u << 0,     // old 8169: link-down power saving off after reset
    kFlagPhyWake = 1u << 1,      // PHY must be woken through MII
    kFlagPhyWakePm = 1u << 2,    // PHY power-down bit in the PM register
    kFlagMacStat = 1u << 3,      // C+ RX enable bit is "MAC statistics disable"
    kFlagEarlyOff = 1u << 4,
    kFlagEarlyOffV2 = 1u << 5,
    kFlagRxdvGated = 1u << 6,
    kFlagFastEthernet = 1u << 7,
};

struct Revision {
    uint32_t id;
    const char* name;
    uint32_t flags;
};

constexpr uint32_t kGen2 = kFlagPhyWake | kFlagMacStat;
constexpr uint32_t kGen3 = kGen2 | kFlagPhyWakePm;
constexpr uint32_t kGen4 = kGen2 | kFlagEarlyOffV2 | kFlagRxdvGated;

constexpr Revision kRevisions[] = {
    {0x00000000u, "RTL8169", kFlagMacReset},
    {0x00800000u, "RTL8169S", kFlagMacReset},
    {0x04000000u, "RTL8110S", kFlagMacReset},
    {0x10000000u, "RTL8169/8110SB", kFlagMacReset | kFlagPhyWake},
    {0x18000000u, "RTL8169/8110SCd", kFlagMacReset | kFlagPhyWake},
    {0x7cc00000u, "RTL8169SBL", kFlagMacReset | kFlagPhyWake},
    {0x98000000u, "RTL8169/8110SCe", kFlagMacReset | kFlagPhyWake},
    {0x30000000u, "RTL8168B", kGen2},
    {0x38000000u, "RTL8168B", kGen2},
    {0x38400000u, "RTL8168B", kGen2},
    {0x3c000000u, "RTL8168C/8111C", kGen2},
    {0x3c400000u, "RTL8168C/8111C", kGen2},
    {0x3c800000u, "RTL8168CP/8111CP", kGen2},
    {0x28000000u, "RTL8168D/8111D", kGen3},
    {0x28800000u, "RTL8168DP/8111DP", kGen2},
    {0x2c000000u, "RTL8168E/8111E", kGen3},
    {0x2c800000u, "RTL8168E/8111E-VL", kGen2 | kFlagEarlyOff},
    {0x48000000u, "RTL8168F/8111F", kGen2 | kFlagEarlyOff},
    {0x48800000u, "RTL8411", kGen2},
    {0x4c000000u, "RTL8168G/8111G", kGen4},
    {0x50000000u, "RTL8168EP/8111EP", kGen4},
    {0x50800000u, "RTL8168GU/8111GU", kGen4},
    {0x54000000u, "RTL8168H/8111H", kGen4},
    {0x54800000u, "RTL8168FP/8111FP", kGen4},
    {0x5c800000u, "RTL8411B", kGen4},
    {0x34000000u, "RTL8101E", kFlagPhyWake | kFlagFastEthernet},
    {0x30800000u, "RTL8100E", kFlagPhyWake | kFlagFastEthernet},
    {0x38800000u, "RTL8100E", kFlagPhyWake | kFlagFastEthernet},
    {0x34800000u, "RTL8102E", kGen2 | kFlagFastEthernet},
    {0x34c00000u, "RTL8103E", kGen2 | kFlagFastEthernet},
    {0x24800000u, "RTL8102EL", kGen2 | kFlagFastEthernet},
    {0x24c00000u, "RTL8102EL", kGen2 | kFlagFastEthernet},
    {0x24000000u, "RTL8401E", kGen3 | kFlagFastEthernet},
    {0x40800000u, "RTL8105E", kGen3 | kFlagFastEthernet},
    {0x40c00000u, "RTL8105E", kGen3 | kFlagFastEthernet},
    {0x44000000u, "RTL8402", kGen3 | kFlagFastEthernet},
    {0x44800000u, "RTL8106E", kGen3 | kFlagFastEthernet},
};

pci::DeviceInfo g_pci_device = {};
volatile uint8_t* g_mmio = nullptr;
uint16_t g_io_base = 0;
bool g_pcie = false;
uint32_t g_revision = 0;
uint32_t g_flags = 0;
const char* g_revision_name = "unknown";

memory::PageAllocation g_ring_allocation = {};
memory::PageAllocation g_rx_buffers = {};
memory::PageAllocation g_tx_buffers = {};
Descriptor* g_rx_ring = nullptr;
Descriptor* g_tx_ring = nullptr;
uint32_t g_rx_next = 0;
uint32_t g_tx_next = 0;

uint8_t g_mac[6] = {};
uint32_t g_tx_frames = 0;
uint32_t g_rx_frames = 0;
uint32_t g_tx_errors = 0;
uint32_t g_rx_errors = 0;
bool g_present = false;
bool g_up = false;
bool g_link_reported = false;

volatile uint32_t g_irq_count = 0;
bool g_irq_routed = false;

nic::Events g_events = {};

void compiler_barrier() { asm volatile("" ::: "memory"); }

void delay_us(uint32_t microseconds) {
    const uint64_t start = timer::monotonic_ns();
    const uint64_t duration = static_cast<uint64_t>(microseconds) * 1000u;
    while (timer::monotonic_ns() - start < duration) {
        asm volatile("pause");
    }
}

// Registers through the memory BAR when there is one, the I/O BAR otherwise.
uint8_t read8(uint16_t reg) {
    return g_mmio != nullptr ? g_mmio[reg] : in8(static_cast<uint16_t>(g_io_base + reg));
}
uint16_t read16(uint16_t reg) {
    return g_mmio != nullptr ? *reinterpret_cast<volatile uint16_t*>(g_mmio + reg)
                             : in16(static_cast<uint16_t>(g_io_base + reg));
}
uint32_t read32(uint16_t reg) {
    return g_mmio != nullptr ? *reinterpret_cast<volatile uint32_t*>(g_mmio + reg)
                             : in32(static_cast<uint16_t>(g_io_base + reg));
}
void write8(uint16_t reg, uint8_t value) {
    if (g_mmio != nullptr) {
        g_mmio[reg] = value;
    } else {
        out8(static_cast<uint16_t>(g_io_base + reg), value);
    }
}
void write16(uint16_t reg, uint16_t value) {
    if (g_mmio != nullptr) {
        *reinterpret_cast<volatile uint16_t*>(g_mmio + reg) = value;
    } else {
        out16(static_cast<uint16_t>(g_io_base + reg), value);
    }
}
void write32(uint16_t reg, uint32_t value) {
    if (g_mmio != nullptr) {
        *reinterpret_cast<volatile uint32_t*>(g_mmio + reg) = value;
    } else {
        out32(static_cast<uint16_t>(g_io_base + reg), value);
    }
}

void report(uint32_t net_status) {
    if (g_events.status != nullptr) {
        g_events.status(net_status);
    }
}

// The internal PHY through the GMII access register: the flag is "busy" on a
// write and "data ready" on a read.
uint16_t phy_read(int reg) {
    write32(kRegPhyAccess, static_cast<uint32_t>(reg) << 16);
    for (int attempt = 0; attempt < 20; ++attempt) {
        delay_us(25);
        const uint32_t value = read32(kRegPhyAccess);
        if ((value & kPhyAccessFlag) != 0) {
            delay_us(20);
            return static_cast<uint16_t>(value & kPhyDataMask);
        }
    }
    console::printf("rtl8168: PHY read of register %u timed out\n", static_cast<unsigned>(reg));
    return 0;
}

void phy_write(int reg, uint16_t value) {
    write32(kRegPhyAccess, kPhyAccessFlag | (static_cast<uint32_t>(reg) << 16) | value);
    for (int attempt = 0; attempt < 20; ++attempt) {
        delay_us(25);
        if ((read32(kRegPhyAccess) & kPhyAccessFlag) == 0) {
            delay_us(20);
            return;
        }
    }
    console::printf("rtl8168: PHY write of register %u timed out\n", static_cast<unsigned>(reg));
}

bool reset_chip() {
    write8(kRegCommand, kCommandReset);
    bool done = false;
    for (int attempt = 0; attempt < 1000; ++attempt) {
        delay_us(10);
        if ((read8(kRegCommand) & kCommandReset) == 0) {
            done = true;
            break;
        }
    }
    if ((g_flags & kFlagMacReset) != 0) {
        write8(kRegLinkDownPowerSaving, 1);
    }
    return done;
}

void wake_phy() {
    if ((g_flags & kFlagPhyWakePm) != 0) {
        write8(kRegPowerManagement, static_cast<uint8_t>(read8(kRegPowerManagement) | 0x80u));
        if (g_revision == 0x24000000u) {  // RTL8401E
            write8(0xd1, static_cast<uint8_t>(read8(0xd1) & ~0x08u));
        }
    }
    if ((g_flags & kFlagPhyWake) != 0) {
        phy_write(kMiiPage, 0);
        phy_write(kMiiPowerSaving, 0);
    }
}

// Reset the PHY and restart autonegotiation advertising everything it can do.
void start_autonegotiation() {
    phy_write(kMiiControl, kMiiControlReset);
    for (int attempt = 0; attempt < 100; ++attempt) {
        delay_us(100);
        if ((phy_read(kMiiControl) & kMiiControlReset) == 0) {
            break;
        }
    }
    phy_write(kMiiAdvertise, kMiiAdvertiseAll);
    if ((g_flags & kFlagFastEthernet) == 0) {
        phy_write(kMiiGigabitControl, kMiiAdvertiseGigabit);
    }
    phy_write(kMiiControl, kMiiControlAutoNeg | kMiiControlRestartAutoNeg);
}

void log_link(uint8_t status) {
    if ((status & kPhyStatusLink) == 0) {
        console::write("rtl8168: no link yet\n");
        return;
    }
    const unsigned speed = (status & kPhyStatus1000) != 0 ? 1000u
        : (status & kPhyStatus100) != 0 ? 100u
        : (status & kPhyStatus10) != 0 ? 10u
        : 0u;
    console::printf("rtl8168: link up, %u Mbit/s, %s duplex\n",
        speed, (status & kPhyStatusFullDuplex) != 0 ? "full" : "half");
}

void arm_rx(uint32_t index) {
    uint32_t command = kBufferBytes | kDescOwn;
    if (index == kRingSize - 1) {
        command |= kDescEndOfRing;
    }
    g_rx_ring[index].vlan = 0;
    compiler_barrier();
    g_rx_ring[index].command_status = command;
}

void init_rings() {
    memset(g_ring_allocation.virtual_address, 0, g_ring_allocation.page_count * memory::kPageSize);
    g_rx_ring = static_cast<Descriptor*>(g_ring_allocation.virtual_address);
    g_tx_ring = reinterpret_cast<Descriptor*>(
        static_cast<uint8_t*>(g_ring_allocation.virtual_address) + memory::kPageSize);

    for (uint32_t index = 0; index < kRingSize; ++index) {
        const uint64_t rx = g_rx_buffers.physical_address + static_cast<uint64_t>(index) * kBufferBytes;
        g_rx_ring[index].buffer_low = static_cast<uint32_t>(rx);
        g_rx_ring[index].buffer_high = static_cast<uint32_t>(rx >> 32);
        arm_rx(index);

        const uint64_t tx = g_tx_buffers.physical_address + static_cast<uint64_t>(index) * kBufferBytes;
        g_tx_ring[index].buffer_low = static_cast<uint32_t>(tx);
        g_tx_ring[index].buffer_high = static_cast<uint32_t>(tx >> 32);
        g_tx_ring[index].command_status = index == kRingSize - 1 ? kDescEndOfRing : 0;
    }
    g_rx_next = 0;
    g_tx_next = 0;
}

bool allocate_dma() {
    // Everything below 4 GiB, so the high halves stay zero on chips whose PCI
    // variants cannot do dual-address cycles. One page per ring: 64 16-byte
    // descriptors, page alignment covers the chip's 256-byte requirement.
    if (g_ring_allocation.physical_address == 0 &&
        !memory::allocate_contiguous_pages_below(2, memory::kDma32Limit, g_ring_allocation)) {
        return false;
    }
    if (g_rx_buffers.physical_address == 0 &&
        !memory::allocate_contiguous_pages_below(kBufferPages, memory::kDma32Limit, g_rx_buffers)) {
        return false;
    }
    if (g_tx_buffers.physical_address == 0 &&
        !memory::allocate_contiguous_pages_below(kBufferPages, memory::kDma32Limit, g_tx_buffers)) {
        return false;
    }
    return true;
}

bool transmit(const void* frame, size_t length) {
    if (!g_up || frame == nullptr || length == 0 || length > kMaxFrameBytes) {
        ++g_tx_errors;
        report(SAVANXP_NET_STATUS_TX_FAILED);
        return false;
    }

    const uint32_t index = g_tx_next;
    Descriptor& descriptor = g_tx_ring[index];
    if ((descriptor.command_status & kDescOwn) != 0) {
        // The previous frame on this slot never completed.
        ++g_tx_errors;
        report(SAVANXP_NET_STATUS_TX_TIMEOUT);
        return false;
    }

    // Pad short frames by hand: not every revision pads on its own.
    auto* buffer = static_cast<uint8_t*>(g_tx_buffers.virtual_address) + static_cast<size_t>(index) * kBufferBytes;
    const size_t wire_length = length < kMinFrameBytes ? kMinFrameBytes : length;
    memcpy(buffer, frame, length);
    if (wire_length > length) {
        memset(buffer + length, 0, wire_length - length);
    }

    uint32_t command = kDescOwn | kDescFirst | kDescLast | static_cast<uint32_t>(wire_length);
    if (index == kRingSize - 1) {
        command |= kDescEndOfRing;
    }
    descriptor.vlan = 0;
    compiler_barrier();
    descriptor.command_status = command;
    compiler_barrier();
    write8(kRegTxPoll, kTxPollStart);
    g_tx_next = (index + 1) % kRingSize;

    const uint64_t deadline = timer::monotonic_ns() + 50000000u;  // 50 ms
    while (timer::monotonic_ns() < deadline) {
        const uint32_t status = descriptor.command_status;
        if ((status & kDescOwn) == 0) {
            if ((status & kTxStatusErrorSummary) != 0) {
                ++g_tx_errors;
                report(SAVANXP_NET_STATUS_TX_FAILED);
                return false;
            }
            ++g_tx_frames;
            return true;
        }
        asm volatile("pause");
    }
    ++g_tx_errors;
    report(SAVANXP_NET_STATUS_TX_TIMEOUT);
    return false;
}

void poll_receive() {
    if (!g_up) {
        return;
    }

    // Called from the INTx handler and from the stack's wait loops: interrupts
    // off keeps one core from preempting itself in the middle of the ring.
    const uint64_t flags = arch::save_and_disable_interrupts();
    const uint16_t pending = read16(kRegInterruptStatus);
    if (pending != 0 && pending != 0xffffu) {
        write16(kRegInterruptStatus, pending);
    }
    if (!g_link_reported && (read8(kRegPhyStatus) & kPhyStatusLink) != 0) {
        g_link_reported = true;
        log_link(read8(kRegPhyStatus));
    }

    for (uint32_t budget = 0; budget < kRingSize; ++budget) {
        const uint32_t index = g_rx_next;
        const uint32_t status = g_rx_ring[index].command_status;
        if ((status & kDescOwn) != 0) {
            break;
        }
        compiler_barrier();

        const uint32_t total = status & kRxLengthMask;
        const bool whole = (status & (kDescFirst | kDescLast)) == (kDescFirst | kDescLast);
        if (!whole || (status & kRxStatusErrorSummary) != 0 ||
            total <= kCrcBytes || total > kBufferBytes) {
            // Fragments of an oversized frame and damaged frames are dropped.
            ++g_rx_errors;
            report(SAVANXP_NET_STATUS_RX_INVALID);
        } else {
            ++g_rx_frames;
            if (g_events.frame != nullptr) {
                const auto* data = static_cast<const uint8_t*>(g_rx_buffers.virtual_address) +
                    static_cast<size_t>(index) * kBufferBytes;
                g_events.frame(data, total - kCrcBytes);
            }
        }

        arm_rx(index);
        g_rx_next = (index + 1) % kRingSize;
    }
    arch::restore_interrupts(flags);
}

void irq_handler() {
    g_irq_count = g_irq_count + 1;
    if (g_irq_count == 1) {
        console::write("rtl8168: first INTx received through the IOAPIC\n");
    }
    poll_receive();
}

void attach(const nic::Events& events) { g_events = events; }

bool is_up() { return g_up; }

const uint8_t* mac_address() { return g_mac; }

void get_stats(nic::Stats& stats) {
    stats.tx_frames = g_tx_frames;
    stats.rx_frames = g_rx_frames;
    stats.tx_errors = g_tx_errors;
    stats.rx_errors = g_rx_errors;
}

bool bring_up() {
    if (!g_present) {
        return false;
    }
    if (g_up) {
        return true;
    }
    if (!allocate_dma()) {
        console::write("rtl8168: no memory below 4 GiB for the rings\n");
        report(SAVANXP_NET_STATUS_BRING_UP_FAILED);
        return false;
    }

    write16(kRegInterruptMask, 0);
    if (!reset_chip()) {
        console::write("rtl8168: reset never completed\n");
        report(SAVANXP_NET_STATUS_BRING_UP_FAILED);
        return false;
    }

    // C+ mode has to be configured before anything else.
    uint16_t cplus = kCPlusTxEnable | kCPlusPciMultiRw;
    cplus |= (g_flags & kFlagMacStat) != 0 ? kCPlusMacStatDisable : kCPlusRxEnable;
    write16(kRegCPlusCommand, cplus);

    init_rings();
    const uint64_t rx_ring = g_ring_allocation.physical_address;
    const uint64_t tx_ring = g_ring_allocation.physical_address + memory::kPageSize;
    write32(kRegRxRingHigh, static_cast<uint32_t>(rx_ring >> 32));
    write32(kRegRxRingLow, static_cast<uint32_t>(rx_ring));
    write32(kRegTxRingHigh, static_cast<uint32_t>(tx_ring >> 32));
    write32(kRegTxRingLow, static_cast<uint32_t>(tx_ring));

    if ((g_flags & kFlagRxdvGated) != 0) {
        write32(kRegMisc, read32(kRegMisc) & ~kMiscRxdvGate);
    }

    write32(kRegTxConfig, kTxConfigDefault);
    write8(kRegEarlyTxThreshold, 16);
    uint32_t rx_config = kRxConfigDefault;
    if ((g_flags & kFlagEarlyOff) != 0) {
        rx_config |= kRxConfigEarlyOff;
    } else if ((g_flags & kFlagEarlyOffV2) != 0) {
        rx_config |= kRxConfigEarlyOffV2;
    }
    write32(kRegRxConfig, rx_config);

    write8(kRegCommand, kCommandTxEnable | kCommandRxEnable);

    // Our address, broadcast and every multicast group: the stack filters.
    write32(kRegMulticast0, 0xffffffffu);
    write32(kRegMulticast4, 0xffffffffu);
    write32(kRegRxConfig, rx_config | kRxAcceptPhysical | kRxAcceptBroadcast | kRxAcceptMulticast);

    write16(kRegInterruptStatus, 0xffff);
    write32(kRegMissedPackets, 0);
    write16(kRegMaxRxPacket, static_cast<uint16_t>(kBufferBytes));
    write8(kRegEepromCommand, kEepromWriteConfig);
    write8(kRegConfig1, static_cast<uint8_t>(read8(kRegConfig1) | kConfig1DriverLoaded));
    write8(kRegEepromCommand, kEepromOff);

    wake_phy();
    start_autonegotiation();

    g_link_reported = false;
    g_up = true;
    if (g_irq_routed) {
        write16(kRegInterruptMask, kInterruptMask);
    }

    // Autonegotiation takes about two seconds. Wait for it once so the first
    // ARP does not go out on a dead link; poll_receive reports a late link.
    const uint64_t deadline = timer::monotonic_ns() + 4000000000ull;
    while (timer::monotonic_ns() < deadline) {
        if ((read8(kRegPhyStatus) & kPhyStatusLink) != 0) {
            break;
        }
        delay_us(10000);
    }
    const uint8_t link = read8(kRegPhyStatus);
    log_link(link);
    g_link_reported = (link & kPhyStatusLink) != 0;
    return true;
}

bool supported_device(uint16_t device_id) {
    for (const uint16_t candidate : kSupportedDevices) {
        if (candidate == device_id) {
            return true;
        }
    }
    return false;
}

bool find_device(pci::DeviceInfo& found) {
    const size_t count = pci::device_count();
    for (size_t index = 0; index < count; ++index) {
        pci::DeviceInfo info = {};
        if (pci::device_info(index, info) && info.vendor_id == kVendorRealtek &&
            supported_device(info.device_id)) {
            found = info;
            return true;
        }
    }
    return false;
}

// The memory BAR first, as re(4) does: 64-bit at BAR2 on PCIe parts, 32-bit
// at BAR1 on PCI ones, and the I/O BAR0 as the last resort.
bool map_registers() {
    const uint8_t memory_bars[] = {2, 1};
    for (const uint8_t bar_index : memory_bars) {
        pci::BarInfo bar = {};
        if (!pci::bar_info(g_pci_device, bar_index, bar) || !bar.present || bar.io_space || bar.base == 0) {
            continue;
        }
        void* virtual_base = nullptr;
        const size_t size = bar.size < memory::kPageSize ? memory::kPageSize : static_cast<size_t>(bar.size);
        if (vm::map_kernel_mmio(bar.base, size, vm::kPageWriteThrough | vm::kPageCacheDisable, &virtual_base)) {
            g_mmio = static_cast<volatile uint8_t*>(virtual_base);
            return true;
        }
    }
    pci::BarInfo io = {};
    if (pci::bar_info(g_pci_device, 0, io) && io.present && io.io_space && io.base != 0) {
        g_io_base = static_cast<uint16_t>(io.base);
        return true;
    }
    return false;
}

// Full power, and no ASPM: re(4) turns L0s/L1 and clock PM off on every part.
void prepare_pci() {
    const uint8_t bus = g_pci_device.bus;
    const uint8_t slot = g_pci_device.slot;
    const uint8_t function = g_pci_device.function;

    uint8_t cap = 0;
    if (pci::find_capability(g_pci_device, kPciCapPowerManagement, cap)) {
        const uint16_t pm = pci::read_config_u16(bus, slot, function, static_cast<uint8_t>(cap + kPciPmControlStatus));
        if ((pm & 0x3u) != 0) {
            pci::write_config_u16(bus, slot, function, static_cast<uint8_t>(cap + kPciPmControlStatus),
                static_cast<uint16_t>(pm & ~0x3u));
            delay_us(10000);
        }
    }
    if (pci::find_capability(g_pci_device, kPciCapExpress, cap)) {
        g_pcie = true;
        const uint8_t offset = static_cast<uint8_t>(cap + kPciExpressLinkControl);
        const uint16_t control = pci::read_config_u16(bus, slot, function, offset);
        pci::write_config_u16(bus, slot, function, offset,
            static_cast<uint16_t>(control & ~(kLinkControlAspmMask | kLinkControlClockPm)));
    }
}

void identify_revision() {
    g_revision = read32(kRegTxConfig) & kTxConfigRevisionMask;
    for (const Revision& revision : kRevisions) {
        if (revision.id == g_revision) {
            g_revision_name = revision.name;
            g_flags = revision.flags;
            return;
        }
    }
    g_revision_name = "unknown";
    g_flags = 0;
}

// Recognize the device and leave it idle: the real bring-up waits for
// NET_IOC_UP on /dev/net0.
bool probe() {
    if (!find_device(g_pci_device)) {
        return false;
    }

    prepare_pci();
    if (!map_registers()) {
        console::printf("rtl8168: %x:%x has no usable BAR\n",
            static_cast<unsigned>(g_pci_device.vendor_id), static_cast<unsigned>(g_pci_device.device_id));
        return false;
    }

    // _PRT on the root bus is keyed by device number. route_pci_intx does not
    // walk bridges, so behind a PCIe root port it would resolve the wrong
    // line: route only devices on bus 0 and poll everything else.
    if (g_pci_device.bus == 0) {
        const uint8_t vector = uacpi_glue::route_pci_intx(
            g_pci_device.bus, g_pci_device.slot, g_pci_device.function, &irq_handler);
        g_irq_routed = vector != 0;
    }

    uint16_t command = pci::read_config_u16(g_pci_device.bus, g_pci_device.slot, g_pci_device.function, kPciCommandOffset);
    command = static_cast<uint16_t>(command | kPciCommandBusMaster | (g_mmio != nullptr ? kPciCommandMemory : kPciCommandIo));
    if (g_irq_routed) {
        command = static_cast<uint16_t>(command & ~kPciCommandInterruptDisable);
    } else {
        command = static_cast<uint16_t>(command | kPciCommandInterruptDisable);
    }
    pci::write_config_u16(g_pci_device.bus, g_pci_device.slot, g_pci_device.function, kPciCommandOffset, command);

    identify_revision();
    write16(kRegInterruptMask, 0);
    for (uint16_t index = 0; index < 6; ++index) {
        g_mac[index] = read8(static_cast<uint16_t>(kRegMac0 + index));
    }

    console::printf(
        "rtl8168: %x:%x at %u:%u.%u, %s (TxConfig 0x%x), %s%s, mac %x:%x:%x:%x:%x:%x, %s\n",
        static_cast<unsigned>(g_pci_device.vendor_id),
        static_cast<unsigned>(g_pci_device.device_id),
        static_cast<unsigned>(g_pci_device.bus),
        static_cast<unsigned>(g_pci_device.slot),
        static_cast<unsigned>(g_pci_device.function),
        g_revision_name,
        static_cast<unsigned>(g_revision >> 16),
        g_mmio != nullptr ? "mmio" : "port i/o",
        g_pcie ? ", pcie" : "",
        static_cast<unsigned>(g_mac[0]), static_cast<unsigned>(g_mac[1]),
        static_cast<unsigned>(g_mac[2]), static_cast<unsigned>(g_mac[3]),
        static_cast<unsigned>(g_mac[4]), static_cast<unsigned>(g_mac[5]),
        g_irq_routed ? "intx" : "polled");
    g_present = true;
    return true;
}

const nic::Nic kNic = {
    &attach,
    &bring_up,
    &is_up,
    &mac_address,
    &transmit,
    &poll_receive,
    &get_stats,
};

const nic::Nic& nic_of() { return kNic; }

const nic::Driver kDriver = {
    "rtl8168",
    kDriverPriority,
    &probe,
    &nic_of,
};

} // namespace

namespace rtl8168 {

const nic::Driver& driver() { return kDriver; }

} // namespace rtl8168
