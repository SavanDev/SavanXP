# The file dialog

`include/sxn/sxnfiledlg.h`, built from `system/runtime/sxnfiledlg.c` into
every SUN program (the linker drops it from the ones that do not call it).
It is SavanXP's `GetOpenFileName` and `GetSaveFileName`: one Open dialog and
one Save As dialog, shaped like Windows 2000's, so that a program asking for
a file does not draw its own. Log Viewer's **File > Save Log...** is its
first consumer ([LOG_VIEWER.md](../userland/LOG_VIEWER.md)).

## The window

- **Save in / Look in**: the folder shown, as a path, with an Up One Level
  button beside it (the toolbar glyph Files uses, from
  `system/gfx/toolbar_glyphs/`). Up is greyed at `/`.
- **The list**: Name, Size and Type, folders first and then by name with
  case folded, each with its icon by type (`sxgfx/mime_icon.h`, the mapping
  in `/mimeicon.ini`). Folders are always listed; files only when they match
  the filter. A folder holds up to 512 entries in the list; past that the
  dialog says only the first 512 are listed.
- **File name**: starts with the name the program suggests. Picking a file
  in the list copies its name here; picking a folder does not.
- **Save as type / Files of type**: the program's one filter, shown, not
  chosen (below).
- **Save / Open** is the default button, **Cancel** next to it. A line at
  the bottom says why something did not happen: a folder that cannot be
  opened, a file that does not exist, a name with `*` or `?`.

## What the buttons do

- Double click or Enter on a folder opens it; so does Save/Open with the
  field empty and a folder selected.
- A name typed in the field is relative to the folder shown, or absolute;
  `..` works. A name that is a folder opens that folder and empties the
  field, as in Windows.
- **Save**: a name without an extension gets the program's default one
  (`log` becomes `log.txt`). The folder it goes in has to exist; the dialog
  creates no folders. A name that exists asks *Confirm Save As: ... already
  exists. Do you want to replace it?*, with **No** as the default, as every
  confirmation in the system. No brings the Save As dialog back as it was.
- **Open**: the file has to exist and be a file.
- Esc, Cancel or the close button cancel: the program is told with a NULL
  path.

The dialog only names the file. Writing or reading it is the program's,
with the native calls (`sxn_create_file`, `sxn_write`, `sxn_flush_file`;
`sxn_open_file`, `sxn_read`), and the permission checks are the kernel's at
that point.

## The API

```c
struct sxn_file_dialog_request request = {0};
request.mode = SXN_FILE_DIALOG_SAVE;
request.initial_directory = "/users/user";
request.file_name = "system-log.txt";
request.filter_description = "Text files (*.txt; *.log)";
request.filter_patterns = "*.txt;*.log";
request.default_extension = "txt";
request.on_done = on_save_chosen;   /* (app, path or NULL, user) */
sxn_file_dialog_begin(&app, &request);
```

The toolkit's dialogs do not block the program's loop, so neither does this
one: `on_done` is called once, later, with the absolute path or NULL. The
strings of the request are copied; the path handed back lives until the next
dialog begins. A folder that cannot be listed falls back to the folder the
last dialog of the process ended in, then `/`, and the bottom line says why.

Patterns are globs separated by `;`, with `*` and `?`, compared with case
folded. `*.*` (the default) lists every file, with an extension or not, as
in Windows.

## How it is built

- **Listing** goes through the native calls VFS-A added for it:
  `sxn_open_directory` and `sxn_read_directory` for the entries, with each
  entry's type and size, and `sxn_query_path` for what a typed name is
  ([KERNEL_NATIVE_API.md](../kernel/KERNEL_NATIVE_API.md#listing-directories-and-streaming-writes)).
- **Icons by type** are the same `mime_icon` Files and the desktop use,
  moved to `system/gfx/` so a SUN program links it too. It makes no system
  call itself: each subsystem installs a file reader (`mime_icon_set_reader`).
  The dialog reads the system's `/mimeicon.ini`; POSIX programs keep reading
  the profile's copy through `mime_icon_load`.
- **One dialog at a time.** The toolkit has one modal dialog per window, so
  the Confirm Save As question is a second dialog that replaces the first,
  and No begins the first again with its state.
- **Knowing it closed.** The toolkit ends a dialog on Esc or its close button
  without telling the program. The SUN frame's `sxngui_app_watch_dialog`
  calls back once the watched dialog is no longer the active one, however it
  went; the file dialog uses it to report a cancel. It lives in the
  statically linked frame, not in `libsxgui`, whose structures every POSIX
  program shares.

## Limits

- **One filter.** The toolkit does not support a combobox inside a dialog,
  so there is no type list to pick from; the filter is shown in a sunken
  field. A program with several types passes them as one pattern list.
- **No New Folder, rename or delete** in the dialog: SUN has no native
  `mkdir`, rename or unlink yet. Each arrives with a consumer of its own.
- **No icon view and no Places bar** (the Windows XP left column).
- **Executables get the default icon**: the dialog does not tell a program
  from a document, which Files does by reading the file's header.
- **The POSIX programs cannot use it.** Notepad, Paint and Display
  Properties keep their own path box until each moves to SUN, in a commit
  of its own.
