#include <stdint.h>

#include "kernel/console.hpp"
#include "kernel/cpu.hpp"
#include "kernel/process.hpp"
#include "kernel/vmm.hpp"

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
constexpr uint64_t kApicBaseAddressMask = 0x000ffffffffff000ULL;
constexpr uint64_t kApicMmioSize = 0x1000;
constexpr uint32_t kApicId = 0x020;
constexpr uint32_t kApicEoi = 0x0b0;
constexpr uint32_t kApicSpuriousVector = 0x0f0;
constexpr uint32_t kApicLvtTimer = 0x320;
constexpr uint32_t kApicLvtLint0 = 0x350;
constexpr uint32_t kApicInitialCount = 0x380;
constexpr uint32_t kApicCurrentCount = 0x390;
constexpr uint32_t kApicDivideConfiguration = 0x3e0;
constexpr uint32_t kApicSoftwareEnable = 1u << 8;
constexpr uint32_t kApicTimerPeriodic = 1u << 17;
constexpr uint32_t kApicLvtExtInt = 0x7u << 8;

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

// Una linea de interrupcion puede tener mas de un dueño legitimo: el SCI lo
// piden la ACPI hand-rolled y uACPI, y varios links PCI pueden caer en la misma
// GSI. El vector acumula handlers en orden de registro y todos corren antes del
// unico EOI.
constexpr uint8_t kMaxSharedHandlers = 4;

struct ExternalHandlerSlot {
    IrqHandler handlers[kMaxSharedHandlers];
    uint8_t count;
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
// Ventana MMIO del APIC local en modo xAPIC (por defecto 0xfee00000, una pagina
// uncacheable). Nula mientras no se mapee o si el CPU expone x2APIC.
volatile uint32_t* g_local_apic_mmio = nullptr;

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

// Los dos modos del APIC local hablan los mismos registros por caminos
// distintos: x2APIC via MSR (0x800 + reg/16) y xAPIC via MMIO, donde cada
// registro es un dword alineado a 16 bytes dentro de la pagina base. El resto
// del kernel solo usa registros de 32 bits (EOI, SVR, LVT, timer), asi que este
// par de helpers alcanza; el ICR de 64 bits del x2APIC no tiene consumidores
// mientras no haya SMP.
void write_local_apic(uint32_t reg, uint32_t value) {
    if (g_local_apic_x2apic) {
        write_msr(x2apic_msr(reg), value);
    } else if (g_local_apic_mmio != nullptr) {
        g_local_apic_mmio[reg / sizeof(uint32_t)] = value;
    }
}

uint32_t read_local_apic(uint32_t reg) {
    if (g_local_apic_x2apic) {
        return static_cast<uint32_t>(read_msr(x2apic_msr(reg)));
    }
    if (g_local_apic_mmio != nullptr) {
        return g_local_apic_mmio[reg / sizeof(uint32_t)];
    }
    return 0;
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
    for (uint8_t index = 0; index < slot.count; ++index) {
        if (slot.handlers[index] != nullptr) {
            slot.handlers[index]();
        }
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
// Vector 32 is PIC IRQ0 (PIT), vector 48 is the local-APIC timer, and both use
// the context-saving timer entry. Vector 49 carries device MSI-X.
DEFINE_EXTERNAL_ISR(49)
// Vectors 50-63 are the IOAPIC GSI routing pool (see kernel/ioapic.cpp). Each is
// a plain external ISR that dispatches to its registered handler and EOIs the
// local APIC. Keep this range in sync with ioapic::g_next_vector/kVectorPoolEnd.
DEFINE_EXTERNAL_ISR(50)
DEFINE_EXTERNAL_ISR(51)
DEFINE_EXTERNAL_ISR(52)
DEFINE_EXTERNAL_ISR(53)
DEFINE_EXTERNAL_ISR(54)
DEFINE_EXTERNAL_ISR(55)
DEFINE_EXTERNAL_ISR(56)
DEFINE_EXTERNAL_ISR(57)
DEFINE_EXTERNAL_ISR(58)
DEFINE_EXTERNAL_ISR(59)
DEFINE_EXTERNAL_ISR(60)
DEFINE_EXTERNAL_ISR(61)
DEFINE_EXTERNAL_ISR(62)
DEFINE_EXTERNAL_ISR(63)
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
    set_idt_gate(32, reinterpret_cast<InterruptHandler>(x86_64_timer_entry), kInterruptGate);
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
    set_idt_gate(49, reinterpret_cast<InterruptHandler>(vector_49), kInterruptGate);
    set_idt_gate(50, reinterpret_cast<InterruptHandler>(vector_50), kInterruptGate);
    set_idt_gate(51, reinterpret_cast<InterruptHandler>(vector_51), kInterruptGate);
    set_idt_gate(52, reinterpret_cast<InterruptHandler>(vector_52), kInterruptGate);
    set_idt_gate(53, reinterpret_cast<InterruptHandler>(vector_53), kInterruptGate);
    set_idt_gate(54, reinterpret_cast<InterruptHandler>(vector_54), kInterruptGate);
    set_idt_gate(55, reinterpret_cast<InterruptHandler>(vector_55), kInterruptGate);
    set_idt_gate(56, reinterpret_cast<InterruptHandler>(vector_56), kInterruptGate);
    set_idt_gate(57, reinterpret_cast<InterruptHandler>(vector_57), kInterruptGate);
    set_idt_gate(58, reinterpret_cast<InterruptHandler>(vector_58), kInterruptGate);
    set_idt_gate(59, reinterpret_cast<InterruptHandler>(vector_59), kInterruptGate);
    set_idt_gate(60, reinterpret_cast<InterruptHandler>(vector_60), kInterruptGate);
    set_idt_gate(61, reinterpret_cast<InterruptHandler>(vector_61), kInterruptGate);
    set_idt_gate(62, reinterpret_cast<InterruptHandler>(vector_62), kInterruptGate);
    set_idt_gate(63, reinterpret_cast<InterruptHandler>(vector_63), kInterruptGate);
    set_idt_gate(kSyscallVector, reinterpret_cast<InterruptHandler>(x86_64_syscall_entry), kUserInterruptGate);
    load_idt();
}

} // namespace

namespace arch::x86_64 {

/* Estado FPU/SSE limpio capturado en el boot; se copia a cada proceso nuevo
 * para que arranque con la FPU en un estado valido (MXCSR con excepciones
 * enmascaradas), en vez de un FXRSTOR sobre bytes en cero. */
alignas(16) static uint8_t g_default_fpu_state[kFpuStateSize];

void enable_fpu() {
    uint64_t cr0 = 0;
    asm volatile("mov %%cr0, %0" : "=r"(cr0));
    cr0 &= ~(1ull << 2); // EM=0: sin emulacion de coprocesador (FPU real)
    cr0 |= (1ull << 1);  // MP=1: monitor coprocessor
    asm volatile("mov %0, %%cr0" ::"r"(cr0) : "memory");

    uint64_t cr4 = 0;
    asm volatile("mov %%cr4, %0" : "=r"(cr4));
    cr4 |= (1ull << 9);  // OSFXSR: habilita FXSAVE/FXRSTOR y SSE
    cr4 |= (1ull << 10); // OSXMMEXCPT: excepciones SSE via #XM
    asm volatile("mov %0, %%cr4" ::"r"(cr4) : "memory");

    asm volatile("fninit");
    const uint32_t mxcsr = 0x1f80u; // todas las excepciones enmascaradas, round-to-nearest
    asm volatile("ldmxcsr %0" ::"m"(mxcsr));

    // Capturar el estado limpio como plantilla para los procesos nuevos.
    asm volatile("fxsave64 (%0)" ::"r"(g_default_fpu_state) : "memory");
}

void fpu_save(void* area) {
    asm volatile("fxsave64 (%0)" ::"r"(area) : "memory");
}

void fpu_restore(const void* area) {
    asm volatile("fxrstor64 (%0)" ::"r"(area) : "memory");
}

void fpu_init_area(void* area) {
    auto* destination = static_cast<uint64_t*>(area);
    const auto* source = reinterpret_cast<const uint64_t*>(g_default_fpu_state);
    for (uint64_t index = 0; index < kFpuStateSize / sizeof(uint64_t); ++index) {
        destination[index] = source[index];
    }
}

void initialize_cpu() {
    disable_interrupts();
    install_tss_descriptor();
    load_gdt();
    remap_pic();
    initialize_idt();
    enable_fpu();
    run_breakpoint_probe();
}

bool register_irq_handler(uint8_t irq, IrqHandler handler) {
    if (irq >= kIrqCount) {
        return false;
    }
    return register_interrupt_handler(static_cast<uint8_t>(kPicVectorBase + irq), handler, InterruptEoi::pic);
}

bool register_interrupt_handler(uint8_t vector, IrqHandler handler, InterruptEoi eoi) {
    if (vector >= kIdtEntryCount || vector < kPicVectorBase || handler == nullptr) {
        return false;
    }

    ExternalHandlerSlot& slot = g_external_handlers[vector];
    // Un vector compartido tiene que acordar como se acusa recibo: mezclar EOI
    // al PIC y al APIC local sobre la misma linea no significa nada.
    if (slot.count != 0 && slot.eoi != eoi) {
        return false;
    }
    for (uint8_t index = 0; index < slot.count; ++index) {
        if (slot.handlers[index] == handler) {
            return true; // idempotente: re-registrar el mismo handler no duplica
        }
    }
    if (slot.count >= kMaxSharedHandlers) {
        return false;
    }

    slot.eoi = eoi;
    slot.handlers[slot.count] = handler;
    // Publicar el handler antes que el contador: si la linea dispara en el medio,
    // el dispatcher ve la cadena vieja completa o la nueva completa, nunca una
    // entrada a medio escribir.
    asm volatile("" ::: "memory");
    slot.count = static_cast<uint8_t>(slot.count + 1);
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
    if (!apic_supported) {
        g_local_apic_ready = false;
        g_local_apic_x2apic = false;
        return false;
    }

    uint64_t apic_base = read_msr(kApicBaseMsr);
    apic_base |= kApicBaseEnable;

    if (x2apic_supported) {
        apic_base |= kApicBaseX2ApicEnable;
        write_msr(kApicBaseMsr, apic_base);
        g_local_apic_x2apic = true;
    } else {
        // Sin x2APIC (VirtualBox) el APIC local sigue existiendo: se habla por
        // MMIO. Mapear la pagina que apunta el IA32_APIC_BASE MSR una sola vez;
        // vm::map_kernel_mmio ya fuerza cache-disable, que es obligatorio aca.
        if (g_local_apic_mmio == nullptr) {
            void* virtual_base = nullptr;
            if (!vm::map_kernel_mmio(
                    apic_base & kApicBaseAddressMask,
                    kApicMmioSize,
                    vm::kPagePresent | vm::kPageWrite | vm::kPageCacheDisable,
                    &virtual_base)) {
                console::printf("cpu: no se pudo mapear el APIC local (xAPIC)\n");
                g_local_apic_ready = false;
                g_local_apic_x2apic = false;
                return false;
            }
            g_local_apic_mmio = static_cast<volatile uint32_t*>(virtual_base);
        }
        write_msr(kApicBaseMsr, apic_base & ~static_cast<uint64_t>(kApicBaseX2ApicEnable));
        g_local_apic_x2apic = false;
    }

    write_local_apic(kApicSpuriousVector, kApicSoftwareEnable | 0xff);

    // Virtual wire. Al software-enablear el APIC el bit de mascara de LINT0 pasa
    // a tener efecto, y hay firmware que lo deja enmascarado (VirtualBox lo hace
    // cuando el I/O APIC esta activo): eso corta el cable del 8259 al CPU y mata
    // todo el camino PIC legacy, incluido el fallback del PIT. Reponer LINT0 como
    // ExtINT desenmascarado deja al 8259 entregando de nuevo. Es seguro aunque no
    // se use: si el PIC esta enmascarado nunca asierta INTR, y monocore cumple la
    // regla de un solo ExtINT por sistema.
    write_local_apic(kApicLvtLint0, kApicLvtExtInt);

    g_local_apic_ready = true;
    console::printf(
        "cpu: APIC local en modo %s (id %u)\n",
        g_local_apic_x2apic ? "x2APIC" : "xAPIC",
        static_cast<unsigned>(local_apic_id()));
    return true;
}

bool local_apic_ready() {
    return g_local_apic_ready;
}

bool local_apic_x2apic_mode() {
    return g_local_apic_x2apic;
}

uint32_t local_apic_id() {
    if (!g_local_apic_ready) {
        return 0;
    }
    // En xAPIC el id vive en los bits [31:24] del registro; en x2APIC es el
    // registro entero.
    const uint32_t raw = read_local_apic(kApicId);
    return g_local_apic_x2apic ? raw : (raw >> 24);
}

bool local_apic_start_oneshot_timer(uint8_t vector, uint32_t initial_count, uint8_t divide_value) {
    if (!g_local_apic_ready || vector >= kIdtEntryCount || initial_count == 0) {
        return false;
    }

    write_local_apic(kApicDivideConfiguration, divide_value);
    write_local_apic(kApicLvtTimer, vector);
    write_local_apic(kApicInitialCount, initial_count);
    return read_local_apic(kApicCurrentCount) != 0;
}

bool local_apic_start_periodic_timer(uint8_t vector, uint32_t initial_count, uint8_t divide_value) {
    if (!g_local_apic_ready || vector >= kIdtEntryCount) {
        return false;
    }

    write_local_apic(kApicDivideConfiguration, divide_value);
    write_local_apic(kApicLvtTimer, kApicTimerPeriodic | vector);
    write_local_apic(kApicInitialCount, initial_count);
    return read_local_apic(kApicCurrentCount) != 0;
}

uint32_t local_apic_current_timer_count() {
    if (!g_local_apic_ready) {
        return 0;
    }
    return read_local_apic(kApicCurrentCount);
}

void initialize_syscall_gate() {
    set_idt_gate(kSyscallVector, reinterpret_cast<InterruptHandler>(x86_64_syscall_entry), kUserInterruptGate);
    load_idt();
}

void acknowledge_local_apic_interrupt() {
    local_apic_eoi();
}

void acknowledge_pic_irq(uint8_t irq) {
    if (irq < kIrqCount) {
        send_pic_eoi(irq);
    }
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
