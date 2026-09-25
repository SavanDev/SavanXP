#pragma once

/* The FFmpeg backend for SxMedia, and the only symbol the Media Player needs
 * from it.
 *
 * A program does not link against this: it calls `sxmedia_ffmpeg_register()`
 * once at startup, before opening anything, and from then on talks to the
 * engine. That one call is the whole relationship between the program and the
 * library that decodes for it, and it is why the window never names FFmpeg.
 *
 * See savanxp/sxmedia.h for the vtable, and docs/SXMEDIA.md for why the roles
 * are separable.
 */

#ifdef __cplusplus
extern "C" {
#endif

/* Registers the FFmpeg backend: a source provider for every container the port
 * enables, a decoder provider for every stream FFmpeg can open, and swscale /
 * swresample as the converters. Returns 0, or a negative errno.
 *
 * Call it once, before `sx_media_open`. A second call fails with EEXIST rather
 * than replacing the entry, because the registry is a link-time fact and two
 * registrations of the same library would be a bug, not a preference. */
int sxmedia_ffmpeg_register(void);

#ifdef __cplusplus
}
#endif
