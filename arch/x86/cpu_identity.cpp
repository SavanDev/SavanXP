// Architecture-independent CPUID query from the pinned v0.4.0 CPU boundary.
#include "kernel/cpu.hpp"
namespace arch::x86_64 {
namespace {
void cpuid(uint32_t leaf, uint32_t subleaf, uint32_t& eax, uint32_t& ebx, uint32_t& ecx, uint32_t& edx) {
    asm volatile("cpuid" : "=a"(eax), "=b"(ebx), "=c"(ecx), "=d"(edx) : "a"(leaf), "c"(subleaf));
}
}
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
    // Hypervisors advertise themselves through the reserved CPUID feature bit.
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
}
