#include "cpu.hpp"
#include "context.hpp"
#include "process.hpp"
#include "paging.hpp"
#include "lapic.hpp"
#include "kernel/string.hpp"

#include <stdint.h>

#include "serial.hpp"
#include "syscall.hpp"

namespace i386 {

namespace {

/* Descriptores de 32 bits: base 0, limite 4 GiB (0xFFFFF con granularidad de
 * pagina) y dos bytes de flags. El de acceso (quinto byte: 0x92, 0x9A, 0xF2 o
 * 0xFA) lleva P en el bit 7, el DPL en 6-5, S en 4 y el tipo en 3-0 (0xA es
 * codigo ejecutable y legible, 0x2 data). El de granularidad (sexto byte,
 * 0xCF) lleva G en 7, D/B en 6 y L en 5 a cero. El DPL 3 del par de usuario va
 * en el byte de acceso, no en los 16 bits altos: con 0xDA ahi el descriptor es
 * DPL 2 y el iret falla con #GP(#SS) al cargar el selector de datos. */
constexpr uint64_t kKernelCodeEntry = 0x00CF9A000000FFFFull;
constexpr uint64_t kKernelDataEntry = 0x00CF92000000FFFFull;
constexpr uint64_t kUserDataEntry = 0x00CFF2000000FFFFull;
constexpr uint64_t kUserCodeEntry = 0x00CFFA000000FFFFull;

// Five fixed entries followed by one eight-byte TSS descriptor per CPU.
constexpr uint32_t kEntryCount = 5 + cpu::kMaxCpus;

// Architectural 104-byte protected-mode TSS. Unused selector slots include
// their reserved halfword. The I/O bitmap starts beyond the descriptor limit,
// denying ring-3 port I/O rather than interpreting zeroed TSS fields as a bitmap.
struct TaskStateSegment {
    uint32_t previous_tss;
    uint32_t esp0;
    uint32_t ss0;
    uint32_t esp1;
    uint32_t ss1;
    uint32_t esp2;
    uint32_t ss2;
    uint32_t cr3;
    uint32_t eip;
    uint32_t eflags;
    uint32_t eax;
    uint32_t ecx;
    uint32_t edx;
    uint32_t ebx;
    uint32_t esp;
    uint32_t ebp;
    uint32_t esi;
    uint32_t edi;
    uint32_t es;
    uint32_t cs;
    uint32_t ss;
    uint32_t ds;
    uint32_t fs;
    uint32_t gs;
    uint32_t ldt;
    uint16_t trap;
    uint16_t iomap_base;
};
static_assert(sizeof(TaskStateSegment) == 104);
static_assert(__builtin_offsetof(TaskStateSegment, esp0) == 4);
static_assert(__builtin_offsetof(TaskStateSegment, iomap_base) == 102);

/* lgdt y lidt leen seis bytes seguidos: limite (2) y base (4). Sin packed el
 * compilador mete relleno entre ambos y el processor leeria relleno como parte
 * de la base. */
struct [[gnu::packed]] TableDescriptor {
    uint16_t limit;
    uint32_t base;
};

/* Puerta de IDT de 8 bytes: offset de 32 bits partido en dos, selector y
 * atributos (tipo en los 4 bits bajos, DPL en 5-6, P en 7). */
struct IdtEntry {
    uint16_t offset_low;
    uint16_t selector;
    uint8_t zero;
    uint8_t type_attributes;
    uint16_t offset_high;
};

TaskStateSegment g_tss[cpu::kMaxCpus];
uint64_t g_gdt[kEntryCount];
IdtEntry g_idt[256];

static_assert(sizeof(IdtEntry) == 8, "las puertas de IDT de 32 bits ocupan 8 bytes");

constexpr uint8_t kTypeInterruptGate = 0x8E; /* presente, DPL 0 */
constexpr uint8_t kTypeUserInterruptGate = 0xEE; /* presente, DPL 3 */

/* Los stubs y la tabla de direcciones por vector viven en interrupts32.S. */
extern "C" void i386_syscall_entry();
extern "C" const uint32_t i386_exception_table[32];

/* Tabla de direcciones de las 16 stubs de IRQ (interrupts32.S). */
extern "C" const uint32_t i386_irq_table[16];
extern "C" const uint32_t i386_external_table[24];
extern "C" void i386_external_255();
struct ExternalHandlers {
    cpu::IrqHandler handlers[4];
    uint32_t count;
    bool local_eoi;
};
ExternalHandlers g_external[256];

constexpr uint8_t kIrqCount = 16;

cpu::IrqHandler g_irq_handlers[kIrqCount];
cpu::IrqHandler g_shared_irq_handlers[kIrqCount][4];

/* Primer vector del PIC tras el remapeo de initialize(): los IRQ 0..15 viven
 * en 0x20..0x2F. */
constexpr uint32_t kPicVectorOffset = 0x20;

/* Marco comun de los IRQ: el stub apilo el numero, el handler corre y luego
 * se acusa al PIC. El llamador es interrupts32.S. */
extern "C" ContextFrame* i386_handle_irq(ContextFrame* frame) {
    const uint32_t irq = frame->vector;
    if (irq < kIrqCount && g_irq_handlers[irq] != nullptr) {
        g_irq_handlers[irq]();
    }
    if (irq < kIrqCount)
        for (auto handler : g_shared_irq_handlers[irq]) if (handler) handler();
    cpu::acknowledge_irq(static_cast<uint8_t>(irq));
    return irq == 0 ? process::timer(frame) : frame;
}

extern "C" ContextFrame* i386_handle_external(ContextFrame* frame) {
    auto& entry = g_external[frame->vector];
    for (uint32_t n = 0; n < entry.count; ++n) entry.handlers[n]();
    if (entry.local_eoi) lapic::acknowledge();
    return frame;
}

inline void outb(uint16_t port, uint8_t value) {
    asm volatile("outb %0, %1" : : "a"(value), "Nd"(port));
}

inline uint32_t read_cr0() {
    uint32_t value;
    asm volatile("mov %%cr0, %0" : "=r"(value));
    return value;
}

inline void write_cr0(uint32_t value) {
    asm volatile("mov %0, %%cr0" : : "r"(value) : "memory");
}

inline uint32_t read_cr4() {
    uint32_t value;
    asm volatile("mov %%cr4, %0" : "=r"(value));
    return value;
}

inline void write_cr4(uint32_t value) {
    asm volatile("mov %0, %%cr4" : : "r"(value) : "memory");
}

inline uint8_t inb(uint16_t port) {
    uint8_t value;
    asm volatile("inb %1, %0" : "=a"(value) : "Nd"(port));
    return value;
}

inline uint32_t handler_address(void (*handler)()) {
    return static_cast<uint32_t>(reinterpret_cast<uintptr_t>(handler));
}

// Each protected-mode TSS has its own available descriptor (type 9, present).
void write_tss_entry(uint32_t index) {
    auto& tss = g_tss[index];
    const uint32_t base = static_cast<uint32_t>(reinterpret_cast<uintptr_t>(&tss));
    const uint32_t limit = static_cast<uint32_t>(sizeof(TaskStateSegment)) - 1u;
    tss.iomap_base = sizeof(TaskStateSegment);
    tss.ss0 = cpu::kKernelDataSelector;

    const uint64_t low = (limit & 0xFFFFu) | (static_cast<uint64_t>(base & 0xFFFFu) << 16);
    const uint64_t high = ((base >> 16) & 0xFFu) | (0x89ull << 8) |
                          ((static_cast<uint64_t>(limit >> 16) & 0xFu) << 16) |
                          ((static_cast<uint64_t>(base) >> 24) << 24);
    g_gdt[5 + index] = low | (high << 32);
}

void write_idt_gate(uint32_t vector, uint32_t handler, uint8_t attributes) {
    g_idt[vector].offset_low = static_cast<uint16_t>(handler & 0xFFFFu);
    g_idt[vector].selector = cpu::kKernelCodeSelector;
    g_idt[vector].zero = 0;
    g_idt[vector].type_attributes = attributes;
    g_idt[vector].offset_high = static_cast<uint16_t>((handler >> 16) & 0xFFFFu);
}

}

extern "C" ContextFrame* i386_report_exception(const uint32_t* frame) {
    // Exception frames have an extra error word before the hardware frame.
    const uint32_t vector = frame[12];
    if (vector == 14 && (frame[15] & 3u) == 3u && !(frame[13] & 1u)) {
        uint32_t address;
        asm volatile("mov %%cr2, %0" : "=r"(address));
        if (address >= paging::kUserStackBottom && address < paging::kUserStackTop &&
            process::current() && paging::map_user_range(process::current()->directory,
                address & ~4095u, paging::kPageBytes, true)) {
            // Drop only the normalized error word, retaining the saved user CPU
            // context. The common assembly restores the normal ContextFrame.
            auto* resumed = const_cast<uint32_t*>(frame) + 1;
            memmove(resumed, frame, 13 * sizeof(uint32_t));
            return reinterpret_cast<ContextFrame*>(resumed);
        }
    }
    serial::write("i386: exception vector ");
    serial::write_dec(vector);
    serial::write(", error ");
    serial::write_hex(frame[13]);
    serial::write(", eip ");
    serial::write_hex(frame[14]);
    serial::write(", cs ");
    serial::write_hex(frame[15]);
    if (vector == 14u) {
        uint32_t fault_address;
        asm volatile("mov %%cr2, %0" : "=r"(fault_address));
        serial::write(", cr2 ");
        serial::write_hex(fault_address);
    }
    serial::write("\n");
    // NMI, double fault and machine check indicate system-wide conditions,
    // even if they interrupted ring 3. Kernel faults always remain fatal.
    if ((frame[15] & 3u) == 3u && process::current_pid() != 0 &&
        vector != 2 && vector != 8 && vector != 18) {
        serial::write("user: exception #");
        serial::write_dec(vector);
        serial::write(" killed pid=");
        serial::write_dec(process::current_pid());
        serial::write("\n");
        return process::fault(vector);
    }
    serial::write("kernel: fatal exception\n");
    cpu::halt_forever();
}

void cpu::initialize() {
    g_gdt[0] = 0;
    g_gdt[1] = kKernelCodeEntry;
    g_gdt[2] = kKernelDataEntry;
    g_gdt[3] = kUserDataEntry;
    g_gdt[4] = kUserCodeEntry;
    for (uint32_t index = 0; index < kMaxCpus; ++index) write_tss_entry(index);

    const TableDescriptor gdt_descriptor{static_cast<uint16_t>(sizeof(g_gdt) - 1u),
                                         static_cast<uint32_t>(reinterpret_cast<uintptr_t>(g_gdt))};
    asm volatile("lgdt %0" : : "m"(gdt_descriptor));

    /* Exceptions normalize their vector/error frames; PIC IRQs use vectors
     * 32..47, local APIC/MSI/IPI gates use 48..71, and other gates share the
     * spurious entry. Only int 0x80 has DPL 3. Install the IDT before loading
     * the TSS so an initialization fault is reported instead of triple-faulting. */
    for (uint32_t vector = 0; vector < 256; ++vector) {
        const uint32_t handler = vector < 32u
                                     ? i386_exception_table[vector]
                                     : (vector < kPicVectorOffset + kIrqCount
                                            ? i386_irq_table[vector - kPicVectorOffset]
                                            : handler_address(i386_external_255));
        write_idt_gate(vector, handler, kTypeInterruptGate);
    }
    write_idt_gate(kSyscallVector, handler_address(i386_syscall_entry), kTypeUserInterruptGate);
    for (uint32_t vector = 48; vector < 72; ++vector)
        write_idt_gate(vector, i386_external_table[vector - 48], kTypeInterruptGate);

    const TableDescriptor idt_descriptor{static_cast<uint16_t>(sizeof(g_idt) - 1u),
                                         static_cast<uint32_t>(reinterpret_cast<uintptr_t>(g_idt))};
    asm volatile("lidt %0" : : "m"(idt_descriptor));

    /* Los selectores de datos no se recargan: la GDT del arranque y esta tienen
     * descriptores identicos para 0x08 y 0x10, asi que lo que el ljmp de
     * entry32.S cargo sigue siendo valido y CS no necesita un salto lejano
     * mas. Ademas el ensamblador en linea de clang convierte "mov %ax, %ds"
     * en la forma de 32 bits, que no existe y acaba en #GP; hacerlo aqui no
     * compra nada. */
    const uint16_t data_selector = kKernelDataSelector;
    (void)data_selector;
    const uint16_t tss_selector = kTssSelector;
    asm volatile("ltr %0" : : "r"(tss_selector));

    /* El PIC legacy arranca con los vectores en 0x08 y 0x70, que en modo
     * protegido son excepcion 8 (#DF) y la 0x70. Se remapea a 0x20..0x2F y se
     * deja todo enmascarado: cada IRQ abre su mascara cuando su driver esta
     * listo. El teclado es el primero (ps2_32.cpp). */
    outb(0x20, 0x11); /* ICW1: iniciar el cascade, se espera ICW4 */
    outb(0xA0, 0x11);
    outb(0x21, 0x20); /* ICW2: base de vectores del maestro */
    outb(0xA1, 0x28); /* ICW2: base de vectores del esclavo */
    outb(0x21, 0x04); /* ICW3: el maestro esta en el pin 2 del esclavo */
    outb(0xA1, 0x02);
    outb(0x21, 0x01); /* ICW4: modo 8086 */
    outb(0xA1, 0x01);
    outb(0x21, 0xFF); /* mascara: todo cerrado */
    outb(0xA1, 0xFF);
}

void cpu::initialize_secondary(uint32_t index) {
    if (!index || index >= kMaxCpus) halt_forever();
    const TableDescriptor gdt{static_cast<uint16_t>(sizeof(g_gdt) - 1),
        static_cast<uint32_t>(reinterpret_cast<uintptr_t>(g_gdt))};
    const TableDescriptor idt{static_cast<uint16_t>(sizeof(g_idt) - 1),
        static_cast<uint32_t>(reinterpret_cast<uintptr_t>(g_idt))};
    asm volatile("lgdt %0; lidt %1" :: "m"(gdt), "m"(idt) : "memory");
    const uint16_t selector = kTssSelector + index * 8;
    asm volatile("ltr %0" :: "r"(selector) : "memory");
    enable_fpu();
}

bool cpu::register_external_handler(uint8_t vector, IrqHandler handler, bool local_eoi) {
    if ((vector < 48 || vector >= 72) && vector != 255) return false;
    if (!handler) return false;
    auto& entry = g_external[vector];
    if (entry.count && entry.local_eoi != local_eoi) return false;
    for (uint32_t n = 0; n < entry.count; ++n) if (entry.handlers[n] == handler) return true;
    if (entry.count == 4) return false;
    entry.local_eoi = local_eoi;
    entry.handlers[entry.count] = handler;
    asm volatile("" ::: "memory");
    ++entry.count;
    return true;
}

void cpu::register_irq_handler(uint8_t irq, IrqHandler handler) {
    if (irq >= kIrqCount) {
        return;
    }
    g_irq_handlers[irq] = handler;
    write_idt_gate(kPicVectorOffset + irq, i386_irq_table[irq], kTypeInterruptGate);
}

bool cpu::register_shared_irq_handler(uint8_t irq, IrqHandler handler) {
    if (irq >= kIrqCount || !handler) return false;
    if (g_irq_handlers[irq] == handler) return true;
    for (auto existing : g_shared_irq_handlers[irq]) if (existing == handler) return true;
    for (auto& entry : g_shared_irq_handlers[irq]) {
        if (entry) continue;
        entry = handler;
        write_idt_gate(kPicVectorOffset + irq, i386_irq_table[irq], kTypeInterruptGate);
        return true;
    }
    return false;
}

void cpu::enable_irq(uint8_t irq) {
    if (irq >= kIrqCount) {
        return;
    }
    const uint8_t mask_register = irq < 8u ? 0x21u : 0xA1u;
    const uint8_t bit = static_cast<uint8_t>(1u << (irq & 7u));
    outb(mask_register, static_cast<uint8_t>(inb(mask_register) & ~bit));
    /* Slave IRQs reach the CPU through master IRQ2; unmask the cascade too. */
    if (irq >= 8u) {
        outb(0x21, static_cast<uint8_t>(inb(0x21) & ~0x04u));
    }
}

void cpu::acknowledge_irq(uint8_t irq) {
    /* El esclavo se acusa primero y con su EOI al maestro despues. */
    if (irq >= 8u) {
        outb(0xA0, 0x20);
        outb(0x20, 0x20);
        return;
    }
    outb(0x20, 0x20);
}

void cpu::disable_interrupts() {
    asm volatile("cli");
}

void cpu::enable_interrupts() {
    asm volatile("sti");
}

void cpu::set_ring0_stack(uint32_t stack_top) {
    g_tss[index()].esp0 = stack_top;
}

uint32_t cpu::ring0_stack_top() {
    return g_tss[index()].esp0;
}

/* Definido en interrupts32.S. */
extern "C" void i386_load_user_segments();

void cpu::load_user_segments() {
    i386_load_user_segments();
}

void cpu::enable_fpu() {
    uint32_t cr0 = read_cr0();
    cr0 &= ~((1u << 2) | (1u << 3)); // Clear EM and TS for eager FPU switching.
    cr0 |= (1u << 1);  /* MP: el FPU real, sin emulacion */ 
    write_cr0(cr0);

    uint32_t cr4 = read_cr4();
    cr4 |= (1u << 9) | (1u << 10); // OSFXSR | OSXMMEXCPT.
    write_cr4(cr4);
}

void cpu::halt_forever() {
    for (;;) {
        asm volatile("cli\n\thlt");
    }
}

void cpu::idle_with_interrupts() {
    for (;;) {
        asm volatile("sti\n\thlt");
    }
}

}
