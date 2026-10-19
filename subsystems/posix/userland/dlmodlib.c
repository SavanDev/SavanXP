/* Three libraries for dltest, one per DEFINE, like the diamond.
 *
 *   libdlmod_base  defines dl_probe() = 100, and has a constructor
 *   libdlmod_one   defines its own dl_probe() = 1, unrelated to base's
 *   libdlmod_two   needs base, defines dl_two_probe() that calls dl_probe()
 *
 * The collision is the point. It is the shape of Xash3D's client.so and hl.so,
 * which both carry the HLSDK's PM_* code: with one loaded RTLD_LOCAL, the other
 * must still bind to its own tree. */
#include "libc.h"

#if defined(DLMOD_BASE)
/* Set by a constructor, so dltest can tell dlopen ran it. Xash3D's client.so is
 * C++ with global objects; without its constructors it starts half-built. */
static int g_constructed = 0;

__attribute__((constructor)) static void dl_base_construct(void)
{
    g_constructed = 1;
}

int dl_base_constructed(void)
{
    return g_constructed;
}

int dl_probe(void)
{
    return 100;
}

#elif defined(DLMOD_ONE)
int dl_probe(void)
{
    return 1;
}

int dl_one_only(void)
{
    return 11;
}

#elif defined(DLMOD_TWO)
extern int dl_probe(void);

int dl_two_probe(void)
{
    return dl_probe();
}
#endif
