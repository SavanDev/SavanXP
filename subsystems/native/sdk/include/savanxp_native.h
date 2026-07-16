/*
 * SavanXP - SDK del subsistema nativo (Haxe), runtime v1 / Fase 2.
 *
 * API que consume el C++ generado por reflaxe.CPP y el glue del runtime. Esta
 * es la capa "userland" del contrato: la capa kernel (numeros de syscall
 * propios, structs compartidos) vive en savanxp_native_abi.h. Cuando llegue la
 * etapa HashLink, la VM implementara estas mismas primitivas sobre el mismo
 * ABI: el codigo Haxe no cambia.
 */
#pragma once

#include "savanxp_native_abi.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Marca e_ident[EI_OSABI] (byte 7 del ELF) con la que el kernel reconoce un
 * binario nativo y lo corre con identidad de subsistema "native". El build
 * (subsystems/native/build.ps1) estampa este valor en el ELF; el kernel lo lee
 * en elf.cpp y lo compara contra elf::kOsAbiNative (deben coincidir). */
#define SXN_ELF_OSABI_NATIVE 0x53 /* 'S' de SavanXP */

/* Numeros de syscall del baseline transitorio compartido con posix
 * (< SXN_SYS_BASE). Espejo de subsystems/posix/sdk/v1/include/savanxp/syscall.h. */
#define SXN_SYS_READ 0
#define SXN_SYS_WRITE 1
#define SXN_SYS_EXIT 7
#define SXN_SYS_SLEEP_MS 11
#define SXN_SYS_POLL 39
#define SXN_SYS_EVENT_SET 43
#define SXN_SYS_EVENT_RESET 44
#define SXN_SYS_WAIT_ONE 45
#define SXN_SYS_WAIT_MANY 46
#define SXN_SYS_MAP_VIEW 51
#define SXN_SYS_UNMAP_VIEW 52

#define SXN_SECTION_READ 1u
#define SXN_SECTION_WRITE 2u
#define SXN_WAIT_FLAG_ANY 0u
#define SXN_POLLIN 0x0001
#define SXN_POLLHUP 0x0010

/* Envolturas crudas de syscall (int $0x80). */
long sxn_syscall1(long number, long a);
long sxn_syscall3(long number, long a, long b, long c);
long sxn_syscall5(long number, long a, long b, long c, long d, long e);

/* --- Identidad y log (syscalls propias del ABI nativo) --------------------- */

/* Llena `out` con la identidad del proceso y la version del ABI del kernel.
 * Devuelve 0 o -errno. El runtime debe verificar out->abi_version contra
 * SXN_ABI_VERSION al arrancar. */
long sxn_info(struct sxn_native_info *out);

/* Emite `message` en el log del kernel (prefijado con el pid). */
long sxn_log(const char *message);

/* Como sxn_log, con un valor numerico: "label=valor". */
long sxn_log_num(const char *label, long value);

/* --- Graficos (syscalls propias del ABI nativo, bloque 0x1010) -------------- */

/* Geometria del display. Devuelve 0 o -errno. */
long sxn_gfx_info(struct sxn_gfx_info *out);

/* Sesion exclusiva de display (un dueno por vez; se libera sola si el proceso
 * muere). Devuelven 0 o -errno (-EBUSY si esta tomada / no somos duenos). */
long sxn_gfx_acquire(void);
long sxn_gfx_release(void);

/* Presenta el rectangulo (x,y,w,h) de `frame`, un buffer con el mismo layout
 * que la pantalla (filas de `pitch` bytes, pixeles de 32 bits XRGB). */
long sxn_gfx_present(const void *frame, unsigned int pitch,
                     unsigned int x, unsigned int y,
                     unsigned int width, unsigned int height);

/* --- Info del sistema (sx_sysinfo.c; syscalls del baseline via posix) -------
 * Toma un snapshot con sxn_sys_refresh() y los getters leen del cache. Lo usa
 * el port de aboutapp. */
void sxn_sys_refresh(void);
const char *sxn_sys_version(void);
int sxn_sys_uptime_ms(void);
int sxn_sys_process_count(void);
int sxn_sys_memory_usable_mib(void);
int sxn_sys_memory_reclaimable_mib(void);
int sxn_sys_disk_used_mib(void);
int sxn_sys_disk_total_mib(void);
int sxn_sys_clock_valid(void);
int sxn_sys_clock_hour(void);
int sxn_sys_clock_minute(void);
int sxn_sys_clock_second(void);

/* --- Listado de directorios (sx_fs.c; open/readdir/stat via posix) ----------
 * Carga un directorio al cache (con ".." y ordenado) y expone las entradas por
 * indice. Lo usa el port de filesapp. */
int sxn_fs_load(const char *path); /* devuelve cantidad de entradas o -1 */
int sxn_fs_count(void);
const char *sxn_fs_name(int index);
int sxn_fs_is_dir(int index);
const char *sxn_fs_join(const char *base, const char *name);
const char *sxn_fs_parent(const char *path);
int sxn_fs_path_is_dir(const char *path);
int sxn_fs_is_launchable(const char *path); /* /bin/ o /disk/bin/ */
int sxn_fs_size(const char *path); /* bytes, o -1 */
/* Preview: lee el comienzo del archivo y lo parte en lineas saneadas. */
int sxn_fs_preview_load(const char *path); /* devuelve lineas o -1 */
int sxn_fs_preview_count(void);
const char *sxn_fs_preview_line(int index);

/* --- I/O y proceso (baseline transitorio sobre la tabla posix) ------------- */

long sxn_write(int fd, const char *buf, int len);
void sxn_exit(int code) __attribute__((noreturn));
void sxn_sleep_ms(long milliseconds);

/* Demo de la Fase 0: escribe un saludo por stdout. El string vive aca (en C)
 * para que el C++ generado por reflaxe.CPP no arrastre <string>/<iostream>. */
void sxn_hello(void);

/* --- Heap del runtime ------------------------------------------------------- */

/* Asignador del runtime nativo: arena BSS propia con free-list (first-fit,
 * split y coalescing). Es el respaldo de operator new/delete del C++ generado
 * y del mini <memory> freestanding. En la etapa HashLink, el GC de la VM
 * reemplaza esta capa sin tocar el ABI. */
void *sxn_alloc(unsigned long size);
void *sxn_realloc(void *ptr, unsigned long size);
void sxn_free(void *ptr);

/* --- Builtins de memoria (requeridos en freestanding) ----------------------- */

void *memcpy(void *destination, const void *source, unsigned long count);
void *memmove(void *destination, const void *source, unsigned long count);
void *memset(void *destination, int value, unsigned long count);
int memcmp(const void *left, const void *right, unsigned long count);

#ifdef __cplusplus
}
#endif

/* Cliente del compositor (apps ventaneadas bajo el escritorio). */
#include "savanxp_native_gui.h"
