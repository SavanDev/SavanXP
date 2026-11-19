#include "sxn/sxnprofile.h"

#include <stdio.h>
#include <string.h>

#include "sxn/sxn.h"

/* The people the system knows, as the POSIX runtime's passwd table has them
 * (docs/kernel/USERS_AND_PERMISSIONS.md, "Decided": one user, uid 1000,
 * /users/user, until there is a second one). */
#define SXN_PROFILE_USER_UID 1000u
#define SXN_PROFILE_USER_DIR "/users/user"
#define SXN_PROFILE_SYSTEM_DIR "/"

const char *sxn_profile_directory(void)
{
    struct sxn_identity identity;

    if (sxn_query_identity(&identity) == 0 && identity.uid == SXN_PROFILE_USER_UID)
    {
        return SXN_PROFILE_USER_DIR;
    }
    return SXN_PROFILE_SYSTEM_DIR;
}

static long join_profile(const char *directory, const char *name, char *out, size_t capacity)
{
    const int written = strcmp(directory, "/") == 0
        ? snprintf(out, capacity, "/%s", name)
        : snprintf(out, capacity, "%s/%s", directory, name);

    return written > 0 && (size_t)written < capacity ? 0 : -SAVANXP_E2BIG;
}

static int path_exists(const char *path)
{
    struct sxn_file_info info;

    return sxn_query_path(path, &info) == 0;
}

/* The profile's Settings folder, made if it is missing. SUN has no chown, so
 * an elevated program (euid 0) leaves it alone rather than make the person a
 * folder the system owns; init makes it at boot, so it is there anyway. */
static void ensure_settings_dir(const char *directory)
{
    struct sxn_identity identity;

    if (path_exists(directory))
    {
        return;
    }
    if (sxn_query_identity(&identity) == 0 && identity.euid != identity.uid)
    {
        return;
    }
    (void)sxn_make_directory(directory);
}

long sxn_settings_path(const char *name, int for_write, char *out, size_t capacity)
{
    const char *profile = sxn_profile_directory();
    char directory[256];
    char loose[256];
    char legacy[256];

    if (name == 0 || name[0] == '\0' || name[0] == '/' || out == 0 || capacity == 0)
    {
        return -SAVANXP_EINVAL;
    }
    /* The system's profile is the root, and its settings stay loose there. */
    if (strcmp(profile, "/") == 0)
    {
        return join_profile(profile, name, out, capacity);
    }
    if (join_profile(profile, SXN_SETTINGS_DIR, directory, sizeof(directory)) < 0 ||
        join_profile(directory, name, out, capacity) < 0 ||
        join_profile(profile, name, loose, sizeof(loose)) < 0)
    {
        return -SAVANXP_E2BIG;
    }
    if (path_exists(out))
    {
        return 0;
    }
    /* Not in Settings yet: a copy loose in the profile, from before the
     * folder, moves in. If it cannot, a read still finds it where it is. */
    ensure_settings_dir(directory);
    if (path_exists(loose))
    {
        if (sxn_rename_path(loose, out) == 0 || for_write)
        {
            return 0;
        }
        return join_profile(profile, name, out, capacity);
    }
    if (for_write)
    {
        return 0;
    }
    /* Nor in the profile: the copy at the root from before profiles, if there
     * is one. The root is the system's, so it stays; the first save goes to
     * Settings. */
    if (join_profile("/", name, legacy, sizeof(legacy)) == 0 && path_exists(legacy) && strlen(legacy) < capacity)
    {
        memcpy(out, legacy, strlen(legacy) + 1u);
    }
    return 0;
}
