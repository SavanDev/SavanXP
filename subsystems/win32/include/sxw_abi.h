/*
 * sxw_abi.h -- The private contract between winhost (or the host harness)
 * and the subsystem's own 32-bit DLLs.
 *
 * It is not a public ABI and not the NT system call table: winhost and the
 * DLLs are built and installed together, and ntdll refuses a host whose
 * SXW_ABI_VERSION differs. Every field is a fixed-width integer or a guest
 * address below 4 GiB. See docs/userland/WIN32.md, "The gate".
 */
#ifndef SAVANXP_WIN32_SXW_ABI_H
#define SAVANXP_WIN32_SXW_ABI_H

#define SXW_ABI_VERSION 11u

/* ---- Thread environment block (32-bit layout, the fields we fill) ------- */

#define SXW_TEB_EXCEPTION_LIST 0x00u /* 0xffffffff: no handler frame */
#define SXW_TEB_STACK_BASE 0x04u
#define SXW_TEB_STACK_LIMIT 0x08u
#define SXW_TEB_SELF 0x18u
#define SXW_TEB_PROCESS_ID 0x20u
#define SXW_TEB_THREAD_ID 0x24u
#define SXW_TEB_TLS_POINTER 0x2cu /* ThreadLocalStoragePointer: array of TLS blocks */
#define SXW_TEB_PEB 0x30u
#define SXW_TEB_LAST_ERROR 0x34u
/* WOW32Reserved in Windows' own TEB: the 32 -> 64 transition. Here it is the
 * guest-callable address of the service gate. */
#define SXW_TEB_GATE 0xc0u
/* Win32ThreadInfo: user32's per-thread state (its message queue), 0 until
 * the thread first needs one. */
#define SXW_TEB_WIN32_THREAD 0x40u
/* The host's index of the thread, which the gate finds its state by. Not a
 * field of Windows' TEB: past every one Windows XP has. */
#define SXW_TEB_HOST_THREAD 0xff0u
#define SXW_TEB_SIZE 0x1000u

/* ---- Process environment block ------------------------------------------ */

#define SXW_PEB_IMAGE_BASE 0x08u
#define SXW_PEB_PROCESS_PARAMETERS 0x10u
#define SXW_PEB_SIZE 0x1000u

/* RTL_USER_PROCESS_PARAMETERS, 32-bit layout, the fields we fill. The host
 * puts it in the PEB's page at SXW_PEB_PARAMETERS_OFFSET, its strings after
 * it. A UNICODE_STRING is {uint16 Length, uint16 MaximumLength, uint32
 * Buffer}, lengths in bytes without the terminator. */
#define SXW_PEB_PARAMETERS_OFFSET 0x400u
#define SXW_PARAMS_STD_INPUT 0x18u
#define SXW_PARAMS_STD_OUTPUT 0x1cu
#define SXW_PARAMS_STD_ERROR 0x20u
#define SXW_PARAMS_IMAGE_PATH 0x38u
#define SXW_PARAMS_COMMAND_LINE 0x40u
#define SXW_PARAMS_SIZE 0x90u

/* ---- Services ------------------------------------------------------------ */
/*
 * The guest calls the gate as
 *     uint32_t __stdcall gate(uint32_t service, const uint32_t *arguments);
 * The arguments are a flat array of 32-bit values in guest memory. The host
 * checks every guest address before using it and never calls guest code.
 */

#define SXW_SVC_VERSION 0u       /* () -> SXW_ABI_VERSION */
#define SXW_SVC_EXIT_PROCESS 1u  /* (status) -> does not return */
#define SXW_SVC_STD_HANDLE 2u    /* (which: 0 in, 1 out, 2 err) -> handle or 0 */
#define SXW_SVC_WRITE 3u         /* (handle, buffer, length) -> bytes written, or SXW_FAILED */
#define SXW_SVC_DEBUG_OUTPUT 4u  /* (buffer, length) -> 0 */
/* () -> seconds since 1970 UTC, as a 32-bit time_t; a test run may fix the
 * start time (winhost --time). */
#define SXW_SVC_TIME 5u
/* (size) -> address of a new zeroed read-write region of at least size
 * bytes, or 0. The heap's arenas. */
#define SXW_SVC_ALLOCATE 6u
/* (name) -> base of the loaded module with that file name (an ANSI string,
 * any case, a path allowed), or 0. */
#define SXW_SVC_FIND_MODULE 7u
/* (name, modules, capacity, error) -> base, or 0 with a Win32 error code in
 * *error. Loads the DLL and what it needs; the modules loaded by this call
 * are written to modules[capacity] (sxw_start_module, initialization order)
 * and their count is the low half of *error on success. */
#define SXW_SVC_LOAD_LIBRARY 8u
/* (base, name or ordinal, modules, capacity, error) -> address, or 0 with a
 * Win32 error in *error. A name below 0x10000 is an ordinal. A forwarder may
 * load DLLs, reported as SXW_SVC_LOAD_LIBRARY reports them. */
#define SXW_SVC_GET_PROC 9u
/* Registry. Keys are handles from the host; the predefined ones are
 * Windows' (SXW_HKEY_*). Strings are UTF-16. Each returns a Win32 error. */
#define SXW_SVC_REG_OPEN 10u  /* (hkey, subkey, create, out hkey, out disposition) */
#define SXW_SVC_REG_CLOSE 11u /* (hkey) */
#define SXW_SVC_REG_QUERY 12u /* (hkey, name, out type, data, in/out size) */
#define SXW_SVC_REG_SET 13u   /* (hkey, name, type, data, size) */

/* Windows and input (W3). A window here is a top-level window of the host
 * (a windowd window on SavanXP, an offscreen surface in the harness); its
 * frame and caption are the host's. The guest owns the pixels: user32 keeps
 * one buffer per window, 0x00RRGGBB per pixel, and presents rectangles of it.
 * Child windows, the message queue and the rest of the window manager are
 * user32's. */
/* () -> milliseconds since the process started; GetTickCount and the
 * timers. A test run may drive it (the harness's input script). */
#define SXW_SVC_TICKS 14u
/* (out size[2]) -> 0: the screen's width and height in pixels. */
#define SXW_SVC_DISPLAY 15u
/* (title, width, height, out size[2], owner, kind, x, y) -> a window id, or
 * 0. Asks for a client area of width x height; out size is the one the host
 * gave, which a window manager may clip. The title is UTF-16. kind is one of
 * SXW_WINDOW_*; owner is a window id, 0 for SXW_WINDOW_MAIN. */
#define SXW_SVC_WINDOW_CREATE 16u
/* The program's own top-level window, framed. */
#define SXW_WINDOW_MAIN 0u
/* Framed, above its owner, placed by the host (a dialog). */
#define SXW_WINDOW_OWNED 1u
/* Frameless, at (x, y) of its owner's client area (a menu). It never takes
 * the keyboard: keys go to the owner. A press outside every window of the
 * program closes it with SXW_EVENT_CLOSE; one on another of its windows
 * goes to that window. */
#define SXW_WINDOW_POPUP 2u
/* Framed, above its owner, placed by the host, with its owner still usable:
 * a modeless dialog (CreateDialog's). */
#define SXW_WINDOW_MODELESS 3u
/* (window) -> 0 */
#define SXW_SVC_WINDOW_DESTROY 17u
/* (window, pixels, stride, size[2], rects, count) -> 0, or SXW_FAILED.
 * Copies count RECTs (left, top, right, bottom) of the guest's buffer
 * (stride bytes per row, size[0] x size[1]) to the window and shows them. */
#define SXW_SVC_WINDOW_PRESENT 18u
/* (window, title) -> 0, or SXW_FAILED */
#define SXW_SVC_WINDOW_TITLE 19u
/* (timeout, out event) -> 1 with an event, 0 when the timeout passed, 2
 * when another thread woke this one (SXW_SVC_WAKE_THREAD). timeout is in
 * milliseconds, SXW_INFINITE to wait for an event, 0 to poll. out points at
 * an sxw_event. Only the events of the calling thread's windows come. */
#define SXW_SVC_WAIT_EVENT 20u
/* (window, pixels) -> 0, or SXW_FAILED: the window's 16x16 icon, 256 words
 * 0xAARRGGBB, top row first, for its caption and its taskbar button. */
#define SXW_SVC_WINDOW_ICON 21u
#define SXW_ICON_EXTENT 16u
/* (base, buffer, capacity) -> units written: the Windows path of the module
 * loaded at base (C:\... or Z:\...), UTF-16 and NUL-terminated in
 * buffer[capacity] units, truncated to fit; 0 when nothing is loaded there.
 * GetModuleFileName. */
#define SXW_SVC_MODULE_PATH 22u
/* (handler) -> 1 if the address is an executable image's allowed x86 SEH
 * handler, or 0. The guest calls the handler itself. */
#define SXW_SVC_SEH_HANDLER 23u

/* Handles (WP4 of WIN32_WORDPAD_ROADMAP.md). A handle is the host's: a file,
 * a directory search, and with threads the synchronization objects. Each
 * service returns a Win32 error, 0 on success. Paths are full Windows paths
 * in UTF-16 (C:\... under the host's drive_c, Z:\... from the host's root),
 * looked up without case, as kernel32's GetFullPathNameW makes them. */
/* (handle) */
#define SXW_SVC_CLOSE 24u
/* (handle, out handle): a second handle to the same object, which shares a
 * file's position. */
#define SXW_SVC_DUPLICATE 25u
/* (path, access, share, disposition, out[2]): CreateFileW's access
 * (GENERIC_READ, GENERIC_WRITE), share mode and disposition (CREATE_NEW to
 * TRUNCATE_EXISTING). out[0] is the handle, out[1] 1 when the file existed
 * before. */
#define SXW_SVC_FILE_OPEN 26u
/* (handle, buffer, count, out done): at the handle's position, which moves. */
#define SXW_SVC_FILE_READ 27u
#define SXW_SVC_FILE_WRITE 28u
/* (handle, low, high, origin, out[2]): FILE_BEGIN, FILE_CURRENT or FILE_END;
 * out is the new position, low then high. */
#define SXW_SVC_FILE_SEEK 29u
/* (handle): the file ends at the handle's position. */
#define SXW_SVC_FILE_SET_END 30u
/* (handle, out sxw_file_entry without its name) */
#define SXW_SVC_FILE_INFO 31u
/* (handle) */
#define SXW_SVC_FILE_FLUSH 32u
/* (path, out sxw_file_entry without its name) */
#define SXW_SVC_PATH_INFO 33u
/* (action, path, second path): SXW_PATH_* */
#define SXW_SVC_PATH_ACTION 34u
#define SXW_PATH_DELETE_FILE 1u
#define SXW_PATH_MAKE_DIRECTORY 2u
#define SXW_PATH_REMOVE_DIRECTORY 3u
#define SXW_PATH_MOVE 4u /* the second path must not exist */
/* (directory path, mask, out handle, out sxw_file_entry): the first entry of
 * the directory whose name matches the mask (* and ?, without case), in
 * NTFS's order, "." and ".." first in any directory but a drive's root. */
#define SXW_SVC_FIND_FIRST 35u
/* (handle, out sxw_file_entry): the next one; ERROR_NO_MORE_FILES at the end. */
#define SXW_SVC_FIND_NEXT 36u
/* (base) -> 0, or SXW_FAILED: gives back a region SXW_SVC_ALLOCATE made (a
 * destroyed heap's arena). */
#define SXW_SVC_FREE 37u

/* Threads and the objects they wait on (WP3). Guest threads are host
 * threads that take turns: one runs guest code at a time, and it lets
 * another run when it waits, sleeps, or calls the host while another
 * thread is ready (docs/userland/WIN32.md, "Threads"). Each returns a Win32
 * error. The current thread's pseudo-handle, (HANDLE)-2, names it. */
/* (start, parameter, stack bytes, flags, out[2]): a thread whose entry is
 * ntdll!SxwThreadStart(start, parameter), suspended with CREATE_SUSPENDED;
 * out[0] its handle, out[1] its id. */
#define SXW_SVC_THREAD_CREATE 38u
/* (exit code): the calling thread ends; does not return. */
#define SXW_SVC_THREAD_EXIT 39u
/* (thread, resume, out previous count): SuspendThread (resume 0) or
 * ResumeThread (1). A running thread stops at its next call to the host. */
#define SXW_SVC_THREAD_SUSPEND 40u
/* (thread, out[3]): its exit code (STILL_ACTIVE while it runs), its
 * priority and its id. */
#define SXW_SVC_THREAD_QUERY 41u
/* (thread, priority): kept and answered; the host does not schedule by it. */
#define SXW_SVC_THREAD_PRIORITY 42u
/* (kind, a, b, name, out[2]): SXW_OBJECT_EVENT (a manual reset, b
 * signaled), SXW_OBJECT_MUTEX (a owned by the caller), SXW_OBJECT_SEMAPHORE
 * (a count, b maximum). name is UTF-16 or 0; out[0] the handle, out[1] 1
 * when a named object of the kind already existed. */
#define SXW_SVC_OBJECT_CREATE 43u
#define SXW_OBJECT_EVENT 3u
#define SXW_OBJECT_MUTEX 4u
#define SXW_OBJECT_SEMAPHORE 5u
#define SXW_OBJECT_THREAD 6u
/* (handle, operation, count, out previous): SXW_SIGNAL_*. */
#define SXW_SVC_OBJECT_SIGNAL 44u
#define SXW_SIGNAL_SET 1u     /* SetEvent */
#define SXW_SIGNAL_RESET 2u   /* ResetEvent */
#define SXW_SIGNAL_PULSE 3u   /* PulseEvent */
#define SXW_SIGNAL_RELEASE 4u /* ReleaseMutex, ReleaseSemaphore(count) */
/* (handles, count, all, milliseconds, out result): WaitForMultipleObjects;
 * result is WAIT_OBJECT_0 + i, WAIT_ABANDONED_0 + i or WAIT_TIMEOUT. */
#define SXW_SVC_WAIT 45u
/* (milliseconds): Sleep. 0 lets a ready thread run. */
#define SXW_SVC_SLEEP 46u
/* (thread id): wakes that thread from SXW_SVC_WAIT_EVENT, which returns 2:
 * user32 posted or sent it a message. */
#define SXW_SVC_WAKE_THREAD 47u
/* (): every other thread stops for good where it is; ExitProcess calls it
 * before the DLLs detach, as Windows XP ends them first. */
#define SXW_SVC_END_OTHER_THREADS 48u

/* The clipboard (WP5, docs/desktop/CLIPBOARD.md): the host's set of named
 * formats, which user32 maps Win32's formats onto. */
/* (entries, count) -> 0, or SXW_FAILED: replaces the whole set with count
 * (at most SXW_CLIPBOARD_FORMATS) sxw_clip_entry; count 0 empties it. */
#define SXW_SVC_CLIPBOARD_SET 49u
/* (out info[3]) -> 0, or SXW_FAILED: the sequence's low and high words,
 * which rise with every change, and how many formats the set has. */
#define SXW_SVC_CLIPBOARD_INFO 50u
/* (index, out name[SXW_CLIPBOARD_NAME], out length) -> 0, or SXW_FAILED
 * past the last: the format at index, in the order it was given. */
#define SXW_SVC_CLIPBOARD_FORMAT 51u
/* (name, buffer, capacity, out length) -> bytes copied, or SXW_FAILED when
 * the set has no such format; *length is its whole length. */
#define SXW_SVC_CLIPBOARD_GET 52u
/* (windows path, out buffer, capacity) -> a Win32 error: the host path, UTF-8
 * and NUL-terminated, of a full Windows path whose last part may not exist
 * (a file list on the clipboard, a drop). */
#define SXW_SVC_PATH_TO_HOST 53u
/* (host path, out buffer, capacity in units) -> units written, or 0: the
 * Windows path of a host path, C: for what is under drive_c, Z: for the
 * rest; UTF-16, NUL-terminated. */
#define SXW_SVC_PATH_FROM_HOST 54u
/* Drag and drop (docs/desktop/DRAG_AND_DROP.md). (window, effect) -> 0, or
 * SXW_FAILED: the answer to the last SXW_EVENT_DRAG enter or move the window
 * had, the SXW_DRAG_EFFECT_* it would take there or 0. */
#define SXW_SVC_DRAG_REPLY 55u
/* The drag's set of formats, as SXW_SVC_CLIPBOARD_FORMAT and _GET read the
 * clipboard's: (index, out name, out length) and (name, buffer, capacity,
 * out length). */
#define SXW_SVC_DRAG_FORMAT 56u
#define SXW_SVC_DRAG_GET 57u
/* Fonts (WP6, docs/userland/WIN32.md, "Text"): the host's TrueType faces,
 * drawn by libsxfont. gdi32 keeps its 13 px strike for the faces the
 * interface asks for and takes every other one from here. A font is a host
 * id, 0 for none. */
/* (request sxw_font_request, out sxw_font_metrics) -> font id, or 0 when the
 * host has no face at all. The face is the nearest the host has: the one
 * named, a metric-compatible one (Arial -> Liberation Sans), or the default
 * for the pitch and family asked for. */
#define SXW_SVC_FONT_OPEN 58u
/* (font) -> 0 */
#define SXW_SVC_FONT_CLOSE 59u
/* (font, UTF-16 units, count, out int32[count]) -> 0, or SXW_FAILED: each
 * unit's advance in pixels, synthetic bold included. */
#define SXW_SVC_FONT_ADVANCES 60u
/* (font, codepoint, out sxw_font_glyph, buffer, capacity) -> 0, or
 * SXW_FAILED. The glyph's coverage, width x height bytes from the top row,
 * 0 to 255 (only 0 and 255 for a 1-bit font), when it fits in capacity;
 * out.size says how many bytes it needs. */
#define SXW_SVC_FONT_GLYPH 61u
/* (index, out sxw_font_face_entry) -> 0, or SXW_FAILED past the last: the
 * host's faces, a family's styles together, regular first. */
#define SXW_SVC_FONT_FACE 62u
#define SXW_SVC_COUNT 63u

#define SXW_FONT_FACE_UNITS 32u /* LOGFONT's lfFaceName */
#define SXW_FONT_FULL_UNITS 64u /* ENUMLOGFONTEX's elfFullName */

/* What CreateFontIndirectW asked for; heights as LOGFONT's. */
typedef struct sxw_font_request {
    int height;
    int width;
    unsigned int weight;
    unsigned int italic;
    unsigned int charset;
    unsigned int pitch_family;
    unsigned int quality;
    unsigned short face[SXW_FONT_FACE_UNITS];
} sxw_font_request;

/* A font at its size, in pixels, as GetTextMetricsW and
 * GetOutlineTextMetricsW report it (names UTF-16, NUL-terminated). */
typedef struct sxw_font_metrics {
    int height, ascent, descent, internal_leading, external_leading;
    int average_width, maximum_width;
    unsigned int weight;
    unsigned int first_char, last_char, default_char, break_char;
    unsigned int italic;       /* 255 or 0, as TEXTMETRIC's */
    unsigned int pitch_family; /* tmPitchAndFamily: TMPF_* | FF_* */
    unsigned int charset;
    unsigned int ppem;
    unsigned int em_square;
    int typo_ascent, typo_descent, typo_line_gap;
    int cap_height, x_height;
    int italic_angle;          /* tenths of a degree */
    int slope_rise, slope_run;
    int underscore_size, underscore_position;
    int strikeout_size, strikeout_position;
    unsigned int fs_selection, fs_type;
    int mac_ascent, mac_descent, mac_line_gap;
    unsigned int minimum_ppem;
    int box[4];                /* left, top, right, bottom */
    unsigned char panose[12];  /* 10 used */
    unsigned int code_pages[2];
    unsigned int smooth;       /* drawn grey where the face asks */
    unsigned short family[SXW_FONT_FACE_UNITS];
    unsigned short style[SXW_FONT_FACE_UNITS];
    unsigned short full[SXW_FONT_FULL_UNITS];
} sxw_font_metrics;

/* One glyph as drawn: from the pen, left and up to its top row. */
typedef struct sxw_font_glyph {
    int left, top;
    unsigned int width, height;
    int advance;
    unsigned int size; /* width * height */
} sxw_font_glyph;

/* One face of the host, for EnumFontFamiliesExW: its names and what the
 * enumeration's TEXTMETRIC says, at an em of 32 pixels as Windows XP gives
 * it there. */
typedef struct sxw_font_face_entry {
    unsigned short family[SXW_FONT_FACE_UNITS];
    unsigned short style[SXW_FONT_FACE_UNITS];
    unsigned short full[SXW_FONT_FULL_UNITS];
    unsigned int weight;
    unsigned int italic;       /* 255 or 0 */
    unsigned int pitch_family; /* tmPitchAndFamily */
    unsigned int lf_pitch_family; /* lfPitchAndFamily */
    int height, ascent, descent, internal_leading, external_leading;
    int average_width, maximum_width;
    int lf_width;              /* the LOGFONT's lfWidth: the average at an em of 32 */
    unsigned int first_char, last_char;
    unsigned int em_square, cell_height, average_units;
    unsigned int code_pages[2];
    unsigned int unicode_ranges[4];
    unsigned int ntm_flags;    /* what the file adds to ntmFlags: NTM_DSIG, NTM_TT_OPENTYPE */
} sxw_font_face_entry;

#define SXW_CLIPBOARD_FORMATS 8u
#define SXW_CLIPBOARD_NAME 64u /* NUL included */
#define SXW_CLIPBOARD_CAPACITY (1024u * 1024u)

typedef struct sxw_clip_entry {
    unsigned int name;   /* guest address of an ASCII name */
    unsigned int data;   /* guest address of its bytes */
    unsigned int length;
} sxw_clip_entry;

#define SXW_WAIT_OBJECT_0 0u
#define SXW_WAIT_ABANDONED_0 0x80u
#define SXW_WAIT_TIMEOUT 258u
#define SXW_STILL_ACTIVE 259u
#define SXW_CURRENT_THREAD 0xfffffffeu
#define SXW_CURRENT_PROCESS 0xffffffffu

/* What a file query or a search returns: FILE_ATTRIBUTE_* (0x10 a directory,
 * 0x20 a file, 0x01 one that cannot be written), the size, the last write
 * in seconds since 1970 UTC (the hosts keep no other time), and the name,
 * UTF-16 and NUL-terminated, for a search. */
#define SXW_FILE_NAME_UNITS 260u
typedef struct sxw_file_entry {
    unsigned int attributes;
    unsigned int size_low;
    unsigned int size_high;
    unsigned int mtime;
    unsigned short name[SXW_FILE_NAME_UNITS];
} sxw_file_entry;
#define SXW_FILE_ENTRY_HEADER 16u

#define SXW_INFINITE 0xffffffffu

/* Events from the host, in client coordinates of the window they name. */
#define SXW_EVENT_POINTER 1u /* a = x, b = y, c = buttons (SXW_BUTTON_*) */
#define SXW_EVENT_KEY 2u     /* a = virtual key, b = 1 down / 0 up, c = UTF-16 character or 0 */
#define SXW_EVENT_RESIZE 3u  /* a = client width, b = client height */
#define SXW_EVENT_CLOSE 4u   /* the frame's close button, or its shortcut */
/* A drag over the window: a = x, b = y, c = SXW_DRAG_* phase in the low byte,
 * the effects the source allows in the next and, for a drop, the effect in
 * the third. */
#define SXW_EVENT_DRAG 5u

#define SXW_DRAG_ENTER 1u
#define SXW_DRAG_OVER 2u
#define SXW_DRAG_LEAVE 3u
#define SXW_DRAG_DROP 4u
#define SXW_DRAG_EFFECT_COPY 1u
#define SXW_DRAG_EFFECT_MOVE 2u
#define SXW_DRAG_EFFECT_LINK 4u

#define SXW_BUTTON_LEFT 1u
#define SXW_BUTTON_RIGHT 2u
#define SXW_BUTTON_MIDDLE 4u

typedef struct sxw_event {
    unsigned int kind;
    unsigned int window;
    unsigned int a;
    unsigned int b;
    unsigned int c;
    unsigned int time; /* SXW_SVC_TICKS when it happened */
} sxw_event;

#define SXW_FAILED 0xffffffffu

#define SXW_HKEY_CLASSES_ROOT 0x80000000u
#define SXW_HKEY_CURRENT_USER 0x80000001u
#define SXW_HKEY_LOCAL_MACHINE 0x80000002u

/* Win32 error codes the services return. */
#define SXW_ERROR_SUCCESS 0u
#define SXW_ERROR_FILE_NOT_FOUND 2u
#define SXW_ERROR_ACCESS_DENIED 5u
#define SXW_ERROR_PATH_NOT_FOUND 3u
#define SXW_ERROR_TOO_MANY_OPEN_FILES 4u
#define SXW_ERROR_INVALID_HANDLE 6u
#define SXW_ERROR_NOT_ENOUGH_MEMORY 8u
#define SXW_ERROR_NO_MORE_FILES 18u
#define SXW_ERROR_WRITE_FAULT 29u
#define SXW_ERROR_READ_FAULT 30u
#define SXW_ERROR_SHARING_VIOLATION 32u
#define SXW_ERROR_HANDLE_DISK_FULL 39u
#define SXW_ERROR_FILE_EXISTS 80u
#define SXW_ERROR_INVALID_PARAMETER 87u
#define SXW_ERROR_NEGATIVE_SEEK 131u
#define SXW_ERROR_DIR_NOT_EMPTY 145u
#define SXW_ERROR_BUSY 170u
#define SXW_ERROR_ALREADY_EXISTS 183u
#define SXW_ERROR_DIRECTORY 267u
#define SXW_ERROR_NOT_OWNER 288u
#define SXW_ERROR_TOO_MANY_POSTS 298u
#define SXW_ERROR_INVALID_THREAD_ID 1444u
#define SXW_ERROR_MOD_NOT_FOUND 126u
#define SXW_ERROR_PROC_NOT_FOUND 127u
#define SXW_ERROR_INVALID_ORDINAL 182u
#define SXW_ERROR_BAD_EXE_FORMAT 193u
#define SXW_ERROR_MORE_DATA 234u
#define SXW_ERROR_DLL_INIT_FAILED 1114u
#define SXW_ERROR_REGISTRY_IO_FAILED 1016u

/* ---- Process start ------------------------------------------------------- */
/*
 * The host loads every image, binds the imports and prepares TLS, then
 * enters the guest once, at ntdll's exported SxwProcessStart, with the
 * address of a start block. ntdll runs each module's TLS callbacks and
 * DllMain in the block's order (dependencies first), then the executable's
 * TLS callbacks and its entry point, and exits with the entry's result.
 *
 * Exit goes through ntdll's RtlExitUserProcess, which detaches in the order
 * Windows XP does: each DLL in reverse initialization order (its TLS
 * callbacks, then DllMain), then the executable's TLS callbacks.
 */

#define SXW_START_DLL 1u /* the module is a DLL: call DllMain */

typedef struct sxw_start_module {
    unsigned int base;          /* the module's load address */
    unsigned int entry;         /* entry point VA, or 0 */
    unsigned int tls_callbacks; /* VA of a NUL-terminated array of VAs, or 0 */
    unsigned int flags;         /* SXW_START_DLL */
} sxw_start_module;

typedef struct sxw_start_block {
    unsigned int version;       /* SXW_ABI_VERSION */
    unsigned int count;         /* modules, the executable last */
    sxw_start_module modules[1];
} sxw_start_block;

/* DllMain reasons, as Windows defines them. */
#define SXW_DLL_PROCESS_DETACH 0u
#define SXW_DLL_PROCESS_ATTACH 1u
#define SXW_DLL_THREAD_ATTACH 2u
#define SXW_DLL_THREAD_DETACH 3u

/* NT status values a process ends with when it cannot start, the same ones
 * Windows XP reports for the same failure. */
#define SXW_STATUS_NO_MEMORY 0xc0000017u
#define SXW_STATUS_INVALID_IMAGE_FORMAT 0xc000007bu
#define SXW_STATUS_DLL_NOT_FOUND 0xc0000135u
#define SXW_STATUS_ORDINAL_NOT_FOUND 0xc0000138u
#define SXW_STATUS_ENTRYPOINT_NOT_FOUND 0xc0000139u
#define SXW_STATUS_DLL_INIT_FAILED 0xc0000142u

#endif
