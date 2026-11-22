#pragma once

/* Web Viewer's downloads (docs/desktop/WEBVIEWER.md#downloads): what the
 * browser saves instead of showing. A URL the page worker found to be a file
 * comes here with the name it suggests; the person confirms it in Save As;
 * the body streams on a worker of its own into <name>.part, renamed when it
 * is whole and deleted otherwise, while a modeless window shows how it goes.
 * One download at a time. Everything here runs on the UI thread except the
 * download worker. */

#include "sxn/sxngui.h"
#include "sxn/sxnhttp.h"

/* Once, before the loop: the window the dialogs and the download window
 * belong to. */
void webview_download_init(struct sxngui_app *app);

/* 1 while a download runs, or is still cleaning up after a cancel. */
int webview_download_busy(void);

/* Ask where to save `url` (absolute, http or https), suggesting
 * `suggested_name`, then download it through `dns`. 0, or -1 when a
 * download is already running or the dialog could not open. */
int webview_download_begin(const char *url, const char *suggested_name, const struct sxn_ipv4_endpoint *dns);

/* From the app's on_wake: take the worker's progress and result. */
void webview_download_wake(void);

/* The browser is closing: cancel what runs, wait for the worker to delete
 * its partial file, and stop it. */
void webview_download_shutdown(void);
