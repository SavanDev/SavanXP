#pragma once

/* SxCodecs' first occupant: an Ogg Vorbis decoder, and the only symbol a
 * program needs from it.
 *
 * This is what the fallback is supposed to look like when it works. The program
 * that links this knows the word "vorbis" and nothing else -- not that stb_vorbis
 * is the library inside, not that Ogg is a container, not that the file is read
 * a second time. It registers this the same way it would register any other
 * backend and from then on talks to the engine.
 *
 * A program does not need to link this to play something else: registration is
 * the only relationship, and a program that never calls
 * `sxmedia_vorbis_register()` never drags stb_vorbis in. It is not a link-time
 * dependency, it is a runtime decision, which is the difference between a codec
 * and a library.
 *
 * See savanxp/sxmedia.h for the vtable and docs/SXMEDIA.md for why the roles are
 * separable and why this one is a whole-file decoder.
 */

#ifdef __cplusplus
extern "C" {
#endif

/* Registers the Vorbis decoder: the decoder role for `vorbis` streams, and
 * nothing else. No source role, so it never enumerates a container, and no
 * converter role, so it never claims to resample or scale. Returns 0, or a
 * negative errno.
 *
 * Call it once, before `sx_media_open`. A second call fails with EEXIST rather
 * than replacing the entry, because the registry is a link-time fact and two
 * registrations of the same library would be a bug, not a preference. */
int sxmedia_vorbis_register(void);

#ifdef __cplusplus
}
#endif
