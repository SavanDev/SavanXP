/* TinyCC configured for SavanXP: the compiler that runs on the system and,
 * with TCC_SX_TCCDIR defined, the host-side cross compiler that builds
 * libtcc1.a for it. */
#define TCC_VERSION "0.9.28rc"
#define TCC_TARGET_X86_64 1
#define TARGETOS_SavanXP 1
#ifdef TCC_SX_TCCDIR
# define CONFIG_TCCDIR TCC_SX_TCCDIR
#else
# define CONFIG_TCCDIR "/system/tcc"
# define CONFIG_TCC_STATIC 1
# define CONFIG_SAVANXP_RUNMEM 1
#endif
#define CONFIG_TCC_SYSINCLUDEPATHS "{B}/include:{B}/sdk/include"
#define CONFIG_TCC_LIBPATHS "{B}/lib"
#define CONFIG_TCC_CRTPREFIX "{B}/lib"
#define CONFIG_TCC_BACKTRACE 0
#define CONFIG_TCC_BCHECK 0
#define CONFIG_TCC_SEMLOCK 0
#define CONFIG_TCC_PREDEFS 1
