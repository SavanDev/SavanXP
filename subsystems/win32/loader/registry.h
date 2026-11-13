/*
 * registry.h -- HKEY_CURRENT_USER for the Win32 subsystem, kept by the host.
 *
 * Portable C, no library dependency. The whole store is in memory, loaded
 * from a small text file at start and written back whole after every change
 * (the platform does the file I/O). Names are UTF-8 here; the services
 * convert the guest's UTF-16. Keys and value names compare without case for
 * ASCII letters, as Windows compares them.
 *
 * Only HKEY_CURRENT_USER is backed. HKEY_CLASSES_ROOT and
 * HKEY_LOCAL_MACHINE exist and are empty: opening a subkey of either finds
 * nothing, and creating one is refused.
 *
 * The file (docs/userland/WIN32.md, "The registry"):
 *
 *     SavanXP registry 1
 *     [Software\Microsoft\Solitaire]
 *     "Opciones"=dword:00000035
 *     "Name"=hex(1):41,00,00,00
 *     @=hex(3):
 */
#ifndef SAVANXP_WIN32_REGISTRY_H
#define SAVANXP_WIN32_REGISTRY_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define SXW_REG_MAX_KEYS 64u
#define SXW_REG_MAX_VALUES 256u
#define SXW_REG_MAX_PATH 256u /* bytes of a key's path, with its NUL */
#define SXW_REG_MAX_NAME 128u /* bytes of a value's name, with its NUL */
#define SXW_REG_DATA_BYTES 16384u
#define SXW_REG_MAX_HANDLES 32u

#define SXW_REG_SZ 1u
#define SXW_REG_BINARY 3u
#define SXW_REG_DWORD 4u

#define SXW_REG_CREATED_NEW_KEY 1u
#define SXW_REG_OPENED_EXISTING_KEY 2u

typedef struct sxw_reg_key {
    uint8_t used;
    char path[SXW_REG_MAX_PATH]; /* under HKEY_CURRENT_USER, '\\' separated */
} sxw_reg_key;

typedef struct sxw_reg_value {
    uint8_t used;
    uint8_t key;
    uint32_t type;
    uint32_t offset;
    uint32_t size;
    char name[SXW_REG_MAX_NAME]; /* "" is the key's default value */
} sxw_reg_value;

typedef struct sxw_registry {
    sxw_reg_key keys[SXW_REG_MAX_KEYS];
    sxw_reg_value values[SXW_REG_MAX_VALUES];
    uint8_t data[SXW_REG_DATA_BYTES];
    uint32_t data_used;
    /* Open handles: key index + 1, or one of the predefined roots' markers. */
    uint32_t handles[SXW_REG_MAX_HANDLES];
    int dirty; /* changed since the last serialize */
} sxw_registry;

void sxw_reg_init(sxw_registry *reg);

/* Loads a file's contents. 0, or -1 for a malformed file (the store is then
 * left empty, and the next change replaces the file). */
int sxw_reg_parse(sxw_registry *reg, const char *text, size_t length);

/* Writes the store as text. 0, or -1 when capacity is too small. */
int sxw_reg_serialize(const sxw_registry *reg, char *out, size_t capacity, size_t *length);

/* Each returns a Win32 error code (0 on success). */
uint32_t sxw_reg_open(sxw_registry *reg, uint32_t hkey, const char *subkey, int create,
                      uint32_t *result, uint32_t *disposition);
uint32_t sxw_reg_close(sxw_registry *reg, uint32_t hkey);
/* With data NULL only *size_out is set. *size_out is always the value's
 * size, also with ERROR_MORE_DATA. */
uint32_t sxw_reg_query(const sxw_registry *reg, uint32_t hkey, const char *name, uint32_t *type,
                       uint8_t *data, uint32_t capacity, uint32_t *size_out);
uint32_t sxw_reg_set(sxw_registry *reg, uint32_t hkey, const char *name, uint32_t type,
                     const uint8_t *data, uint32_t size);

#ifdef __cplusplus
}
#endif

#endif
