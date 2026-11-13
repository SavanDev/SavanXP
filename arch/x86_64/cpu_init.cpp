#include <stdint.h>

#include "arch/cpu.hpp"
#include "arch/x86/io.hpp"
#include "kernel/console.hpp"
#include "kernel/panic.hpp"
#include "kernel/process.hpp"
#include "kernel/smp.hpp"
#include "kernel/vmm.hpp"

namespace {

using namespace arch::x86::port_io;
using arch::read_msr;
using arch::read_timestamp_counter;
using arch::write_msr;

constexpr uint16_t kKernelCodeSelector = 0x08;
constexpr uint16_t kKernelDataSelector = 0x10;
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
constexpr uint32_t kEferMsr = 0xc0000080;
constexpr uint64_t kEferNoExecuteEnable = 1ull << 11;
constexpr uint64_t kCr0WriteProtect = 1ull << 16;
constexpr uint64_t kCr4Smep = 1ull << 20;
constexpr uint64_t kCr4Smap = 1ull << 21;
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
constexpr uint32_t kApicIcrLow = 0x300;
constexpr uint32_t kApicIcrHigh = 0x310;
constexpr uint32_t kApicInitialCount = 0x380;
constexpr uint32_t kApicCurrentCount = 0x390;
constexpr uint32_t kApicDivideConfiguration = 0x3e0;
constexpr uint32_t kApicSoftwareEnable = 1u << 8;
constexpr uint32_t kApicTimerPeriodic = 1u << 17;
constexpr uint32_t kApicLvtExtInt = 0x7u << 8;
// ICR: entrega pendiente (solo xAPIC) y nivel en assert. El resto de los campos
// que usamos valen cero -- modo fixed, destino fisico, flanco, sin shorthand --
// asi que el comando es el vector mas este bit.
constexpr uint32_t kApicIcrDeliveryStatus = 1u << 12;
constexpr uint32_t kApicIcrAssert = 1u << 14;
// Delivery mode NMI (bits 8-10 = 100): the vector field is ignored.
constexpr uint32_t kApicIcrDeliveryNmi = 4u << 8;
constexpr uint32_t kIpiDeliverySpinLimit = 1000000u;

extern "C" void x86_64_syscall_entry();
extern "C" void x86_64_timer_entry();
extern "C" void x86_64_reschedule_entry();

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
using arch::InterruptEoi;
using arch::IrqHandler;

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

// Six fixed entries -- null, kernel code and data, user data, user code, and
// user 32-bit code -- then one TSS descriptor per core. There is one GDT,
// shared by every core; what each core owns is its TSS.
//
// User data (0x1b) is flat with G and D/B set: ignored in 64-bit mode, and a
// 32-bit stack and data segment in compatibility mode. User 32-bit code (0x2b)
// is L=0, D=1. Neither reaches anything a 64-bit program could not: both are
// ring 3 and flat.
constexpr uint32_t kGdtFixedEntries = 6;
static_assert(kGdtFixedEntries * 8 == arch::kTssSelectorBase,
              "the first TSS descriptor must follow the fixed entries");
static_assert(arch::kUserDataSelector == (3 * 8 | 3) && arch::kUserCodeSelector == (4 * 8 | 3) &&
                  arch::kUserCode32Selector == (5 * 8 | 3),
              "the user selectors index the fixed entries at ring 3");

uint64_t g_gdt[kGdtFixedEntries + 2 * arch::kMaxCpus] = {
    0x0000000000000000ULL,
    0x00af9a000000ffffULL,
    0x00af92000000ffffULL,
    0x00cff2000000ffffULL,
    0x00affa000000ffffULL,
    0x00cffa000000ffffULL,
};

Tss g_tss[arch::kMaxCpus] = {};
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
bool g_rdrand_available = false;
uint64_t g_entropy_counter = 0;
extern "C" uintptr_t __stack_chk_guard = 0;
// Ventana MMIO del APIC local en modo xAPIC (por defecto 0xfee00000, una pagina
// uncacheable). Nula mientras no se mapee o si el CPU expone x2APIC.
volatile uint32_t* g_local_apic_mmio = nullptr;

void cpuid(uint32_t leaf, uint32_t subleaf, uint32_t& eax, uint32_t& ebx, uint32_t& ecx, uint32_t& edx) {
    asm volatile("cpuid" : "=a"(eax), "=b"(ebx), "=c"(ecx), "=d"(edx) : "a"(leaf), "c"(subleaf));
}

bool enable_nx() {
    uint32_t eax = 0;
    uint32_t ebx = 0;
    uint32_t ecx = 0;
    uint32_t edx = 0;
    cpuid(0x80000000u, 0, eax, ebx, ecx, edx);
    if (eax < 0x80000001u) {
        return false;
    }

    cpuid(0x80000001u, 0, eax, ebx, ecx, edx);
    if ((edx & (1u << 20)) == 0) {
        return false;
    }
    write_msr(kEferMsr, read_msr(kEferMsr) | kEferNoExecuteEnable);
    return true;
}

// QEMU's TCG says so in the hypervisor leaf, "TCGTCGTCGTCG", behind the
// hypervisor bit of leaf 1.
bool running_under_tcg() {
    uint32_t eax = 0;
    uint32_t ebx = 0;
    uint32_t ecx = 0;
    uint32_t edx = 0;
    cpuid(1, 0, eax, ebx, ecx, edx);
    if ((ecx & (1u << 31)) == 0) {
        return false;
    }
    cpuid(0x40000000u, 0, eax, ebx, ecx, edx);
    // The signature in EBX, ECX, EDX: "TCGT" "CGTC" "GTCG", little-endian.
    return ebx == 0x54474354u && ecx == 0x43544743u && edx == 0x47435447u;
}

// Family and model as CPUID leaf 1 encodes them, with the extended fields
// folded in the way both vendors define: the extended family counts only on
// family 0Fh, the extended model on families 06h and 0Fh.
struct CpuSignature {
    uint32_t family;
    uint32_t model;
    uint32_t stepping;
};

CpuSignature cpu_signature() {
    uint32_t eax = 0;
    uint32_t ebx = 0;
    uint32_t ecx = 0;
    uint32_t edx = 0;
    cpuid(1, 0, eax, ebx, ecx, edx);
    CpuSignature signature = {};
    const uint32_t base_family = (eax >> 8) & 0xfu;
    const uint32_t base_model = (eax >> 4) & 0xfu;
    signature.stepping = eax & 0xfu;
    signature.family = base_family == 0xfu ? base_family + ((eax >> 20) & 0xffu) : base_family;
    signature.model = (base_family == 0x6u || base_family == 0xfu)
        ? base_model | (((eax >> 16) & 0xfu) << 4)
        : base_model;
    return signature;
}

bool cpu_vendor_is(uint32_t ebx_word) {
    uint32_t eax = 0;
    uint32_t ebx = 0;
    uint32_t ecx = 0;
    uint32_t edx = 0;
    cpuid(0, 0, eax, ebx, ecx, edx);
    return ebx == ebx_word;
}

constexpr uint32_t kVendorIntelEbx = 0x756e6547u;  // "Genu"
constexpr uint32_t kVendorAmdEbx = 0x68747541u;    // "Auth"

bool running_under_hypervisor() {
    uint32_t eax = 0;
    uint32_t ebx = 0;
    uint32_t ecx = 0;
    uint32_t edx = 0;
    cpuid(1, 0, eax, ebx, ecx, edx);
    return (ecx & (1u << 31)) != 0;
}

// HW-10 (docs/hardware/REAL_HARDWARE.md). Intel firmware can turn XD off
// ("Execute Disable Bit"), which sets IA32_MISC_ENABLE[34] and makes CPUID
// stop reporting NX on a CPU that has it. It can also cap the basic CPUID
// leaves (bit 22, "Limit CPUID Maxval"), which hides leaf 7. Both are undone
// before anything reads CPUID, on every core, as Linux does. The MSR exists
// from the Pentium 4 and from family 6 model 0Dh (Dothan) on.
constexpr uint32_t kMiscEnableMsr = 0x1a0;
constexpr uint64_t kMiscEnableLimitCpuid = 1ull << 22;
constexpr uint64_t kMiscEnableXdDisable = 1ull << 34;

// What the BSP's quirks found, for the log line it prints once.
uint64_t g_misc_enable_cleared = 0;

uint64_t clear_intel_cpuid_limits() {
    if (!cpu_vendor_is(kVendorIntelEbx)) {
        return 0;
    }
    const CpuSignature signature = cpu_signature();
    if (!(signature.family == 0xfu || (signature.family == 0x6u && signature.model >= 0xdu))) {
        return 0;
    }
    const uint64_t misc = read_msr(kMiscEnableMsr);
    const uint64_t wanted = misc & (kMiscEnableXdDisable | kMiscEnableLimitCpuid);
    if (wanted != 0) {
        write_msr(kMiscEnableMsr, misc & ~wanted);
    }
    return wanted;
}

// HW-9, AMD erratum 400. On K8 from revision F and on family 10h, firmware
// that enables C1E makes the processor enter it once every core has halted,
// and the local APIC timer stops there -- the tick this kernel runs on. The
// two "on compare halt" bits of the interrupt pending message register
// (MSR C001_0055) are what send it there; FreeBSD clears them, and so does
// this, on every core and again before every idle halt, because SMM code is
// free to set them back. Skipped under a hypervisor, which may not implement
// the register at all: QEMU's default qemu64 claims to be a K8.
constexpr uint32_t kAmdInterruptPendingMsr = 0xc0010055u;
constexpr uint64_t kAmdC1eOnCmpHalt = 1ull << 28;
constexpr uint64_t kAmdSmiOnCmpHalt = 1ull << 27;
constexpr uint64_t kAmdCmpHalt = kAmdC1eOnCmpHalt | kAmdSmiOnCmpHalt;

bool g_amd_c1e_quirk = false;
bool g_amd_c1e_was_enabled = false;

bool amd_c1e_erratum_applies() {
    if (!cpu_vendor_is(kVendorAmdEbx) || running_under_hypervisor()) {
        return false;
    }
    const CpuSignature signature = cpu_signature();
    return (signature.family == 0xfu && signature.model >= 0x40u) || signature.family == 0x10u;
}

// Returns whether the bits were set.
bool clear_amd_c1e() {
    const uint64_t value = read_msr(kAmdInterruptPendingMsr);
    if ((value & kAmdCmpHalt) == 0) {
        return false;
    }
    write_msr(kAmdInterruptPendingMsr, value & ~kAmdCmpHalt);
    return true;
}

// One line naming the processor and one with the features the kernel cares
// about. On a machine with no serial port this is the only way to tell from
// Log Viewer which processor the run was on.
void report_cpu() {
    arch::CpuIdentity identity = {};
    arch::query_cpu_identity(identity);
    const CpuSignature signature = cpu_signature();
    console::printf("cpu: %s family 0x%x model 0x%x stepping %u, \"%s\"\n",
        identity.vendor, signature.family, signature.model, signature.stepping,
        identity.brand[0] != '\0' ? identity.brand : "no brand string");

    uint32_t eax = 0;
    uint32_t ebx = 0;
    uint32_t ecx = 0;
    uint32_t edx = 0;
    cpuid(1, 0, eax, ebx, ecx, edx);
    const uint32_t leaf1_ecx = ecx;
    const uint32_t leaf1_edx = edx;
    uint32_t leaf7_ebx = 0;
    cpuid(0, 0, eax, ebx, ecx, edx);
    if (eax >= 7) {
        cpuid(7, 0, eax, leaf7_ebx, ecx, edx);
    }
    uint32_t ext1_ecx = 0;
    uint32_t ext1_edx = 0;
    uint32_t ext7_edx = 0;
    cpuid(0x80000000u, 0, eax, ebx, ecx, edx);
    const uint32_t highest_extended = eax;
    if (highest_extended >= 0x80000001u) {
        cpuid(0x80000001u, 0, eax, ebx, ext1_ecx, ext1_edx);
    }
    if (highest_extended >= 0x80000007u) {
        cpuid(0x80000007u, 0, eax, ebx, ecx, ext7_edx);
    }

    struct Flag {
        bool present;
        const char* name;
    };
    const Flag flags[] = {
        {(ext1_edx & (1u << 29)) != 0, "lm"},
        {(ext1_edx & (1u << 20)) != 0, "nx"},
        {(leaf1_edx & (1u << 26)) != 0, "sse2"},
        {(leaf1_ecx & (1u << 0)) != 0, "sse3"},
        {(leaf1_ecx & (1u << 9)) != 0, "ssse3"},
        {(leaf1_ecx & (1u << 19)) != 0, "sse4.1"},
        {(leaf1_ecx & (1u << 20)) != 0, "sse4.2"},
        {(ext1_ecx & (1u << 6)) != 0, "sse4a"},
        {(leaf1_ecx & (1u << 13)) != 0, "cx16"},
        {(ext1_ecx & (1u << 0)) != 0, "lahf_lm"},
        {(leaf1_ecx & (1u << 23)) != 0, "popcnt"},
        {(leaf1_ecx & (1u << 28)) != 0, "avx"},
        {(leaf1_edx & (1u << 9)) != 0, "apic"},
        {(leaf1_ecx & (1u << 21)) != 0, "x2apic"},
        {(leaf1_ecx & (1u << 30)) != 0, "rdrand"},
        {(leaf7_ebx & (1u << 7)) != 0, "smep"},
        {(leaf7_ebx & (1u << 20)) != 0, "smap"},
        {(ext7_edx & (1u << 8)) != 0, "invtsc"},
        {(ext1_edx & (1u << 26)) != 0, "pdpe1gb"},
        {(leaf1_ecx & (1u << 31)) != 0, "hypervisor"},
    };
    char line[160] = {};
    size_t length = 0;
    for (const Flag& flag : flags) {
        if (!flag.present) {
            continue;
        }
        for (const char* p = flag.name; *p != '\0' && length + 2 < sizeof(line); ++p) {
            line[length++] = *p;
        }
        if (length + 2 < sizeof(line)) {
            line[length++] = ' ';
        }
    }
    if (length > 0) {
        line[length - 1] = '\0';
    }
    console::printf("cpu: features %s\n", line);
}

// Whether the BSP turned SMEP on. Every AP does the same as the BSP: all
// cores enforce it or none does.
bool g_smep_enabled = false;

bool enable_smep() {
    uint32_t maximum_leaf = 0;
    uint32_t ebx = 0;
    uint32_t ecx = 0;
    uint32_t edx = 0;
    cpuid(0, 0, maximum_leaf, ebx, ecx, edx);
    if (maximum_leaf < 7) {
        return false;
    }

    cpuid(7, 0, maximum_leaf, ebx, ecx, edx);
    if ((ebx & (1u << 7)) == 0) {
        return false;
    }

    uint64_t cr0 = 0;
    asm volatile("mov %%cr0, %0" : "=r"(cr0));
    cr0 |= kCr0WriteProtect;
    asm volatile("mov %0, %%cr0" ::"r"(cr0) : "memory");

    uint64_t cr4 = 0;
    asm volatile("mov %%cr4, %0" : "=r"(cr4));
    cr4 |= kCr4Smep;
    asm volatile("mov %0, %%cr4" ::"r"(cr4) : "memory");
    return true;
}

// SMAP: the kernel faults on a user page unless RFLAGS.AC opens the access
// (arch::user_access_begin). Turned on after SMEP and on the same terms: the
// BSP decides, every AP follows or stays offline. AC is cleared first, so the
// core does not come out of here with the window already open.
bool enable_smap() {
    uint32_t maximum_leaf = 0;
    uint32_t ebx = 0;
    uint32_t ecx = 0;
    uint32_t edx = 0;
    cpuid(0, 0, maximum_leaf, ebx, ecx, edx);
    if (maximum_leaf < 7) {
        return false;
    }

    cpuid(7, 0, maximum_leaf, ebx, ecx, edx);
    if ((ebx & (1u << 20)) == 0) {
        return false;
    }

    uint64_t cr4 = 0;
    asm volatile("mov %%cr4, %0" : "=r"(cr4));
    cr4 |= kCr4Smap;
    asm volatile("clac\n\tmov %0, %%cr4" ::"r"(cr4) : "memory");
    return true;
}

uint64_t random_u64_local() {
    for (uint32_t attempt = 0; g_rdrand_available && attempt < 16; ++attempt) {
        uint64_t value = 0;
        uint8_t success = 0;
        asm volatile("rdrand %0; setc %1" : "=r"(value), "=qm"(success));
        if (success != 0 && value != 0) {
            return value;
        }
    }

    const uint64_t tsc = read_timestamp_counter();
    return (tsc ^ ++g_entropy_counter) * 0x9e3779b97f4a7c15ULL;
}

uint32_t x2apic_msr(uint32_t reg) {
    return kX2ApicMsrBase + (reg >> 4);
}

// Los dos modos del APIC local hablan los mismos registros por caminos
// distintos: x2APIC via MSR (0x800 + reg/16) y xAPIC via MMIO, donde cada
// registro es un dword alineado a 16 bytes dentro de la pagina base. El resto
// del kernel solo usa registros de 32 bits (EOI, SVR, LVT, timer), asi que este
// par de helpers alcanza. El ICR de 64 bits del x2APIC es la excepcion: lo
// escribe send_ipi() con su propio write_msr.
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
    // The other cores first: they must not keep running on top of whatever
    // state this fault came from, nor print over the report.
    uint32_t targets = 0;
    const uint32_t stopped = smp::stop_other_cores(targets);
    console::write_line("");
    console::printf("exception: #%u %s\n", static_cast<unsigned>(vector), exception_name(vector));
    if (has_error_code) {
        console::printf("error code: 0x%llx\n", error_code);
    }
    if (vector == 14) {
        console::printf("cr2: 0x%llx\n", read_cr2());
        // Present page, supervisor access, not an instruction fetch, on a
        // user address: SMAP refused it. A kernel path read or wrote user
        // memory outside arch::user_access_begin/end.
        constexpr uint64_t kPfPresent = 1u << 0;
        constexpr uint64_t kPfUser = 1u << 2;
        constexpr uint64_t kPfFetch = 1u << 4;
        constexpr uint64_t kLowerHalfEnd = 0x0000800000000000ULL;
        if (arch::g_smap_enabled && has_error_code &&
            (error_code & (kPfPresent | kPfUser | kPfFetch)) == kPfPresent &&
            read_cr2() < kLowerHalfEnd) {
            console::printf("exception: SMAP violation, kernel access to user memory outside a user-access window\n");
        }
    }
    if (frame != nullptr) {
        console::printf("rip: 0x%llx cs: 0x%llx rflags: 0x%llx\n", frame->rip, frame->cs, frame->rflags);
        console::printf("rsp: 0x%llx ss: 0x%llx\n", frame->rsp, frame->ss);
    }
    smp::report_stop(stopped, targets);

    arch::disable_interrupts();
    arch::halt_forever();
}

void handle_exception(uint8_t vector, InterruptFrame* frame, uint64_t error_code, bool has_error_code) {
    // The NMI another core sends when it panics. Checked before anything else:
    // it can land in ring 3 or in the middle of the kernel, lock held or not.
    if (vector == 2 && smp::stopping()) {
        smp::park_stopped_core();
    }

    if (vector == 3 && g_breakpoint_probe_active) {
        g_breakpoint_probe_hits = g_breakpoint_probe_hits + 1;
        return;
    }

    if (frame != nullptr && (frame->cs & 0x3) == 0x3) {
        // An exception from ring 3 enters the kernel like a syscall: it touches
        // the process, its page tables and, if it kills it, the scheduler.
        // Coming from ring 3, this core cannot already hold the lock.
        smp::lock_kernel(smp::Door::exception);
        if (process::current() == nullptr) {
            smp::unlock_kernel();
            stop_on_exception(vector, frame, error_code, has_error_code);
        }
        // #PF de no-presente adentro de la region del stack: se mapea la pagina
        // que falta y se reintenta la instruccion. El bit 0 del codigo de error
        // distingue "la pagina no esta" de "esta pero el acceso no se permite";
        // solo el primero se puede resolver creciendo el stack.
        const bool not_present = has_error_code && (error_code & 0x1) == 0;
        if (vector == 14 && not_present && process::grow_user_stack(read_cr2())) {
            // Se vuelve al mismo proceso sobre la misma pila: soltar aca no
            // tiene el problema que resuelve el stub de context.S.
            smp::unlock_kernel();
            return;
        }
        console::printf(
            "user: exception #%u pid=%u name=%s cr2=0x%llx rip=0x%llx\n",
            static_cast<unsigned>(vector),
            process::current_pid(),
            process::current()->name,
            read_cr2(),
            frame->rip
        );
        // No vuelve: resume_context suelta el lock ya sobre la pila del
        // proceso siguiente.
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

    // Device IRQs run drivers, and drivers are kernel state: they take the
    // lock. IPIs do not -- the ping arrives while the BSP, which sent it, is
    // running the boot and waiting for the acknowledgement. These handlers do
    // not switch context, so releasing the lock here is safe.
    const bool takes_kernel_lock = vector < arch::kIpiPingVector;
    if (takes_kernel_lock) {
        smp::lock_kernel(smp::Door::device_irq);
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

    if (takes_kernel_lock) {
        smp::unlock_kernel();
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

void install_tss_descriptor(uint32_t index) {
    Tss& tss = g_tss[index];
    const uint64_t base = reinterpret_cast<uint64_t>(&tss);
    const uint64_t limit = sizeof(Tss) - 1;
    const uint32_t entry = kGdtFixedEntries + 2 * index;
    g_gdt[entry] = (limit & 0xffffULL) |
        ((base & 0xffffffULL) << 16) |
        (0x89ULL << 40) |
        (((limit >> 16) & 0xfULL) << 48) |
        (((base >> 24) & 0xffULL) << 56);
    g_gdt[entry + 1] = base >> 32;
    tss.iomap_base = sizeof(Tss);
}

// Todos los descriptores de una vez, desde el BSP y antes de que arranque ningun
// AP: la GDT queda completa y ningun core la vuelve a escribir. La unica
// escritura posterior es la del propio procesador, que al hacer `ltr` prende el
// bit Busy del descriptor que carga -- cada core el suyo, sin carrera.
void install_tss_descriptors() {
    for (uint32_t index = 0; index < arch::kMaxCpus; ++index) {
        install_tss_descriptor(index);
    }
}

// Apuntar este core a la GDT del kernel y recargar los selectores. Separado del
// `ltr` porque un AP llega aca antes de saber que indice le toca: carga la GDT,
// se identifica, y recien entonces carga su propio TSS.
void load_gdt_without_tss() {
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
}

void load_task_register_for(uint32_t index) {
    const uint16_t selector = static_cast<uint16_t>(
        arch::kTssSelectorBase + index * arch::kTssDescriptorSize);
    asm volatile("ltr %0" : : "r"(selector) : "memory");
}

void load_gdt() {
    load_gdt_without_tss();
    load_task_register_for(0); // el BSP es siempre el core 0
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
        arch::halt_forever();
    }
}

// DF es parte del RFLAGS del proceso interrumpido, asi que un programa de
// usuario puede entrar al kernel con la bandera de direccion prendida. Las
// rutinas de memoria del kernel usan instrucciones de string (rep movs/stos),
// que con DF=1 caminan hacia atras y pisan memoria ajena. El atributo interrupt
// no emite el cld por su cuenta, asi que lo ponemos en cada punto de entrada,
// igual que los stubs de syscall y timer en context.S.
#define CLEAR_DIRECTION_FLAG() asm volatile("cld")

// The same for RFLAGS.AC: ring 3 can set it with popf, and with SMAP on it is
// the bit that lets the kernel touch user pages. Every entry closes it; iretq
// gives the interrupted code its own AC back.
#define CLOSE_USER_ACCESS() arch::user_access_end()

#define DEFINE_ISR_NOERR(VECTOR) \
    __attribute__((interrupt)) void isr_##VECTOR(InterruptFrame* frame) { \
        CLEAR_DIRECTION_FLAG(); \
        CLOSE_USER_ACCESS(); \
        handle_exception(VECTOR, frame, 0, false); \
    }

#define DEFINE_ISR_ERR(VECTOR) \
    __attribute__((interrupt)) void isr_##VECTOR(InterruptFrame* frame, uint64_t error_code) { \
        CLEAR_DIRECTION_FLAG(); \
        CLOSE_USER_ACCESS(); \
        handle_exception(VECTOR, frame, error_code, true); \
    }

#define DEFINE_EXTERNAL_ISR(VECTOR) \
    __attribute__((interrupt)) void vector_##VECTOR(InterruptFrame* frame) { \
        (void)frame; \
        CLEAR_DIRECTION_FLAG(); \
        CLOSE_USER_ACCESS(); \
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
// Vectores 64-71: mensajes entre cores (IPIs). El 64 es el ping con el que el
// arranque de los APs comprueba que el ICR entrega de verdad. El 65, replanificar,
// entra por el stub con contexto de context.S y no esta aca. El resto del rango
// queda para el shootdown de TLB.
DEFINE_EXTERNAL_ISR(64)
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
    set_idt_gate(64, reinterpret_cast<InterruptHandler>(vector_64), kInterruptGate);
    set_idt_gate(arch::kIpiRescheduleVector, reinterpret_cast<InterruptHandler>(x86_64_reschedule_entry), kInterruptGate);
    set_idt_gate(kSyscallVector, reinterpret_cast<InterruptHandler>(x86_64_syscall_entry), kUserInterruptGate);
    load_idt();
}

} // namespace

namespace arch {

__attribute__((no_stack_protector))
void initialize_stack_chk_guard() {
    uint32_t eax = 0;
    uint32_t ebx = 0;
    uint32_t ecx = 0;
    uint32_t edx = 0;
    cpuid(1, 0, eax, ebx, ecx, edx);
    g_rdrand_available = (ecx & (1u << 30)) != 0;

    uint64_t value = random_u64_local();
    if ((value & 0xffu) == 0) {
        value ^= 0xa5u;
    }
    __stack_chk_guard = static_cast<uintptr_t>(value);
}

uint64_t random_u64() {
    return random_u64_local();
}

/* Estado FPU/SSE limpio capturado en el boot; se copia a cada proceso nuevo
 * para que arranque con la FPU en un estado valido (MXCSR con excepciones
 * enmascaradas), en vez de un FXRSTOR sobre bytes en cero. */
alignas(16) static uint8_t g_default_fpu_state[kFpuStateSize];

// La parte de la FPU que le toca a cada core: los bits de CR0/CR4 y un estado
// x87/SSE limpio. Un AP corre solo esto. La plantilla para procesos nuevos la
// captura el BSP una sola vez, en enable_fpu().
void configure_fpu_for_this_core() {
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
}

void enable_fpu() {
    configure_fpu_for_this_core();

    // Capturar el estado limpio como plantilla para los procesos nuevos.
    asm volatile("fxsave64 (%0)" ::"r"(g_default_fpu_state) : "memory");
}

void fpu_save(void* area) {
    asm volatile("fxsave64 (%0)" ::"r"(area) : "memory");
}

void fpu_restore(const void* area) {
    asm volatile("fxrstor64 (%0)" ::"r"(area) : "memory");
}

void save_user_segment_bases(uint64_t& fs_base, uint64_t& gs_base) {
    constexpr uint32_t kFsBaseMsr = 0xc0000100;
    constexpr uint32_t kGsBaseMsr = 0xc0000101;
    fs_base = read_msr(kFsBaseMsr);
    gs_base = read_msr(kGsBaseMsr);
}

void load_user_segment_bases(uint64_t fs_base, uint64_t gs_base) {
    constexpr uint32_t kFsBaseMsr = 0xc0000100;
    constexpr uint32_t kGsBaseMsr = 0xc0000101;
    write_msr(kFsBaseMsr, fs_base);
    write_msr(kGsBaseMsr, gs_base);
}

void save_user_selectors(uint16_t (&selectors)[kUserSelectorCount]) {
    asm volatile("movw %%ds, %0" : "=r"(selectors[0]));
    asm volatile("movw %%es, %0" : "=r"(selectors[1]));
    asm volatile("movw %%fs, %0" : "=r"(selectors[2]));
    asm volatile("movw %%gs, %0" : "=r"(selectors[3]));
}

// Every value comes from save_user_selectors or is 0, so it is a selector the
// thread could load itself: null, or a ring-3 entry of the fixed GDT. Loading
// one at ring 0 cannot fault, and iretq to ring 3 keeps it.
void load_user_selectors(const uint16_t (&selectors)[kUserSelectorCount]) {
    asm volatile("movw %0, %%ds" : : "r"(selectors[0]) : "memory");
    asm volatile("movw %0, %%es" : : "r"(selectors[1]) : "memory");
    asm volatile("movw %0, %%fs" : : "r"(selectors[2]) : "memory");
    asm volatile("movw %0, %%gs" : : "r"(selectors[3]) : "memory");
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
    // Before anything reads CPUID: these change what it reports.
    g_misc_enable_cleared = clear_intel_cpuid_limits();
    report_cpu();
    if ((g_misc_enable_cleared & kMiscEnableXdDisable) != 0) {
        console::printf("cpu: XD was disabled by the firmware; re-enabled\n");
    }
    if ((g_misc_enable_cleared & kMiscEnableLimitCpuid) != 0) {
        console::printf("cpu: CPUID was limited by the firmware; limit removed\n");
    }
    g_amd_c1e_quirk = amd_c1e_erratum_applies();
    if (g_amd_c1e_quirk) {
        g_amd_c1e_was_enabled = clear_amd_c1e();
        console::printf("cpu: AMD erratum 400 range, C1E on halt %s\n",
            g_amd_c1e_was_enabled ? "was enabled by the firmware; disabled" : "is off");
    }
    uint32_t eax = 0;
    uint32_t ebx = 0;
    uint32_t ecx = 0;
    uint32_t edx = 0;
    cpuid(1, 0, eax, ebx, ecx, edx);
    g_rdrand_available = (ecx & (1u << 30)) != 0;
    // The canary was seeded in _start, before any protected frame could retain
    // the boot-time zero. It stays stable for this boot so in-flight frames
    // continue to compare against the value they loaded.
    if (!enable_nx()) {
        panic("cpu: NX/W^X is required");
    }
    // Before any AP starts: they all spin on the kernel lock with this.
    if (running_under_tcg()) {
        g_lock_spin_pause = false;
        console::printf("cpu: QEMU TCG, the kernel lock spins without pause\n");
    }
    g_smep_enabled = enable_smep();
    if (!g_smep_enabled) {
        console::printf("cpu: SMEP unavailable on this processor\n");
    }
    arch::g_smap_enabled = enable_smap();
    if (!arch::g_smap_enabled) {
        console::printf("cpu: SMAP unavailable on this processor\n");
    }
    install_tss_descriptors();
    load_gdt();
    remap_pic();
    initialize_idt();
    enable_fpu();
    run_breakpoint_probe();
}

void ap_initialize_cpu() {
    disable_interrupts();
    // The BSP's quirks again, on this core: the MSRs are per core.
    clear_intel_cpuid_limits();
    if (g_amd_c1e_quirk) {
        clear_amd_c1e();
    }
    // EFER belongs to each logical processor. Do not rely on Limine leaving
    // NXE enabled on an AP before the scheduler can run user NX mappings there.
    if (!enable_nx()) {
        halt_forever();
    }
    // SMEP as on the BSP. Requiring it unconditionally left every AP halted
    // on a processor without it (QEMU's qemu64), and the system on one core.
    if (g_smep_enabled && !enable_smep()) {
        halt_forever();
    }
    if (arch::g_smap_enabled && !enable_smap()) {
        halt_forever();
    }
    // Las tablas ya las armo el BSP y desde aca son de solo lectura: este core
    // solo tiene que apuntarles. Su TSS lo carga despues, con
    // load_task_register(), cuando ya sabe que indice le toca. Nada de
    // remap_pic() (el 8259 es del sistema, no del core) y nada de la sonda de
    // breakpoint, que ya corrio.
    load_gdt_without_tss();
    load_idt();
    configure_fpu_for_this_core();
}

bool ap_initialize_local_apic() {
    // El APIC local es por core, asi que cada AP tiene que habilitar el suyo.
    // Pero solo eso: el mapeo MMIO en modo xAPIC ya lo hizo el BSP y es global,
    // y LINT0 no se toca -- la regla del APIC es un solo ExtINT por sistema y
    // ese lugar ya lo ocupo el BSP en initialize_local_apic().
    if (!g_local_apic_ready) {
        return false;
    }

    uint64_t apic_base = read_msr(kApicBaseMsr);
    apic_base |= kApicBaseEnable;
    if (g_local_apic_x2apic) {
        apic_base |= kApicBaseX2ApicEnable;
    } else {
        apic_base &= ~static_cast<uint64_t>(kApicBaseX2ApicEnable);
    }
    write_msr(kApicBaseMsr, apic_base);

    write_local_apic(kApicSpuriousVector, kApicSoftwareEnable | 0xff);
    return true;
}

// El bit de entrega pendiente del ICR solo existe en xAPIC. Sondearlo antes de
// pisar el registro evita perder un IPI que todavia esta en vuelo.
static bool wait_for_ipi_delivery() {
    if (g_local_apic_x2apic) {
        return true;
    }
    for (uint32_t spin = 0; spin < kIpiDeliverySpinLimit; ++spin) {
        if ((read_local_apic(kApicIcrLow) & kApicIcrDeliveryStatus) == 0) {
            return true;
        }
        asm volatile("pause");
    }
    return false;
}

// Writes one ICR command to a core by its LAPIC id, physical destination.
static bool send_icr(uint32_t destination_lapic_id, uint32_t command) {
    if (!g_local_apic_ready) {
        return false;
    }

    if (g_local_apic_x2apic) {
        // One 64-bit MSR: destination above, command below. The x2APIC has no
        // delivery-pending bit, so there is nothing to poll.
        write_msr(x2apic_msr(kApicIcrLow),
                  (static_cast<uint64_t>(destination_lapic_id) << 32) | command);
        return true;
    }

    // In xAPIC mode the physical destination is 8 bits. A larger id is not
    // addressable that way; it would need logical destination mode, which is
    // work for when there are more than 255 cores.
    if (destination_lapic_id > 0xffu) {
        return false;
    }
    if (!wait_for_ipi_delivery()) {
        return false;
    }

    // High half first: writing the low half is what sends it.
    write_local_apic(kApicIcrHigh, destination_lapic_id << 24);
    write_local_apic(kApicIcrLow, command);
    return wait_for_ipi_delivery();
}

bool send_ipi(uint32_t destination_lapic_id, uint8_t vector) {
    return send_icr(destination_lapic_id, static_cast<uint32_t>(vector) | kApicIcrAssert);
}

bool send_nmi(uint32_t destination_lapic_id) {
    return send_icr(destination_lapic_id, kApicIcrDeliveryNmi | kApicIcrAssert);
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
    // El rsp0 de ESTE core: ahi aterriza la proxima transicion de ring 3 a
    // ring 0 que ocurra aca, y el proceso que la dispara es el que corre aca.
    g_tss[cpu_index()].rsp0 = stack_top;
}

void load_task_register(uint32_t index) {
    if (index < kMaxCpus) {
        load_task_register_for(index);
    }
}

/* Identidad del CPU tal como la declara el, para la ventana de propiedades del
 * sistema. Se consulta a demanda y no se cachea: es una lectura de CPUID cada
 * varios segundos, no algo que valga la pena guardar.
 *
 * Las tres hojas del brand string se copian crudas y recien despues se recorta:
 * el relleno de Intel va ADELANTE ("   Intel(R) Core(TM)..."), asi que cortar
 * hoja por hoja partiria el nombre al medio. */
void query_cpu_identity(CpuIdentity& out) {
    uint32_t eax = 0;
    uint32_t ebx = 0;
    uint32_t ecx = 0;
    uint32_t edx = 0;

    for (size_t index = 0; index < sizeof(out.vendor); ++index) {
        out.vendor[index] = '\0';
    }
    for (size_t index = 0; index < sizeof(out.brand); ++index) {
        out.brand[index] = '\0';
    }
    out.features = 0;

    cpuid(0, 0, eax, ebx, ecx, edx);
    const uint32_t vendor_words[3] = {ebx, edx, ecx};
    for (size_t word = 0; word < 3; ++word) {
        for (size_t byte = 0; byte < 4; ++byte) {
            out.vendor[word * 4 + byte] = static_cast<char>((vendor_words[word] >> (byte * 8)) & 0xff);
        }
    }
    out.vendor[12] = '\0';

    cpuid(1, 0, eax, ebx, ecx, edx);
    if ((edx & (1u << 26)) != 0) {
        out.features |= kCpuFeatureSse2;
    }
    if ((edx & (1u << 6)) != 0) {
        out.features |= kCpuFeaturePae;
    }
    if ((ecx & (1u << 21)) != 0) {
        out.features |= kCpuFeatureX2Apic;
    }
    // Bit reservado en hardware real; los hipervisores lo aciertan para
    // anunciarse. Es la unica via portable de saber que esto es una VM.
    if ((ecx & (1u << 31)) != 0) {
        out.features |= kCpuFeatureHypervisor;
    }

    cpuid(0x80000000u, 0, eax, ebx, ecx, edx);
    const uint32_t highest_extended = eax;
    if (highest_extended >= 0x80000001u) {
        uint32_t ext_edx = 0;
        cpuid(0x80000001u, 0, eax, ebx, ecx, ext_edx);
        if ((ext_edx & (1u << 20)) != 0) {
            out.features |= kCpuFeatureNx;
        }
        if ((ext_edx & (1u << 29)) != 0) {
            out.features |= kCpuFeatureLongMode;
        }
    }

    if (highest_extended < 0x80000004u) {
        return;
    }

    char raw[49] = {};
    for (uint32_t leaf = 0; leaf < 3; ++leaf) {
        cpuid(0x80000002u + leaf, 0, eax, ebx, ecx, edx);
        const uint32_t words[4] = {eax, ebx, ecx, edx};
        for (size_t word = 0; word < 4; ++word) {
            for (size_t byte = 0; byte < 4; ++byte) {
                raw[leaf * 16 + word * 4 + byte] = static_cast<char>((words[word] >> (byte * 8)) & 0xff);
            }
        }
    }
    raw[48] = '\0';

    size_t first = 0;
    while (raw[first] == ' ') {
        first += 1;
    }
    size_t last = first;
    for (size_t index = first; raw[index] != '\0'; ++index) {
        if (raw[index] != ' ') {
            last = index;
        }
    }

    size_t length = raw[first] == '\0' ? 0 : (last - first + 1);
    if (length >= sizeof(out.brand)) {
        length = sizeof(out.brand) - 1;
    }
    for (size_t index = 0; index < length; ++index) {
        out.brand[index] = raw[first + index];
    }
    out.brand[length] = '\0';
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

uint64_t save_and_disable_interrupts() {
    uint64_t flags = 0;
    asm volatile("pushfq; pop %0; cli" : "=r"(flags) : : "memory");
    return flags;
}

void restore_interrupts(uint64_t flags) {
    asm volatile("push %0; popfq" : : "r"(flags) : "memory", "cc");
}

void flush_tlb() {
    constexpr uint64_t kCr4Pge = 1ull << 7;
    uint64_t cr4 = 0;
    asm volatile("mov %%cr4, %0" : "=r"(cr4));
    if ((cr4 & kCr4Pge) != 0) {
        asm volatile("mov %0, %%cr4" : : "r"(cr4 & ~kCr4Pge) : "memory");
        asm volatile("mov %0, %%cr4" : : "r"(cr4) : "memory");
        return;
    }
    load_page_table_root(read_page_table_root());
}

void halt_once() {
    asm volatile("hlt");
}

void enable_interrupts_and_halt() {
    // AMD erratum 400: SMM may have set C1E on halt back since the last idle.
    if (g_amd_c1e_quirk) {
        clear_amd_c1e();
    }
    asm volatile("sti; hlt");
}

// See lock_spin_hint() in cpu.hpp. initialize_cpu() clears it under TCG,
// before any AP starts.
bool g_lock_spin_pause = true;

// See user_access_begin() in cpu.hpp. Set by initialize_cpu() on the BSP.
bool g_smap_enabled = false;

[[noreturn]] void halt_forever() {
    for (;;) {
        halt_once();
    }
}

} // namespace arch
