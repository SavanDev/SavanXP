#include "../../arch/x86/madt.hpp"
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <vector>

namespace {
void put32(uint8_t* p, uint32_t value) {
    for (unsigned n = 0; n < 4; ++n) p[n] = value >> (n * 8);
}
std::vector<uint8_t> header() {
    std::vector<uint8_t> bytes(44);
    bytes[0] = 'A'; bytes[1] = 'P'; bytes[2] = 'I'; bytes[3] = 'C';
    put32(bytes.data() + 36, 0xfee00000);
    return bytes;
}
void cpu(std::vector<uint8_t>& bytes, uint32_t id, uint32_t flags = 1, bool x2 = false) {
    const size_t offset = bytes.size();
    bytes.resize(offset + (x2 ? 16 : 8));
    auto* entry = bytes.data() + offset;
    entry[0] = x2 ? 9 : 0; entry[1] = x2 ? 16 : 8;
    if (x2) { put32(entry + 4, id); put32(entry + 8, flags); }
    else { entry[3] = id; put32(entry + 4, flags); }
}
void checksum(std::vector<uint8_t>& bytes) {
    put32(bytes.data() + 4, bytes.size());
    bytes[9] = 0;
    uint8_t sum = 0;
    for (auto value : bytes) sum += value;
    bytes[9] = -sum;
}
bool parse(std::vector<uint8_t> bytes, i386::madt::Topology& out, uint32_t bsp = 0) {
    checksum(bytes);
    return i386::madt::parse(bytes.data(), bytes.size(), bsp, out);
}
}
int main() {
    i386::madt::Topology out;
    auto bytes = header();
    cpu(bytes, 7); cpu(bytes, 3, 2); cpu(bytes, 0); cpu(bytes, 19, 1, true);
    assert(parse(bytes, out));
    assert(out.reported == 3 && out.count == 3 && out.ids[0] == 0 && out.ids[1] == 7 && out.ids[2] == 19);
    assert(!parse(bytes, out, 1)); // The BSP must be in the enabled CPU list.
    auto duplicate = bytes; cpu(duplicate, 7, 1, true);
    assert(!parse(duplicate, out));
    auto high_id = bytes; cpu(high_id, 256, 1, true);
    assert(!parse(high_id, out));
    auto truncated = bytes; truncated.push_back(0); truncated.push_back(8);
    assert(!parse(truncated, out));
    auto short_cpu = header(); short_cpu.push_back(0); short_cpu.push_back(2);
    assert(!parse(short_cpu, out));
    auto zero_length = bytes; zero_length.push_back(42); zero_length.push_back(0);
    assert(!parse(zero_length, out));
    auto unknown = bytes; unknown.push_back(42); unknown.push_back(2);
    assert(parse(unknown, out));
    auto override = bytes;
    size_t offset = override.size(); override.resize(offset + 12);
    override[offset] = 5; override[offset + 1] = 12;
    put32(override.data() + offset + 4, 0xfec00000);
    assert(parse(override, out) && out.lapic_address == 0xfec00000);
    put32(override.data() + offset + 8, 1);
    assert(!parse(override, out)); // Native MMIO cannot address above 4 GiB.
    auto many = header();
    for (uint32_t id = 1; id < 40; ++id) cpu(many, id);
    cpu(many, 0); // Keep the BSP even when it appears after the storage bound.
    assert(parse(many, out) && out.reported == 40 && out.count == 32 && out.ids[0] == 0);
    checksum(bytes);
    assert(!i386::madt::parse(bytes.data(), 43, 0, out));
    assert(!i386::madt::parse(bytes.data(), bytes.size() - 1, 0, out));
    bytes[9] ^= 1;
    assert(!i386::madt::parse(bytes.data(), bytes.size(), 0, out));
    assert(!i386::madt::parse(nullptr, 44, 0, out));
    puts("i386 MADT topology, bounds, IDs and BSP ordering: PASS");
}
