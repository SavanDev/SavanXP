/*
 * sxfs_ownership.h -- who owns what on a SxFS volume, by default.
 *
 * The policy half of SXFS_FEATURE_OWNERSHIP (docs/kernel/USERS_AND_PERMISSIONS.md):
 * the format says where uid, gid and mode live; this says what they are for a
 * path when nothing recorded them. It is applied when a volume without the
 * feature is stamped, to an inode whose mode is still 0, and by whoever
 * creates an inode while there are no tokens to say who created it.
 *
 * Freestanding like sxfs_format.h, so the kernel driver and the host tools
 * apply the same table. Paths are relative to the volume's root, without a
 * leading '/', as libsxfs and the kernel's records keep them ("" is the root).
 */
#ifndef SXFS_SXFS_OWNERSHIP_H
#define SXFS_SXFS_OWNERSHIP_H

#include <stdint.h>

#include "sxfs/sxfs_format.h"

/* The two identities (USERS_AND_PERMISSIONS.md, "The identities"). */
#define SXFS_UID_SYSTEM 0u
#define SXFS_GID_SYSTEM 0u
#define SXFS_UID_USER 1000u
#define SXFS_GID_USERS 100u

#define SXFS_MODE_TYPE_MASK 0170000u
#define SXFS_MODE_DIRECTORY 0040000u
#define SXFS_MODE_FILE 0100000u
#define SXFS_MODE_PERMISSIONS 07777u
#define SXFS_MODE_STICKY 01000u

/* The type bits an inode of `type` must carry. */
static inline uint16_t sxfs_mode_type_bits(uint16_t type) {
    return (uint16_t)(type == SXFS_INODE_DIRECTORY ? SXFS_MODE_DIRECTORY : SXFS_MODE_FILE);
}

/* A recorded mode is valid when its type bits agree with the inode's type. */
static inline int sxfs_mode_matches_type(uint16_t mode, uint16_t type) {
    return (mode & SXFS_MODE_TYPE_MASK) == sxfs_mode_type_bits(type);
}

/* `path` is `top` or below it. */
static inline int sxfs_ownership_path_under(const char* path, const char* top) {
    while (*top != '\0') {
        if (*path != *top) {
            return 0;
        }
        ++path;
        ++top;
    }
    return *path == '\0' || *path == '/';
}

/* The default uid, gid and mode for an inode of `type` at `path`:
 *
 *   users/<name> and below   the user, 0700 directories, 0600 files
 *   tmp                      system, 1777
 *   bin, lib and below       system, 0755
 *   everything else          system, 0755 directories, 0644 files
 *
 * The profile is the second component and below: "users" itself is the
 * system's, as C:\Users is on NT. */
static inline void sxfs_default_ownership(const char* path, uint16_t type,
                                          uint32_t* uid, uint32_t* gid, uint16_t* mode) {
    const int directory = type == SXFS_INODE_DIRECTORY;
    uint32_t permissions = directory ? 0755u : 0644u;
    *uid = SXFS_UID_SYSTEM;
    *gid = SXFS_GID_SYSTEM;

    if (sxfs_ownership_path_under(path, "users") && path[5] == '/' && path[6] != '\0') {
        *uid = SXFS_UID_USER;
        *gid = SXFS_GID_USERS;
        permissions = directory ? 0700u : 0600u;
    } else if (path[0] == 't' && path[1] == 'm' && path[2] == 'p' && path[3] == '\0' && directory) {
        permissions = SXFS_MODE_STICKY | 0777u;
    } else if (sxfs_ownership_path_under(path, "bin") || sxfs_ownership_path_under(path, "lib")) {
        permissions = 0755u;
    }
    *mode = (uint16_t)(sxfs_mode_type_bits(type) | permissions);
}

#endif /* SXFS_SXFS_OWNERSHIP_H */
