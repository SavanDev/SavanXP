#include <stdint.h>

#include "kernel/console.hpp"
#include "kernel/cpu.hpp"
#include "kernel/process.hpp"

namespace {

constexpr uint16_t kKernelCodeSelector = 0x08;
constexpr uint16_t kKernelDataSelector = 0x10;
constexpr uint16_t kTssSelector = 0x28;
constexpr uint8_t kIrqCount = 16;
constexpr uint8_t kPicVectorBase = 32;
constexpr uint8_t kSyscallVector = 0x80;
constexpr uint16_t kIdtEntryCount = 129;
constexpr uint8_t kInterruptGate = 0x8e;
constexpr uint8_t kUserInterruptGate = 0xee;
constexpr uint16_t kPicMasterCommand = 0x20;
constexpr uint16_t kPicMasterData = 0x21;
constexpr uint16_t kPicSlaveCommand = 0xa0;
constexpr uint16_t kPicSlaveData = 0xa1;
constexpr uint8_t kPicEoi = 0x20;
constexpr uint32_t kApicBaseMsr = 0x1b;
constexpr uint32_t kApicBaseEnable = 1u << 11;
constexpr uint32_t kApicBaseX2ApicEnable = 1u << 10;
constexpr uint32_t kX2ApicMsrBase = 0x800;
constexpr uint32_t kApicEoi = 0x0b0;
constexpr uint32_t kApicSpuriousVector = 0x0f0;
constexpr uint32_t kApicLvtTimer = 0x320;
constexpr uint32_t kApicInitialCount = 0x380;
constexpr uint32_t kApicCurrentCount = 0x390;
constexpr uint32_t kApicDivideConfiguration = 0x3e0;
constexpr uint32_t kApicSoftwareEnable = 1u << 8;
constexpr uint32_t kApicTimerPeriodic = 1u << 17;

extern "C" void x86_64_syscall_entry();
extern "C" void x86_64_timer_entry();

struct [[gnu::packed]] GdtDescriptor {
    uint16_t limit;
    uint64_t base;
};

struct [[gnu::packed]] IdtDescriptor {
    uint16_t limit;
    uint64_t base;
};

struct [[gnu::packed]] IdtEntry {
    uint16_t offset_low;
    uint16_t selector;
    uint8_t ist;
    uint8_t type_attributes;
    uint16_t offset_mid;
    uint32_t offset_high;
    uint32_t zero;
};

struct [[gnu::packed]] Tss {
    uint32_t reserved0;
    uint64_t rsp0;
    uint64_t rsp1;
    uint64_t rsp2;
    uint64_t reserved1;
    uint64_t ist1;
    uint64_t ist2;
    uint64_t ist3;
    uint64_t ist4;
    uint64_t ist5;
    uint64_t ist6;
    uint64_t ist7;
    uint64_t reserved2;
    uint16_t reserved3;
    uint16_t iomap_base;
};

struct InterruptFrame {
    uint64_t rip;
    uint64_t cs;
    uint64_t rflags;
    uint64_t rsp;
    uint64_t ss;
};

using InterruptHandler = void (*)();
using arch::x86_64::InterruptEoi;
using arch::x86_64::IrqHandler;

struct ExternalHandlerSlot {
    IrqHandler handler;
    InterruptEoi eoi;
};

uint64_t g_gdt[7] = {
    0x0000000000000000ULL,
    0x00af9a000000ffffULL,
    0x00af92000000ffffULL,
    0x00aff2000000ffffULL,
    0x00affa000000ffffULL,
    0,
    0,
};

Tss g_tss = {};
GdtDescriptor g_gdt_descriptor = {
    .limit = static_cast<uint16_t>(sizeof(g_gdt) - 1),
    .base = reinterpret_cast<uint64_t>(&g_gdt[0]),
};

IdtEntry g_idt[kIdtEntryCount] = {};
IdtDescriptor g_idt_descriptor = {
    .limit = static_cast<uint16_t>(sizeof(g_idt) - 1),
    .base = reinterpret_cast<uint64_t>(&g_idt[0]),
};

volatile uint64_t g_breakpoint_probe_hits = 0;
volatile bool g_breakpoint_probe_active = false;
ExternalHandlerSlot g_external_handlers[kIdtEntryCount] = {};
bool g_local_apic_ready = false;
bool g_local_apic_x2apic = false;

inline void out8(uint16_t port, uint8_t value) {
    asm volatile("outb %0, %1" : : "a"(value), "Nd"(port));
}

inline uint8_t in8(uint16_t port) {
    uint8_t value = 0;
    asm volatile("inb %1, %0" : "=a"(value) : "Nd"(port));
    return value;
}

void io_wait() {
    out8(0x80, 0);
}

void cpuid(uint32_t leaf, uint32_t subleaf, uint32_t& eax, uint32_t& ebx, uint32_t& ecx, uint32_t& edx) {
    asm volatile("cpuid" : "=a"(eax), "=b"(ebx), "=c"(ecx), "=d"(edx) : "a"(leaf), "c"(subleaf));
}

uint64_t read_msr(uint32_t msr) {
    uint32_t low = 0;
    uint32_t high = 0;
    asm volatile("rdmsr" : "=a"(low), "=d"(high) : "c"(msr));
    return (static_cast<uint64_t>(high) << 32) | low;
}

void write_msr(uint32_t msr, uint64_t value) {
    const uint32_t low = static_cast<uint32_t>(value);
    const uint32_t high = static_cast<uint32_t>(value >> 32);
    asm volatile("wrmsr" : : "c"(msr), "a"(low), "d"(high));
}

uint32_t x2apic_msr(uint32_t reg) {
    return kX2ApicMsrBase + (reg >> 4);
}

void write_local_apic(uint32_t reg, uint32_t value) {
    write_msr(x2apic_msr(reg), value);
}

uint32_t read_local_apic(uint32_t reg) {
    return static_cast<uint32_t>(read_msr(x2apic_msr(reg)));
}

void local_apic_eoi() {
    if (g_local_apic_ready) {
        write_local_apic(kApicEoi, 0);
    }
}

const char* exception_name(uint8_t vector) {
    switch (vector) {
        case 0: return "divide error";
        case 3: return "breakpoint";
        case 6: return "invalid opcode";
        case 13: return "general protection fault";
        case 14: return "page fault";
        default: return "exception";
    }
}

uint64_t read_cr2() {
    uint64_t value = 0;
    asm volatile("mov %%cr2, %0" : "=r"(value));
    return value;
}

[[noreturn]] void stop_on_exception(uint8_t vector, InterruptFrame* frame, uint64_t error_code, bool has_error_code) {
    console::write_line("");
    console::printf("exception: #%u %s\n", static_cast<unsigned>(vector), exception_name(vector));
    if (has_error_code) {
        console::printf("error code: 0x%llx\n", error_code);
    }
    if (vector == 14) {
        console::printf("cr2: 0x%llx\n", read_cr2());
    }
    if (frame != nullptr) {
        console::printf("rip: 0x%llx cs: 0x%llx rflags: 0x%llx\n", frame->rip, frame->cs, frame->rflags);
        console::printf("rsp: 0x%llx ss: 0x%llx\n", frame->rsp, frame->ss);
    }

    arch::x86_64::disable_interrupts();
    arch::x86_64::halt_forever();
}

void handle_exception(uint8_t vector, InterruptFrame* frame, uint64_t error_code, bool has_error_code) {
    if (vector == 3 && g_breakpoint_probe_active) {
        g_breakpoint_probe_hits = g_breakpoint_probe_hits + 1;
        return;
    }

    if (frame != nullptr && (frame->cs & 0x3) == 0x3 && process::current() != nullptr) {
        (void)error_code;
        (void)has_error_code;
        process::terminate_current_from_exception(vector);
    }

    stop_on_exception(vector, frame, error_code, has_error_code);
}

void send_pic_eoi(uint8_t irq) {
    if (irq >= 8) {
        out8(kPicSlaveCommand, kPicEoi);
    }
    out8(kPicMasterCommand, kPicEoi);
}

void dispatch_external_vector(uint8_t vector) {
    if (vector >= kIdtEntryCount) {
        return;
    }

    const ExternalHandlerSlot& slot = g_external_handlers[vector];
    if (slot.handler != nullptr) {
        slot.handler();
    }

    switch (slot.eoi) {
        case InterruptEoi::pic:
            send_pic_eoi(static_cast<uint8_t>(vector - kPicVectorBase));
            break;
        case InterruptEoi::local_apic:
            local_apic_eoi();
            break;
        case InterruptEoi::none:
        default:
            break;
    }
}

void set_idt_gate(uint8_t vector, InterruptHandler handler, uint8_t attributes) {
    const uint64_t address = reinterpret_cast<uint64_t>(handler);
    g_idt[vector] = {
        .offset_low = static_cast<uint16_t>(address & 0xffff),
        .selector = kKernelCodeSelector,
        .ist = 0,
        .type_attributes = attributes,
        .offset_mid = static_cast<uint16_t>((address >> 16) & 0xffff),
        .offset_high = static_cast<uint32_t>((address >> 32) & 0xffffffff),
        .zero = 0,
    };
}

void install_tss_descriptor() {
    const uint64_t base = reinterpret_cast<uint64_t>(&g_tss);
    const uint64_t limit = sizeof(g_tss) - 1;
    g_gdt[5] = (limit & 0xffffULL) |
        ((base & 0xffffffULL) << 16) |
        (0x89ULL << 40) |
        (((limit >> 16) & 0xfULL) << 48) |
        (((base >> 24) & 0xffULL) << 56);
    g_gdt[6] = base >> 32;
    g_tss.iomap_base = sizeof(g_tss);
}

void load_gdt() {
    asm volatile(
        "lgdt (%0)\n\t"
        "movw %1, %%ax\n\t"
        "movw %%ax, %%ds\n\t"
        "movw %%ax, %%es\n\t"
        "movw %%ax, %%fs\n\t"
        "movw %%ax, %%gs\n\t"
        "movw %%ax, %%ss\n\t"
        "pushq %2\n\t"
        "leaq 1f(%%rip), %%rax\n\t"
        "pushq %%rax\n\t"
        "lretq\n\t"
        "1:\n\t"
        :
        : "r"(&g_gdt_descriptor),
          "i"(kKernelDataSelector),
          "i"(static_cast<uint64_t>(kKernelCodeSelector))
        : "rax", "memory"
    );

    asm volatile("ltr %0" : : "r"(kTssSelector) : "memory");
}

void load_idt() {
    asm volatile("lidt (%0)" : : "r"(&g_idt_descriptor) : "memory");
}

void remap_pic() {
    out8(kPicMasterCommand, 0x11);
    io_wait();
    out8(kPicSlaveCommand, 0x11);
    io_wait();
    out8(kPicMasterData, kPicVectorBase);
    io_wait();
    out8(kPicSlaveData, kPicVectorBase + 8);
    io_wait();
    out8(kPicMasterData, 0x04);
    io_wait();
    out8(kPicSlaveData, 0x02);
    io_wait();
    out8(kPicMasterData, 0x01);
    io_wait();
    out8(kPicSlaveData, 0x01);
    io_wait();
    out8(kPicMasterData, 0xff);
    io_wait();
    out8(kPicSlaveData, 0xff);
    io_wait();
}

void run_breakpoint_probe() {
    g_breakpoint_probe_hits = 0;
    g_breakpoint_probe_active = true;
    asm volatile("int3");
    g_breakpoint_probe_active = false;

    if (g_breakpoint_probe_hits != 1) {
        console::printf("cpu: breakpoint probe failed (%llu)\n", g_breakpoint_probe_hits);
        arch::x86_64::halt_forever();
    }
}

#define DEFINE_ISR_NOERR(VECTOR) \
    __attribute__((interrupt)) void isr_##VECTOR(InterruptFrame* frame) { \
        handle_exception(VECTOR, frame, 0, false); \
    }

#define DEFINE_ISR_ERR(VECTOR) \
    __attribute__((interrupt)) void isr_##VECTOR(InterruptFrame* frame, uint64_t error_code) { \
        handle_exception(VECTOR, frame, error_code, true); \
    }

#define DEFINE_EXTERNAL_ISR(VECTOR) \
    __attribute__((interrupt)) void vector_##VECTOR(InterruptFrame* frame) { \
        (void)frame; \
        dispatch_external_vector(VECTOR); \
    }

DEFINE_ISR_NOERR(0)
DEFINE_ISR_NOERR(1)
DEFINE_ISR_NOERR(2)
DEFINE_ISR_NOERR(3)
DEFINE_ISR_NOERR(4)
DEFINE_ISR_NOERR(5)
DEFINE_ISR_NOERR(6)
DEFINE_ISR_NOERR(7)
DEFINE_ISR_ERR(8)
DEFINE_ISR_NOERR(9)
DEFINE_ISR_ERR(10)
DEFINE_ISR_ERR(11)
DEFINE_ISR_ERR(12)
DEFINE_ISR_ERR(13)
DEFINE_ISR_ERR(14)
DEFINE_ISR_NOERR(15)
DEFINE_ISR_NOERR(16)
DEFINE_ISR_ERR(17)
DEFINE_ISR_NOERR(18)
DEFINE_ISR_NOERR(19)
DEFINE_ISR_NOERR(20)
DEFINE_ISR_ERR(21)
DEFINE_ISR_NOERR(22)
DEFINE_ISR_NOERR(23)
DEFINE_ISR_NOERR(24)
DEFINE_ISR_NOERR(25)
DEFINE_ISR_NOERR(26)
DEFINE_ISR_NOERR(27)
DEFINE_ISR_NOERR(28)
DEFINE_ISR_ERR(29)
DEFINE_ISR_ERR(30)
DEFINE_ISR_NOERR(31)

DEFINE_EXTERNAL_ISR(32)
DEFINE_EXTERNAL_ISR(33)
DEFINE_EXTERNAL_ISR(34)
DEFINE_EXTERNAL_ISR(35)
DEFINE_EXTERNAL_ISR(36)
DEFINE_EXTERNAL_ISR(37)
DEFINE_EXTERNAL_ISR(38)
DEFINE_EXTERNAL_ISR(39)
DEFINE_EXTERNAL_ISR(40)
DEFINE_EXTERNAL_ISR(41)
DEFINE_EXTERNAL_ISR(42)
DEFINE_EXTERNAL_ISR(43)
DEFINE_EXTERNAL_ISR(44)
DEFINE_EXTERNAL_ISR(45)
DEFINE_EXTERNAL_ISR(46)
DEFINE_EXTERNAL_ISR(47)
#undef DEFINE_ISR_NOERR
#undef DEFINE_ISR_ERR
#undef DEFINE_EXTERNAL_ISR

void initialize_idt() {
    set_idt_gate(0, reinterpret_cast<InterruptHandler>(isr_0), kInterruptGate);
    set_idt_gate(1, reinterpret_cast<InterruptHandler>(isr_1), kInterruptGate);
    set_idt_gate(2, reinterpret_cast<InterruptHandler>(isr_2), kInterruptGate);
    set_idt_gate(3, reinterpret_cast<InterruptHandler>(isr_3), kInterruptGate);
    set_idt_gate(4, reinterpret_cast<InterruptHandler>(isr_4), kInterruptGate);
    set_idt_gate(5, reinterpret_cast<InterruptHandler>(isr_5), kInterruptGate);
    set_idt_gate(6, reinterpret_cast<InterruptHandler>(isr_6), kInterruptGate);
    set_idt_gate(7, reinterpret_cast<InterruptHandler>(isr_7), kInterruptGate);
    set_idt_gate(8, reinterpret_cast<InterruptHandler>(isr_8), kInterruptGate);
    set_idt_gate(9, reinterpret_cast<InterruptHandler>(isr_9), kInterruptGate);
    set_idt_gate(10, reinterpret_cast<InterruptHandler>(isr_10), kInterruptGate);
    set_idt_gate(11, reinterpret_cast<InterruptHandler>(isr_11), kInterruptGate);
    set_idt_gate(12, reinterpret_cast<InterruptHandler>(isr_12), kInterruptGate);
    set_idt_gate(13, reinterpret_cast<InterruptHandler>(isr_13), kInterruptGate);
    set_idt_gate(14, reinterpret_cast<InterruptHandler>(isr_14), kInterruptGate);
    set_idt_gate(15, reinterpret_cast<InterruptHandler>(isr_15), kInterruptGate);
    set_idt_gate(16, reinterpret_cast<InterruptHandler>(isr_16), kInterruptGate);
    set_idt_gate(17, reinterpret_cast<InterruptHandler>(isr_17), kInterruptGate);
    set_idt_gate(18, reinterpret_cast<InterruptHandler>(isr_18), kInterruptGate);
    set_idt_gate(19, reinterpret_cast<InterruptHandler>(isr_19), kInterruptGate);
    set_idt_gate(20, reinterpret_cast<InterruptHandler>(isr_20), kInterruptGate);
    set_idt_gate(21, reinterpret_cast<InterruptHandler>(isr_21), kInterruptGate);
    set_idt_gate(22, reinterpret_cast<InterruptHandler>(isr_22), kInterruptGate);
    set_idt_gate(23, reinterpret_cast<InterruptHandler>(isr_23), kInterruptGate);
    set_idt_gate(24, reinterpret_cast<InterruptHandler>(isr_24), kInterruptGate);
    set_idt_gate(25, reinterpret_cast<InterruptHandler>(isr_25), kInterruptGate);
    set_idt_gate(26, reinterpret_cast<InterruptHandler>(isr_26), kInterruptGate);
    set_idt_gate(27, reinterpret_cast<InterruptHandler>(isr_27), kInterruptGate);
    set_idt_gate(28, reinterpret_cast<InterruptHandler>(isr_28), kInterruptGate);
    set_idt_gate(29, reinterpret_cast<InterruptHandler>(isr_29), kInterruptGate);
    set_idt_gate(30, reinterpret_cast<InterruptHandler>(isr_30), kInterruptGate);
    set_idt_gate(31, reinterpret_cast<InterruptHandler>(isr_31), kInterruptGate);
    set_idt_gate(32, reinterpret_cast<InterruptHandler>(vector_32), kInterruptGate);
    set_idt_gate(33, reinterpret_cast<InterruptHandler>(vector_33), kInterruptGate);
    set_idt_gate(34, reinterpret_cast<InterruptHandler>(vector_34), kInterruptGate);
    set_idt_gate(35, reinterpret_cast<InterruptHandler>(vector_35), kInterruptGate);
    set_idt_gate(36, reinterpret_cast<InterruptHandler>(vector_36), kInterruptGate);
    set_idt_gate(37, reinterpret_cast<InterruptHandler>(vector_37), kInterruptGate);
    set_idt_gate(38, reinterpret_cast<InterruptHandler>(vector_38), kInterruptGate);
    set_idt_gate(39, reinterpret_cast<InterruptHandler>(vector_39), kInterruptGate);
    set_idt_gate(40, reinterpret_cast<InterruptHandler>(vector_40), kInterruptGate);
    set_idt_gate(41, reinterpret_cast<InterruptHandler>(vector_41), kInterruptGate);
    set_idt_gate(42, reinterpret_cast<InterruptHandler>(vector_42), kInterruptGate);
    set_idt_gate(43, reinterpret_cast<InterruptHandler>(vector_43), kInterruptGate);
    set_idt_gate(44, reinterpret_cast<InterruptHandler>(vector_44), kInterruptGate);
    set_idt_gate(45, reinterpret_cast<InterruptHandler>(vector_45), kInterruptGate);
    set_idt_gate(46, reinterpret_cast<InterruptHandler>(vector_46), kInterruptGate);
    set_idt_gate(47, reinterpret_cast<InterruptHandler>(vector_47), kInterruptGate);
    set_idt_gate(48, reinterpret_cast<InterruptHandler>(x86_64_timer_entry), kInterruptGate);
    set_idt_gate(kSyscallVector, reinterpret_cast<InterruptHandler>(x86_64_syscall_entry), kUserInterruptGate);
    load_idt();
}

} // namespace

namespace arch::x86_64 {

void initialize_cpu() {
    disable_interrupts();
    install_tss_descriptor();
    load_gdt();
    remap_pic();
    initialize_idt();
    run_breakpoint_probe();
}

bool register_irq_handler(uint8_t irq, IrqHandler handler) {
    if (irq >= kIrqCount) {
        return false;
    }
    return register_interrupt_handler(static_cast<uint8_t>(kPicVectorBase + irq), handler, InterruptEoi::pic);
}

bool register_interrupt_handler(uint8_t vector, IrqHandler handler, InterruptEoi eoi) {
    if (vector >= kIdtEntryCount || vector < kPicVectorBase) {
        return false;
    }
    g_external_handlers[vector] = {.handler = handler, .eoi = eoi};
    return true;
}

bool initialize_local_apic() {
    uint32_t eax = 0;
    uint32_t ebx = 0;
    uint32_t ecx = 0;
    uint32_t edx = 0;
    cpuid(1, 0, eax, ebx, ecx, edx);

    const bool apic_supported = (edx & (1u << 9)) != 0;
    const bool x2apic_supported = (ecx & (1u << 21)) != 0;
    if (!apic_supported || !x2apic_supported) {
        g_local_apic_ready = false;
        g_local_apic_x2apic = false;
        return false;
    }

    uint64_t apic_base = read_msr(kApicBaseMsr);
    apic_base |= kApicBaseEnable;
    apic_base |= kApicBaseX2ApicEnable;
    write_msr(kApicBaseMsr, apic_base);

    write_local_apic(kApicSpuriousVector, kApicSoftwareEnable | 0xff);
    g_local_apic_ready = true;
    g_local_apic_x2apic = true;
    return true;
}

bool local_apic_ready() {
    return g_local_apic_ready;
}

bool local_apic_x2apic_mode() {
    return g_local_apic_x2apic;
}

bool local_apic_start_periodic_timer(uint8_t vector, uint32_t initial_count, uint8_t divide_value) {
    if (!g_local_apic_ready || !g_local_apic_x2apic || vector >= kIdtEntryCount) {
        return false;
    }

    write_local_apic(kApicDivideConfiguration, divide_value);
    write_local_apic(kApicLvtTimer, kApicTimerPeriodic | vector);
    write_local_apic(kApicInitialCount, initial_count);
    return read_local_apic(kApicCurrentCount) != 0;
}

void initialize_syscall_gate() {
    set_idt_gate(kSyscallVector, reinterpret_cast<InterruptHandler>(x86_64_syscall_entry), kUserInterruptGate);
    load_idt();
}

void acknowledge_local_apic_interrupt() {
    local_apic_eoi();
}

void set_kernel_stack(uint64_t stack_top) {
    g_tss.rsp0 = stack_top;
}

void enable_irq(uint8_t irq) {
    if (irq >= kIrqCount) {
        return;
    }

    const uint16_t port = irq < 8 ? kPicMasterData : kPicSlaveData;
    const uint8_t bit = irq < 8 ? irq : static_cast<uint8_t>(irq - 8);
    out8(port, static_cast<uint8_t>(in8(port) & ~(1u << bit)));
    if (irq >= 8) {
        out8(kPicMasterData, static_cast<uint8_t>(in8(kPicMasterData) & ~(1u << 2)));
    }
}

void disable_irq(uint8_t irq) {
    if (irq >= kIrqCount) {
        return;
    }
    const uint16_t port = irq < 8 ? kPicMasterData : kPicSlaveData;
    const uint8_t bit = irq < 8 ? irq : static_cast<uint8_t>(irq - 8);
    out8(port, static_cast<uint8_t>(in8(port) | (1u << bit)));
}

void enable_interrupts() {
    asm volatile("sti");
}

void disable_interrupts() {
    asm volatile("cli");
}

void halt_once() {
    asm volatile("hlt");
}

[[noreturn]] void halt_forever() {
    for (;;) {
        halt_once();
    }
}

} // namespace arch::x86_64
