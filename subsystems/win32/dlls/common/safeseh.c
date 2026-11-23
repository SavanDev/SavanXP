/* The x86 PE load configuration points to the table built by the linker
 * from this image's .safeseh declarations. */
#include <windows.h>

extern const BYTE __safe_se_handler_table[];
extern const BYTE __safe_se_handler_count[];

const struct {
    DWORD size;
    DWORD reserved[15];
    const BYTE *handler_table;
    const BYTE *handler_count;
} _load_config_used = {72, {0}, __safe_se_handler_table, __safe_se_handler_count};
