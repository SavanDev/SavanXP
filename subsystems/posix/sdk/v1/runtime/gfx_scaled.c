/* sx_scaled_presenter: integer nearest-neighbour scaling of a fixed-size frame
 * onto a POSIX window (savanxp_gfx_context), presenting only the rows that
 * changed. It lives with the POSIX window client because it presents; the
 * scaling itself is the platform painter's (sxgfx/gfx2d.h). */

#include "savanxp/libc.h"
#include "savanxp/gfx2d.h"

#include <stdlib.h>
#include <string.h>

static int sx_scaled_presenter_validate_source(uint32_t width, uint32_t height)
{
    size_t pixel_count;

    if (width == 0 || height == 0 || width > (UINT32_MAX / 4u))
    {
        return 0;
    }
    pixel_count = (size_t)width * (size_t)height;
    return pixel_count <= (size_t)UINT32_MAX / sizeof(uint32_t);
}

static int sx_scaled_presenter_relayout(
    struct sx_scaled_presenter* presenter,
    uint32_t background)
{
    uint32_t horizontal_scale;
    uint32_t vertical_scale;
    uint32_t scale;
    uint32_t scaled_width;
    uint32_t scaled_height;
    size_t expanded_capacity;

    if (presenter == 0 || presenter->gfx == 0)
    {
        return -SAVANXP_EINVAL;
    }
    presenter->target.pixels = presenter->gfx->pixels;
    if (presenter->source.pixels == 0 || presenter->target.pixels == 0 ||
        presenter->gfx->info.width == 0 || presenter->gfx->info.height == 0 ||
        (presenter->gfx->info.pitch % sizeof(uint32_t)) != 0 ||
        presenter->gfx->info.pitch < (presenter->gfx->info.width * sizeof(uint32_t)) ||
        presenter->gfx->info.buffer_size <
            ((size_t)presenter->gfx->info.pitch * (size_t)presenter->gfx->info.height))
    {
        return -SAVANXP_EINVAL;
    }

    horizontal_scale = presenter->gfx->info.width / presenter->source.info.width;
    vertical_scale = presenter->gfx->info.height / presenter->source.info.height;
    scale = horizontal_scale < vertical_scale ? horizontal_scale : vertical_scale;
    if (scale == 0)
    {
        return -SAVANXP_EINVAL;
    }

    scaled_width = presenter->source.info.width * scale;
    scaled_height = presenter->source.info.height * scale;
    expanded_capacity = (size_t)scaled_width;
    if (expanded_capacity > SIZE_MAX / sizeof(uint32_t))
    {
        return -SAVANXP_EINVAL;
    }
    if (expanded_capacity > presenter->expanded_row_capacity)
    {
        uint32_t* expanded = (uint32_t*)realloc(
            presenter->expanded_row, expanded_capacity * sizeof(uint32_t));
        if (expanded == 0)
        {
            return -SAVANXP_ENOMEM;
        }
        presenter->expanded_row = expanded;
        presenter->expanded_row_capacity = expanded_capacity;
    }

    presenter->scale = scale;
    presenter->destination = sx_rect_make(
        (int)((presenter->gfx->info.width - scaled_width) / 2u),
        (int)((presenter->gfx->info.height - scaled_height) / 2u),
        (int)scaled_width,
        (int)scaled_height);
    sx_bitmap_wrap(
        &presenter->target,
        presenter->gfx->pixels,
        &presenter->gfx->info,
        SX_PIXEL_FORMAT_BGRX8888);
    gfx_clear(presenter->target.pixels, &presenter->target.info, background);
    presenter->previous_valid = 0;
    presenter->force_surface_present = 1;
    return 0;
}

int sx_scaled_presenter_init(
    struct sx_scaled_presenter* presenter,
    struct savanxp_gfx_context* gfx,
    uint32_t source_width,
    uint32_t source_height,
    uint32_t* source_pixels,
    uint32_t background)
{
    size_t source_pixels_count;
    int result;

    if (presenter == 0)
    {
        return -SAVANXP_EINVAL;
    }
    memset(presenter, 0, sizeof(*presenter));
    if (gfx == 0 || gfx->fb_fd < 0 || gfx->pixels == 0 || source_pixels == 0 ||
        !sx_scaled_presenter_validate_source(source_width, source_height))
    {
        return -SAVANXP_EINVAL;
    }

    source_pixels_count = (size_t)source_width * (size_t)source_height;
    presenter->previous = (uint32_t*)malloc(source_pixels_count * sizeof(uint32_t));
    if (presenter->previous == 0)
    {
        return -SAVANXP_ENOMEM;
    }

    presenter->gfx = gfx;
    presenter->target.pixels = gfx->pixels;
    presenter->source_pixel_count = source_pixels_count;
    sx_bitmap_wrap(
        &presenter->source,
        source_pixels,
        &(struct savanxp_fb_info){
            .width = source_width,
            .height = source_height,
            .pitch = source_width * (uint32_t)sizeof(uint32_t),
            .bpp = 32u,
            .buffer_size = (uint32_t)(source_pixels_count * sizeof(uint32_t)),
        },
        SX_PIXEL_FORMAT_BGRX8888);

    result = sx_scaled_presenter_relayout(presenter, background);
    if (result < 0)
    {
        sx_scaled_presenter_destroy(presenter);
    }
    return result;
}

int sx_scaled_presenter_retarget(
    struct sx_scaled_presenter* presenter,
    uint32_t background)
{
    if (presenter == 0 || presenter->gfx == 0)
    {
        return -SAVANXP_EINVAL;
    }
    return sx_scaled_presenter_relayout(presenter, background);
}

static void sx_scaled_presenter_expand_rows(
    struct sx_scaled_presenter* presenter,
    int first_row,
    int end_row)
{
    const uint32_t target_stride = gfx_stride_pixels(&presenter->target.info);
    const uint32_t source_stride = gfx_stride_pixels(&presenter->source.info);
    int source_y;

    for (source_y = first_row; source_y <= end_row; ++source_y)
    {
        const uint32_t* source_row = presenter->source.pixels +
            ((size_t)source_y * source_stride);
        uint32_t* expanded = presenter->expanded_row;
        uint32_t* destination_row = presenter->target.pixels +
            ((size_t)(presenter->destination.y + source_y * (int)presenter->scale) * target_stride) +
            (size_t)presenter->destination.x;
        uint32_t source_x;
        uint32_t repeat_y;

        for (source_x = 0; source_x < presenter->source.info.width; ++source_x)
        {
            uint32_t colour = source_row[source_x];
            uint32_t repeat_x;
            for (repeat_x = 0; repeat_x < presenter->scale; ++repeat_x)
            {
                *expanded++ = colour;
            }
        }

        memcpy(
            destination_row,
            presenter->expanded_row,
            (size_t)presenter->destination.width * sizeof(uint32_t));
        for (repeat_y = 1; repeat_y < presenter->scale; ++repeat_y)
        {
            memcpy(
                destination_row + ((size_t)repeat_y * target_stride),
                presenter->expanded_row,
                (size_t)presenter->destination.width * sizeof(uint32_t));
        }
    }
}

long sx_scaled_presenter_present(
    struct sx_scaled_presenter* presenter,
    const uint32_t* source_pixels)
{
    int first_row;
    int end_row;
    long result;
    int source_y;
    uint32_t present_x;
    uint32_t present_y;
    uint32_t present_width;
    uint32_t present_height;

    if (presenter == 0 || source_pixels == 0 || presenter->previous == 0 ||
        presenter->expanded_row == 0)
    {
        return -SAVANXP_EINVAL;
    }
    presenter->source.pixels = (uint32_t*)source_pixels;

    if (!presenter->previous_valid)
    {
        first_row = 0;
        end_row = (int)presenter->source.info.height - 1;
    }
    else
    {
        const uint32_t source_stride = gfx_stride_pixels(&presenter->source.info);
        first_row = (int)presenter->source.info.height;
        end_row = -1;
        for (source_y = 0; source_y < (int)presenter->source.info.height; ++source_y)
        {
            const uint32_t* current_row = source_pixels + ((size_t)source_y * source_stride);
            const uint32_t* previous_row = presenter->previous +
                ((size_t)source_y * presenter->source.info.width);
            if (memcmp(previous_row, current_row,
                       (size_t)presenter->source.info.width * sizeof(uint32_t)) == 0)
            {
                continue;
            }
            if (source_y < first_row)
            {
                first_row = source_y;
            }
            end_row = source_y;
        }
        if (end_row < first_row)
        {
            return 0;
        }
    }

    sx_scaled_presenter_expand_rows(presenter, first_row, end_row);
    if (presenter->force_surface_present)
    {
        present_x = 0;
        present_y = 0;
        present_width = presenter->target.info.width;
        present_height = presenter->target.info.height;
    }
    else
    {
        present_x = (uint32_t)presenter->destination.x;
        present_y = (uint32_t)(presenter->destination.y + first_row * (int)presenter->scale);
        present_width = (uint32_t)presenter->destination.width;
        present_height = (uint32_t)((end_row - first_row + 1) * (int)presenter->scale);
    }
    result = gfx_present_region(
        presenter->gfx,
        presenter->target.pixels,
        present_x,
        present_y,
        present_width,
        present_height);
    if (result < 0)
    {
        return result;
    }

    for (source_y = first_row; source_y <= end_row; ++source_y)
    {
        const uint32_t* current_row = source_pixels +
            ((size_t)source_y * gfx_stride_pixels(&presenter->source.info));
        memcpy(
            presenter->previous + ((size_t)source_y * presenter->source.info.width),
            current_row,
            (size_t)presenter->source.info.width * sizeof(uint32_t));
    }
    presenter->previous_valid = 1;
    presenter->force_surface_present = 0;
    return 1;
}

void sx_scaled_presenter_destroy(struct sx_scaled_presenter* presenter)
{
    if (presenter == 0)
    {
        return;
    }
    if (presenter->expanded_row != 0)
    {
        free(presenter->expanded_row);
    }
    if (presenter->previous != 0)
    {
        free(presenter->previous);
    }
    memset(presenter, 0, sizeof(*presenter));
}
