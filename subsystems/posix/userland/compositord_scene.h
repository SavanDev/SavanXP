#pragma once

#include <stdint.h>

#include "savanxp/compositor_protocol.h"
#include "savanxp/gfx2d.h"

/*
 * compositord's scene (docs/desktop/DESKTOP_ARCHITECTURE.md#the-scene): the
 * surfaces the WM attached, the layers of its last commit, the damage not yet
 * presented, and the composition itself. No GPU here: compositord.c presents
 * what scene_compose returns and drives the cursor plane.
 */

/* What a commit or a frame left to show. */
struct scene_cursor
{
    int x;
    int y;
    int shape;
    int visible;
};

/* The display the scene composes into: compositord's mapping of the display
 * section, with the current mode. Changing it damages everything. */
void scene_set_display(uint32_t *pixels, const struct savanxp_fb_info *info);
/* Whether the cursor is drawn by the scene (no hardware plane). */
void scene_set_software_cursor(int enabled);

/* Maps the WM's scene section. 0 or -errno. */
int scene_map_scene_section(long section);

/* request.surface plus its handles: the section, and the wake event for a
 * client surface (-1 otherwise). Takes ownership of both handles. */
int scene_attach(const struct savanxp_compositor_surface *desc, long section, long wake_event);
void scene_detach(uint32_t surface_id);

/* Copies the scene section: layers, damage, cursor. Consumes every published
 * batch first, so a window the WM just put in the scene shows its frame.
 * Stages the damaged part of the WM's surface. 0 or -errno. */
int scene_commit(void);
/* Consumes the batches clients published since the last look, adding their
 * damage. */
void scene_consume_frames(void);

int scene_has_damage(void);
/* Composes the pending damage into the display and hands it over in
 * `presented` (cleared in the scene). Surfaces whose frames it consumed are
 * marked composed. */
void scene_compose(struct sx_region *presented);
/* The frames composed by the last scene_compose are on the screen. */
void scene_retire(void);

/* The cursor of the last commit. */
struct scene_cursor scene_current_cursor(void);
/* Client surfaces whose batches were malformed since the last call, a bit per
 * surface id. */
uint32_t scene_take_broken(void);
/* Unmaps and closes everything. */
void scene_reset(void);
