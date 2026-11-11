/*
 * sxfs_namespace_test.cpp -- Host test of the atomicity of SxFS namespace
 * mutations (docs/fs/SXFS_NAMESPACE.md), running the REAL driver
 * (kernel/sxfs.cpp) against the fake block device of sxfs_host_stubs.cpp.
 *
 * Every mutation that changes a directory -- create, mkdir, unlink, rmdir,
 * rename within a directory, across directories, of a directory, and over an
 * existing name -- plus the rewrite of a file and the reuse of freed blocks,
 * is cut at every sector it writes. Each cut is then recovered, and the
 * recovery itself is cut again at a sample of its own sectors. What remounts
 * must be writable, pass fscheck with no findings and no pending deletion,
 * and show the whole tree exactly as it was before the mutation or exactly as
 * it is after one of its steps: never a name lost, never a file with another
 * file's bytes.
 *
 * The same scenarios run on a volume filled as far as file data may fill it,
 * on one with not a sector free, and on a fragmented one; and with an I/O
 * error injected at every write instead of a power cut.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "kernel/sxfs.hpp"
#include "kernel/vfs.hpp"
#include "sxfs/sxfs_format.h"
#include "sxfs_core.h"
#include "sxfs_host_stubs.hpp"

namespace {

constexpr uint32_t kTotalSectors = 8192;
constexpr size_t kImageBytes = static_cast<size_t>(kTotalSectors) * SXFS_SECTOR_SIZE;

int g_failures = 0;
int g_checks = 0;

bool check(bool ok, const char* what) {
    ++g_checks;
    if (!ok) {
        ++g_failures;
        printf("  FAIL %s\n", what);
    }
    return ok;
}

// --- The tree ----------------------------------------------------------------

constexpr size_t kKeepBytes = 1500;
constexpr size_t kNewBytes = 3000;
constexpr size_t kReuseBytes = 700;
constexpr uint32_t kBigEntries = 70; // 5600 bytes of entries: 11 sectors

const char kHello[] = "hello: the file most scenarios move or delete\n";
const char kOther[] = "other: its neighbour, which only a replacing rename may touch\n";
uint8_t g_keep[kKeepBytes];
uint8_t g_new[kNewBytes];
uint8_t g_reuse[kReuseBytes];

void fill_patterns() {
    for (size_t index = 0; index < kKeepBytes; ++index) {
        g_keep[index] = static_cast<uint8_t>((index * 13u + 5u) & 0xffu);
    }
    for (size_t index = 0; index < kNewBytes; ++index) {
        g_new[index] = static_cast<uint8_t>((index * 31u + 7u) & 0xffu);
    }
    for (size_t index = 0; index < kReuseBytes; ++index) {
        g_reuse[index] = static_cast<uint8_t>((index * 7u + 201u) & 0xffu);
    }
}

enum Kind : int {
    kAbsent = 0,
    kFile = 1,      // a file with exactly these bytes
    kDirectory = 2,
    kOpaqueFile = 3, // a file whose bytes the test does not look at
};

struct Expect {
    const char* path;
    Kind kind;
    const void* data;
    size_t size;
};

constexpr size_t kMaxExpects = 12;

struct State {
    Expect overrides[kMaxExpects];
    size_t count;
    // Change in the number of the variant's own files (Extras) in this state.
    int extra_files;
};

// What every variant of the volume holds before a scenario runs.
const Expect kBase[] = {
    {"/docs", kDirectory, nullptr, 0},
    {"/docs/hello.txt", kFile, kHello, sizeof(kHello) - 1u},
    {"/docs/other.txt", kFile, kOther, sizeof(kOther) - 1u},
    {"/keep.txt", kFile, g_keep, kKeepBytes},
    {"/empty", kDirectory, nullptr, 0},
    {"/dest", kDirectory, nullptr, 0},
};
constexpr size_t kBaseCount = sizeof(kBase) / sizeof(kBase[0]);

// --- Image construction --------------------------------------------------------

struct ImageCookie {
    uint8_t* base;
    uint32_t sector_count;
};

int image_read(void* cookie, uint32_t lba, uint32_t count, void* buffer) {
    ImageCookie* image = static_cast<ImageCookie*>(cookie);
    if (static_cast<uint64_t>(lba) + count > image->sector_count) {
        return SXFS_ERR_IO;
    }
    memcpy(buffer, image->base + static_cast<size_t>(lba) * SXFS_SECTOR_SIZE,
           static_cast<size_t>(count) * SXFS_SECTOR_SIZE);
    return SXFS_OK;
}

int image_write(void* cookie, uint32_t lba, uint32_t count, const void* buffer) {
    ImageCookie* image = static_cast<ImageCookie*>(cookie);
    if (static_cast<uint64_t>(lba) + count > image->sector_count) {
        return SXFS_ERR_IO;
    }
    memcpy(image->base + static_cast<size_t>(lba) * SXFS_SECTOR_SIZE, buffer,
           static_cast<size_t>(count) * SXFS_SECTOR_SIZE);
    return SXFS_OK;
}

enum class Variant {
    plain,       // most of the volume free
    file_full,   // filled through the driver until file data is refused
    truly_full,  // every data sector owned by some inode
    fragmented,  // every data sector owned, except scattered one-sector holes
};

const char* variant_name(Variant variant) {
    switch (variant) {
        case Variant::plain: return "plain";
        case Variant::file_full: return "full for file data";
        case Variant::truly_full: return "no free sector";
        case Variant::fragmented: return "fragmented";
    }
    return "?";
}

constexpr uint32_t kFillFiles = 8;
constexpr uint32_t kFragFiles = 64;

// Hands every free run of the volume to the /fill files as extra extents, so
// not a sector is left: libsxfs writes a file as one run and would stop at the
// first that does not fit.
bool claim_free_space(sxfs_ctx* ctx) {
    uint32_t sector = SXFS_DATA_LBA;
    while (sector < ctx->total_sectors) {
        if (sxfs_bitmap_test(ctx->block_bitmap, sector)) {
            ++sector;
            continue;
        }
        uint32_t end = sector;
        while (end < ctx->total_sectors && !sxfs_bitmap_test(ctx->block_bitmap, end)) {
            ++end;
        }
        // Find the fill file with room for one more extent.
        sxfs_inode* owner = nullptr;
        for (uint32_t index = 0; index < SXFS_MAX_INODES && owner == nullptr; ++index) {
            sxfs_inode& inode = ctx->inodes[index];
            if (inode.type == SXFS_INODE_FILE && inode.reserved[0] == 0xf1 &&
                inode.extent_count < SXFS_MAX_EXTENTS) {
                owner = &inode;
            }
        }
        if (owner == nullptr) {
            return false;
        }
        owner->extents[owner->extent_count].start_lba = sector;
        owner->extents[owner->extent_count].sector_count = end - sector;
        owner->extent_count += 1u;
        owner->size += (end - sector) * SXFS_SECTOR_SIZE;
        for (uint32_t bit = sector; bit < end; ++bit) {
            sxfs_bitmap_set(ctx->block_bitmap, bit, 1);
        }
        sector = end;
    }
    return true;
}

// Marks the /fill files so claim_free_space can find them without a path
// lookup. The byte is in the inode's reserved tail and cleared before flush.
bool tag_fill_files(sxfs_ctx* ctx, uint8_t tag) {
    // The fill files are the only ones of exactly 1 byte.
    uint32_t tagged = 0;
    for (uint32_t index = 0; index < SXFS_MAX_INODES; ++index) {
        sxfs_inode& inode = ctx->inodes[index];
        if (inode.type == SXFS_INODE_FILE && (inode.size == 1u || inode.reserved[0] == 0xf1)) {
            inode.reserved[0] = tag;
            ++tagged;
        }
    }
    return tagged >= kFillFiles;
}

bool build_image(uint8_t* image, Variant variant) {
    memset(image, 0, kImageBytes);
    ImageCookie cookie = {image, kTotalSectors};
    sxfs_ctx* ctx = static_cast<sxfs_ctx*>(calloc(1, sizeof(sxfs_ctx)));
    if (ctx == nullptr) {
        return false;
    }
    sxfs_ctx_init(ctx, &cookie, &image_read, &image_write);

    bool ok = sxfs_format(ctx, kTotalSectors) == SXFS_OK &&
        sxfs_mkdir_p(ctx, "docs") == SXFS_OK &&
        sxfs_write_file(ctx, "docs/hello.txt", kHello, sizeof(kHello) - 1u) == SXFS_OK &&
        sxfs_write_file(ctx, "docs/other.txt", kOther, sizeof(kOther) - 1u) == SXFS_OK &&
        sxfs_write_file(ctx, "keep.txt", g_keep, kKeepBytes) == SXFS_OK &&
        sxfs_mkdir_p(ctx, "empty") == SXFS_OK &&
        sxfs_mkdir_p(ctx, "dest") == SXFS_OK;

    if (variant == Variant::truly_full || variant == Variant::fragmented) {
        char path[48];
        for (uint32_t index = 0; ok && index < kBigEntries; ++index) {
            snprintf(path, sizeof(path), "big/e%02u", index);
            ok = sxfs_write_file(ctx, path, "", 0) == SXFS_OK;
        }
        for (uint32_t index = 0; ok && index < kFragFiles; ++index) {
            snprintf(path, sizeof(path), "frag/h%02u", index);
            ok = sxfs_write_file(ctx, path, "hh", 2) == SXFS_OK;
        }
        for (uint32_t index = 0; ok && index < kFillFiles; ++index) {
            snprintf(path, sizeof(path), "fill/f%u", index);
            ok = sxfs_write_file(ctx, path, "f", 1) == SXFS_OK;
        }
        ok = ok && tag_fill_files(ctx, 0xf1) && claim_free_space(ctx) && tag_fill_files(ctx, 0);
        if (variant == Variant::fragmented) {
            // Every other /frag file goes: one-sector holes, far apart.
            for (uint32_t index = 0; ok && index < kFragFiles; index += 2u) {
                snprintf(path, sizeof(path), "frag/h%02u", index);
                ok = sxfs_remove(ctx, path) == SXFS_OK;
            }
        }
    }
    ok = ok && sxfs_flush(ctx) == SXFS_OK;
    free(ctx);
    return ok;
}

// --- Mounting and looking -------------------------------------------------------

sxfs::VolumeId g_volume = sxfs::kInvalidVolume;

sxfs::VolumeId mount_image(uint8_t* image) {
    hoststub::reset_vfs();
    hoststub::attach_device(image, kTotalSectors, /*writable=*/true);
    sxfs::initialize();
    g_volume = sxfs::probe(0, sxfs::kRootMountPoint);
    if (g_volume != sxfs::kInvalidVolume && !sxfs::attach(g_volume)) {
        g_volume = sxfs::kInvalidVolume;
    }
    return g_volume;
}

sxfs::FileRecord* record_for(const char* path) {
    vfs::Vnode* node = hoststub::find_node(path);
    sxfs::FileRecord* record = node != nullptr ? sxfs::file_from_vnode(*node) : nullptr;
    // The stub tree keeps the nodes of names that are gone; the record says
    // whether the driver still has the name.
    return record;
}

bool path_matches(const Expect& expect) {
    sxfs::FileRecord* record = record_for(expect.path);
    vfs::Vnode* node = hoststub::find_node(expect.path);
    switch (expect.kind) {
        case kAbsent:
            return record == nullptr;
        case kDirectory:
            return record != nullptr && record->directory;
        case kOpaqueFile:
            return record != nullptr && !record->directory;
        case kFile: {
            if (record == nullptr || record->directory || node == nullptr || node->size != expect.size) {
                return false;
            }
            if (expect.size == 0) {
                return true;
            }
            uint8_t* buffer = static_cast<uint8_t*>(malloc(expect.size));
            const bool same = buffer != nullptr && sxfs::read_file(*record, 0, buffer, expect.size) &&
                memcmp(buffer, expect.data, expect.size) == 0;
            free(buffer);
            return same;
        }
    }
    return false;
}

// Files and directories the variant adds to the base, which no scenario
// touches: counted, and (their names being regular) checked by count only.
struct Extras {
    uint32_t files;
    uint32_t directories;
};

// Whether the freshly mounted volume shows exactly the base tree with
// `state`'s overrides, and nothing else.
bool tree_is(const State& state, const Extras& extras) {
    uint32_t files = static_cast<uint32_t>(static_cast<int>(extras.files) + state.extra_files);
    uint32_t directories = 1u + extras.directories; // the root
    for (size_t index = 0; index < kBaseCount; ++index) {
        const Expect* expect = &kBase[index];
        for (size_t other = 0; other < state.count; ++other) {
            if (strcmp(state.overrides[other].path, expect->path) == 0) {
                expect = &state.overrides[other];
            }
        }
        if (!path_matches(*expect)) {
            return false;
        }
        files += (expect->kind == kFile || expect->kind == kOpaqueFile) ? 1u : 0u;
        directories += expect->kind == kDirectory ? 1u : 0u;
    }
    for (size_t other = 0; other < state.count; ++other) {
        const Expect& expect = state.overrides[other];
        bool in_base = false;
        for (size_t index = 0; index < kBaseCount; ++index) {
            in_base = in_base || strcmp(kBase[index].path, expect.path) == 0;
        }
        if (in_base) {
            continue;
        }
        if (!path_matches(expect)) {
            return false;
        }
        files += (expect.kind == kFile || expect.kind == kOpaqueFile) ? 1u : 0u;
        directories += expect.kind == kDirectory ? 1u : 0u;
    }
    sxfs::CheckReport report = {};
    return sxfs::check(g_volume, report) && report.files == files && report.directories == directories;
}

bool volume_is_clean() {
    sxfs::CheckReport report = {};
    return sxfs::check(g_volume, report) && report.clean() && report.journal_pending == 0 &&
        report.pending_deletions == 0 && report.data_used_sectors == report.data_claimed_sectors;
}

// --- Scenarios ---------------------------------------------------------------

constexpr size_t kMaxStates = 10;

struct Scenario {
    const char* name;
    // Runs the mutation and returns how many of its steps completed: the
    // index of the state the volume is in as far as the driver reported.
    int (*run)();
    size_t state_count;
    State states[kMaxStates];
    // Needs room the full variants may not have: there it may fail, cleanly.
    bool needs_space;
    // A state only a volume without room may pass through; -1 for none.
    int only_without_room;
};

int run_create() {
    return sxfs::create_file("/docs/nuevo.txt") != nullptr ? 1 : 0;
}

int run_mkdir() {
    return sxfs::create_directory("/docs/sub") != nullptr ? 1 : 0;
}

int run_unlink() {
    sxfs::FileRecord* record = record_for("/docs/hello.txt");
    return record != nullptr && sxfs::unlink_file(*record) ? 1 : 0;
}

int run_rmdir() {
    sxfs::FileRecord* record = record_for("/empty");
    return record != nullptr && sxfs::remove_directory(*record) ? 1 : 0;
}

int run_rename_same_directory() {
    return sxfs::rename_path("/docs/hello.txt", "/docs/moved.txt") ? 1 : 0;
}

int run_rename_to_root() {
    return sxfs::rename_path("/docs/hello.txt", "/moved.txt") ? 1 : 0;
}

int run_rename_into_directory() {
    return sxfs::rename_path("/docs/hello.txt", "/dest/hello.txt") ? 1 : 0;
}

int run_rename_directory() {
    return sxfs::rename_path("/docs", "/dest/docs") ? 1 : 0;
}

int run_rename_replacing() {
    return sxfs::rename_path("/docs/hello.txt", "/docs/other.txt") ? 1 : 0;
}

int run_rename_replacing_across() {
    return sxfs::rename_path("/docs/hello.txt", "/keep.txt") ? 1 : 0;
}

int run_rename_directory_replacing() {
    return sxfs::rename_path("/empty", "/dest") ? 1 : 0;
}

int run_rewrite() {
    sxfs::FileRecord* record = record_for("/keep.txt");
    size_t written = 0;
    if (record == nullptr) {
        return 0;
    }
    if (sxfs::write_file(*record, 0, g_new, kNewBytes, true, written)) {
        return 2;
    }
    // Without room beside the old contents the truncation is its own commit.
    vfs::Vnode* node = hoststub::find_node("/keep.txt");
    return node != nullptr && node->size == 0 ? 1 : 0;
}

// Frees a file's blocks and has the next file take blocks: the old file's
// bytes must stay its own until the commit that deletes it is down.
int run_block_reuse() {
    sxfs::FileRecord* record = record_for("/docs/hello.txt");
    if (record == nullptr || !sxfs::unlink_file(*record)) {
        return 0;
    }
    sxfs::FileRecord* created = sxfs::create_file("/docs/reuse.txt");
    if (created == nullptr) {
        return 1;
    }
    size_t written = 0;
    return sxfs::write_file(*created, 0, g_reuse, kReuseBytes, true, written) ? 3 : 2;
}

// Enough entries for the directory to cross a sector and grow.
int run_grow_directory() {
    static const char* const kNames[] = {"/dest/a", "/dest/b", "/dest/c", "/dest/d",
                                         "/dest/e", "/dest/f", "/dest/g", "/dest/h"};
    int done = 0;
    for (const char* name : kNames) {
        if (sxfs::create_file(name) == nullptr) {
            break;
        }
        ++done;
    }
    return done;
}

const void* const H = kHello;
constexpr size_t HN = sizeof(kHello) - 1u;
const void* const O = kOther;
constexpr size_t ON = sizeof(kOther) - 1u;

Scenario g_scenarios[] = {
    {"create a file", &run_create, 2,
     {{}, {{{"/docs/nuevo.txt", kFile, "", 0}}, 1}}, true, -1},
    {"create a directory", &run_mkdir, 2,
     {{}, {{{"/docs/sub", kDirectory, nullptr, 0}}, 1}}, true, -1},
    {"unlink a file", &run_unlink, 2,
     {{}, {{{"/docs/hello.txt", kAbsent, nullptr, 0}}, 1}}, false, -1},
    {"remove a directory", &run_rmdir, 2,
     {{}, {{{"/empty", kAbsent, nullptr, 0}}, 1}}, false, -1},
    {"rename within a directory", &run_rename_same_directory, 2,
     {{}, {{{"/docs/hello.txt", kAbsent, nullptr, 0}, {"/docs/moved.txt", kFile, H, HN}}, 2}}, true, -1},
    {"rename to the parent directory", &run_rename_to_root, 2,
     {{}, {{{"/docs/hello.txt", kAbsent, nullptr, 0}, {"/moved.txt", kFile, H, HN}}, 2}}, true, -1},
    {"rename into another directory", &run_rename_into_directory, 2,
     {{}, {{{"/docs/hello.txt", kAbsent, nullptr, 0}, {"/dest/hello.txt", kFile, H, HN}}, 2}}, true, -1},
    {"rename a directory with its contents", &run_rename_directory, 2,
     {{},
      {{{"/docs", kAbsent, nullptr, 0},
        {"/docs/hello.txt", kAbsent, nullptr, 0},
        {"/docs/other.txt", kAbsent, nullptr, 0},
        {"/dest/docs", kDirectory, nullptr, 0},
        {"/dest/docs/hello.txt", kFile, H, HN},
        {"/dest/docs/other.txt", kFile, O, ON}},
       6}},
     true, -1},
    {"rename over a file in the same directory", &run_rename_replacing, 2,
     {{}, {{{"/docs/hello.txt", kAbsent, nullptr, 0}, {"/docs/other.txt", kFile, H, HN}}, 2}}, true, -1},
    {"rename over a file in another directory", &run_rename_replacing_across, 2,
     {{}, {{{"/docs/hello.txt", kAbsent, nullptr, 0}, {"/keep.txt", kFile, H, HN}}, 2}}, true, -1},
    {"rename a directory over an empty directory", &run_rename_directory_replacing, 2,
     {{}, {{{"/empty", kAbsent, nullptr, 0}}, 1}}, true, -1},
    {"rewrite a file from its start", &run_rewrite, 3,
     {{}, {{{"/keep.txt", kFile, "", 0}}, 1}, {{{"/keep.txt", kFile, g_new, kNewBytes}}, 1}}, true, 1},
    {"delete a file and reuse what it freed", &run_block_reuse, 4,
     {{},
      {{{"/docs/hello.txt", kAbsent, nullptr, 0}}, 1},
      {{{"/docs/hello.txt", kAbsent, nullptr, 0}, {"/docs/reuse.txt", kFile, "", 0}}, 2},
      {{{"/docs/hello.txt", kAbsent, nullptr, 0}, {"/docs/reuse.txt", kFile, g_reuse, kReuseBytes}}, 2}},
     true, -1},
    {"grow a directory across sectors", &run_grow_directory, 9,
     {{},
      {{{"/dest/a", kFile, "", 0}}, 1},
      {{{"/dest/a", kFile, "", 0}, {"/dest/b", kFile, "", 0}}, 2},
      {{{"/dest/a", kFile, "", 0}, {"/dest/b", kFile, "", 0}, {"/dest/c", kFile, "", 0}}, 3},
      {{{"/dest/a", kFile, "", 0}, {"/dest/b", kFile, "", 0}, {"/dest/c", kFile, "", 0},
        {"/dest/d", kFile, "", 0}}, 4},
      {{{"/dest/a", kFile, "", 0}, {"/dest/b", kFile, "", 0}, {"/dest/c", kFile, "", 0},
        {"/dest/d", kFile, "", 0}, {"/dest/e", kFile, "", 0}}, 5},
      {{{"/dest/a", kFile, "", 0}, {"/dest/b", kFile, "", 0}, {"/dest/c", kFile, "", 0},
        {"/dest/d", kFile, "", 0}, {"/dest/e", kFile, "", 0}, {"/dest/f", kFile, "", 0}}, 6},
      {{{"/dest/a", kFile, "", 0}, {"/dest/b", kFile, "", 0}, {"/dest/c", kFile, "", 0},
        {"/dest/d", kFile, "", 0}, {"/dest/e", kFile, "", 0}, {"/dest/f", kFile, "", 0},
        {"/dest/g", kFile, "", 0}}, 7},
      {{{"/dest/a", kFile, "", 0}, {"/dest/b", kFile, "", 0}, {"/dest/c", kFile, "", 0},
        {"/dest/d", kFile, "", 0}, {"/dest/e", kFile, "", 0}, {"/dest/f", kFile, "", 0},
        {"/dest/g", kFile, "", 0}, {"/dest/h", kFile, "", 0}}, 8}},
     true, -1},
};

// Deleting from the directory of 70 entries the full variants carry: its copy
// is 11 sectors, which neither of them has.
int run_unlink_in_big_directory() {
    sxfs::FileRecord* record = record_for("/big/e33");
    return record != nullptr && sxfs::unlink_file(*record) ? 1 : 0;
}

Scenario g_big_unlink = {"unlink in a directory too large to copy", &run_unlink_in_big_directory, 2,
                         {{}, {}}, false, -1};

// --- The runners -------------------------------------------------------------

// Index of the state the mounted volume is in, or -1 when it is in none.
int state_of(const Scenario& scenario, const Extras& extras) {
    for (size_t index = 0; index < scenario.state_count; ++index) {
        if (tree_is(scenario.states[index], extras)) {
            return static_cast<int>(index);
        }
    }
    return -1;
}

// Builds the variant, brings it to rest through the driver, and leaves it in
// `pristine`. `extras` receives what the variant adds to the base tree.
bool prepare(uint8_t* image, uint8_t* pristine, Variant variant, Extras& extras) {
    extras = {0, 0};
    if (!build_image(image, variant) || mount_image(image) == sxfs::kInvalidVolume) {
        return false;
    }
    if (variant == Variant::file_full) {
        sxfs::FileRecord* fill = sxfs::create_file("/fill.bin");
        if (fill == nullptr) {
            return false;
        }
        static uint8_t chunk[64u * 1024u];
        memset(chunk, 0x5a, sizeof(chunk));
        size_t offset = 0;
        size_t written = 0;
        for (size_t step = sizeof(chunk); step >= 512u; step /= 2u) {
            while (sxfs::write_file(*fill, offset, chunk, step, false, written)) {
                offset += step;
            }
        }
        extras.files = 1;
    } else if (variant == Variant::truly_full || variant == Variant::fragmented) {
        extras.files = kBigEntries + kFillFiles +
            (variant == Variant::fragmented ? kFragFiles / 2u : kFragFiles);
        extras.directories = 3; // big, frag, fill
    }
    if (!sxfs::sync(g_volume)) {
        return false;
    }
    memcpy(pristine, image, kImageBytes);
    return mount_image(image) != sxfs::kInvalidVolume;
}

// Sectors of a recovery at which it is cut again: its first writes, the edges
// of each metadata region, and a stride through the rest.
size_t recovery_cuts(size_t* cuts, size_t capacity) {
    static const size_t kEdges[] = {0, 1, 2, 3, 5, 8, 13, 511, 512, 513, 514, 1024, 1536, 1537, 1538, 1539, 1540};
    size_t count = 0;
    for (size_t edge : kEdges) {
        if (count < capacity) {
            cuts[count++] = edge;
        }
    }
    return count;
}

void run_power_cuts(Scenario& scenario, Variant variant, uint8_t* image, uint8_t* pristine, uint8_t* crashed) {
    Extras extras = {};
    if (!check(prepare(image, pristine, variant, extras), "the volume is prepared")) {
        return;
    }
    if (!check(state_of(scenario, extras) == 0 && volume_is_clean(), "the volume starts in the state before")) {
        return;
    }

    const int reported = scenario.run();
    const size_t total = hoststub::sectors_written();
    const int final_state = static_cast<int>(scenario.state_count) - 1;
    const bool roomy = variant == Variant::plain;
    if (roomy || !scenario.needs_space) {
        check(reported == final_state, "the uninterrupted mutation completes");
    }
    check(mount_image(image) != sxfs::kInvalidVolume && state_of(scenario, extras) == reported &&
              volume_is_clean(),
          "the volume remounts in the state the driver reported");

    size_t unmountable = 0;
    size_t dirty = 0;
    size_t unexpected = 0;
    size_t recovery_failures = 0;
    size_t first_bad = static_cast<size_t>(-1);
    bool seen[kMaxStates] = {};
    size_t recovery_points[32];
    const size_t recovery_count = recovery_cuts(recovery_points, 32);

    for (size_t cut = 0; cut <= total; ++cut) {
        memcpy(image, pristine, kImageBytes);
        if (mount_image(image) == sxfs::kInvalidVolume) {
            ++unmountable;
            continue;
        }
        hoststub::power_cut_after(cut);
        (void)scenario.run();
        hoststub::clear_power_cut();
        memcpy(crashed, image, kImageBytes);

        // Recovery with no further trouble, then with a second cut inside it.
        for (size_t attempt = 0; attempt <= recovery_count; ++attempt) {
            bool bad = false;
            if (attempt != 0) {
                memcpy(image, crashed, kImageBytes);
                hoststub::reset_vfs();
                hoststub::attach_device(image, kTotalSectors, /*writable=*/true);
                sxfs::initialize();
                hoststub::power_cut_after(recovery_points[attempt - 1u]);
                const sxfs::VolumeId interrupted = sxfs::probe(0, sxfs::kRootMountPoint);
                if (interrupted != sxfs::kInvalidVolume) {
                    (void)sxfs::attach(interrupted);
                }
                hoststub::clear_power_cut();
            }
            if (mount_image(image) == sxfs::kInvalidVolume ||
                sxfs::status(g_volume) != sxfs::MountStatus::mounted) {
                ++unmountable;
                bad = true;
            } else {
                if (!volume_is_clean()) {
                    ++dirty;
                    bad = true;
                }
                const int state = state_of(scenario, extras);
                if (state < 0) {
                    ++unexpected;
                    bad = true;
                } else if (attempt == 0) {
                    seen[state] = true;
                }
            }
            if (bad && attempt != 0) {
                ++recovery_failures;
            }
            if (bad && first_bad == static_cast<size_t>(-1)) {
                first_bad = cut;
                printf("  first bad cut: after %zu of %zu sectors (recovery attempt %zu)\n", cut, total, attempt);
            }
        }
    }

    check(unmountable == 0, "every cut remounts writable");
    check(dirty == 0, "every cut remounts with a clean fscheck and no pending deletion");
    check(unexpected == 0, "every cut shows the tree before the mutation or after one of its steps");
    check(recovery_failures == 0, "a recovery cut again recovers the same way");
    check(seen[0], "some cut leaves the state before");
    if (roomy && scenario.only_without_room >= 0) {
        check(!seen[scenario.only_without_room], "with room, the old contents last until the new ones are committed");
    }
    if (reported == final_state) {
        check(seen[final_state], "some cut leaves the state after");
    }
}

// An I/O error at each write of the mutation. Whatever the driver reports,
// the volume keeps working, and after the next commit the disk holds exactly
// what the driver says it holds.
void run_io_failures(Scenario& scenario, uint8_t* image, uint8_t* pristine) {
    Extras extras = {};
    if (!check(prepare(image, pristine, Variant::plain, extras), "the volume is prepared")) {
        return;
    }
    const size_t writes_before = hoststub::writes_issued();
    (void)scenario.run();
    const size_t writes = hoststub::writes_issued() - writes_before;

    size_t disagreements = 0;
    size_t first_bad = static_cast<size_t>(-1);
    for (size_t failing = 1; failing <= writes; ++failing) {
        memcpy(image, pristine, kImageBytes);
        if (mount_image(image) == sxfs::kInvalidVolume) {
            ++disagreements;
            continue;
        }
        hoststub::fail_write_number(failing);
        const int reported = scenario.run();
        hoststub::fail_write_number(0);

        // The commit after a failed one is what reconciles disk and memory.
        const bool followed = sxfs::create_file("/after-the-error") != nullptr;
        State expected = scenario.states[reported];
        expected.overrides[expected.count++] = {"/after-the-error", kFile, "", 0};

        const bool agree = followed && mount_image(image) != sxfs::kInvalidVolume &&
            sxfs::status(g_volume) == sxfs::MountStatus::mounted && volume_is_clean() &&
            tree_is(expected, extras);
        if (!agree) {
            ++disagreements;
            if (first_bad == static_cast<size_t>(-1)) {
                first_bad = failing;
                printf("  first disagreement: write %zu of %zu failed, driver reported step %d\n",
                       failing, writes, reported);
            }
        }
    }
    check(disagreements == 0, "after an I/O error at any write, disk and driver agree");
}

} // namespace

int main() {
    printf("SxFS NAMESPACE TEST START\n");
    fill_patterns();

    uint8_t* image = static_cast<uint8_t*>(malloc(kImageBytes));
    uint8_t* pristine = static_cast<uint8_t*>(malloc(kImageBytes));
    uint8_t* crashed = static_cast<uint8_t*>(malloc(kImageBytes));
    if (image == nullptr || pristine == nullptr || crashed == nullptr) {
        printf("SxFS NAMESPACE TEST FAIL: no memory for the images\n");
        return 1;
    }

    g_big_unlink.states[1] = {{{"/big/e33", kAbsent, nullptr, 0}}, 1, -1};

    const Variant variants[] = {Variant::plain, Variant::file_full, Variant::truly_full, Variant::fragmented};
    for (Variant variant : variants) {
        for (Scenario& scenario : g_scenarios) {
            const int before = g_failures;
            printf("case: power cuts, %s volume: %s\n", variant_name(variant), scenario.name);
            run_power_cuts(scenario, variant, image, pristine, crashed);
            if (g_failures != before) {
                printf("  ^ failed\n");
            }
        }
    }
    const Variant full_variants[] = {Variant::truly_full, Variant::fragmented};
    for (Variant variant : full_variants) {
        printf("case: power cuts, %s volume: %s\n", variant_name(variant), g_big_unlink.name);
        // /big/e33 is one of the extras: one fewer once it is gone.
        run_power_cuts(g_big_unlink, variant, image, pristine, crashed);
    }
    for (Scenario& scenario : g_scenarios) {
        printf("case: I/O errors: %s\n", scenario.name);
        run_io_failures(scenario, image, pristine);
    }

    free(image);
    free(pristine);
    free(crashed);

    printf("%s (%d checks, %d failures)\n",
           g_failures == 0 ? "SxFS NAMESPACE TEST PASS" : "SxFS NAMESPACE TEST FAIL",
           g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
