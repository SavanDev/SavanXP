/*
 * win32_pe_test.cpp -- Host test of the Win32 PE32 parser and mapper
 * (subsystems/win32/pe/).
 *
 * With no argument it builds a small valid DLL in memory (imports, exports
 * with a forwarder, base relocations, a TLS directory with callbacks), checks
 * every stage against it, and then runs the malformed corpus: one mutation of
 * that image per rejection the parser promises.
 *
 * With `--fixtures DIR` (or SAVANXP_WIN32_FIXTURES) it reads the user's
 * Windows XP sol.exe and cards.dll, verifies the W0 hashes, and checks what
 * docs/userland/WIN32_SOLITAIRE_PROFILE.md records about them. Without the
 * files it exits with 77, which CTest reports as skipped.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <functional>
#include <string>
#include <vector>

extern "C" {
#include "pe.h"
}

namespace {

int g_failures = 0;
int g_checks = 0;

bool check(bool ok, const std::string &what) {
    ++g_checks;
    if (!ok) {
        ++g_failures;
        printf("  FAIL %s\n", what.c_str());
    }
    return ok;
}

std::string describe(const pe_error &error) {
    char text[160];
    pe_format_error(&error, text, sizeof(text));
    return text;
}

// ---- A synthetic DLL --------------------------------------------------------

constexpr uint32_t kBase = 0x10000000u;
constexpr uint32_t kLfanew = 0x80u;
constexpr uint32_t kCoff = kLfanew + 4u;
constexpr uint32_t kOpt = kCoff + 20u;
constexpr uint32_t kOptSize = 224u;
constexpr uint32_t kSections = kOpt + kOptSize;

struct SectionSpec {
    const char *name;
    uint32_t rva;
    uint32_t vsize;
    uint32_t raw;
    uint32_t raw_size;
    uint32_t flags;
};

constexpr uint32_t kR = PE_SCN_MEM_READ;
constexpr SectionSpec kLayout[] = {
    {".text", 0x1000u, 0x100u, 0x400u, 0x200u, kR | PE_SCN_MEM_EXECUTE | PE_SCN_CNT_CODE},
    {".rdata", 0x2000u, 0x400u, 0x600u, 0x400u, kR | PE_SCN_CNT_INITIALIZED_DATA},
    {".data", 0x3000u, 0x400u, 0xa00u, 0x200u, kR | PE_SCN_MEM_WRITE | PE_SCN_CNT_INITIALIZED_DATA},
    {".reloc", 0x4000u, 0x20u, 0xc00u, 0x200u, kR | PE_SCN_CNT_INITIALIZED_DATA},
};
constexpr uint32_t kSectionCount = 4u;

struct Image {
    std::vector<uint8_t> file = std::vector<uint8_t>(0xe00u, 0);

    void u8(uint32_t at, uint8_t v) { file.at(at) = v; }
    void u16(uint32_t at, uint16_t v) {
        u8(at, (uint8_t)v);
        u8(at + 1, (uint8_t)(v >> 8));
    }
    void u32(uint32_t at, uint32_t v) {
        u16(at, (uint16_t)v);
        u16(at + 2, (uint16_t)(v >> 16));
    }
    void str(uint32_t at, const char *s) {
        for (size_t i = 0; i <= strlen(s); ++i) u8(at + (uint32_t)i, (uint8_t)s[i]);
    }
    static uint32_t off(uint32_t rva) {
        for (const SectionSpec &s : kLayout) {
            if (rva >= s.rva && rva < s.rva + 0x1000u) return s.raw + (rva - s.rva);
        }
        abort();
    }
    // Writes at an RVA, through the section that holds it.
    void r16(uint32_t rva, uint16_t v) { u16(off(rva), v); }
    void r32(uint32_t rva, uint32_t v) { u32(off(rva), v); }
    void rstr(uint32_t rva, const char *s) { str(off(rva), s); }
    void directory(uint32_t index, uint32_t rva, uint32_t size) {
        u32(kOpt + 96u + 8u * index, rva);
        u32(kOpt + 100u + 8u * index, size);
    }
    uint32_t section_header(uint32_t index) const { return kSections + 40u * index; }
};

Image make_dll() {
    Image m;
    m.str(0, "MZ");
    m.u32(0x3c, kLfanew);
    m.str(kLfanew, "PE");
    m.u16(kCoff, 0x14c);
    m.u16(kCoff + 2, kSectionCount);
    m.u16(kCoff + 16, kOptSize);
    m.u16(kCoff + 18, PE_FILE_EXECUTABLE | PE_FILE_32BIT_MACHINE | PE_FILE_DLL);

    m.u16(kOpt, 0x10b);
    m.u32(kOpt + 16, 0x1000u);  // entry point
    m.u32(kOpt + 28, kBase);
    m.u32(kOpt + 32, 0x1000u);  // section alignment
    m.u32(kOpt + 36, 0x200u);   // file alignment
    m.u32(kOpt + 56, 0x5000u);  // size of image
    m.u32(kOpt + 60, 0x400u);   // size of headers
    m.u16(kOpt + 68, 3);        // console subsystem
    m.u32(kOpt + 72, 0x100000u);
    m.u32(kOpt + 76, 0x1000u);
    m.u32(kOpt + 92, 16u);

    for (uint32_t i = 0; i < kSectionCount; ++i) {
        const SectionSpec &s = kLayout[i];
        uint32_t h = m.section_header(i);
        m.str(h, s.name);
        m.u32(h + 8, s.vsize);
        m.u32(h + 12, s.rva);
        m.u32(h + 16, s.raw_size);
        m.u32(h + 20, s.raw);
        m.u32(h + 36, s.flags);
    }

    // .text: the entry point and two TLS callbacks, each a `ret`.
    m.u8(Image::off(0x1000), 0xc3);
    m.u8(Image::off(0x1010), 0xc3);
    m.u8(Image::off(0x1020), 0xc3);

    // Imports: KERNEL32.dll by name, other.dll by ordinal.
    m.r32(0x2000, 0x2080);  // lookup table
    m.r32(0x200c, 0x2100);  // name
    m.r32(0x2010, 0x3000);  // IAT
    m.r32(0x2014, 0x2090);
    m.r32(0x2020, 0x2110);
    m.r32(0x2024, 0x3010);
    m.r32(0x2080, 0x2120);
    m.r32(0x2084, 0x2130);
    m.r32(0x2090, 0x80000007u);
    m.rstr(0x2100, "KERNEL32.dll");
    m.rstr(0x2110, "other.dll");
    m.r16(0x2120, 5);
    m.rstr(0x2122, "WriteFile");
    m.r16(0x2130, 0);
    m.rstr(0x2132, "ExitProcess");
    m.r32(0x3000, 0x2120);  // the IAT starts as a copy of the lookup table
    m.r32(0x3004, 0x2130);
    m.r32(0x3010, 0x80000007u);
    m.directory(PE_DIR_IMPORT, 0x2000, 60);
    m.directory(PE_DIR_IAT, 0x3000, 0x20);

    // Exports: Alpha (ordinal 1), Beta forwarded to other.Gamma (ordinal 2),
    // ordinal 3 unused.
    m.r32(0x220c, 0x2280);
    m.r32(0x2210, 1);
    m.r32(0x2214, 3);
    m.r32(0x2218, 2);
    m.r32(0x221c, 0x2240);
    m.r32(0x2220, 0x2250);
    m.r32(0x2224, 0x2260);
    m.r32(0x2240, 0x1000);
    m.r32(0x2244, 0x22a0);
    m.r32(0x2250, 0x2290);
    m.r32(0x2254, 0x2298);
    m.r16(0x2260, 0);
    m.r16(0x2262, 1);
    m.rstr(0x2280, "test.dll");
    m.rstr(0x2290, "Alpha");
    m.rstr(0x2298, "Beta");
    m.rstr(0x22a0, "other.Gamma");
    m.directory(PE_DIR_EXPORT, 0x2200, 0x100);

    // TLS: an 0x10-byte template, 0x20 bytes of zero fill, two callbacks.
    m.r32(0x2300, kBase + 0x3200);
    m.r32(0x2304, kBase + 0x3210);
    m.r32(0x2308, kBase + 0x3300);
    m.r32(0x230c, kBase + 0x2340);
    m.r32(0x2310, 0x20);
    m.r32(0x2340, kBase + 0x1010);
    m.r32(0x2344, kBase + 0x1020);
    m.directory(PE_DIR_TLS, 0x2300, 24);

    // .data: a pointer the relocations must move, and the TLS template.
    m.r32(0x3100, kBase + 0x1000);
    for (uint32_t i = 0; i < 0x10; ++i) m.u8(Image::off(0x3200 + i), (uint8_t)(0xa0 + i));

    // Relocations: the five VAs in .rdata, padding, then the .data pointer.
    const uint16_t rdata[] = {0x3300, 0x3304, 0x3308, 0x330c, 0x3340, 0x3344, 0x0000, 0x0000};
    m.r32(0x4000, 0x2000);
    m.r32(0x4004, 8 + sizeof(rdata));
    for (uint32_t i = 0; i < 8; ++i) m.r16(0x4008 + 2 * i, rdata[i]);
    m.r32(0x4018, 0x3000);
    m.r32(0x401c, 12);
    m.r16(0x4020, 0x3100);
    m.r16(0x4022, 0x0000);
    m.directory(PE_DIR_BASERELOC, 0x4000, 0x24);
    m.u32(m.section_header(3) + 8, 0x24);
    return m;
}

struct Loaded {
    pe_image image;
    std::vector<uint8_t> memory;
    pe_mapped mapped;
    pe_error error;
    pe_status status;
};

// Parses and maps; status is the first failure.
Loaded load(const std::vector<uint8_t> &file) {
    Loaded l{};
    l.status = pe_parse(&l.image, file.data(), file.size(), &l.error);
    if (l.status != PE_OK) return l;
    // A mutated size of image can legally reach 2 GiB; the test maps at most
    // 1 MiB, and pe_map() must refuse a buffer that is too small.
    l.memory.assign(l.image.size_of_image < 0x100000u ? l.image.size_of_image : 0x100000u, 0xcc);
    l.mapped = {l.memory.data(), (uint32_t)l.memory.size()};
    l.status = pe_map(&l.image, &l.mapped, &l.error);
    return l;
}

std::vector<std::string> collect_imports(Loaded &l, pe_status *status) {
    std::vector<std::string> out;
    *status = pe_for_each_import(
        &l.image, &l.mapped,
        [](void *context, const pe_import *import) {
            auto *list = static_cast<std::vector<std::string> *>(context);
            char line[300];
            if (import->name != nullptr) {
                snprintf(line, sizeof(line), "%s!%s@%x", import->dll, import->name, import->iat_rva);
            } else {
                snprintf(line, sizeof(line), "%s!#%u@%x", import->dll, import->ordinal, import->iat_rva);
            }
            list->push_back(line);
            return 0;
        },
        &out, &l.error);
    return out;
}

void test_valid_image() {
    printf("valid image\n");
    Image m = make_dll();
    Loaded l = load(m.file);
    if (!check(l.status == PE_OK, "parse and map: " + describe(l.error))) return;

    const pe_image &im = l.image;
    check(im.image_base == kBase && im.size_of_image == 0x5000u, "base and size");
    check(im.entry_rva == 0x1000u && im.subsystem == 3, "entry point and subsystem");
    check(im.section_count == 4 && strcmp(im.sections[2].name, ".data") == 0, "section names");
    check(im.sections[2].virtual_size == 0x1000u && im.sections[2].raw_size == 0x200u,
          "section extent is aligned, copy stops at the file data");

    check(l.memory[0] == 'M' && l.memory[kLfanew] == 'P', "headers copied");
    check(l.memory[0x1000] == 0xc3 && l.memory[0x2290] == 'A', "section data copied");
    bool zero = true;
    for (uint32_t i = 0x3200 + 0x200; i < 0x4000; ++i) zero = zero && l.memory[i] == 0;
    for (uint32_t i = 0x400; i < 0x1000; ++i) zero = zero && l.memory[i] == 0;
    check(zero, "tails and gaps are zero");

    pe_run runs[8];
    uint32_t count = 0;
    check(pe_protection_runs(&im, runs, 8, &count, &l.error) == PE_OK && count == 5,
          "five protection runs");
    const pe_run expected[] = {{0, 0x1000, PE_PROT_R},
                               {0x1000, 0x1000, PE_PROT_RX},
                               {0x2000, 0x1000, PE_PROT_R},
                               {0x3000, 0x1000, PE_PROT_RW},
                               {0x4000, 0x1000, PE_PROT_R}};
    bool same = count == 5;
    for (uint32_t i = 0; same && i < 5; ++i) {
        same = runs[i].rva == expected[i].rva && runs[i].size == expected[i].size &&
               runs[i].protection == expected[i].protection;
    }
    check(same, "protection runs match the sections");
    check(pe_protection_runs(&im, runs, 4, &count, &l.error) == PE_ERR_BUFFER,
          "too few run slots is refused");

    pe_status status;
    std::vector<std::string> imports = collect_imports(l, &status);
    check(status == PE_OK, "imports walk");
    check(imports == std::vector<std::string>{"KERNEL32.dll!WriteFile@3000",
                                              "KERNEL32.dll!ExitProcess@3004",
                                              "other.dll!#7@3010"},
          "imports by name and ordinal, with their IAT slots");
    std::vector<std::string> dlls;
    check(pe_for_each_imported_dll(
              &im, &l.mapped,
              [](void *c, const char *dll, uint32_t) {
                  static_cast<std::vector<std::string> *>(c)->push_back(dll);
                  return 0;
              },
              &dlls, &l.error) == PE_OK &&
              dlls == std::vector<std::string>{"KERNEL32.dll", "other.dll"},
          "imported DLLs in order");
    check(pe_for_each_import(&im, &l.mapped, [](void *, const pe_import *) { return 9; }, nullptr,
                             &l.error) == PE_ERR_CALLBACK &&
              l.error.value == 9,
          "a visitor can stop the walk");

    pe_export e{};
    int found = 0;
    check(pe_find_export_by_name(&im, &l.mapped, "Alpha", &e, &found, &l.error) == PE_OK && found &&
              e.rva == 0x1000u && e.forwarder == nullptr && e.ordinal == 1,
          "export by name");
    check(pe_find_export_by_name(&im, &l.mapped, "Beta", &e, &found, &l.error) == PE_OK && found &&
              e.rva == 0 && e.forwarder != nullptr && strcmp(e.forwarder, "other.Gamma") == 0,
          "forwarded export by name");
    check(pe_find_export_by_ordinal(&im, &l.mapped, 2, &e, &found, &l.error) == PE_OK && found &&
              e.forwarder != nullptr,
          "forwarded export by ordinal");
    check(pe_find_export_by_ordinal(&im, &l.mapped, 3, &e, &found, &l.error) == PE_OK && !found,
          "an empty ordinal slot is not exported");
    check(pe_find_export_by_ordinal(&im, &l.mapped, 0, &e, &found, &l.error) == PE_OK && !found &&
              pe_find_export_by_ordinal(&im, &l.mapped, 4, &e, &found, &l.error) == PE_OK && !found,
          "ordinals outside the table are not exported");
    check(pe_find_export_by_name(&im, &l.mapped, "Alph", &e, &found, &l.error) == PE_OK && !found,
          "a prefix is not a match");

    // Move the image and check every relocated VA.
    constexpr uint32_t kNewBase = 0x20000000u;
    check(pe_relocate(&im, &l.mapped, kNewBase, &l.error) == PE_OK, "relocate");
    uint32_t pointer = 0;
    pe_read_u32(&l.mapped, 0x3100, &pointer);
    check(pointer == kNewBase + 0x1000u, "a relocated pointer");
    pe_tls tls{};
    int present = 0;
    check(pe_read_tls(&im, &l.mapped, kNewBase, &tls, &present, &l.error) == PE_OK && present,
          "TLS directory after relocation");
    check(tls.template_rva == 0x3200u && tls.template_size == 0x10u && tls.zero_fill == 0x20u &&
              tls.index_rva == 0x3300u && tls.callbacks_rva == 0x2340u,
          "TLS fields");
    uint32_t callbacks[3] = {1, 1, 1};
    for (uint32_t i = 0; i < 3; ++i) {
        pe_tls_callback(&im, &l.mapped, kNewBase, &tls, i, &callbacks[i], &l.error);
    }
    check(callbacks[0] == 0x1010u && callbacks[1] == 0x1020u && callbacks[2] == 0,
          "TLS callbacks in order, then the end");
    check(pe_read_tls(&im, &l.mapped, kBase, &tls, &present, &l.error) == PE_ERR_TLS,
          "TLS read against the old base is refused");
    check(pe_relocate(&im, &l.mapped, kNewBase, &l.error) == PE_OK, "relocating to the same base is a no-op");
    check(pe_relocate(&im, &l.mapped, 0x20001000u, &l.error) == PE_ERR_IMAGE_BASE,
          "an unaligned new base is refused");

    std::vector<uint8_t> small(0x1000);
    pe_mapped tiny = {small.data(), (uint32_t)small.size()};
    check(pe_map(&im, &tiny, &l.error) == PE_ERR_BUFFER, "a short mapping buffer is refused");
}

void test_stripped_relocations() {
    printf("stripped relocations\n");
    Image m = make_dll();
    m.u16(kCoff + 18, PE_FILE_EXECUTABLE | PE_FILE_32BIT_MACHINE | PE_FILE_DLL | PE_FILE_RELOCS_STRIPPED);
    Loaded l = load(m.file);
    check(l.status == PE_OK, "parses");
    check(pe_relocate(&l.image, &l.mapped, kBase, &l.error) == PE_OK, "loading in place is fine");
    check(pe_relocate(&l.image, &l.mapped, 0x30000000u, &l.error) == PE_ERR_RELOCS_STRIPPED,
          "moving is refused: " + describe(l.error));
    Image n = make_dll();
    n.directory(PE_DIR_BASERELOC, 0, 0);
    Loaded k = load(n.file);
    check(pe_relocate(&k.image, &k.mapped, 0x30000000u, &k.error) == PE_ERR_RELOCS_STRIPPED,
          "no relocation directory is the same");
}

// ---- The malformed corpus ---------------------------------------------------

enum class Stage { Parse, Relocate, Imports, Exports, Tls };

struct Case {
    const char *name;
    Stage stage;
    pe_status expected;
    std::function<void(Image &)> mutate;
};

pe_status run_stage(Loaded &l, Stage stage) {
    if (l.status != PE_OK || stage == Stage::Parse) return l.status;
    switch (stage) {
    case Stage::Relocate:
        return pe_relocate(&l.image, &l.mapped, 0x20000000u, &l.error);
    case Stage::Imports: {
        // A real loader stops at the first symbol it cannot resolve; stop
        // after 64 so a mutated table cannot make the walk quadratic here.
        uint32_t seen = 0;
        pe_status status = pe_for_each_import(
            &l.image, &l.mapped,
            [](void *c, const pe_import *) { return ++*static_cast<uint32_t *>(c) > 64u ? 1 : 0; },
            &seen, &l.error);
        return status == PE_ERR_CALLBACK ? PE_OK : status;
    }
    case Stage::Exports: {
        pe_export e;
        int found;
        pe_status s = pe_find_export_by_name(&l.image, &l.mapped, "Beta", &e, &found, &l.error);
        return s != PE_OK ? s : pe_find_export_by_ordinal(&l.image, &l.mapped, 1, &e, &found, &l.error);
    }
    case Stage::Tls: {
        pe_tls tls;
        int present;
        pe_status s = pe_read_tls(&l.image, &l.mapped, kBase, &tls, &present, &l.error);
        for (uint32_t i = 0; s == PE_OK && i < 3; ++i) {
            uint32_t rva;
            s = pe_tls_callback(&l.image, &l.mapped, kBase, &tls, i, &rva, &l.error);
        }
        return s;
    }
    default:
        return l.status;
    }
}

void test_malformed_corpus() {
    printf("malformed corpus\n");
    const uint32_t text = Image().section_header(0);
    const uint32_t data = Image().section_header(2);
    const Case cases[] = {
        {"shorter than a DOS header", Stage::Parse, PE_ERR_TRUNCATED, [](Image &m) { m.file.resize(0x30); }},
        {"no MZ", Stage::Parse, PE_ERR_NOT_MZ, [](Image &m) { m.u8(0, 'Z'); }},
        {"e_lfanew past the end", Stage::Parse, PE_ERR_TRUNCATED, [](Image &m) { m.u32(0x3c, 0xfffffff0u); }},
        {"no PE signature", Stage::Parse, PE_ERR_NOT_PE, [](Image &m) { m.u8(kLfanew + 1, 'X'); }},
        {"AMD64 machine", Stage::Parse, PE_ERR_MACHINE, [](Image &m) { m.u16(kCoff, 0x8664); }},
        {"ARM machine", Stage::Parse, PE_ERR_MACHINE, [](Image &m) { m.u16(kCoff, 0x1c0); }},
        {"PE32+ optional header", Stage::Parse, PE_ERR_PE32PLUS, [](Image &m) { m.u16(kOpt, 0x20b); }},
        {"ROM optional header", Stage::Parse, PE_ERR_OPTIONAL_HEADER, [](Image &m) { m.u16(kOpt, 0x107); }},
        {"optional header too small", Stage::Parse, PE_ERR_OPTIONAL_HEADER, [](Image &m) { m.u16(kCoff + 16, 0x50); }},
        {"directories past the optional header", Stage::Parse, PE_ERR_OPTIONAL_HEADER, [](Image &m) { m.u16(kCoff + 16, 96 + 8 * 4); }},
        {"optional header past the end", Stage::Parse, PE_ERR_TRUNCATED, [](Image &m) { m.file.resize(kOpt + 100); }},
        {"not executable", Stage::Parse, PE_ERR_NOT_EXECUTABLE, [](Image &m) { m.u16(kCoff + 18, PE_FILE_DLL); }},
        {"no sections", Stage::Parse, PE_ERR_SECTION_COUNT, [](Image &m) { m.u16(kCoff + 2, 0); }},
        {"too many sections", Stage::Parse, PE_ERR_SECTION_COUNT, [](Image &m) { m.u16(kCoff + 2, 200); }},
        {"section table past the end", Stage::Parse, PE_ERR_TRUNCATED, [](Image &m) { m.u16(kCoff + 2, 90); }},
        {"low section alignment", Stage::Parse, PE_ERR_ALIGNMENT, [](Image &m) { m.u32(kOpt + 32, 0x200); }},
        {"section alignment not a power of two", Stage::Parse, PE_ERR_ALIGNMENT, [](Image &m) { m.u32(kOpt + 32, 0x3000); }},
        {"file alignment above section alignment", Stage::Parse, PE_ERR_ALIGNMENT, [](Image &m) { m.u32(kOpt + 36, 0x2000); }},
        {"file alignment below 512", Stage::Parse, PE_ERR_ALIGNMENT, [](Image &m) { m.u32(kOpt + 36, 0x100); }},
        {"unaligned image base", Stage::Parse, PE_ERR_IMAGE_BASE, [](Image &m) { m.u32(kOpt + 28, kBase + 0x1000); }},
        {"image wraps 4 GiB", Stage::Parse, PE_ERR_IMAGE_BASE, [](Image &m) { m.u32(kOpt + 28, 0xffff0000u); m.u32(kOpt + 56, 0x11000); }},
        {"zero size of image", Stage::Parse, PE_ERR_IMAGE_SIZE, [](Image &m) { m.u32(kOpt + 56, 0); }},
        {"huge size of image", Stage::Parse, PE_ERR_IMAGE_SIZE, [](Image &m) { m.u32(kOpt + 56, 0x90000000u); }},
        {"headers past the end of the file", Stage::Parse, PE_ERR_IMAGE_SIZE, [](Image &m) { m.u32(kOpt + 60, 0x10000); }},
        {"headers end inside the section table", Stage::Parse, PE_ERR_IMAGE_SIZE, [](Image &m) { m.u32(kOpt + 60, kSections + 8); }},
        {"unaligned section", Stage::Parse, PE_ERR_SECTION_LAYOUT, [](Image &m) { m.u32(m.section_header(1) + 12, 0x2800); }},
        {"overlapping sections", Stage::Parse, PE_ERR_SECTION_LAYOUT, [](Image &m) { m.u32(m.section_header(1) + 8, 0x1400); m.u32(m.section_header(2) + 12, 0x2000); }},
        {"gap between sections", Stage::Parse, PE_ERR_SECTION_LAYOUT, [](Image &m) { m.u32(m.section_header(3) + 12, 0x5000); m.u32(kOpt + 56, 0x6000); }},
        {"section outside the image", Stage::Parse, PE_ERR_SECTION_LAYOUT, [](Image &m) { m.u32(kOpt + 56, 0x4000); }},
        {"empty section", Stage::Parse, PE_ERR_SECTION_LAYOUT, [](Image &m) { m.u32(m.section_header(3) + 8, 0); m.u32(m.section_header(3) + 16, 0); }},
        {"section data truncated", Stage::Parse, PE_ERR_SECTION_RAW, [](Image &m) { m.file.resize(0xb00); }},
        {"section data offset past the end", Stage::Parse, PE_ERR_SECTION_RAW, [data](Image &m) { m.u32(data + 20, 0xfffffe00u); }},
        {"writable code", Stage::Parse, PE_ERR_SECTION_WX, [text](Image &m) { m.u32(text + 36, kR | PE_SCN_MEM_EXECUTE | PE_SCN_MEM_WRITE); }},
        {"entry point in data", Stage::Parse, PE_ERR_ENTRY_POINT, [](Image &m) { m.u32(kOpt + 16, 0x3000); }},
        {"entry point outside the image", Stage::Parse, PE_ERR_ENTRY_POINT, [](Image &m) { m.u32(kOpt + 16, 0x90000); }},
        {"executable with no entry point", Stage::Parse, PE_ERR_ENTRY_POINT, [](Image &m) { m.u32(kOpt + 16, 0); m.u16(kCoff + 18, PE_FILE_EXECUTABLE); }},
        {"import directory outside the image", Stage::Parse, PE_ERR_DIRECTORY, [](Image &m) { m.directory(PE_DIR_IMPORT, 0x4f00, 0x200); }},
        {"export directory at RVA 0", Stage::Parse, PE_ERR_DIRECTORY, [](Image &m) { m.directory(PE_DIR_EXPORT, 0, 0x40); }},
        {"delay-load imports", Stage::Parse, PE_ERR_UNSUPPORTED_DIRECTORY, [](Image &m) { m.directory(PE_DIR_DELAY_IMPORT, 0x2400, 0x20); }},
        {"managed image", Stage::Parse, PE_ERR_UNSUPPORTED_DIRECTORY, [](Image &m) { m.directory(PE_DIR_CLR, 0x2400, 0x48); }},

        {"relocation block shorter than its header", Stage::Relocate, PE_ERR_RELOC_BLOCK, [](Image &m) { m.r32(0x4004, 6); }},
        {"relocation block past the directory", Stage::Relocate, PE_ERR_RELOC_BLOCK, [](Image &m) { m.r32(0x401c, 0x40); }},
        {"odd relocation block size", Stage::Relocate, PE_ERR_RELOC_BLOCK, [](Image &m) { m.r32(0x401c, 11); }},
        {"64-bit relocation", Stage::Relocate, PE_ERR_RELOC_TYPE, [](Image &m) { m.r16(0x4020, 0xa100); }},
        {"relocation outside the image", Stage::Relocate, PE_ERR_RELOC_TARGET, [](Image &m) { m.r32(0x4018, 0x4ffe0); }},
        {"relocation straddling the end", Stage::Relocate, PE_ERR_RELOC_TARGET, [](Image &m) { m.r32(0x4018, 0x4000); m.r16(0x4020, 0x3ffe); }},

        {"DLL name outside the image", Stage::Imports, PE_ERR_STRING, [](Image &m) { m.r32(0x200c, 0x90000); }},
        {"empty DLL name", Stage::Imports, PE_ERR_STRING, [](Image &m) { m.r32(0x200c, 0x2400); }},
        {"descriptor with no IAT", Stage::Imports, PE_ERR_IMPORT, [](Image &m) { m.r32(0x2010, 0); }},
        {"bound import with no lookup table", Stage::Imports, PE_ERR_IMPORT_BOUND, [](Image &m) { m.r32(0x2000, 0); m.r32(0x2004, 0xffffffffu); }},
        {"ordinal with stray bits", Stage::Imports, PE_ERR_IMPORT, [](Image &m) { m.r32(0x2090, 0x80010007u); }},
        {"hint/name outside the image", Stage::Imports, PE_ERR_IMPORT, [](Image &m) { m.r32(0x2080, 0x7fff0000u); }},
        {"import name unterminated at the image end", Stage::Imports, PE_ERR_STRING, [](Image &m) {
             // The last page of .reloc is file data of 'x' with no NUL.
             m.r32(0x2080, 0x4ff0);
             m.u32(m.section_header(3) + 8, 0x1000);
             m.u32(m.section_header(3) + 16, 0x1000);
             m.file.resize(0xc00 + 0x1000, 'x');
         }},
        {"lookup table running off the image", Stage::Imports, PE_ERR_IMPORT, [](Image &m) { m.r32(0x2014, 0x4ffe); }},
        {"descriptors running off the image", Stage::Imports, PE_ERR_IMPORT, [](Image &m) { m.directory(PE_DIR_IMPORT, 0x4ff0, 0x10); }},

        {"export function count too large", Stage::Exports, PE_ERR_EXPORT, [](Image &m) { m.r32(0x2214, 0x01000000u); }},
        {"export base above 65535", Stage::Exports, PE_ERR_EXPORT, [](Image &m) { m.r32(0x2210, 0x10000); }},
        {"export directory too small", Stage::Exports, PE_ERR_EXPORT, [](Image &m) { m.directory(PE_DIR_EXPORT, 0x2200, 0x20); }},
        {"export name ordinal past the table", Stage::Exports, PE_ERR_EXPORT, [](Image &m) { m.r16(0x2262, 9); }},
        {"forwarder without a dot", Stage::Exports, PE_ERR_EXPORT, [](Image &m) { m.rstr(0x22a0, "otherGamma"); }},
        {"export outside the image", Stage::Exports, PE_ERR_EXPORT, [](Image &m) { m.r32(0x2240, 0x90000); }},
        {"export name outside the image", Stage::Exports, PE_ERR_STRING, [](Image &m) { m.r32(0x2250, 0x90000); }},

        {"TLS template ends before it starts", Stage::Tls, PE_ERR_TLS, [](Image &m) { m.r32(0x2304, kBase + 0x3100); }},
        {"TLS index outside the image", Stage::Tls, PE_ERR_TLS, [](Image &m) { m.r32(0x2308, kBase + 0x90000); }},
        {"TLS directory too small", Stage::Tls, PE_ERR_TLS, [](Image &m) { m.directory(PE_DIR_TLS, 0x2300, 8); }},
        {"TLS callback in data", Stage::Tls, PE_ERR_TLS, [](Image &m) { m.r32(0x2344, kBase + 0x3000); }},
        {"TLS callback below the image", Stage::Tls, PE_ERR_TLS, [](Image &m) { m.r32(0x2340, 0x1000); }},
    };

    for (const Case &c : cases) {
        Image m = make_dll();
        c.mutate(m);
        Loaded l = load(m.file);
        pe_status got = run_stage(l, c.stage);
        std::string text = describe(l.error);
        check(got == c.expected && !text.empty(),
              std::string(c.name) + ": got " + std::to_string(got) + " (" + text + "), expected " +
                  std::to_string(c.expected));
    }
    printf("  %zu cases\n", sizeof(cases) / sizeof(cases[0]));
}

// Random byte changes to the valid image, through every stage. Whatever the
// result, nothing may read or write outside its buffers (the sanitizers
// check that) and every failure must have a message.
void test_mutations() {
    printf("mutations\n");
    const Image base = make_dll();
    uint32_t state = 0x5a17u;
    auto next = [&state]() {
        state ^= state << 13;
        state ^= state >> 17;
        state ^= state << 5;
        return state;
    };
    uint32_t accepted = 0;
    bool messages = true;
    const Stage stages[] = {Stage::Relocate, Stage::Imports, Stage::Exports, Stage::Tls};
    for (int round = 0; round < 20000; ++round) {
        Image m = base;
        uint32_t edits = 1 + next() % 4;
        for (uint32_t e = 0; e < edits; ++e) {
            // Mostly headers and tables, where the parser makes decisions.
            uint32_t at = next() % 4 == 0 ? next() % (uint32_t)m.file.size()
                                          : next() % 2 == 0 ? next() % 0x400u : 0x600u + next() % 0x400u;
            m.file[at] = (uint8_t)next();
        }
        if (next() % 16 == 0) m.file.resize(next() % m.file.size());
        for (Stage stage : stages) {
            Loaded l = load(m.file);
            pe_status status = run_stage(l, stage);
            if (status != PE_OK) {
                messages = messages && describe(l.error) != "unknown error";
            } else if (stage == Stage::Tls) {
                ++accepted;
            }
        }
    }
    check(messages, "every rejection has a message");
    printf("  20000 mutated images, %u accepted through every stage\n", accepted);
}

void test_diagnostics() {
    printf("diagnostics\n");
    pe_error e = {PE_ERR_MACHINE, 0, 0x8664};
    check(describe(e) == "unsupported machine 0x00008664 (only i386)", "machine is named");
    e = {PE_ERR_SECTION_WX, 2, 0};
    check(describe(e) == "section 2 is both writable and executable", "section index is named");
    char tiny[8];
    size_t n = pe_format_error(&e, tiny, sizeof(tiny));
    check(n == 7 && strlen(tiny) == 7, "a short buffer is truncated and terminated");
    for (int s = PE_OK; s <= PE_ERR_CALLBACK; ++s) {
        e = {(pe_status)s, 0, 0};
        check(describe(e) != "unknown error", "status " + std::to_string(s) + " has a message");
    }
}

// ---- The user's Windows XP files ------------------------------------------

struct Sha256 {
    uint32_t h[8] = {0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a,
                     0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19};

    static uint32_t rotr(uint32_t x, int n) { return (x >> n) | (x << (32 - n)); }

    void block(const uint8_t *p) {
        static const uint32_t k[64] = {
            0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5,
            0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3, 0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174,
            0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da,
            0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967,
            0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85,
            0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
            0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3,
            0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208, 0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2};
        uint32_t w[64];
        for (int i = 0; i < 16; ++i) {
            w[i] = (uint32_t)p[4 * i] << 24 | (uint32_t)p[4 * i + 1] << 16 | (uint32_t)p[4 * i + 2] << 8 | p[4 * i + 3];
        }
        for (int i = 16; i < 64; ++i) {
            uint32_t s0 = rotr(w[i - 15], 7) ^ rotr(w[i - 15], 18) ^ (w[i - 15] >> 3);
            uint32_t s1 = rotr(w[i - 2], 17) ^ rotr(w[i - 2], 19) ^ (w[i - 2] >> 10);
            w[i] = w[i - 16] + s0 + w[i - 7] + s1;
        }
        uint32_t a = h[0], b = h[1], c = h[2], d = h[3], e = h[4], f = h[5], g = h[6], hh = h[7];
        for (int i = 0; i < 64; ++i) {
            uint32_t t1 = hh + (rotr(e, 6) ^ rotr(e, 11) ^ rotr(e, 25)) + ((e & f) ^ (~e & g)) + k[i] + w[i];
            uint32_t t2 = (rotr(a, 2) ^ rotr(a, 13) ^ rotr(a, 22)) + ((a & b) ^ (a & c) ^ (b & c));
            hh = g; g = f; f = e; e = d + t1; d = c; c = b; b = a; a = t1 + t2;
        }
        h[0] += a; h[1] += b; h[2] += c; h[3] += d; h[4] += e; h[5] += f; h[6] += g; h[7] += hh;
    }

    static std::string hex(const std::vector<uint8_t> &data) {
        Sha256 s;
        std::vector<uint8_t> m = data;
        uint64_t bits = (uint64_t)data.size() * 8u;
        m.push_back(0x80);
        while (m.size() % 64 != 56) m.push_back(0);
        for (int i = 7; i >= 0; --i) m.push_back((uint8_t)(bits >> (8 * i)));
        for (size_t i = 0; i < m.size(); i += 64) s.block(&m[i]);
        std::string out;
        char digits[9];
        for (uint32_t v : s.h) {
            snprintf(digits, sizeof(digits), "%08x", v);
            out += digits;
        }
        return out;
    }
};

bool read_file(const std::string &path, std::vector<uint8_t> &out) {
    FILE *f = fopen(path.c_str(), "rb");
    if (f == nullptr) return false;
    uint8_t buffer[65536];
    size_t n;
    while ((n = fread(buffer, 1, sizeof(buffer), f)) > 0) out.insert(out.end(), buffer, buffer + n);
    fclose(f);
    return true;
}

struct ImportCount {
    std::vector<std::string> dlls;
    uint32_t symbols = 0;
    uint32_t by_ordinal = 0;
};

ImportCount count_imports(Loaded &l) {
    ImportCount count;
    pe_for_each_import(
        &l.image, &l.mapped,
        [](void *c, const pe_import *import) {
            auto *count = static_cast<ImportCount *>(c);
            if (count->dlls.empty() || count->dlls.back() != import->dll) count->dlls.push_back(import->dll);
            ++count->symbols;
            count->by_ordinal += import->name == nullptr;
            return 0;
        },
        &count, &l.error);
    return count;
}

int test_fixtures(const std::string &dir) {
    printf("Windows XP fixtures in %s\n", dir.c_str());
    std::vector<uint8_t> sol, cards;
    if (!read_file(dir + "/sol.exe", sol) || !read_file(dir + "/cards.dll", cards)) {
        printf("  sol.exe or cards.dll not found: skipped\n");
        return 77;
    }
    if (!check(Sha256::hex(sol) == "ff301ce29c449239910c5be7224e6b65b81ed1f5ca7bcc8e6b72b0423d3f6341",
               "sol.exe matches the W0 hash") ||
        !check(Sha256::hex(cards) == "f90f768014ca4d8413987b758361a8d12a0b7b72e7546800617e21c189b0816d",
               "cards.dll matches the W0 hash")) {
        return 1;
    }

    Loaded s = load(sol);
    if (check(s.status == PE_OK, "sol.exe parses and maps: " + describe(s.error))) {
        check(s.image.image_base == 0x01000000u && s.image.size_of_image == 0x10000u &&
                  s.image.entry_rva == 0x5f85u && s.image.subsystem == 2,
              "sol.exe base, size, entry point and GUI subsystem");
        check(s.image.section_count == 3, "sol.exe has three sections");
        check(pe_relocate(&s.image, &s.mapped, 0x01000000u, &s.error) == PE_OK &&
                  pe_relocate(&s.image, &s.mapped, 0x02000000u, &s.error) == PE_ERR_RELOCS_STRIPPED,
              "sol.exe loads at 0x01000000 or not at all");
        ImportCount count = count_imports(s);
        check(s.error.status == PE_OK && count.symbols == 122 && count.dlls.size() == 8 &&
                  count.by_ordinal == 0,
              "sol.exe imports 122 functions by name from eight DLLs (got " +
                  std::to_string(count.symbols) + " from " + std::to_string(count.dlls.size()) + ")");
        pe_run runs[8];
        uint32_t n = 0;
        check(pe_protection_runs(&s.image, runs, 8, &n, &s.error) == PE_OK && n == 4 &&
                  runs[1].protection == PE_PROT_RX && runs[2].protection == PE_PROT_RW &&
                  runs[3].protection == PE_PROT_R,
              "sol.exe maps as headers, RX, RW, R");
        pe_tls tls;
        int present = 1;
        check(pe_read_tls(&s.image, &s.mapped, 0x01000000u, &tls, &present, &s.error) == PE_OK && !present,
              "sol.exe has no TLS directory");
    }

    Loaded c = load(cards);
    if (check(c.status == PE_OK, "cards.dll parses and maps: " + describe(c.error))) {
        check(c.image.image_base == 0x70140000u && c.image.size_of_image == 364u * 1024u &&
                  c.image.entry_rva == 0x1086u && (c.image.characteristics & PE_FILE_DLL) != 0,
              "cards.dll base, size, entry point, DLL flag");
        ImportCount count = count_imports(c);
        check(count.symbols == 15 && count.dlls.size() == 2, "cards.dll imports 15 functions from two DLLs");
        const char *names[] = {"WEP", "cdtAnimate", "cdtDraw", "cdtDrawExt", "cdtInit", "cdtTerm"};
        for (uint16_t i = 0; i < 6; ++i) {
            pe_export by_name{}, by_ordinal{};
            int found_name = 0, found_ordinal = 0;
            pe_find_export_by_name(&c.image, &c.mapped, names[i], &by_name, &found_name, &c.error);
            pe_find_export_by_ordinal(&c.image, &c.mapped, (uint16_t)(i + 1), &by_ordinal, &found_ordinal,
                                      &c.error);
            check(found_name && found_ordinal && by_name.ordinal == i + 1 && by_name.rva == by_ordinal.rva &&
                      pe_rva_is_executable(&c.image, by_name.rva),
                  std::string("cards.dll exports ") + names[i] + " as ordinal " + std::to_string(i + 1));
        }
        const pe_section *data = nullptr;
        for (uint32_t i = 0; i < c.image.section_count; ++i) {
            if (strcmp(c.image.sections[i].name, ".data") == 0) data = &c.image.sections[i];
        }
        check(data != nullptr && data->raw_size == 0, "cards.dll .data has no file data");
        uint32_t before = 0, after = 0, slot = 0;
        // Pick the first relocated slot and watch it move with the image.
        const pe_directory &relocs = c.image.directories[PE_DIR_BASERELOC];
        uint32_t page = 0, entry = 0;
        pe_read_u32(&c.mapped, relocs.rva, &page);
        pe_read_u32(&c.mapped, relocs.rva + 8, &entry);
        slot = page + (entry & 0x0fffu);
        pe_read_u32(&c.mapped, slot, &before);
        check(pe_relocate(&c.image, &c.mapped, 0x10000000u, &c.error) == PE_OK, "cards.dll relocates");
        pe_read_u32(&c.mapped, slot, &after);
        check((entry & 0xf000u) == 0x3000u && after - before == 0x10000000u - 0x70140000u,
              "a cards.dll relocation moved by the delta");
    }
    return 0;
}

}  // namespace

int main(int argc, char **argv) {
    if (argc == 3 && strcmp(argv[1], "--fixtures") == 0) {
        std::string dir = argv[2];
        if (dir.empty()) {
            const char *env = getenv("SAVANXP_WIN32_FIXTURES");
            dir = env != nullptr ? env : "";
        }
        if (dir.empty()) {
            printf("SAVANXP_WIN32_FIXTURES is not set: skipped\n");
            return 77;
        }
        int r = test_fixtures(dir);
        if (r == 77) return 77;
    } else {
        test_valid_image();
        test_stripped_relocations();
        test_malformed_corpus();
        test_mutations();
        test_diagnostics();
    }
    printf("%d checks, %d failures\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
