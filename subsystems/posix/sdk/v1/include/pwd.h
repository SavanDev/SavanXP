#pragma once

#include "sys/types.h"

/* The two identities of docs/kernel/USERS_AND_PERMISSIONS.md, from a fixed
 * table: uid 0 is "root" (the system, by POSIX's name for it) and uid 1000 is
 * "user", whose name is provisional until the system asks for one. pw_name and
 * pw_dir come first, where binaries built before the other fields read them. */
struct passwd {
    char* pw_name;
    char* pw_dir;
    uid_t pw_uid;
    gid_t pw_gid;
    char* pw_shell;
};

struct passwd* getpwnam(const char* name);
struct passwd* getpwuid(uid_t uid);
