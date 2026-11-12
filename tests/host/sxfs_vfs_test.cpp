/*
 * sxfs_vfs_test.cpp -- Host test of the file contract the VFS gives its
 * callers over SxFS (docs/kernel/FILESYSTEMS.md, "The file contract"), with
 * the REAL kernel/vfs.cpp, kernel/fs.cpp and kernel/sxfs.cpp linked together
 * over the fake block device of sxfs_host_stubs.cpp. A driver test with a
 * stubbed VFS cannot check any of this: what is under test is what the two
 * agree on.
 *
 *   - a driver failure reaches the caller as its own errno, and a read error
 *     is not an end of file;
 *   - rename replaces an existing destination, with the type rules;
 *   - a file unlinked or replaced while a handle holds it stays readable and
 *     writable through that handle, is freed exactly once at the last
 *     release, and is freed by the next mount when there was no release;
 *   - node slots are reused, so creating and deleting does not use the tree
 *     up.
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
const char kHello[] = "hello, held file\n";
const char kOther[] = "the other one\n";

// --- The holders: what an open handle does in the kernel ----------------------

void hold(vfs::Vnode* node) {
    vfs::retain(*node);
}

// The last close: the handle goes, and the VFS is told.
void let_go(vfs::Vnode* node) {
    vfs::release(*node);
}

// --- Image and mount -----------------------------------------------------------

struct ImageCookie {
    uint8_t* base;
    uint32_t sector_count;
};

int image_read(void* cookie, uint32_t lba, uint32_t count, void* buffer) {
    ImageCookie* image = static_cast<ImageCookie*>(cookie);
    memcpy(buffer, image->base + static_cast<size_t>(lba) * SXFS_SECTOR_SIZE,
           static_cast<size_t>(count) * SXFS_SECTOR_SIZE);
    return SXFS_OK;
}

int image_write(void* cookie, uint32_t lba, uint32_t count, const void* buffer) {
    ImageCookie* image = static_cast<ImageCookie*>(cookie);
    memcpy(image->base + static_cast<size_t>(lba) * SXFS_SECTOR_SIZE, buffer,
           static_cast<size_t>(count) * SXFS_SECTOR_SIZE);
    return SXFS_OK;
}

bool build_image(uint8_t* image) {
    memset(image, 0, kImageBytes);
    ImageCookie cookie = {image, kTotalSectors};
    sxfs_ctx* ctx = static_cast<sxfs_ctx*>(calloc(1, sizeof(sxfs_ctx)));
    if (ctx == nullptr) {
        return false;
    }
    sxfs_ctx_init(ctx, &cookie, &image_read, &image_write);
    const bool ok = sxfs_format(ctx, kTotalSectors) == SXFS_OK &&
        sxfs_write_file(ctx, "docs/hello.txt", kHello, sizeof(kHello) - 1u) == SXFS_OK &&
        sxfs_write_file(ctx, "docs/other.txt", kOther, sizeof(kOther) - 1u) == SXFS_OK &&
        sxfs_mkdir_p(ctx, "empty") == SXFS_OK &&
        sxfs_mkdir_p(ctx, "full") == SXFS_OK &&
        sxfs_write_file(ctx, "full/inside.txt", "x", 1) == SXFS_OK &&
        sxfs_flush(ctx) == SXFS_OK;
    free(ctx);
    return ok;
}

// An initramfs with nothing in it: the VFS is ready once it reads the trailer.
const char kEmptyArchive[] =
    "070701" "00000000" "00000000" "00000000" "00000000" "00000001" "00000000"
    "00000000" "00000000" "00000000" "00000000" "00000000" "0000000B" "00000000"
    "TRAILER!!!\0\0\0\0";

size_t g_mount = fs::kInvalidMount;

// Boots the whole stack on `image`: VFS, registry, driver, root mounted at "/".
bool mount_image(uint8_t* image, bool writable = true, uint32_t sectors = kTotalSectors) {
    hoststub::attach_device(image, sectors, writable);
    vfs::initialize(kEmptyArchive, sizeof(kEmptyArchive) - 1u);
    fs::initialize();
    sxfs::initialize();
    static bool registered = false;
    if (!registered) {
        registered = fs::register_driver(sxfs::driver());
    }
    g_mount = fs::mount_any(fs::kRootMountPoint);
    return vfs::ready() && g_mount != fs::kInvalidMount && fs::attach(g_mount);
}

sxfs::CheckReport report_of() {
    sxfs::CheckReport report = {};
    (void)sxfs::check(sxfs::root(), report);
    return report;
}

bool volume_is_clean() {
    const sxfs::CheckReport report = report_of();
    return report.clean() && report.data_used_sectors == report.data_claimed_sectors;
}

vfs::Vnode* open_read(const char* path) {
    return vfs::open(kSystem, path, SAVANXP_OPEN_READ, 0);
}

vfs::Vnode* create(const char* path) {
    return vfs::open(kSystem, path, SAVANXP_OPEN_WRITE | SAVANXP_OPEN_CREATE, 0644);
}

bool node_reads(vfs::Vnode* node, const void* expected, size_t size) {
    char buffer[256] = {};
    return node != nullptr && node->size == size && size <= sizeof(buffer) &&
        vfs::read(*node, 0, buffer, size) == size && memcmp(buffer, expected, size) == 0;
}

bool path_reads(const char* path, const void* expected, size_t size) {
    return node_reads(open_read(path), expected, size);
}

bool gone(const char* path) {
    return vfs::resolve(path) == nullptr;
}

// --- Cases -------------------------------------------------------------------

void case_errors_are_precise(uint8_t* image) {
    printf("case: a failure reaches the caller as its own error\n");
    if (!check(build_image(image) && mount_image(image), "volume mounted")) {
        return;
    }

    check(create("/missing/new.txt") == nullptr && vfs::last_error() == SX_ENOENT,
          "create under a missing directory: ENOENT");
    check(vfs::mkdir(kSystem, "/docs", 0755) != nullptr && vfs::last_error() == SX_EEXIST,
          "mkdir of an existing directory: EEXIST");
    check(!vfs::unlink(kSystem, "/docs") && vfs::last_error() == SX_EISDIR, "unlink of a directory: EISDIR");
    check(!vfs::rmdir(kSystem, "/full") && vfs::last_error() == SX_ENOTEMPTY, "rmdir of a full directory: ENOTEMPTY");
    check(!vfs::rmdir(kSystem, "/docs/hello.txt") && vfs::last_error() == SX_ENOTDIR, "rmdir of a file: ENOTDIR");
    check(!vfs::rename(kSystem, "/docs/hello.txt", "/rescue/hello.txt") && vfs::last_error() == SX_EXDEV,
          "rename out of the volume: EXDEV");

    // A full volume is ENOSPC, with nothing written.
    vfs::Vnode* big = create("/big.bin");
    static uint8_t chunk[64u * 1024u];
    memset(chunk, 0x33, sizeof(chunk));
    size_t offset = 0;
    size_t wrote = 0;
    while (big != nullptr && (wrote = vfs::write(*big, offset, chunk, sizeof(chunk), false)) == sizeof(chunk)) {
        offset += wrote;
    }
    check(big != nullptr && wrote == 0 && vfs::last_error() == SX_ENOSPC, "write on a full volume: ENOSPC");
    check(vfs::unlink(kSystem, "/big.bin"), "the file that filled it can be deleted");

    // A device that stops writing is EIO, not "no space".
    vfs::Vnode* file = create("/io.txt");
    hoststub::fail_next_writes(1);
    check(file != nullptr && vfs::write(*file, 0, "abc", 3, false) == 0 && vfs::last_error() == SX_EIO,
          "write on a failing device: EIO");
    check(file != nullptr && vfs::write(*file, 0, "abc", 3, false) == 3, "the next write goes through");

    // A read the device fails is an error, where it used to be an end of file.
    vfs::Vnode* hello = open_read("/docs/hello.txt");
    char buffer[32] = {};
    hoststub::fail_reads(true);
    const size_t got = hello != nullptr ? vfs::read(*hello, 0, buffer, sizeof(kHello) - 1u) : 1;
    const int read_error = vfs::last_error();
    hoststub::fail_reads(false);
    check(got == 0 && read_error == SX_EIO, "read on a failing device: 0 bytes and EIO");
    check(hello != nullptr && vfs::read(*hello, sizeof(kHello) - 1u, buffer, 4) == 0 && vfs::last_error() == 0,
          "read at the end of the file: 0 bytes and no error");

    check(volume_is_clean(), "fscheck finds nothing");
    hoststub::detach_device();
}

void case_rename_replaces(uint8_t* image) {
    printf("case: rename replaces what is at the new name\n");
    if (!check(build_image(image) && mount_image(image), "volume mounted")) {
        return;
    }

    check(vfs::rename(kSystem, "/docs/hello.txt", "/docs/other.txt"), "a file over a file");
    check(gone("/docs/hello.txt") && path_reads("/docs/other.txt", kHello, sizeof(kHello) - 1u),
          "the new name has the moved file's contents, the old name is gone");

    check(!vfs::rename(kSystem, "/docs/other.txt", "/empty") && vfs::last_error() == SX_EISDIR,
          "a file over a directory: EISDIR");
    check(!vfs::rename(kSystem, "/empty", "/docs/other.txt") && vfs::last_error() == SX_ENOTDIR,
          "a directory over a file: ENOTDIR");
    check(!vfs::rename(kSystem, "/empty", "/full") && vfs::last_error() == SX_ENOTEMPTY,
          "a directory over a directory with entries: ENOTEMPTY");
    check(vfs::rename(kSystem, "/full", "/empty"), "a directory over an empty directory");
    check(gone("/full") && path_reads("/empty/inside.txt", "x", 1), "its contents came with it");

    // The temporary-file update: write beside, then rename over.
    vfs::Vnode* temporary = create("/docs/other.txt.tmp");
    check(temporary != nullptr && vfs::write(*temporary, 0, "v2\n", 3, false) == 3, "the new version is written beside");
    check(vfs::rename(kSystem, "/docs/other.txt.tmp", "/docs/other.txt"), "and renamed over the old one");
    check(path_reads("/docs/other.txt", "v2\n", 3) && gone("/docs/other.txt.tmp"), "one name, the new contents");

    check(volume_is_clean() && report_of().pending_deletions == 0, "nothing leaked, nothing pending");
    const sxfs::CheckReport before = report_of();
    check(mount_image(image) && path_reads("/docs/other.txt", "v2\n", 3) && gone("/docs/hello.txt") &&
              path_reads("/empty/inside.txt", "x", 1),
          "the same tree after a remount");
    check(volume_is_clean() && report_of().files == before.files, "and the same population");
    hoststub::detach_device();
}

void case_unlinked_file_lives_while_held(uint8_t* image) {
    printf("case: a file unlinked while open lives until the last close\n");
    if (!check(build_image(image) && mount_image(image), "volume mounted")) {
        return;
    }
    const sxfs::CheckReport start = report_of();

    vfs::Vnode* handle = open_read("/docs/hello.txt");
    hold(handle);
    check(vfs::unlink(kSystem, "/docs/hello.txt"), "unlink of an open file succeeds");
    check(gone("/docs/hello.txt"), "the name is gone");
    check(node_reads(handle, kHello, sizeof(kHello) - 1u), "the handle still reads the contents");
    check(handle != nullptr && vfs::write(*handle, 0, "HELLO", 5, false) == 5 &&
              vfs::read(*handle, 0, image /*scratch*/, 0) == 0,
          "the handle still writes");
    check(report_of().pending_deletions == 1 && report_of().clean(), "fscheck: one pending deletion, no finding");
    check(report_of().files == start.files - 1u, "the tree has one file fewer");

    // The name is free for a new file, which is not the old one.
    vfs::Vnode* again = create("/docs/hello.txt");
    check(again != nullptr && again != handle && vfs::write(*again, 0, "new", 3, false) == 3, "the name is reused");
    char buffer[8] = {};
    check(handle != nullptr && vfs::read(*handle, 0, buffer, 5) == 5 && memcmp(buffer, "HELLO", 5) == 0,
          "the old handle still sees the old file");

    let_go(handle);
    check(report_of().pending_deletions == 0 && volume_is_clean(), "the last close frees it: nothing pending, no leak");
    check(report_of().inodes_allocated == start.inodes_allocated, "one inode went and one came");
    check(path_reads("/docs/hello.txt", "new", 3), "the new file is untouched");

    // Replaced by a rename while held: the same rule.
    vfs::Vnode* victim = open_read("/docs/other.txt");
    hold(victim);
    check(vfs::rename(kSystem, "/docs/hello.txt", "/docs/other.txt"), "rename over an open file succeeds");
    check(node_reads(victim, kOther, sizeof(kOther) - 1u), "the replaced file's handle still reads it");
    check(path_reads("/docs/other.txt", "new", 3), "the name has the new contents");
    check(report_of().pending_deletions == 1, "one pending deletion");
    let_go(victim);
    check(report_of().pending_deletions == 0 && volume_is_clean(), "freed at the last close");

    // A directory someone has open is not removed from under them.
    vfs::Vnode* directory = vfs::open(kSystem, "/empty", SAVANXP_OPEN_READ, 0);
    hold(directory);
    check(!vfs::rmdir(kSystem, "/empty") && vfs::last_error() == SX_EBUSY, "rmdir of an open directory: EBUSY");
    let_go(directory);
    check(vfs::rmdir(kSystem, "/empty"), "and it goes once closed");
    hoststub::detach_device();
}

// Holders are counted on the node: a handle and a section on the same file
// are two, and the file is freed only when both are gone, exactly once.
void case_holders_are_counted(uint8_t* image) {
    printf("case: holders are counted\n");
    if (!check(build_image(image) && mount_image(image), "volume mounted")) {
        return;
    }
    vfs::Vnode* file = open_read("/docs/hello.txt");
    if (!check(file != nullptr && !vfs::is_held(*file), "a node nobody retained is not held")) {
        hoststub::detach_device();
        return;
    }
    hold(file);
    hold(file);
    check(vfs::is_held(*file) && file->references == 2, "two holders, two references");
    check(vfs::unlink(kSystem, "/docs/hello.txt") && report_of().pending_deletions == 1, "unlinked while held");
    let_go(file);
    check(report_of().pending_deletions == 1 && node_reads(file, kHello, sizeof(kHello) - 1u),
          "one holder left: still pending, still readable");
    let_go(file);
    check(report_of().pending_deletions == 0 && volume_is_clean(), "the second release frees it");
    check(file->type == vfs::NodeType::invalid && file->references == 0, "the slot is cleared");

    // An unbalanced release changes nothing: the count does not wrap into
    // "held forever" and nothing is freed twice.
    let_go(file);
    check(!vfs::is_held(*file) && volume_is_clean(), "an extra release is ignored");

    // A held directory cannot be removed or detached; a released one can.
    vfs::Vnode* directory = vfs::open(kSystem, "/full", SAVANXP_OPEN_READ, 0);
    hold(directory);
    check(!vfs::detach_subtree("/full", &vfs::is_held) && vfs::last_error() == SX_EBUSY,
          "a held subtree does not detach");
    let_go(directory);
    check(!vfs::is_held(*directory), "released");
    hoststub::detach_device();
}

// --- Directories published on demand (VFS-C) ------------------------------------

// A driver with nothing behind it but what populate publishes: "/lazy" holds
// a, b and the directory sub; "/lazy/sub" holds x; "/broken" publishes two
// entries and then fails, as a driver that ran out of memory would.
int g_populates = 0;
int g_forgets = 0;
bool g_broken_fails = true;
fs::Driver g_lazy_driver = {};

fs::Status lazy_populate(vfs::Vnode& directory) {
    ++g_populates;
    char path[vfs::kMaxPathLength] = {};
    if (!vfs::path_of(directory, path, sizeof(path))) {
        return fs::Status::invalid;
    }
    if (strcmp(path, "/lazy") == 0) {
        const bool ok = vfs::install_volume_file("/lazy/a", g_lazy_driver, nullptr, 0, false) != nullptr &&
            vfs::install_volume_file("/lazy/b", g_lazy_driver, nullptr, 0, false) != nullptr &&
            vfs::install_volume_directory("/lazy/sub", g_lazy_driver) != nullptr;
        return ok ? fs::Status::ok : fs::Status::no_space;
    }
    if (strcmp(path, "/lazy/sub") == 0) {
        return vfs::install_volume_file("/lazy/sub/x", g_lazy_driver, nullptr, 0, false) != nullptr
            ? fs::Status::ok : fs::Status::no_space;
    }
    if (strcmp(path, "/broken") == 0) {
        (void)vfs::install_volume_file("/broken/one", g_lazy_driver, nullptr, 0, false);
        (void)vfs::install_volume_directory("/broken/two", g_lazy_driver);
        if (g_broken_fails) {
            return fs::Status::no_space;
        }
        return fs::Status::ok;
    }
    return fs::Status::ok;
}

void lazy_forget(vfs::Vnode&) {
    ++g_forgets;
}

fs::Status lazy_read(vfs::Vnode&, size_t, void*, size_t) {
    return fs::Status::ok;
}

void lazy_refresh(vfs::Vnode&) {}

void case_directories_published_on_demand(uint8_t* image) {
    printf("case: directories published on demand\n");
    if (!check(build_image(image) && mount_image(image), "volume mounted")) {
        return;
    }
    g_lazy_driver = {};
    g_lazy_driver.name = "lazy";
    g_lazy_driver.read = &lazy_read;
    g_lazy_driver.refresh = &lazy_refresh;
    g_lazy_driver.populate = &lazy_populate;
    g_lazy_driver.forget = &lazy_forget;
    g_populates = 0;
    g_forgets = 0;
    g_broken_fails = true;

    vfs::Vnode* lazy = vfs::install_volume_directory("/lazy", g_lazy_driver);
    check(lazy != nullptr && !lazy->populated && g_populates == 0, "installed, nothing published yet");
    check(vfs::child_count(*lazy) == 3 && g_populates == 1 && lazy->populated, "listing it publishes it, once");
    check(vfs::child_count(*lazy) == 3 && g_populates == 1, "a second listing does not populate again");
    check(vfs::resolve("/lazy/sub/x") != nullptr && g_populates == 2, "a lookup publishes each directory on the way");
    check(vfs::resolve("/lazy/sub/missing") == nullptr && g_populates == 2, "a miss in a published directory");

    vfs::Vnode* broken = vfs::install_volume_directory("/broken", g_lazy_driver);
    check(vfs::resolve("/broken/one") == nullptr, "a failed populate fails the lookup");
    check(broken != nullptr && !broken->populated && vfs::child_count(*broken) == 0 && g_forgets == 4,
          "what it published is taken back, and the driver told");
    g_broken_fails = false;
    check(vfs::resolve("/broken/one") != nullptr && broken->populated, "the next look tries again");
    check(vfs::child_count(*broken) == 2, "and publishes each entry once");
    hoststub::detach_device();
}

void case_power_cut_with_an_unlinked_file_open(uint8_t* image) {
    printf("case: a power cut with an unlinked file still open\n");
    if (!check(build_image(image) && mount_image(image), "volume mounted")) {
        return;
    }
    const sxfs::CheckReport start = report_of();
    vfs::Vnode* handle = open_read("/docs/hello.txt");
    hold(handle);
    check(vfs::unlink(kSystem, "/docs/hello.txt") && report_of().pending_deletions == 1, "unlinked and held");

    // No close, no shutdown: the machine just stops. The next mount finds a
    // doomed inode nothing names and frees it.
    check(mount_image(image), "the volume mounts again");
    check(gone("/docs/hello.txt"), "the file did not come back");
    check(report_of().pending_deletions == 0 && volume_is_clean(), "the mount freed it");
    check(report_of().inodes_allocated == start.inodes_allocated - 1u, "its inode is free");
    check(report_of().data_used_sectors < start.data_used_sectors, "and so are its blocks");
    hoststub::detach_device();
}

// More creations and deletions than the tree has nodes: before slots were
// reused, every unlink left its node behind and the 2048th create failed.
void case_churn_reuses_nodes(uint8_t* image) {
    printf("case: creating and deleting does not use the tree up\n");
    if (!check(build_image(image) && mount_image(image), "volume mounted")) {
        return;
    }
    constexpr int kRounds = 5000;
    int done = 0;
    for (; done < kRounds; ++done) {
        vfs::Vnode* node = create("/churn.tmp");
        if (node == nullptr || vfs::write(*node, 0, "x", 1, false) != 1) {
            break;
        }
        // Alternate the two ways a name goes: unlink, and a replacing rename.
        if ((done & 1) == 0) {
            if (!vfs::unlink(kSystem, "/churn.tmp")) {
                break;
            }
        } else if (!vfs::rename(kSystem, "/churn.tmp", "/docs/other.txt")) {
            break;
        }
    }
    check(done == kRounds, "5000 create/delete rounds all succeed");
    check(path_reads("/docs/other.txt", "x", 1), "the last replacement is in place");
    check(volume_is_clean(), "fscheck finds nothing");
    hoststub::detach_device();
}

// A file rewritten must not be served from a cache filled before the write.
void case_content_generation_moves(uint8_t* image) {
    printf("case: a file's content generation moves with its bytes\n");
    if (!check(build_image(image) && mount_image(image), "volume mounted")) {
        return;
    }
    vfs::Vnode* node = vfs::open(kSystem, "/docs/hello.txt", SAVANXP_OPEN_READ | SAVANXP_OPEN_WRITE, 0);
    if (!check(node != nullptr, "file opened")) {
        return;
    }
    uint32_t generation = node->content_generation;
    check(vfs::write(*node, 0, "J", 1, false) == 1 && node->content_generation != generation, "a write moves it");
    generation = node->content_generation;
    check(vfs::truncate(kSystem, "/docs/hello.txt", 2) && node->content_generation != generation, "a truncate moves it");
    generation = node->content_generation;
    char buffer[4] = {};
    check(vfs::read(*node, 0, buffer, 2) == 2 && node->content_generation == generation, "a read does not");

    // A slot that is reused does not start over at a generation it had.
    check(vfs::unlink(kSystem, "/docs/hello.txt"), "the file is deleted");
    vfs::Vnode* next = create("/docs/next.txt");
    check(next != node || next->content_generation > generation,
          "the next file in the same slot has a generation the old one never had");
    hoststub::detach_device();
}

// The durable-update protocol, the way a program replaces a file it must not
// lose: write a temporary, synchronize, rename it over the old name,
// synchronize. Returns how many of the four steps completed.
int durable_update() {
    vfs::Vnode* node = create("/docs/hello.tmp");
    if (node == nullptr || vfs::write(*node, 0, kOther, sizeof(kOther) - 1u, false) != sizeof(kOther) - 1u) {
        return 0;
    }
    if (!fs::sync_all_deferred()) {
        return 1;
    }
    if (!vfs::rename(kSystem, "/docs/hello.tmp", "/docs/hello.txt")) {
        return 2;
    }
    return fs::sync_all_deferred() ? 4 : 3;
}

void case_durable_update_survives_any_cut(uint8_t* image) {
    printf("case: the durable-update protocol across a power cut at every sector\n");
    uint8_t* pristine = static_cast<uint8_t*>(malloc(kImageBytes));
    if (!check(pristine != nullptr && build_image(image), "image built")) {
        free(pristine);
        return;
    }
    memcpy(pristine, image, kImageBytes);
    if (!check(mount_image(image), "volume mounted")) {
        free(pristine);
        return;
    }
    const size_t before = hoststub::sectors_written();
    check(durable_update() == 4, "the uninterrupted protocol completes");
    const size_t total = hoststub::sectors_written() - before;

    const size_t old_size = sizeof(kHello) - 1u;
    const size_t new_size = sizeof(kOther) - 1u;
    size_t wrong = 0;
    size_t first_wrong = 0;
    bool saw_old = false;
    bool saw_new = false;
    for (size_t cut = 0; cut <= total; ++cut) {
        memcpy(image, pristine, kImageBytes);
        if (!mount_image(image)) {
            ++wrong;
            continue;
        }
        hoststub::power_cut_after(cut);
        const int reached = durable_update();
        hoststub::clear_power_cut();

        bool good = mount_image(image) && volume_is_clean();
        const bool is_old = path_reads("/docs/hello.txt", kHello, old_size);
        const bool is_new = path_reads("/docs/hello.txt", kOther, new_size);
        const bool temporary = !gone("/docs/hello.tmp");
        // The name always holds one whole version; the new one never shares
        // the volume with its temporary.
        good = good && (is_old || is_new) && !(is_new && temporary);
        // Nothing replaces the file before the rename is asked for.
        good = good && (reached >= 2 || is_old);
        // A temporary that was synchronized is whole if it is still there.
        good = good && (reached < 1 || !temporary || reached < 2 ||
                        path_reads("/docs/hello.tmp", kOther, new_size));
        // Once the last synchronization returned, the update is on the medium.
        good = good && (reached < 4 || is_new);
        saw_old = saw_old || is_old;
        saw_new = saw_new || is_new;
        if (!good && wrong++ == 0) {
            first_wrong = cut;
        }
    }
    if (wrong != 0) {
        printf("  first bad cut: %zu of %zu\n", first_wrong, total);
    }
    check(wrong == 0, "every cut leaves the old file or the new one, whole, and a clean volume");
    check(saw_old && saw_new, "both outcomes were reached");
    hoststub::detach_device();
    free(pristine);
}

// --- Allocation: two files growing side by side ---------------------------------

constexpr uint32_t kBigSectors = 49152; // 24 MiB
constexpr size_t kBigBytes = static_cast<size_t>(kBigSectors) * SXFS_SECTOR_SIZE;
constexpr size_t kAppend = 32 * 1024;
constexpr size_t kGrowTo = 6 * 1024 * 1024;

uint8_t pattern_byte(int file, size_t offset) {
    return static_cast<uint8_t>(offset * 7u + (offset >> 9) + static_cast<size_t>(file) * 13u);
}

bool append_pattern(vfs::Vnode* node, int file) {
    static uint8_t chunk[kAppend];
    const size_t offset = node->size;
    for (size_t index = 0; index < kAppend; ++index) {
        chunk[index] = pattern_byte(file, offset + index);
    }
    return vfs::write(*node, offset, chunk, kAppend, false) == kAppend;
}

bool holds_pattern(const char* path, int file, size_t size) {
    static uint8_t chunk[kAppend];
    vfs::Vnode* node = open_read(path);
    if (node == nullptr || node->size != size) {
        return false;
    }
    for (size_t offset = 0; offset < size; offset += kAppend) {
        const size_t count = size - offset < kAppend ? size - offset : kAppend;
        if (vfs::read(*node, offset, chunk, count) != count) {
            return false;
        }
        for (size_t index = 0; index < count; ++index) {
            if (chunk[index] != pattern_byte(file, offset + index)) {
                return false;
            }
        }
    }
    return true;
}

bool build_big_image(uint8_t* image) {
    memset(image, 0, kBigBytes);
    ImageCookie cookie = {image, kBigSectors};
    sxfs_ctx* ctx = static_cast<sxfs_ctx*>(calloc(1, sizeof(sxfs_ctx)));
    if (ctx == nullptr) {
        return false;
    }
    sxfs_ctx_init(ctx, &cookie, &image_read, &image_write);
    const bool ok = sxfs_format(ctx, kBigSectors) == SXFS_OK && sxfs_flush(ctx) == SXFS_OK;
    free(ctx);
    return ok;
}

uint32_t extents_of(uint8_t* image, const char* path) {
    ImageCookie cookie = {image, kBigSectors};
    sxfs_ctx* ctx = static_cast<sxfs_ctx*>(calloc(1, sizeof(sxfs_ctx)));
    uint32_t extents = 0;
    if (ctx != nullptr) {
        sxfs_ctx_init(ctx, &cookie, &image_read, &image_write);
        if (sxfs_open(ctx) == SXFS_OK) {
            // The two files are the first two inodes after the root.
            extents = ctx->inodes[strcmp(path, "/a.bin") == 0 ? 1 : 2].extent_count;
        }
        free(ctx);
    }
    return extents;
}

// Grows /a.bin and /b.bin in turns, `rounds` appends each. Returns the round
// in which an append of /a.bin wrote the most sectors (the one that moved the
// file), and how many it wrote.
bool grow_in_turns(size_t rounds, size_t& heaviest_round, size_t& heaviest_sectors) {
    vfs::Vnode* a = vfs::open(kSystem, "/a.bin", SAVANXP_OPEN_WRITE | SAVANXP_OPEN_CREATE, 0644);
    vfs::Vnode* b = vfs::open(kSystem, "/b.bin", SAVANXP_OPEN_WRITE | SAVANXP_OPEN_CREATE, 0644);
    heaviest_round = 0;
    heaviest_sectors = 0;
    for (size_t round = 0; round < rounds; ++round) {
        const size_t before = hoststub::sectors_written();
        if (a == nullptr || b == nullptr || !append_pattern(a, 0)) {
            return false;
        }
        const size_t wrote = hoststub::sectors_written() - before;
        if (wrote > heaviest_sectors) {
            heaviest_sectors = wrote;
            heaviest_round = round;
        }
        if (!append_pattern(b, 1)) {
            return false;
        }
    }
    return true;
}

// Each file's growth lands after the other's, so each growth is a new extent
// and a file has eight. Before files could be consolidated this stopped at
// 4 MiB with "no space" on a volume that was mostly empty.
void case_files_growing_side_by_side(uint8_t* small_image) {
    (void)small_image;
    printf("case: two files growing in turns outlive their eight extents\n");
    uint8_t* image = static_cast<uint8_t*>(malloc(kBigBytes));
    uint8_t* pristine = static_cast<uint8_t*>(malloc(kBigBytes));
    if (!check(image != nullptr && pristine != nullptr && build_big_image(image) &&
                   mount_image(image, true, kBigSectors),
               "a 24 MiB volume mounted")) {
        free(image);
        free(pristine);
        return;
    }
    const size_t rounds = kGrowTo / kAppend;
    size_t heaviest_round = 0;
    size_t heaviest_sectors = 0;
    check(grow_in_turns(rounds, heaviest_round, heaviest_sectors), "both files reach 6 MiB");
    check(holds_pattern("/a.bin", 0, kGrowTo) && holds_pattern("/b.bin", 1, kGrowTo), "with every byte they were given");
    check(volume_is_clean(), "fscheck finds nothing");
    check(heaviest_sectors > 4096, "one append moved its file to a contiguous run");
    (void)fs::sync_all();
    hoststub::detach_device();
    const uint32_t a_extents = extents_of(image, "/a.bin");
    const uint32_t b_extents = extents_of(image, "/b.bin");
    printf("  extents after 6 MiB each: a %u, b %u; the move was round %zu, %zu sectors\n", a_extents, b_extents,
           heaviest_round, heaviest_sectors);
    check(a_extents != 0 && a_extents <= SXFS_MAX_EXTENTS && b_extents != 0 && b_extents <= SXFS_MAX_EXTENTS,
          "both within the format's eight extents");

    // The append that moves the file, cut at points across its whole length.
    size_t ignored_round = 0;
    size_t ignored_sectors = 0;
    if (!check(build_big_image(image) && mount_image(image, true, kBigSectors) &&
                   grow_in_turns(heaviest_round, ignored_round, ignored_sectors) && fs::sync_all(),
               "the volume just before that append")) {
        free(image);
        free(pristine);
        return;
    }
    hoststub::detach_device();
    memcpy(pristine, image, kBigBytes);
    const size_t old_size = heaviest_round * kAppend;
    size_t wrong = 0;
    size_t moved = 0;
    constexpr size_t kPoints = 48;
    for (size_t point = 0; point <= kPoints; ++point) {
        // Dense at both ends, where the journal is; spread through the copy.
        size_t cut = point * heaviest_sectors / kPoints;
        if (point < 8) {
            cut = point;
        } else if (point > kPoints - 8) {
            cut = heaviest_sectors - (kPoints - point);
        }
        memcpy(image, pristine, kBigBytes);
        if (!mount_image(image, true, kBigSectors)) {
            ++wrong;
            continue;
        }
        vfs::Vnode* a = vfs::open(kSystem, "/a.bin", SAVANXP_OPEN_WRITE, 0);
        hoststub::power_cut_after(cut);
        if (a != nullptr) {
            (void)append_pattern(a, 0);
        }
        hoststub::clear_power_cut();
        const bool mounted = mount_image(image, true, kBigSectors) && volume_is_clean();
        const bool is_old = mounted && holds_pattern("/a.bin", 0, old_size);
        const bool is_new = mounted && holds_pattern("/a.bin", 0, old_size + kAppend);
        if (!(is_old || is_new) || !holds_pattern("/b.bin", 1, old_size)) {
            ++wrong;
        }
        moved += is_new ? 1u : 0u;
        hoststub::detach_device();
    }
    check(wrong == 0, "a power cut during the move leaves the file whole, before or after, and the other intact");
    check(moved != 0 && moved != kPoints + 1u, "both outcomes were reached");
    free(image);
    free(pristine);
}

} // namespace

// The bounded cache: published directories are given back, least recently
// looked into first, never with anything under them held, and published
// again on the next look.
constexpr size_t kHuge = 1u << 20;

void case_published_directories_are_trimmed(uint8_t* image) {
    printf("case: published directories are trimmed\n");
    if (!check(build_image(image) && mount_image(image), "volume mounted")) {
        return;
    }
    g_lazy_driver = {};
    g_lazy_driver.name = "lazy";
    g_lazy_driver.read = &lazy_read;
    g_lazy_driver.refresh = &lazy_refresh;
    g_lazy_driver.populate = &lazy_populate;
    g_lazy_driver.forget = &lazy_forget;
    g_populates = 0;
    g_forgets = 0;

    vfs::Vnode* lazy = vfs::install_volume_directory("/lazy", g_lazy_driver);
    vfs::Vnode* x = const_cast<vfs::Vnode*>(vfs::resolve("/lazy/sub/x"));
    vfs::Vnode* sub = const_cast<vfs::Vnode*>(vfs::resolve("/lazy/sub"));
    if (!check(lazy != nullptr && x != nullptr && sub != nullptr && g_populates == 2, "both directories published")) {
        hoststub::detach_device();
        return;
    }
    const size_t free_before = vfs::free_slots();

    vfs::retain(*x);
    check(vfs::trim(kHuge) == 0 && g_forgets == 0, "a held file keeps every directory above it");
    vfs::release(*x);

    // /lazy is looked into after /lazy/sub: sub is the older one.
    (void)vfs::child_count(*lazy);
    check(vfs::trim(free_before + 1) == 1 && g_forgets == 1, "the least recently used directory goes first");
    check(!sub->populated && lazy->populated, "and only as much as was asked");
    check(vfs::free_slots() == free_before + 1, "its slot is free");

    vfs::retain(*lazy);
    (void)vfs::resolve("/lazy/sub/x");
    check(vfs::trim(kHuge) == 1 && lazy->populated, "a held directory keeps its entries, not its subdirectories'");
    vfs::release(*lazy);

    check(vfs::trim(kHuge) == 3 && !lazy->populated && g_forgets == 5, "the rest goes, the driver told of each");
    check(vfs::free_slots() == free_before + 4, "every slot is back");
    const int populates_before = g_populates;
    check(vfs::resolve("/lazy/sub/x") != nullptr && g_populates == populates_before + 2,
          "the next look publishes it again");

    const size_t free_now = vfs::free_slots();
    vfs::trim_if_low();
    check(vfs::free_slots() == free_now && lazy->populated, "with slots to spare, a system call trims nothing");
    hoststub::detach_device();
}

// SxFS publishes on demand too (VFS-C slice 3): a mount publishes the mount
// point, and a volume with more entries than the VFS's 2048 nodes is read
// whole, the bounded cache giving back what was visited before.
void case_sxfs_tree_larger_than_the_pool() {
    printf("case: an SxFS tree larger than the node pool\n");
    constexpr uint32_t kBigSectors = 32768;
    constexpr int kDirectories = 52;
    constexpr int kFilesEach = 50;
    uint8_t* big = static_cast<uint8_t*>(calloc(kBigSectors, SXFS_SECTOR_SIZE));
    sxfs_ctx* ctx = static_cast<sxfs_ctx*>(calloc(1, sizeof(sxfs_ctx)));
    if (!check(big != nullptr && ctx != nullptr, "memory for the image")) {
        free(big);
        free(ctx);
        return;
    }
    ImageCookie cookie = {big, kBigSectors};
    sxfs_ctx_init(ctx, &cookie, &image_read, &image_write);
    bool built = sxfs_format(ctx, kBigSectors) == SXFS_OK;
    char path[64] = {};
    for (int directory = 0; built && directory < kDirectories; ++directory) {
        for (int file = 0; built && file < kFilesEach; ++file) {
            snprintf(path, sizeof(path), "d%02d/f%02d", directory, file);
            built = sxfs_write_file(ctx, path, "x", 1) == SXFS_OK;
        }
    }
    built = built && sxfs_flush(ctx) == SXFS_OK;
    free(ctx);
    if (!check(built, "an image with 2652 entries") || !check(mount_image(big, true, kBigSectors), "it mounts")) {
        hoststub::detach_device();
        free(big);
        return;
    }
    const size_t free_at_mount = vfs::free_slots();
    check(vfs::resolve("/d00") != nullptr && vfs::free_slots() + kDirectories == free_at_mount,
          "the first look publishes the root's entries only");

    int read_back = 0;
    size_t lowest = vfs::free_slots();
    for (int directory = 0; directory < kDirectories; ++directory) {
        for (int file = 0; file < kFilesEach; ++file) {
            vfs::trim_if_low(); // each read is a system call
            snprintf(path, sizeof(path), "/d%02d/f%02d", directory, file);
            if (path_reads(path, "x", 1)) {
                ++read_back;
            }
            if (vfs::free_slots() < lowest) {
                lowest = vfs::free_slots();
            }
        }
    }
    check(read_back == kDirectories * kFilesEach, "every file reads back");
    check(lowest > 0, "the pool never ran out");
    vfs::trim_if_low();
    check(path_reads("/d00/f00", "x", 1), "a directory given back is published again");
    check(volume_is_clean(), "fscheck walks a directory with more than 32 subdirectories");
    hoststub::detach_device();
    free(big);
}

int main() {
    printf("SxFS VFS TEST START\n");
    uint8_t* image = static_cast<uint8_t*>(malloc(kImageBytes));
    if (image == nullptr) {
        printf("SxFS VFS TEST FAIL: no memory for the image\n");
        return 1;
    }

    case_errors_are_precise(image);
    case_rename_replaces(image);
    case_unlinked_file_lives_while_held(image);
    case_holders_are_counted(image);
    case_directories_published_on_demand(image);
    case_published_directories_are_trimmed(image);
    case_sxfs_tree_larger_than_the_pool();
    case_power_cut_with_an_unlinked_file_open(image);
    case_churn_reuses_nodes(image);
    case_content_generation_moves(image);
    case_durable_update_survives_any_cut(image);
    case_files_growing_side_by_side(image);

    free(image);
    printf("%s (%d checks, %d failures)\n", g_failures == 0 ? "SxFS VFS TEST PASS" : "SxFS VFS TEST FAIL",
           g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
