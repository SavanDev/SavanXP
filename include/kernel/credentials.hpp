#pragma once

#include <stdint.h>

/* Who a process acts as (docs/kernel/USERS_AND_PERMISSIONS.md, "The token").
 *
 * Every process carries one, inherited unchanged by spawn, fork and exec. It
 * only ever goes down: setuid lowers it, and nothing raises it. The one way to
 * a higher token will be a new process the elevation broker creates.
 *
 * uid is who the person is, euid whom the process acts as, and they differ in
 * an elevated process (uid 1000, euid 0). Permission checks use euid; access()
 * asks with uid, as POSIX says. */
namespace cred {

constexpr uint32_t kSystemUid = 0;
constexpr uint32_t kSystemGid = 0;

struct Credentials {
    uint32_t uid;
    uint32_t euid;
    uint32_t gid;
};

// What the kernel and init run as. All zero, so a process slot cleared with
// memset is already the system's.
constexpr Credentials system() {
    return {kSystemUid, kSystemUid, kSystemGid};
}

inline bool acts_as_system(const Credentials& who) {
    return who.euid == kSystemUid;
}

// The same person, acting as the system: what the broker will create.
inline bool elevated(const Credentials& who) {
    return who.euid == kSystemUid && who.uid != kSystemUid;
}

// The same credentials seen through the real uid, for access().
inline Credentials as_real(const Credentials& who) {
    return {who.uid, who.uid, who.gid};
}

} // namespace cred
