#pragma once

#include <stddef.h>

#include "abi/savanxp_abi.h"

/*
 * File associations (docs/userland/SXE_FORMAT.md, phase 5).
 *
 * Resolves Open and Edit with the capability/policy split in the document:
 *
 *   - The BINARY declares a CAPABILITY, in the EXT_OPEN tag of its .sxmeta:
 *     "I can open .txt". Declaring it does not make it the one that opens them.
 *   - The REGISTRY resolves each ACTION: Settings/assoc.ini overrides the
 *     shipped /system/assoc.ini defaults independently for Open and Edit.
 *     Policy lives outside the executable.
 *
 * Precedence: user policy > /system/assoc.ini > the first binary declaring
 * the extension > nothing. Capabilities provide Open only; Edit is policy.
 *
 * BY EXTENSION ONLY AT THIS STAGE. The MIME_OPEN tag is still stamped and can
 * be read, but resolving by content type needs a detection layer -- extension
 * to mime, or sniffing -- that the system does not have yet. Baking a ".txt is
 * text/plain" table in here would bring back exactly the kind of central table
 * this design came to remove.
 *
 * No malloc: fixed capacities and truncation, as in progman_registry. It
 * makes no system call: reading and listing go through shell/shell_io.h.
 */

/* With the dot and in lower case: ".txt". Enough for what a .sxres declares. */
#define FILE_ASSOC_EXT_CAPACITY 16
#define FILE_ASSOC_PATH_CAPACITY SAVANXP_DESKTOP_LAUNCH_PATH_CAPACITY
#define FILE_ASSOC_MAX_ENTRIES 64

/* Policy lines: `.ext=/path/program` (Open), or `edit:.ext=/path/editor`.
 * `open:.ext` is also accepted; '#' and ';' start comments. */
#define FILE_ASSOC_POLICY_NAME "assoc.ini" /* in the profile: shell_io_settings_path */
#define FILE_ASSOC_POLICY_MAX_BYTES 4096
#define FILE_ASSOC_DEFAULTS_PATH "/system/assoc.ini"

/*
 * The first directory scanned for programs that declare extensions; /system
 * follows. The first declaration wins: without a declared tie-break rule, the stable
 * order of the scan is more predictable than any heuristic.
 */
#define FILE_ASSOC_SCAN_DIR "/bin"

struct file_assoc_entry
{
    char extension[FILE_ASSOC_EXT_CAPACITY];
    char program[FILE_ASSOC_PATH_CAPACITY];
    char editor[FILE_ASSOC_PATH_CAPACITY];
    /* Non-zero for an explicit user choice, independently for each action. */
    int from_policy;
    int edit_from_policy;
};

/* Load user policy, shipped defaults, then executable Open capabilities.
 * The caller keeps this in memory for its lifetime; there is no disk cache. */
int file_assoc_load(void);

/* Only the policy, without scanning. The scan is added on top. */
int file_assoc_load_policy(void);

/* Parses policy text alone: the selftest's entry point, which exercises the
 * format this way without depending on an assoc.ini being on the disk. */
int file_assoc_parse_policy(const char *text, size_t length);

/*
 * Scans the program directories and adds what they declare, without
 * overwriting what the policy already set. Returns how many associations it
 * added.
 *
 * `exists` can be injected for the same reason as in progman_registry: it
 * lets the scan be tested without depending on what is on the disk. 0 = use
 * the real one.
 */
typedef int (*file_assoc_path_exists_fn)(const char *path);
int file_assoc_scan_programs(file_assoc_path_exists_fn exists);

/*
 * The program associated with the file, or 0 when none claims it. The caller
 * decides what to do with the 0 -- filesapp falls back to its default editor
 * -- because "no association" is not an error.
 */
const char *file_assoc_program_for_file(const char *file_path);
/* Edit has no implicit fallback. A missing association returns NULL. */
const char *file_assoc_editor_for_file(const char *file_path);

/* A path's extension, with the dot and in lower case. Returns 0 when it has
 * none. Exposed because the caller usually wants to show it. */
const char *file_assoc_extension_of(const char *file_path, char *out, size_t capacity);

/* How many executables the last scan opened. It is the number to look at
 * before deciding whether a disk cache is needed. */
int file_assoc_scan_examined(void);

int file_assoc_count(void);
const struct file_assoc_entry *file_assoc_at(int index);

/* Checks parsing, precedence, truncation, bounds and the scan. 0 when all pass. */
int file_assoc_selftest(void);
