#include "cpu.hpp"

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

/* Cinco entradas fijas (nula, codigo y datos de ring 0, datos y codigo de
 * ring 3) y las dos palabras del descriptor del TSS. Los selectores salen de
 * ahi: 0x08, 0x10, 0x1b, 0x23 y 0x28. */
constexpr uint32_t kEntryCount = 7;

/* TSS de 32 bits. Solo esp0 (offset 4) y ss0 (offset 8) se usan hoy: son los
 * unicos campos que el processor carga al bajar de ring 3. El resto esta para
 * que el descriptor sea un TSS completo y no media estructura. */
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
    uint32_t cs;
    uint32_t eflags_after_switch;
    uint32_t ss_after_switch;
    uint32_t ds;
    uint32_t es;
    uint32_t fs;
    uint32_t gs;
    uint32_t ldt;
    uint32_t trap;
    uint32_t iomap_base;
};

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

TaskStateSegment g_tss;
uint64_t g_gdt[kEntryCount];
IdtEntry g_idt[256];

static_assert(sizeof(IdtEntry) == 8, "las puertas de IDT de 32 bits ocupan 8 bytes");

constexpr uint8_t kTypeInterruptGate = 0x8E; /* presente, DPL 0 */
constexpr uint8_t kTypeUserInterruptGate = 0xEE; /* presente, DPL 3 */

/* Vectores de excepcion de CPU que empujan un codigo de error: sin esta tabla
 * el marco que sigue al vector no se puede leer. */
constexpr bool kErrorCodeVectors[32] = {
    /* 0-7 */ false, false, false, false, false, false, false, false,
    /* 8 */ true, /* 9 */ false, /* 10 */ true, /* 11 */ true,
    /* 12 */ true, /* 13 */ true, /* 14 */ true, /* 15 */ false,
    /* 16 */ false, /* 17 */ true, /* 18 */ false, /* 19 */ false,
    /* 20 */ false, /* 21 */ true, /* 22 */ false, /* 23 */ false,
    /* 24 */ false, /* 25 */ false, /* 26 */ false, /* 27 */ false,
    /* 28 */ false, /* 29 */ true, /* 30 */ true, /* 31 */ false,
};

/* Los stubs y la tabla de direcciones por vector viven en interrupts32.S. */
extern "C" void i386_syscall_entry();
extern "C" const uint32_t i386_exception_table[32];

/* Tabla de direcciones de las 16 stubs de IRQ (interrupts32.S). */
extern "C" const uint32_t i386_irq_table[16];

constexpr uint8_t kIrqCount = 16;

cpu::IrqHandler g_irq_handlers[kIrqCount];

/* Primer vector del PIC tras el remapeo de initialize(): los IRQ 0..15 viven
 * en 0x20..0x2F. */
constexpr uint32_t kPicVectorOffset = 0x20;

/* Marco comun de los IRQ: el stub apilo el numero, el handler corre y luego
 * se acusa al PIC. El llamador es interrupts32.S. */
extern "C" void i386_handle_irq(uint32_t irq) {
    if (irq < kIrqCount && g_irq_handlers[irq] != nullptr) {
        g_irq_handlers[irq]();
    }
    cpu::acknowledge_irq(static_cast<uint8_t>(irq));
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

/* El descriptor de TSS ocupa dos entradas de la GDT: base y limite repartidos
 * entre las dos palabras y el byte de flags (tipo 9 = TSS de 32 bits
 * disponible, P=1) en la segunda. */
void write_tss_entry(uint32_t index) {
    const uint32_t base = static_cast<uint32_t>(reinterpret_cast<uintptr_t>(&g_tss));
    const uint32_t limit = static_cast<uint32_t>(sizeof(TaskStateSegment)) - 1u;
    g_tss.iomap_base = static_cast<uint32_t>(sizeof(TaskStateSegment));

    const uint64_t low = (limit & 0xFFFFu) | (static_cast<uint64_t>(base & 0xFFFFu) << 16);
    const uint64_t high = ((base >> 16) & 0xFFu) | (0x89ull << 8) |
                          ((static_cast<uint64_t>(limit >> 16) & 0xFu) << 16) |
                          ((static_cast<uint64_t>(base) >> 24) << 24);
    g_gdt[index] = low | (high << 32);
}

void write_idt_gate(uint32_t vector, uint32_t handler, uint8_t attributes) {
    g_idt[vector].offset_low = static_cast<uint16_t>(handler & 0xFFFFu);
    g_idt[vector].selector = cpu::kKernelCodeSelector;
    g_idt[vector].zero = 0;
    g_idt[vector].type_attributes = attributes;
    g_idt[vector].offset_high = static_cast<uint16_t>((handler >> 16) & 0xFFFFu);
}

}

extern "C" [[noreturn]] void i386_report_exception(SyscallFrame* frame, uint32_t vector) {
    /* Lo que apilo el processor, de menor a mayor direccion: codigo de error
     * (si el vector lo genera), eip, cs y eflags, mas esp y ss si hubo cambio
     * de privilegio. El codigo de error va el primero, medido contra QEMU con
     * -d int en los dos casos que aparecieron (#SS al cargar el selector de
     * datos y #GP con un cs basura). */
    const uint32_t* saved = reinterpret_cast<const uint32_t*>(frame) + kSyscallFrameWords;
    const bool has_error = vector < 32u && kErrorCodeVectors[vector];

    serial::write("i386: excepcion vector ");
    serial::write_dec(vector);
    if (has_error) {
        serial::write(", codigo de error ");
        serial::write_hex(saved[0]);
    }
    const uint32_t shift = has_error ? 1u : 0u;
    serial::write(", eip ");
    serial::write_hex(saved[shift]);
    serial::write(", cs ");
    serial::write_hex(saved[shift + 1u]);
    if (vector == 14u) {
        uint32_t fault_address;
        asm volatile("mov %%cr2, %0" : "=r"(fault_address));
        serial::write(", cr2 ");
        serial::write_hex(fault_address);
    }
    serial::write("\n");
    cpu::halt_forever();
}

void cpu::initialize() {
    g_gdt[0] = 0;
    g_gdt[1] = kKernelCodeEntry;
    g_gdt[2] = kKernelDataEntry;
    g_gdt[3] = kUserDataEntry;
    g_gdt[4] = kUserCodeEntry;
    write_tss_entry(5);
    g_tss.ss0 = kKernelDataSelector;

    const TableDescriptor gdt_descriptor{static_cast<uint16_t>(sizeof(g_gdt) - 1u),
                                         static_cast<uint32_t>(reinterpret_cast<uintptr_t>(g_gdt))};
    asm volatile("lgdt %0" : : "m"(gdt_descriptor));

    /* Todas las vectores caen en el stub de excepcion hasta que haya
     * interrupciones propias; int $0x80 es el unico con DPL 3. La IDT se
     * instala antes de recargar segmentos y cargar el TSS: un fallo a partir
     * de aqui se cuenta en vez de acabar en triple fault. */
    /* Cada excepcion tiene su propia puerta porque el processor no apila el
     * numero de vector: lo apila el stub (interrupts32.S). Los vectores 32..47
     * son las IRQ del PIC remapeado (0x20..0x2F) y apuntan a sus entradas
     * aunque todavia no haya driver: el marco comun solo acusa al PIC cuando no
     * hay handler, que es lo que evita que el PIT (IRQ0, sin instalar todavia)
     * desalinee la pila al llegar. El resto son marcadores. */
    for (uint32_t vector = 0; vector < 256; ++vector) {
        const uint32_t handler = vector < 32u
                                     ? i386_exception_table[vector]
                                     : (vector < kPicVectorOffset + kIrqCount
                                            ? i386_irq_table[vector - kPicVectorOffset]
                                            : i386_irq_table[0]);
        write_idt_gate(vector, handler, kTypeInterruptGate);
    }
    write_idt_gate(kSyscallVector, handler_address(i386_syscall_entry), kTypeUserInterruptGate);

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

void cpu::register_irq_handler(uint8_t irq, IrqHandler handler) {
    if (irq >= kIrqCount) {
        return;
    }
    g_irq_handlers[irq] = handler;
    write_idt_gate(kPicVectorOffset + irq, i386_irq_table[irq], kTypeInterruptGate);
}

void cpu::enable_irq(uint8_t irq) {
    if (irq >= kIrqCount) {
        return;
    }
    const uint8_t mask_register = irq < 8u ? 0x21u : 0xA1u;
    const uint8_t bit = static_cast<uint8_t>(1u << (irq & 7u));
    outb(mask_register, static_cast<uint8_t>(inb(mask_register) & ~bit));
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
    g_tss.esp0 = stack_top;
}

uint32_t cpu::ring0_stack_top() {
    return g_tss.esp0;
}

/* Definido en interrupts32.S. */
extern "C" void i386_load_user_segments();

void cpu::load_user_segments() {
    i386_load_user_segments();
}

void cpu::enable_fpu() {
    uint32_t cr0 = read_cr0();
    cr0 &= ~(1u << 2); /* EM: sin emulacion, el FPU es real */
    cr0 |= (1u << 1);  /* MP: el FPU real, sin emulacion */ 
    write_cr0(cr0);

    uint32_t cr4 = read_cr4();
    cr4 |= (1u << 9); /* OSFXSR: el CR0 ya no ignora los errores de SSE */
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