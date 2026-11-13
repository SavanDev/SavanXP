/*
 * sxw_gdi.h -- What user32 and gdi32 share and no program sees: the surface a
 * window's device contexts draw on, and gdi32's private exports.
 *
 * A top-level window owns one surface in guest memory; its children draw on
 * it at an offset. gdi32 adds what each call touches to `dirty`, and user32
 * presents that to the host (SXW_SVC_WINDOW_PRESENT) and clears it.
 */
#ifndef SAVANXP_WIN32_SXW_GDI_H
#define SAVANXP_WIN32_SXW_GDI_H

#include "region.h"

typedef struct sxw_surface {
    unsigned int *pixels; /* 0x00RRGGBB */
    int stride;           /* pixels per row */
    int width;
    int height;
    sxw_region dirty;
} sxw_surface;

#ifdef SXW_BUILDING_GDI32
#define SXW_GDI32_PRIVATE __declspec(dllexport)
#else
#define SXW_GDI32_PRIVATE __declspec(dllimport)
#endif

/* A device context drawing on surface with its logical (0, 0) at (x, y) and
 * clipped to visible, in surface coordinates. DeleteDC is not in the profile:
 * DeleteObject on it frees it. */
SXW_GDI32_PRIVATE void *__stdcall SxwCreateSurfaceDC(sxw_surface *surface, int x, int y,
                                                     const sxw_region *visible);
/* SetBkMode for user32's own drawing: OPAQUE or TRANSPARENT; the old mode. */
SXW_GDI32_PRIVATE int __stdcall SxwSetBkMode(void *dc, int mode);
/* The surface of a device context, or 0. */
SXW_GDI32_PRIVATE sxw_surface *__stdcall SxwSurfaceOfDC(void *dc);

#endif
