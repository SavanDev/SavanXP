#pragma once
#include <array>
#include <map>
#include <tuple>
#include <vector>
#include "azalia/azalia_port.hpp"

namespace azalia_test {
using namespace azalia_port;
struct Hardware {
    azalia_t az;
    std::array<uint32_t, 256> corb = {};
    std::array<rirb_entry_t, 256> rirb = {};
    std::map<unsigned, uint32_t> regs;
    std::map<std::tuple<unsigned,unsigned,unsigned>,uint32_t> replies;
    std::vector<std::tuple<unsigned,unsigned,unsigned>> commands;
    unsigned wp = 0;
    std::tuple<unsigned,unsigned,unsigned> fail_once = {~0u,0,0};
    bool answer = true, saw_full_bdl = false;
    bdlist_entry_t bdl[32] = {};
    unsigned char pcm[32 * 4096] = {};
    Hardware(uint8_t sizes = 0x40) {
        az.io = {this, read, write, delay};
        az.corb_dma = {corb.data(), 0x10000};
        az.rirb_dma = {rirb.data(), 0x11000};
        regs[0x4e] = sizes; regs[0x5e] = sizes;
    }
    static uint32_t read(void* p, uint16_t r, unsigned) { return static_cast<Hardware*>(p)->regs[r]; }
    static void delay(void*, unsigned) {}
    static void write(void* p, uint16_t r, uint32_t v, unsigned) {
        auto& h = *static_cast<Hardware*>(p);
        if (r == 0x4e || r == 0x5e) { h.regs[r] = (h.regs[r] & 0xf0) | (v & 3); return; }
        if (r == 0x4a) { h.regs[r] = v & 0x8000; return; }
        if (r == 0x58 && (v & 0x8000)) { h.regs[r] = 0; h.wp = 0; return; }
        if (r == 0x83 || r == 0x103) { h.regs[r] &= ~v; return; }
        h.regs[r] = v;
        if (r == 0x48 && h.answer) {
            const uint32_t word = h.corb[v & 0xff];
            unsigned nid = (word >> 20) & 0xff, verb = (word >> 8) & 0xfff, payload = word & 0xff;
            if (((word >> 16) & 0xf) < 7) { verb &= 0xf00; payload = word & 0xffff; }
            h.commands.emplace_back(nid,verb,payload);
            if (h.fail_once == std::tuple{nid,verb,payload}) {
                h.fail_once = {~0u,0,0}; return;
            }
            const unsigned entries = h.az.rirb_entries;
            h.wp = (h.wp + 1) % entries;
            h.rirb[h.wp] = {h.replies[{nid,verb,payload}], word >> 28};
            h.regs[0x58] = h.wp;
        }
        if (r == 0x100 && (v & 2)) {
            h.saw_full_bdl = h.regs[0x10c] == 31 && h.regs[0x108] == sizeof(h.pcm);
            for (unsigned i = 0; i < 32; ++i)
                h.saw_full_bdl &= h.bdl[i].length == 4096 && h.bdl[i].flags == 0 &&
                    h.bdl[i].low == 0x20000 + i * 4096;
        }
    }
    bool sent(unsigned nid, unsigned verb, unsigned payload) const {
        for (auto x : commands) if (x == std::tuple{nid,verb,payload}) return true;
        return false;
    }
    void param(unsigned nid, unsigned id, uint32_t value) { replies[{nid,0xf00,id}] = value; }
};
inline void alc892(Hardware& h) {
    // Relevant nodes read from the host ALC892 Linux codec dump, 2026-11-07.
    // Rear green 0x14 -> mixer 0x0c -> DAC 0x02. 0x15 is disconnected despite
    // its firmware "Speaker" device type, and must not outrank that line-out.
    h.param(0,0,0x10ec0892); h.param(0,4,0x00010001); h.param(1,5,1);
    h.param(1,4,0x0002001a); h.param(1,0x0b,1); h.param(1,0x0a,0x00020040);
    h.param(2,9,0x41d); h.param(2,0x0b,1); h.param(2,0x0a,0x00020040);
    h.param(2,0x12,0x00034040);
    h.param(0x0c,9,0x20010b); h.param(0x0c,0x0d,0x80000000);
    h.param(0x0c,0x0e,2); h.replies[{0x0c,0xf02,0}] = 0x00000b02;
    h.param(0x14,9,0x40058d); h.param(0x14,0x0c,0x1003e);
    h.param(0x14,0x12,0x80000000); h.replies[{0x14,0xf1c,0}] = 0x01014010;
    h.param(0x14,0x0e,1); h.replies[{0x14,0xf02,0}] = 0x0c;
    h.param(0x15,9,0x40058d); h.param(0x15,0x0c,0x36);
    h.replies[{0x15,0xf1c,0}] = 0x411111f0;
    h.param(0x15,0x0e,1); h.replies[{0x15,0xf02,0}] = 0x0c;
}
} // namespace azalia_test
