#pragma once

/* sxnprofile: where a SUN program keeps the person's settings, the
 * counterpart of POSIX's savanxp_profile_dir and savanxp_settings_path
 * (docs/kernel/USERS_AND_PERMISSIONS.md#settings-the-users-and-the-machines).
 *
 * The profile is the real uid's, so an elevated program (uid 1000, euid 0)
 * finds the same one as the program that asked for it. The person's settings
 * are in its Settings folder; the system's profile is the root itself and
 * its settings stay loose there.
 *
 * Built from system/runtime/sxnprofile.c, linked into every native program;
 * the linker drops it from the ones that do not call it.
 */

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* The folder the settings live in, under a profile. */
#define SXN_SETTINGS_DIR "Settings"

/* The profile of the person this process is: "/users/user" for the user,
 * "/" for the system and for an identity with no profile. */
const char *sxn_profile_directory(void);

/* The path of the setting `name` (a bare file name) in the person's profile,
 * into `out`. 0, -EINVAL for a bad argument, -E2BIG when it does not fit.
 *
 * Looking for it makes Settings if it is missing and moves in a copy left
 * loose in the profile from before the folder. For reading (for_write 0),
 * when there is neither, it answers the old copy at the root, which stays
 * where it is; the first save goes to Settings. The same rules, in the same
 * order, as savanxp_settings_path. */
long sxn_settings_path(const char *name, int for_write, char *out, size_t capacity);

#ifdef __cplusplus
}
#endif
