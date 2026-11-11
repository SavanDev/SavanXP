/*
 * sxfs_recovery_test.cpp -- Host test of what happens to an SxFS image that
 * was interrupted or damaged (docs/fs/SXFS_CHECK.md), with the real kernel
 * driver, VFS and mount registry on one side and the host library (libsxfs)
 * on the other, over the same images.
 *
 *   - the host recovers a pending journal the way the kernel does, in memory
 *     and without writing; persisting it is idempotent and can be interrupted
 *     at any write;
 *   - the kernel's probe leaves the device untouched, a read-only mount never
 *     writes and refuses every change with EROFS, and a writable mount is
 *     what persists the recovery;
 *   - offline repair gives unreachable files and directories a name, rebuilds
 *     the block bitmap from what the inodes claim, finishes committed
 *     deletions, is deterministic and idempotent, and leaves damage with no
 *     identifiable owner as it found it.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "abi/savanxp_abi.h"
#include "kernel/credentials.hpp"
#include "kernel/fs.hpp"
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
    } else {
        printf("  ok   %s\n", what);
    }
    return ok;
}

const cred::Credentials kSystem = cred::system();
const char kHello[] = "hello, this file gets lost\n";
const char kDeep[] = "deep inside a lost directory\n";
const char kTop[] = "a file at the top\n";
const char kGone[] = "its inode is freed under it\n";

// --- The host library over an image in memory ----------------------------------

struct ImageCookie {
    uint8_t* base;
    // Write calls still allowed; the rest fail, like a tool killed mid-way.
    size_t writes_allowed;
    size_t writes;
};

int image_read(void* cookie, uint32_t lba, uint32_t count, void* buffer) {
    ImageCookie* image = static_cast<ImageCookie*>(cookie);
    memcpy(buffer, image->base + static_cast<size_t>(lba) * SXFS_SECTOR_SIZE,
           static_cast<size_t>(count) * SXFS_SECTOR_SIZE);
    return 0;
}

int image_write(void* cookie, uint32_t lba, uint32_t count, const void* buffer) {
    ImageCookie* image = static_cast<ImageCookie*>(cookie);
    if (image->writes >= image->writes_allowed) {
        return -1;
    }
    ++image->writes;
    memcpy(image->base + static_cast<size_t>(lba) * SXFS_SECTOR_SIZE, buffer,
           static_cast<size_t>(count) * SXFS_SECTOR_SIZE);
    return 0;
}

constexpr size_t kUnlimited = static_cast<size_t>(-1);

// One context, reused: it is over a megabyte.
sxfs_ctx* g_ctx = nullptr;
ImageCookie g_cookie = {};

sxfs_ctx* host_open(uint8_t* image, size_t writes_allowed = kUnlimited) {
    g_cookie = {image, writes_allowed, 0};
    sxfs_ctx_init(g_ctx, &g_cookie, &image_read, &image_write);
    return sxfs_open(g_ctx) == SXFS_OK ? g_ctx : nullptr;
}

// What the image holds, as the host reads it: every path, its type and, for a
// file, its bytes. 0 when the tree does not validate.
struct Digest {
    const sxfs_ctx* ctx;
    uint64_t value;
};

void mix(uint64_t& value, const void* data, size_t size) {
    const uint8_t* bytes = static_cast<const uint8_t*>(data);
    for (size_t index = 0; index < size; ++index) {
        value = (value ^ bytes[index]) * 1099511628211ull;
    }
}

int digest_entry(void* cookie, const char* path, uint32_t inode_id, uint16_t type) {
    Digest* digest = static_cast<Digest*>(cookie);
    static uint8_t contents[64 * 1024];
    mix(digest->value, path, strlen(path) + 1u);
    mix(digest->value, &type, sizeof(type));
    if (type == SXFS_INODE_FILE) {
        uint32_t size = 0;
        if (sxfs_read_inode(digest->ctx, inode_id, contents, sizeof(contents), &size) != SXFS_OK) {
            return SXFS_ERR_INVALID;
        }
        mix(digest->value, contents, size);
    }
    return SXFS_OK;
}

uint64_t digest_of(const sxfs_ctx* ctx) {
    Digest digest = {ctx, 14695981039346656037ull};
    return sxfs_walk(ctx, &digest_entry, &digest) == SXFS_OK ? (digest.value | 1u) : 0;
}

uint64_t host_digest(uint8_t* image) {
    const sxfs_ctx* ctx = host_open(image);
    return ctx != nullptr ? digest_of(ctx) : 0;
}

bool build_image(uint8_t* image) {
    memset(image, 0, kImageBytes);
    g_cookie = {image, kUnlimited, 0};
    sxfs_ctx_init(g_ctx, &g_cookie, &image_read, &image_write);
    return sxfs_format(g_ctx, kTotalSectors) == SXFS_OK &&
        sxfs_write_file(g_ctx, "docs/hello.txt", kHello, sizeof(kHello) - 1u) == SXFS_OK &&
        sxfs_write_file(g_ctx, "docs/sub/deep.txt", kDeep, sizeof(kDeep) - 1u) == SXFS_OK &&
        sxfs_write_file(g_ctx, "top.txt", kTop, sizeof(kTop) - 1u) == SXFS_OK &&
        sxfs_write_file(g_ctx, "extra/gone.txt", kGone, sizeof(kGone) - 1u) == SXFS_OK &&
        sxfs_mkdir_p(g_ctx, "empty") == SXFS_OK &&
        sxfs_flush(g_ctx) == SXFS_OK;
}

// --- The kernel side -----------------------------------------------------------

const char kEmptyArchive[] =
    "070701" "00000000" "00000000" "00000000" "00000000" "00000001" "00000000"
    "00000000" "00000000" "00000000" "00000000" "00000000" "0000000B" "00000000"
    "TRAILER!!!\0\0\0\0";

size_t g_mount = fs::kInvalidMount;

// Attaches the device and probes it: the volume is recognised, not mounted.
bool probe_image(uint8_t* image) {
    hoststub::attach_device(image, kTotalSectors, /*writable=*/true);
    vfs::initialize(kEmptyArchive, sizeof(kEmptyArchive) - 1u);
    fs::initialize();
    sxfs::initialize();
    static bool registered = false;
    if (!registered) {
        registered = fs::register_driver(sxfs::driver());
    }
    g_mount = fs::mount_any(fs::kRootMountPoint);
    return vfs::ready() && g_mount != fs::kInvalidMount;
}

bool mount_image(uint8_t* image, bool read_only = false) {
    return probe_image(image) && fs::attach(g_mount, read_only);
}

bool kernel_is_clean() {
    sxfs::CheckReport report = {};
    return sxfs::check(sxfs::root(), report) && report.clean() &&
        report.data_used_sectors == report.data_claimed_sectors && report.pending_deletions == 0;
}

bool path_reads(const char* path, const void* expected, size_t size) {
    char buffer[256] = {};
    vfs::Vnode* node = vfs::open(kSystem, path, SAVANXP_OPEN_READ, 0);
    return node != nullptr && node->size == size && size <= sizeof(buffer) &&
        vfs::read(*node, 0, buffer, size) == size && memcmp(buffer, expected, size) == 0;
}

// A few mutations, each its own transaction: whichever the cut lands in, the
// journal is left at a different point.
void mutate() {
    static uint8_t payload[3000];
    memset(payload, 0x5a, sizeof(payload));
    vfs::Vnode* node = vfs::open(kSystem, "/docs/new.txt", SAVANXP_OPEN_WRITE | SAVANXP_OPEN_CREATE, 0644);
    if (node != nullptr) {
        (void)vfs::write(*node, 0, payload, sizeof(payload), false);
    }
    (void)vfs::rename(kSystem, "/top.txt", "/docs/moved.txt");
    (void)vfs::unlink(kSystem, "/docs/hello.txt");
    (void)vfs::mkdir(kSystem, "/made", 0755);
}

// --- Cases ---------------------------------------------------------------------

// Kept from the first case for the second: an image with a journal pending.
uint8_t* g_pending_image = nullptr;
bool g_have_pending = false;

void case_host_recovery_matches_the_kernel(uint8_t* image, uint8_t* pristine, uint8_t* crashed, uint8_t* recovered) {
    printf("case: the host recovers an interrupted image as the kernel does\n");
    if (!check(build_image(pristine), "image built")) {
        return;
    }
    memcpy(image, pristine, kImageBytes);
    if (!check(mount_image(image), "volume mounted")) {
        return;
    }
    const size_t before = hoststub::sectors_written();
    mutate();
    const size_t total = hoststub::sectors_written() - before;

    size_t unreadable = 0;
    size_t inspection_wrote = 0;
    size_t recover_differs = 0;
    size_t kernel_differs = 0;
    size_t interrupted_differs = 0;
    size_t replays = 0;
    size_t unclean = 0;
    for (size_t cut = 0; cut <= total; ++cut) {
        memcpy(image, pristine, kImageBytes);
        if (!mount_image(image)) {
            ++unreadable;
            continue;
        }
        hoststub::power_cut_after(cut);
        mutate();
        hoststub::clear_power_cut();
        hoststub::detach_device();
        memcpy(crashed, image, kImageBytes);

        // Inspection: the recovered view, and not one byte written.
        sxfs_ctx* ctx = host_open(image);
        const uint64_t view = ctx != nullptr ? digest_of(ctx) : 0;
        const uint32_t pending = ctx != nullptr ? ctx->recovery : 0;
        if (view == 0) {
            ++unreadable;
            continue;
        }
        if (g_cookie.writes != 0 || memcmp(image, crashed, kImageBytes) != 0) {
            ++inspection_wrote;
        }
        if ((pending & SXFS_RECOVERY_REPLAYED) != 0) {
            ++replays;
            if (!g_have_pending) {
                memcpy(g_pending_image, crashed, kImageBytes);
                g_have_pending = true;
            }
        } else if (pending != 0) {
            ++unclean;
        }

        // Persisting it changes nothing a reader sees, and leaves nothing pending.
        memcpy(recovered, crashed, kImageBytes);
        ctx = host_open(recovered);
        const size_t recovery_writes = (ctx != nullptr && sxfs_recover(ctx) == SXFS_OK) ? g_cookie.writes : 0;
        ctx = host_open(recovered);
        if (ctx == nullptr || ctx->recovery != 0 || digest_of(ctx) != view) {
            ++recover_differs;
        }

        // Stopped after each of its writes and run again: the same image.
        for (size_t allowed = 0; allowed < recovery_writes; ++allowed) {
            memcpy(image, crashed, kImageBytes);
            ctx = host_open(image, allowed);
            const bool stopped = ctx != nullptr && sxfs_recover(ctx) != SXFS_OK;
            ctx = host_open(image);
            if (!stopped || ctx == nullptr || digest_of(ctx) != view || sxfs_recover(ctx) != SXFS_OK ||
                memcmp(image, recovered, kImageBytes) != 0) {
                ++interrupted_differs;
            }
        }

        // The kernel, from the same crashed image, arrives at the same tree.
        memcpy(image, crashed, kImageBytes);
        if (!mount_image(image) || !kernel_is_clean()) {
            ++kernel_differs;
        } else {
            (void)fs::sync_all();
            hoststub::detach_device();
            if (host_digest(image) != view) {
                ++kernel_differs;
            }
        }
    }
    printf("  %zu cuts: %zu with a journal to replay, %zu only unclean\n", total + 1u, replays, unclean);
    check(unreadable == 0, "every crashed image opens and validates on the host");
    check(inspection_wrote == 0, "opening and walking an image writes nothing to it");
    check(replays != 0, "some cuts left a journal pending");
    check(recover_differs == 0, "recover leaves the recovered tree and nothing pending");
    check(interrupted_differs == 0, "recover interrupted at any write, then repeated, gives the same image");
    check(kernel_differs == 0, "the kernel mounts each crashed image to the same tree");
}

void case_inspection_and_read_only_mount(uint8_t* image, uint8_t* crashed) {
    printf("case: probe and a read-only mount leave the device untouched\n");
    if (!check(g_have_pending, "an image with a pending journal")) {
        return;
    }
    memcpy(crashed, g_pending_image, kImageBytes);
    memcpy(image, crashed, kImageBytes);
    const uint64_t view = host_digest(image);

    check(probe_image(image), "the kernel recognises the volume");
    check(hoststub::sectors_written() == 0 && hoststub::writes_issued() == 0 &&
              memcmp(image, crashed, kImageBytes) == 0,
          "probe wrote nothing");
    check(fs::attach(g_mount, /*read_only=*/true), "it mounts read-only");
    check(fs::read_only(g_mount) && !sxfs::writable(sxfs::root()), "and says so");
    check(path_reads("/docs/sub/deep.txt", kDeep, sizeof(kDeep) - 1u), "files read, on the recovered view");

    int refused = 0;
    refused += vfs::open(kSystem, "/new.txt", SAVANXP_OPEN_WRITE | SAVANXP_OPEN_CREATE, 0644) == nullptr &&
        vfs::last_error() == SX_EROFS;
    refused += vfs::open(kSystem, "/docs/sub/deep.txt", SAVANXP_OPEN_WRITE, 0) == nullptr &&
        vfs::last_error() == SX_EROFS;
    refused += vfs::mkdir(kSystem, "/newdir", 0755) == nullptr && vfs::last_error() == SX_EROFS;
    refused += !vfs::unlink(kSystem, "/docs/sub/deep.txt") && vfs::last_error() == SX_EROFS;
    refused += !vfs::rmdir(kSystem, "/empty") && vfs::last_error() == SX_EROFS;
    refused += !vfs::rename(kSystem, "/docs/sub/deep.txt", "/docs/sub/other.txt") && vfs::last_error() == SX_EROFS;
    refused += !vfs::truncate(kSystem, "/docs/sub/deep.txt", 1) && vfs::last_error() == SX_EROFS;
    refused += !vfs::chmod(kSystem, "/docs/sub/deep.txt", 0600) && vfs::last_error() == SX_EROFS;
    check(refused == 8, "create, open for writing, mkdir, unlink, rmdir, rename, truncate and chmod are EROFS");
    check(fs::sync_all() && fs::sync_all_deferred(), "sync succeeds");
    check(hoststub::sectors_written() == 0 && memcmp(image, crashed, kImageBytes) == 0,
          "the image is byte for byte what it was");
    hoststub::detach_device();

    check(mount_image(image) && sxfs::writable(sxfs::root()) && kernel_is_clean(),
          "a writable mount of the same image persists the recovery");
    (void)fs::sync_all();
    hoststub::detach_device();
    const sxfs_ctx* ctx = host_open(image);
    check(ctx != nullptr && ctx->recovery == 0 && digest_of(ctx) == view,
          "after which the host finds nothing pending and the same tree");
}

// --- Damage, done by hand ------------------------------------------------------

struct Found {
    const char* path;
    uint32_t inode_id;
};

int find_entry(void* cookie, const char* path, uint32_t inode_id, uint16_t) {
    Found* found = static_cast<Found*>(cookie);
    if (strcmp(path, found->path) == 0) {
        found->inode_id = inode_id;
    }
    return SXFS_OK;
}

uint32_t inode_of(const sxfs_ctx* ctx, const char* path) {
    if (path[0] == '\0') {
        return SXFS_ROOT_INODE;
    }
    Found found = {path, 0};
    (void)sxfs_walk(ctx, &find_entry, &found);
    return found.inode_id;
}

// The image's byte that holds byte `offset` of an inode's contents.
uint8_t* inode_byte(uint8_t* image, const sxfs_inode& inode, uint32_t offset) {
    for (uint32_t index = 0; index < inode.extent_count; ++index) {
        const uint32_t bytes = inode.extents[index].sector_count * SXFS_SECTOR_SIZE;
        if (offset < bytes) {
            return image + static_cast<size_t>(inode.extents[index].start_lba) * SXFS_SECTOR_SIZE + offset;
        }
        offset -= bytes;
    }
    return nullptr;
}

// Frees the slot of `directory` that names `child`, leaving the child alone.
bool drop_entry(uint8_t* image, sxfs_ctx* ctx, uint32_t directory, uint32_t child) {
    const sxfs_inode& inode = ctx->inodes[directory - 1u];
    for (uint32_t offset = 0; offset + SXFS_DIR_ENTRY_SIZE <= inode.size; offset += SXFS_DIR_ENTRY_SIZE) {
        sxfs_dir_entry entry = {};
        for (uint32_t byte = 0; byte < SXFS_DIR_ENTRY_SIZE; ++byte) {
            reinterpret_cast<uint8_t*>(&entry)[byte] = *inode_byte(image, inode, offset + byte);
        }
        if (entry.inode_id == child) {
            for (uint32_t byte = 0; byte < SXFS_DIR_ENTRY_SIZE; ++byte) {
                *inode_byte(image, inode, offset + byte) = 0;
            }
            return true;
        }
    }
    return false;
}

// Writes the ctx's metadata without going through sxfs_flush's checks.
void store_metadata(uint8_t* image, const sxfs_ctx* ctx) {
    memcpy(image + static_cast<size_t>(SXFS_BLOCK_BITMAP_LBA) * SXFS_SECTOR_SIZE, ctx->block_bitmap,
           sizeof(ctx->block_bitmap));
    memcpy(image + static_cast<size_t>(SXFS_INODE_BITMAP_LBA) * SXFS_SECTOR_SIZE, ctx->inode_bitmap,
           sizeof(ctx->inode_bitmap));
    memcpy(image + static_cast<size_t>(SXFS_INODE_TABLE_LBA) * SXFS_SECTOR_SIZE, ctx->inodes, sizeof(ctx->inodes));
}

struct Damage {
    uint32_t hello;
    uint32_t sub;
    uint32_t top_sectors;
};

// An unreachable file, an unreachable directory with a file in it, an entry
// naming a freed inode, sectors marked that nothing claims, sectors claimed
// that are not marked, and a record left in a free inode.
bool damage_image(uint8_t* image, Damage& damage) {
    sxfs_ctx* ctx = host_open(image);
    if (ctx == nullptr) {
        return false;
    }
    const uint32_t docs = inode_of(ctx, "docs");
    const uint32_t top = inode_of(ctx, "top.txt");
    const uint32_t gone = inode_of(ctx, "extra/gone.txt");
    damage.hello = inode_of(ctx, "docs/hello.txt");
    damage.sub = inode_of(ctx, "docs/sub");
    if (docs == 0 || top == 0 || gone == 0 || damage.hello == 0 || damage.sub == 0 ||
        !drop_entry(image, ctx, docs, damage.hello) || !drop_entry(image, ctx, docs, damage.sub)) {
        return false;
    }
    // The inode goes, its entry stays.
    sxfs_inode& freed = ctx->inodes[gone - 1u];
    for (uint32_t index = 0; index < freed.extent_count; ++index) {
        for (uint32_t sector = 0; sector < freed.extents[index].sector_count; ++sector) {
            sxfs_bitmap_set(ctx->block_bitmap, freed.extents[index].start_lba + sector, 0);
        }
    }
    memset(&freed, 0, sizeof(freed));
    sxfs_bitmap_set(ctx->inode_bitmap, gone - 1u, 0);
    // Five sectors nobody owns.
    for (uint32_t sector = kTotalSectors - 5u; sector < kTotalSectors; ++sector) {
        sxfs_bitmap_set(ctx->block_bitmap, sector, 1);
    }
    // top.txt's sectors, free in the bitmap.
    damage.top_sectors = 0;
    const sxfs_inode& claimed = ctx->inodes[top - 1u];
    for (uint32_t index = 0; index < claimed.extent_count; ++index) {
        for (uint32_t sector = 0; sector < claimed.extents[index].sector_count; ++sector) {
            sxfs_bitmap_set(ctx->block_bitmap, claimed.extents[index].start_lba + sector, 0);
            ++damage.top_sectors;
        }
    }
    // A record where no inode is allocated.
    sxfs_inode& stale = ctx->inodes[SXFS_MAX_INODES - 1u];
    stale.inode_id = SXFS_MAX_INODES;
    stale.type = SXFS_INODE_FILE;
    stale.link_count = 1;
    store_metadata(image, ctx);
    return true;
}

bool host_reads(const sxfs_ctx* ctx, const char* path, const void* expected, size_t size) {
    static uint8_t buffer[1024];
    uint32_t got = 0;
    const uint32_t inode_id = inode_of(ctx, path);
    return inode_id != 0 && sxfs_read_inode(ctx, inode_id, buffer, sizeof(buffer), &got) == SXFS_OK &&
        got == size && memcmp(buffer, expected, size) == 0;
}

bool repair_image(uint8_t* image, sxfs_repair_report& report) {
    sxfs_ctx* ctx = host_open(image);
    return ctx != nullptr && sxfs_repair(ctx, &report) == SXFS_OK &&
        (sxfs_repair_changes(&report) == 0 || sxfs_flush(ctx) == SXFS_OK);
}

void case_repair_preserves_what_it_can_reach(uint8_t* image, uint8_t* damaged, uint8_t* second) {
    printf("case: repair rescues contents and reconciles the bitmap\n");
    Damage damage = {};
    if (!check(build_image(image) && damage_image(image, damage), "image built and damaged")) {
        return;
    }
    memcpy(damaged, image, kImageBytes);
    check(host_digest(image) == 0, "the damaged image does not validate");

    sxfs_repair_report report = {};
    if (!check(repair_image(image, report), "repair runs")) {
        return;
    }
    check(report.files_rescued == 1 && report.directories_rescued == 1,
          "one file and one directory are rescued (the directory's file with it)");
    check(report.entries_removed == 1, "the entry naming a freed inode is removed");
    check(report.sectors_freed == 5 && report.sectors_marked == damage.top_sectors,
          "unclaimed sectors are freed and claimed ones marked");
    check(report.stale_records_cleared == 1, "the record in a free inode is cleared");
    check(sxfs_repair_unresolved(&report) == 0, "nothing is left unresolved");

    const sxfs_ctx* ctx = host_open(image);
    char rescued_file[64];
    char rescued_deep[64];
    snprintf(rescued_file, sizeof(rescued_file), "lost+found/inode-%u", damage.hello);
    snprintf(rescued_deep, sizeof(rescued_deep), "lost+found/dir-%u/deep.txt", damage.sub);
    check(ctx != nullptr && digest_of(ctx) != 0, "the repaired image validates");
    check(ctx != nullptr && host_reads(ctx, rescued_file, kHello, sizeof(kHello) - 1u),
          "the lost file is under lost+found with its contents");
    check(ctx != nullptr && host_reads(ctx, rescued_deep, kDeep, sizeof(kDeep) - 1u),
          "the lost directory is there with its file, under its own name");
    check(ctx != nullptr && host_reads(ctx, "top.txt", kTop, sizeof(kTop) - 1u), "what was fine is untouched");
    check(ctx != nullptr && inode_of(ctx, "extra/gone.txt") == 0 && inode_of(ctx, "extra") != 0,
          "the dangling name is gone, its directory is not");

    memcpy(second, damaged, kImageBytes);
    sxfs_repair_report again = {};
    check(repair_image(second, again) && memcmp(second, image, kImageBytes) == 0 &&
              memcmp(&again, &report, sizeof(report)) == 0,
          "repairing the same damage again gives the same image and report");
    check(repair_image(second, again) && sxfs_repair_changes(&again) == 0 && memcmp(second, image, kImageBytes) == 0,
          "repairing a repaired image changes nothing");

    check(mount_image(image) && kernel_is_clean(), "the kernel mounts it and fscheck finds nothing");
    char kernel_path[80];
    snprintf(kernel_path, sizeof(kernel_path), "/%s", rescued_deep);
    check(path_reads(kernel_path, kDeep, sizeof(kDeep) - 1u), "and reads the rescued file");
    hoststub::detach_device();
}

void case_repair_finishes_committed_deletions(uint8_t* image) {
    printf("case: repair finishes a deletion that was committed\n");
    if (!check(build_image(image), "image built")) {
        return;
    }
    sxfs_ctx* ctx = host_open(image);
    const uint32_t top = ctx != nullptr ? inode_of(ctx, "top.txt") : 0;
    if (!check(top != 0, "file found")) {
        return;
    }
    ctx->inodes[top - 1u].link_count = 0; // doomed, its entry still in the root
    store_metadata(image, ctx);

    sxfs_repair_report report = {};
    check(repair_image(image, report) && report.deletions_finished == 1 && report.entries_removed == 0 &&
              sxfs_repair_unresolved(&report) == 0,
          "one deletion finished, and it is not counted as damage");
    ctx = host_open(image);
    check(ctx != nullptr && digest_of(ctx) != 0 && inode_of(ctx, "top.txt") == 0 &&
              !sxfs_bitmap_test(ctx->inode_bitmap, top - 1u),
          "the name and the inode are gone");
    check(mount_image(image) && kernel_is_clean(), "the kernel finds nothing left to do");
    hoststub::detach_device();
}

void case_repair_leaves_what_it_cannot_attribute(uint8_t* image, uint8_t* before) {
    printf("case: repair reports and leaves damage with no identifiable owner\n");
    if (!check(build_image(image), "image built")) {
        return;
    }
    // Two files claiming the same sectors: either could be the owner.
    sxfs_ctx* ctx = host_open(image);
    const uint32_t top = ctx != nullptr ? inode_of(ctx, "top.txt") : 0;
    const uint32_t hello = ctx != nullptr ? inode_of(ctx, "docs/hello.txt") : 0;
    if (!check(top != 0 && hello != 0, "files found")) {
        return;
    }
    ctx->inodes[top - 1u].extents[0] = ctx->inodes[hello - 1u].extents[0];
    store_metadata(image, ctx);
    memcpy(before, image, kImageBytes);
    sxfs_repair_report report = {};
    check(repair_image(image, report) && report.cross_linked_sectors != 0 && sxfs_repair_unresolved(&report) != 0,
          "sectors with two owners are reported as unresolved");
    ctx = host_open(image);
    check(ctx != nullptr &&
              memcmp(&ctx->inodes[top - 1u], before + static_cast<size_t>(SXFS_INODE_TABLE_LBA) * SXFS_SECTOR_SIZE +
                                                   static_cast<size_t>(top - 1u) * SXFS_INODE_SIZE,
                     SXFS_INODE_SIZE) == 0 &&
              host_reads(ctx, "docs/hello.txt", kHello, sizeof(kHello) - 1u),
          "neither inode is changed and the contents are still there");

    // An inode whose record contradicts itself: sectors nothing else claims
    // may be its, so none is freed.
    if (!check(build_image(image), "image built again")) {
        return;
    }
    ctx = host_open(image);
    const uint32_t victim = inode_of(ctx, "top.txt");
    ctx->inodes[victim - 1u].extents[0].start_lba = kTotalSectors + 10u;
    for (uint32_t sector = kTotalSectors - 5u; sector < kTotalSectors; ++sector) {
        sxfs_bitmap_set(ctx->block_bitmap, sector, 1);
    }
    store_metadata(image, ctx);
    report = {};
    check(repair_image(image, report) && report.damaged_inodes == 1 && report.sectors_freed == 0 &&
              report.leaked_sectors_kept >= 5,
          "with a damaged inode, unclaimed sectors are kept and counted");
    ctx = host_open(image);
    check(ctx != nullptr && host_reads(ctx, "docs/hello.txt", kHello, sizeof(kHello) - 1u),
          "the rest of the tree still reads");
}

} // namespace

int main() {
    printf("SxFS RECOVERY TEST START\n");
    uint8_t* buffers[5] = {};
    for (uint8_t*& buffer : buffers) {
        buffer = static_cast<uint8_t*>(malloc(kImageBytes));
    }
    g_pending_image = static_cast<uint8_t*>(malloc(kImageBytes));
    g_ctx = static_cast<sxfs_ctx*>(calloc(1, sizeof(sxfs_ctx)));
    if (buffers[4] == nullptr || g_pending_image == nullptr || g_ctx == nullptr) {
        printf("SxFS RECOVERY TEST FAIL: no memory\n");
        return 1;
    }

    case_host_recovery_matches_the_kernel(buffers[0], buffers[1], buffers[2], buffers[3]);
    case_inspection_and_read_only_mount(buffers[0], buffers[1]);
    case_repair_preserves_what_it_can_reach(buffers[0], buffers[1], buffers[2]);
    case_repair_finishes_committed_deletions(buffers[0]);
    case_repair_leaves_what_it_cannot_attribute(buffers[0], buffers[1]);

    printf("%s (%d checks, %d failures)\n", g_failures == 0 ? "SxFS RECOVERY TEST PASS" : "SxFS RECOVERY TEST FAIL",
           g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
