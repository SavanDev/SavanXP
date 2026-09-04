/*
 * SavanXP - info del sistema para el subsistema nativo (Fase 3).
 *
 * Envuelve las syscalls del baseline system_info/proc_info/realtime (que el
 * runtime nativo delega en posix) y expone accessors de alto nivel tipados,
 * para que el codigo Haxe no tenga que conocer los structs del ABI. Es lo que
 * necesita el port de aboutapp. Toma un snapshot con sxn_sys_refresh() y los
 * getters leen del cache (evita re-hacer las syscalls por cada campo).
 */
#include "savanxp_native.h"

/* ABI del baseline (numeros de syscall + structs); self-contained (solo
 * stdint). El nativo comparte el baseline con posix, asi que compartir este
 * header es consistente. */
#include "savanxp/syscall.h"
#include "shared/version.h"

static long sys_system_info(struct savanxp_system_info *info) {
    return sxn_syscall1(SAVANXP_SYS_SYSTEM_INFO, (long)info);
}

static long sys_proc_info(unsigned long index, struct savanxp_process_info *proc) {
    return sxn_syscall3(SAVANXP_SYS_PROC_INFO, (long)index, (long)proc, 0);
}

static long sys_realtime(struct savanxp_realtime *rt) {
    return sxn_syscall1(SAVANXP_SYS_REALTIME, (long)rt);
}

static struct savanxp_system_info g_info;
static struct savanxp_realtime g_rt;
static int g_proc_count;

void sxn_sys_refresh(void) {
    struct savanxp_process_info proc;
    unsigned long index = 0;

    g_proc_count = 0;
    (void)sys_system_info(&g_info);
    while (sys_proc_info(index, &proc) > 0) {
        if (proc.state != SAVANXP_PROC_UNUSED) {
            g_proc_count += 1;
        }
        index += 1;
    }
    if (sys_realtime(&g_rt) != 0) {
        g_rt.valid = 0;
    }
}

const char *sxn_sys_version(void) { return SAVANXP_VERSION_STRING; }

/* Getters (int alcanza para lo que muestra aboutapp; uptime en ms cabe en 32
 * bits hasta ~24 dias, de sobra para la demo). */
int sxn_sys_uptime_ms(void) { return (int)g_info.uptime_ms; }
int sxn_sys_process_count(void) { return g_proc_count; }
int sxn_sys_memory_usable_mib(void) { return (int)(g_info.memory_usable_bytes / (1024u * 1024u)); }
int sxn_sys_memory_reclaimable_mib(void) { return (int)(g_info.memory_reclaimable_bytes / (1024u * 1024u)); }
int sxn_sys_disk_used_mib(void) { return (int)(g_info.sxfs_used_bytes / (1024u * 1024u)); }
int sxn_sys_disk_total_mib(void) { return (int)(g_info.sxfs_total_bytes / (1024u * 1024u)); }
int sxn_sys_clock_valid(void) { return g_rt.valid != 0 ? 1 : 0; }
int sxn_sys_clock_hour(void) { return g_rt.hour; }
int sxn_sys_clock_minute(void) { return g_rt.minute; }
int sxn_sys_clock_second(void) { return g_rt.second; }
