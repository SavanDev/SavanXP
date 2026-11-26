/*
 * fonts.c -- The font services (sxw_abi.h, SXW_SVC_FONT_*): the host's
 * TrueType faces for gdi32, through libsxfont (docs/desktop/FONT_LIBRARY.md).
 *
 * The platform names the directories (sxw_services.font_directories) and
 * reads them with its file operations; this file lists the faces once, the
 * first time a font is asked for, keeping only their names and metrics in a
 * libsxfont catalog, and reads a file again when a font uses it, keeping it
 * while any font does.
 *
 * What a font reports is what Windows XP's GDI reports for the same face
 * and request, measured with fixtures/text (text.exe, with Windows XP given
 * the Liberation files): the em is the requested height when it is negative
 * and 16 pixels when it is 0, a positive height is the cell; widths are
 * libsxfont's hinted advances; the horizontal metrics scale by the em, or by
 * the cell for a positive height; the largest width is the font box's.
 * docs/userland/WIN32.md, "Text", has the rules and the measurements.
 */
#include <stdlib.h>
#include <string.h>

#include "sxfont/font.h"
#include "services.h"

#define SXW_FONT_MAX_FILES 160u
#define SXW_FONT_MAX_OPEN 96u
#define SXW_FONT_MAX_FACES 256u
#define SXW_FONT_FILE_LIMIT (32u * 1024u * 1024u)

typedef struct font_file {
    char path[SXW_HOST_PATH];
    void *data;     /* while a font uses it */
    size_t size;
    uint32_t users;
} font_file;

typedef struct open_font {
    uint32_t used;
    uint32_t file;
    struct sx_font_face *face;
    struct sx_font_strike *strike;
    struct sx_font_face_info info;
    unsigned synth;
} open_font;

struct sxw_fonts {
    int ready;
    struct sx_font_engine *engine;
    struct sx_font_catalog *catalog;
    font_file files[SXW_FONT_MAX_FILES];
    uint32_t file_count;
    open_font fonts[SXW_FONT_MAX_OPEN];
    /* Catalog entries in enumeration order: by family, then regular, bold,
     * italic, bold italic. */
    uint32_t order[SXW_FONT_MAX_FACES];
    uint32_t face_count;
};

/* ---- Small helpers ------------------------------------------------------- */

/* v * num / den, rounded half up, for either sign. */
/* round(a / b), halves up, for b > 0. */
static long long divide_round(long long a, long long b) {
    long long twice = 2LL * a + b;
    long long q = twice / (2LL * b);
    if (twice < 0 && twice % (2LL * b) != 0) {
        q -= 1; /* floor */
    }
    return q;
}

/* v * num / den, rounded once: how the enumeration's metrics come out
 * (Liberation Serif Bold Italic's 3792-unit box at 35 of 2268 is 58.52, and
 * 59 in EnumFontFamiliesExW on XP). */
static int scale_exact(long v, long num, long den) {
    return den <= 0 ? 0 : (int)divide_round((long long)v * num, den);
}

/* v * num / den in font units to pixels, through a 16.16 scale rounded
 * first, as GDI does: Liberation Sans Bold's yMax of 2116 at a 20-pixel
 * cell is 18.497 exactly but 19 on XP (text.exe, otmrcFontBox). */
static int scale_round(long v, long num, long den) {
    if (den <= 0) {
        return 0;
    }
    const long long scale = divide_round((long long)num << 16, den);
    return (int)divide_round((long long)v * scale, 1LL << 16);
}

static int lower(int c) {
    return c >= 'A' && c <= 'Z' ? c + 32 : c;
}

static int ends_with(const char *name, const char *suffix) {
    size_t n = strlen(name);
    size_t m = strlen(suffix);
    if (n < m) {
        return 0;
    }
    for (size_t i = 0; i < m; ++i) {
        if (lower((unsigned char)name[n - m + i]) != lower((unsigned char)suffix[i])) {
            return 0;
        }
    }
    return 1;
}

static int compare_folded(const char *a, const char *b) {
    for (;; ++a, ++b) {
        int x = lower((unsigned char)*a);
        int y = lower((unsigned char)*b);
        if (x != y || x == 0) {
            return x - y;
        }
    }
}

/* UTF-8 into UTF-16 units, NUL-terminated, cut to fit. */
static void to_utf16(const char *in, unsigned short *out, uint32_t capacity) {
    uint32_t n = 0;
    const unsigned char *p = (const unsigned char *)in;
    while (*p != 0 && n + 1 < capacity) {
        uint32_t c = *p++;
        if (c >= 0xf0u) {
            c = ((c & 7u) << 18) | ((p[0] & 0x3fu) << 12) | ((p[1] & 0x3fu) << 6) | (p[2] & 0x3fu);
            p += 3;
        } else if (c >= 0xe0u) {
            c = ((c & 15u) << 12) | ((p[0] & 0x3fu) << 6) | (p[1] & 0x3fu);
            p += 2;
        } else if (c >= 0xc0u) {
            c = ((c & 31u) << 6) | (p[0] & 0x3fu);
            p += 1;
        }
        out[n++] = c > 0xffffu ? (unsigned short)'?' : (unsigned short)c;
    }
    out[n] = 0;
}

/* A request's face name, UTF-16 in the guest's structure, as UTF-8. */
static void face_to_utf8(const unsigned short *face, char *out, size_t capacity) {
    size_t n = 0;
    for (uint32_t i = 0; i < SXW_FONT_FACE_UNITS && face[i] != 0; ++i) {
        uint32_t c = face[i];
        if (c < 0x80u && n + 1 < capacity) {
            out[n++] = (char)c;
        } else if (c < 0x800u && n + 2 < capacity) {
            out[n++] = (char)(0xc0u | (c >> 6));
            out[n++] = (char)(0x80u | (c & 0x3fu));
        } else if (n + 3 < capacity) {
            out[n++] = (char)(0xe0u | (c >> 12));
            out[n++] = (char)(0x80u | ((c >> 6) & 0x3fu));
            out[n++] = (char)(0x80u | (c & 0x3fu));
        }
    }
    out[n] = 0;
}

/* ---- Reading files through the platform ----------------------------------- */

static void *read_whole(sxw_services *s, const char *path, size_t *size) {
    const sxw_file_platform *f = s->files;
    long file = -1;
    sxw_host_info info;
    if (f == 0 || f->open(s->context, path, SXW_OPEN_READ, &file) != SXW_FS_OK) {
        return 0;
    }
    void *data = 0;
    if (f->query(s->context, file, &info) == SXW_FS_OK && info.size > 0 && info.size <= SXW_FONT_FILE_LIMIT) {
        data = malloc((size_t)info.size);
        uint64_t done = 0;
        while (data != 0 && done < info.size) {
            uint32_t got = 0;
            uint64_t left = info.size - done;
            if (f->read(s->context, file, done, (char *)data + done, left > 0x100000u ? 0x100000u : (uint32_t)left,
                        &got) != SXW_FS_OK ||
                got == 0) {
                break;
            }
            done += got;
        }
        if (data != 0 && done != info.size) {
            free(data);
            data = 0;
        }
        *size = (size_t)done;
    }
    f->close(s->context, file);
    return data;
}

/* ---- The catalog ----------------------------------------------------------- */

static int style_rank(const struct sx_font_face_info *info) {
    return ((info->flags & SX_FONT_ITALIC) != 0 ? 2 : 0) + (info->weight >= 600u ? 1 : 0);
}

static int add_directory(sxw_services *s, struct sxw_fonts *fonts, const char *directory) {
    const sxw_file_platform *f = s->files;
    long list = -1;
    char name[256];
    sxw_host_info info;
    /* By name, so a family's order does not hang on the directory's. */
    char (*names)[256] = 0;
    uint32_t count = 0;
    if (f == 0 || f->list_open == 0 || f->list_open(s->context, directory, &list) != SXW_FS_OK) {
        return 0;
    }
    names = malloc(sizeof *names * SXW_FONT_MAX_FILES);
    while (names != 0 && count < SXW_FONT_MAX_FILES && f->list_next(s->context, list, name, &info) == 1) {
        if (info.type == SXW_HOST_FILE && (ends_with(name, ".ttf") || ends_with(name, ".ttc"))) {
            memcpy(names[count++], name, sizeof name);
        }
    }
    f->list_close(s->context, list);
    for (uint32_t i = 1; i < count; ++i) {
        for (uint32_t j = i; j > 0 && compare_folded(names[j - 1], names[j]) > 0; --j) {
            char t[256];
            memcpy(t, names[j], sizeof t);
            memcpy(names[j], names[j - 1], sizeof t);
            memcpy(names[j - 1], t, sizeof t);
        }
    }
    for (uint32_t i = 0; i < count && fonts->file_count < SXW_FONT_MAX_FILES; ++i) {
        font_file *file = &fonts->files[fonts->file_count];
        size_t length = strlen(directory);
        if (length + 1 + strlen(names[i]) + 1 > sizeof file->path) {
            continue;
        }
        memcpy(file->path, directory, length);
        file->path[length] = '/';
        memcpy(file->path + length + 1, names[i], strlen(names[i]) + 1);
        size_t size = 0;
        void *data = read_whole(s, file->path, &size);
        if (data == 0) {
            continue;
        }
        if (sx_font_catalog_add(fonts->catalog, data, size, fonts->file_count) > 0) {
            fonts->file_count++;
        }
        free(data);
    }
    free(names);
    return 1;
}

/* Lists the faces the first time a font is asked for. */
static struct sxw_fonts *fonts_of(sxw_services *s) {
    if (s->fonts == 0) {
        s->fonts = calloc(1, sizeof *s->fonts);
        if (s->fonts == 0) {
            return 0;
        }
    }
    struct sxw_fonts *fonts = s->fonts;
    if (fonts->ready) {
        return fonts->catalog != 0 ? fonts : 0;
    }
    fonts->ready = 1;
    if (sx_font_engine_create(&fonts->engine) != SX_FONT_OK ||
        sx_font_catalog_create(fonts->engine, &fonts->catalog) != SX_FONT_OK) {
        fonts->catalog = 0;
        return 0;
    }
    for (uint32_t i = 0; s->font_directories != 0 && s->font_directories[i] != 0; ++i) {
        (void)add_directory(s, fonts, s->font_directories[i]);
    }
    /* The enumeration order: families by name, a family's faces by full
     * name, as Windows XP lists what its Fonts registry key holds (Arial,
     * Arial Bold, Arial Bold Italic, Arial Italic). */
    uint32_t n = sx_font_catalog_count(fonts->catalog);
    static struct sx_font_face_info infos[SXW_FONT_MAX_FACES];
    for (uint32_t i = 0; i < n && fonts->face_count < SXW_FONT_MAX_FACES; ++i) {
        uint32_t tag;
        unsigned index;
        if (sx_font_catalog_entry(fonts->catalog, i, &infos[fonts->face_count], &tag, &index) == SX_FONT_OK) {
            fonts->order[fonts->face_count++] = i;
        }
    }
    for (uint32_t i = 1; i < fonts->face_count; ++i) {
        for (uint32_t j = i; j > 0; --j) {
            const struct sx_font_face_info *a = &infos[j - 1];
            const struct sx_font_face_info *b = &infos[j];
            int c = compare_folded(a->family, b->family);
            if (c == 0) {
                c = compare_folded(a->full_name, b->full_name);
            }
            if (c < 0 || (c == 0 && style_rank(a) <= style_rank(b))) {
                break;
            }
            struct sx_font_face_info t = infos[j];
            infos[j] = infos[j - 1];
            infos[j - 1] = t;
            uint32_t o = fonts->order[j];
            fonts->order[j] = fonts->order[j - 1];
            fonts->order[j - 1] = o;
        }
    }
    return fonts;
}

/* ---- What GDI reports ------------------------------------------------------- */

/* tmPitchAndFamily's family from the face's PANOSE, as GDI derives it. */
static unsigned family_of(const struct sx_font_face_info *info) {
    const unsigned kind = info->panose[0];
    const unsigned serif = info->panose[1];
    if (kind == 3u) {
        return 0x40u; /* FF_SCRIPT */
    }
    if (kind == 4u || kind == 5u) {
        return 0x50u; /* FF_DECORATIVE */
    }
    if ((info->flags & SX_FONT_FIXED_PITCH) != 0) {
        return 0x30u; /* FF_MODERN */
    }
    if (serif >= 11u && serif <= 13u) {
        return 0x20u; /* FF_SWISS: normal, obtuse and perpendicular sans */
    }
    if (serif >= 2u && serif <= 10u) {
        return 0x10u; /* FF_ROMAN */
    }
    return 0x20u;
}

static unsigned tm_pitch_family(const struct sx_font_face_info *info) {
    /* TMPF_FIXED_PITCH is set for a variable-pitch font, as Windows has it. */
    unsigned fixed = (info->flags & SX_FONT_FIXED_PITCH) != 0 ? 0u : 0x01u;
    return family_of(info) | fixed | 0x02u /* TMPF_VECTOR */ | 0x04u /* TMPF_TRUETYPE */;
}

static unsigned lf_pitch_family(const struct sx_font_face_info *info) {
    return family_of(info) | ((info->flags & SX_FONT_FIXED_PITCH) != 0 ? 1u : 2u);
}

/* The line gap libsxfont gives as external leading, in font units. */
static long gap_units(const struct sx_font_face_info *info) {
    long gap = info->hhea_line_gap -
               (((long)info->win_ascent + info->win_descent) - ((long)info->hhea_ascent - info->hhea_descent));
    return gap > 0 ? gap : 0;
}

static void fill_metrics(const open_font *font, int height, unsigned charset, sxw_font_metrics *m) {
    const struct sx_font_face_info *info = &font->info;
    struct sx_font_strike_metrics sm;
    memset(m, 0, sizeof *m);
    (void)sx_font_strike_metrics(font->strike, &sm);
    const long em = (long)info->units_per_em;
    const long ppem = (long)sm.ppem;
    /* Horizontal metrics scale by the em, or by the cell for a positive
     * height, as GDI does; a face with VDMX picked a whole ppem for that
     * cell, and scales by it (Arial at a 12-pixel cell is 9 ppem, its
     * average width 4, not 5). */
    long num = ppem;
    long den = em;
    if (height > 0 && info->vdmx == 0) {
        num = height;
        den = (long)info->win_ascent + info->win_descent;
    }
    const int bold = (font->synth & SX_FONT_SYNTH_BOLD) != 0;
    m->height = sm.height;
    m->ascent = sm.ascent;
    m->descent = sm.descent;
    m->internal_leading = sm.internal_leading;
    m->external_leading = sm.external_leading;
    m->average_width = scale_round(info->avg_char_width, num, den) + bold;
    m->maximum_width = scale_round((long)info->x_max - info->x_min, num, den) + bold;
    m->weight = bold ? 700u : info->weight;
    m->first_char = info->first_char;
    m->last_char = info->last_char;
    m->default_char = info->first_char > 0 ? info->first_char - 1u : 0u;
    m->break_char = 0x20u;
    m->italic = ((info->flags & SX_FONT_ITALIC) != 0 || (font->synth & SX_FONT_SYNTH_ITALIC) != 0) ? 255u : 0u;
    m->pitch_family = tm_pitch_family(info);
    m->charset = (info->flags & SX_FONT_SYMBOL) != 0 ? 2u : charset;
    m->ppem = sm.ppem;
    m->smooth = sm.render == SX_FONT_RENDER_GRAY;
    m->em_square = info->units_per_em;
    /* The outline metrics scale as the horizontal ones do: by the em, or by
     * the cell for a positive height. */
    m->typo_ascent = scale_round(info->typo_ascender, num, den);
    m->typo_descent = scale_round(info->typo_descender, num, den);
    m->typo_line_gap = scale_round(info->typo_line_gap, num, den);
    /* Windows XP reports half and a quarter of the em here, whatever OS/2
     * says (text.exe, every face and size). */
    m->cap_height = scale_round(em, num, 2 * den);
    m->x_height = scale_round(em, num, 4 * den);
    /* Tenths of a degree, rounded down, and the caret's slope as hhea has
     * it (Arial Italic's 33 over 7, Liberation's 100 over 21). */
    long tenths = ((long long)info->italic_angle * 10) >> 16;
    m->italic_angle = (int)tenths;
    if (info->caret_slope_rise != 0 && (info->caret_slope_run != 0 || info->italic_angle == 0)) {
        m->slope_rise = info->caret_slope_rise;
        m->slope_run = info->caret_slope_run;
    } else if (info->italic_angle == 0) {
        m->slope_rise = 1;
        m->slope_run = 0;
    } else {
        /* tan of the angle, by a short series good to a degree or so for
         * the slants fonts have. */
        double degrees = -(double)info->italic_angle / 65536.0;
        double x = degrees * 3.14159265358979 / 180.0;
        double tangent = x + x * x * x / 3.0 + 2.0 * x * x * x * x * x / 15.0;
        m->slope_rise = 100;
        m->slope_run = (int)(tangent * 100.0 + 0.5);
    }
    /* 0 when the line is thinner than half a pixel, as Windows XP says;
     * gdi32 draws it a pixel thick all the same. */
    m->underscore_size = scale_round(info->underline_thickness, num, den);
    m->underscore_position = scale_round(info->underline_position, num, den);
    m->strikeout_size = scale_round(info->strikeout_size, num, den);
    m->strikeout_position = scale_round(info->strikeout_position, num, den);
    m->fs_selection = info->fs_selection;
    m->fs_type = info->fs_type;
    m->mac_ascent = scale_round(info->hhea_ascent, num, den);
    m->mac_descent = scale_round(info->hhea_descent, num, den);
    m->mac_line_gap = scale_round(info->hhea_line_gap, num, den);
    m->minimum_ppem = info->lowest_ppem;
    m->box[0] = scale_round(info->x_min, num, den);
    m->box[1] = scale_round(info->y_max, num, den);
    m->box[2] = scale_round(info->x_max, num, den);
    m->box[3] = scale_round(info->y_min, num, den);
    memcpy(m->panose, info->panose, sizeof info->panose);
    m->code_pages[0] = info->code_page_range[0];
    m->code_pages[1] = info->code_page_range[1];
    to_utf16(info->family, m->family, SXW_FONT_FACE_UNITS);
    to_utf16(info->style, m->style, SXW_FONT_FACE_UNITS);
    to_utf16(info->full_name[0] != 0 ? info->full_name : info->family, m->full, SXW_FONT_FULL_UNITS);
}

/* ---- Opening a font ---------------------------------------------------------- */

static void release_file(struct sxw_fonts *fonts, uint32_t index) {
    font_file *file = &fonts->files[index];
    if (file->users > 0 && --file->users == 0) {
        free(file->data);
        file->data = 0;
    }
}

/* The catalog's choice for a family; 0 when it only had a fallback. */
static int match(struct sxw_fonts *fonts, const char *family, unsigned weight, unsigned flags,
                 struct sx_font_match *out) {
    struct sx_font_request request = {family, weight, flags};
    return sx_font_catalog_match(fonts->catalog, &request, out) == SX_FONT_OK;
}

static uint32_t font_open(sxw_services *s, const sxw_font_request *r, sxw_font_metrics *out) {
    struct sxw_fonts *fonts = fonts_of(s);
    if (fonts == 0) {
        return 0;
    }
    char family[3 * SXW_FONT_FACE_UNITS + 1];
    face_to_utf8(r->face, family, sizeof family);
    const unsigned pitch = r->pitch_family & 0x0fu;
    const unsigned kind = r->pitch_family & 0xf0u;
    unsigned flags = r->italic != 0 ? SX_FONT_ITALIC : 0u;
    if (pitch == 1u) {
        flags |= SX_FONT_FIXED_PITCH;
    }
    unsigned weight = r->weight != 0u ? r->weight : 400u;
    /* From 600 to 699 Windows XP takes the regular face and emboldens it,
     * where 700 and more take the bold face (text.exe's weights). */
    const int embolden = weight >= 600u && weight < 700u;
    if (embolden) {
        weight = 400u;
    }
    struct sx_font_match m;
    int found = family[0] != 0 && match(fonts, family, weight, flags, &m) && m.quality != SX_FONT_MATCH_FALLBACK;
    if (!found) {
        /* A face the host does not have: the one Windows XP's mapper gives
         * for the family and pitch (text.exe's "missing" cases). */
        const char *instead = kind == 0x10u                     ? "Times New Roman"
                              : kind == 0x30u || pitch == 1u    ? "Courier New"
                                                                : "Arial";
        if (!match(fonts, instead, weight, flags, &m)) {
            return 0;
        }
    }
    uint32_t slot = 0;
    while (slot < SXW_FONT_MAX_OPEN && fonts->fonts[slot].used) {
        ++slot;
    }
    if (slot == SXW_FONT_MAX_OPEN || m.tag >= fonts->file_count) {
        return 0;
    }
    font_file *file = &fonts->files[m.tag];
    if (file->users == 0) {
        file->data = read_whole(s, file->path, &file->size);
        if (file->data == 0) {
            return 0;
        }
    }
    file->users++;
    open_font *font = &fonts->fonts[slot];
    memset(font, 0, sizeof *font);
    font->file = m.tag;
    font->synth = m.synth_flags | (embolden ? SX_FONT_SYNTH_BOLD : 0u);
    if (sx_font_face_open(fonts->engine, file->data, file->size, m.face_index, &font->face) != SX_FONT_OK ||
        sx_font_face_info(font->face, &font->info) != SX_FONT_OK) {
        if (font->face != 0) {
            sx_font_face_close(font->face);
        }
        release_file(fonts, m.tag);
        return 0;
    }
    unsigned ppem = 16u;
    if (r->height < 0) {
        ppem = (unsigned)-r->height;
    } else if (r->height > 0 && sx_font_ppem_for_cell_height(font->face, (unsigned)r->height, &ppem) != SX_FONT_OK) {
        ppem = (unsigned)r->height;
    }
    if (ppem > SX_FONT_MAX_PPEM) {
        ppem = SX_FONT_MAX_PPEM;
    }
    struct sx_font_strike_options options = {ppem == 0u ? 1u : ppem, font->synth};
    if (r->quality == 3u) {
        options.flags |= SX_FONT_FORCE_MONO; /* NONANTIALIASED_QUALITY */
    } else if (r->quality == 4u) {
        options.flags |= SX_FONT_FORCE_GRAY; /* ANTIALIASED_QUALITY */
    }
    if (sx_font_strike_open(font->face, &options, &font->strike) != SX_FONT_OK) {
        sx_font_face_close(font->face);
        release_file(fonts, m.tag);
        return 0;
    }
    font->used = 1;
    const unsigned charset = r->charset == 1u ? 0u : r->charset;
    fill_metrics(font, r->height, charset, out);
    return slot + 1u;
}

static open_font *font_of(struct sxw_fonts *fonts, uint32_t id) {
    return fonts != 0 && id >= 1u && id <= SXW_FONT_MAX_OPEN && fonts->fonts[id - 1u].used ? &fonts->fonts[id - 1u] : 0;
}

static void font_close(struct sxw_fonts *fonts, uint32_t id) {
    open_font *font = font_of(fonts, id);
    if (font == 0) {
        return;
    }
    sx_font_face_close(font->face); /* and its strike */
    release_file(fonts, font->file);
    font->used = 0;
}

/* ---- The enumeration ---------------------------------------------------------- */

/* A face as EnumFontFamiliesExW reports it: at a cell of the em's 32
 * pixels' worth, everything scaled by that cell (Windows XP's numbers for
 * every Liberation face). */
static int face_entry(struct sxw_fonts *fonts, uint32_t index, sxw_font_face_entry *e) {
    struct sx_font_face_info info;
    uint32_t tag;
    unsigned face_index;
    if (index >= fonts->face_count ||
        sx_font_catalog_entry(fonts->catalog, fonts->order[index], &info, &tag, &face_index) != SX_FONT_OK) {
        return 0;
    }
    memset(e, 0, sizeof *e);
    to_utf16(info.family, e->family, SXW_FONT_FACE_UNITS);
    to_utf16(info.style, e->style, SXW_FONT_FACE_UNITS);
    to_utf16(info.full_name[0] != 0 ? info.full_name : info.family, e->full, SXW_FONT_FULL_UNITS);
    const long em = (long)info.units_per_em;
    const long cell = (long)info.win_ascent + info.win_descent;
    const int height = scale_exact(cell, 32, em);
    e->weight = info.weight;
    e->italic = (info.flags & SX_FONT_ITALIC) != 0 ? 255u : 0u;
    e->pitch_family = tm_pitch_family(&info);
    e->lf_pitch_family = lf_pitch_family(&info);
    e->height = height;
    e->ascent = scale_exact(info.win_ascent, height, cell);
    e->descent = height - e->ascent;
    e->internal_leading = height - scale_exact(em, height, cell);
    e->external_leading = scale_exact(gap_units(&info), height, cell);
    e->average_width = scale_exact(info.avg_char_width, height, cell);
    e->maximum_width = scale_exact((long)info.x_max - info.x_min, height, cell);
    e->lf_width = scale_exact(info.avg_char_width, 32, em);
    e->first_char = info.first_char;
    e->last_char = info.last_char;
    e->em_square = info.units_per_em;
    e->cell_height = (unsigned)cell;
    e->average_units = (unsigned)info.avg_char_width;
    e->code_pages[0] = info.code_page_range[0];
    e->code_pages[1] = info.code_page_range[1];
    memcpy(e->unicode_ranges, info.unicode_range, sizeof e->unicode_ranges);
    /* Windows XP calls a signed TrueType file OpenType, and only then:
     * Arial has DSIG and gets both flags, Liberation has GSUB but no
     * signature and gets neither (text.exe). */
    if ((info.flags & SX_FONT_SIGNED) != 0) {
        e->ntm_flags = 0x00200000u /* NTM_DSIG */ | 0x00040000u /* NTM_TT_OPENTYPE */;
    }
    return 1;
}

/* ---- The services ----------------------------------------------------------- */

static void *guest(const sxw_services *s, uint32_t address, uint32_t length) {
    return address != 0u && sxw_guest_range(s->process, address, length) ? (void *)(uintptr_t)address : 0;
}

uint32_t sxw_font_service(sxw_services *s, uint32_t service, const uint32_t *a) {
    switch (service) {
    case SXW_SVC_FONT_OPEN: {
        const sxw_font_request *r = guest(s, a[0], sizeof *r);
        sxw_font_metrics *out = guest(s, a[1], sizeof *out);
        if (r == 0 || out == 0) {
            return 0;
        }
        sxw_font_request copy = *r;
        sxw_font_metrics m;
        uint32_t id = font_open(s, &copy, &m);
        if (id != 0u) {
            *out = m;
        }
        return id;
    }
    case SXW_SVC_FONT_CLOSE:
        font_close(s->fonts, a[0]);
        return 0;
    case SXW_SVC_FONT_ADVANCES: {
        open_font *font = font_of(s->fonts, a[0]);
        const uint32_t count = a[2];
        const unsigned short *units = count != 0u ? guest(s, a[1], 2u * count) : 0;
        int32_t *out = count != 0u ? guest(s, a[3], 4u * count) : 0;
        if (font == 0 || count > 0x10000u || (count != 0u && (units == 0 || out == 0))) {
            return SXW_FAILED;
        }
        for (uint32_t i = 0; i < count; ++i) {
            int advance = 0;
            (void)sx_font_glyph_advance(font->strike, sx_font_glyph_index(font->face, units[i]), &advance);
            out[i] = advance;
        }
        return 0;
    }
    case SXW_SVC_FONT_GLYPH: {
        open_font *font = font_of(s->fonts, a[0]);
        sxw_font_glyph *out = guest(s, a[2], sizeof *out);
        if (font == 0 || out == 0) {
            return SXW_FAILED;
        }
        struct sx_font_glyph g;
        if (sx_font_glyph_render(font->strike, sx_font_glyph_index(font->face, a[1]), &g) != SX_FONT_OK) {
            return SXW_FAILED;
        }
        sxw_font_glyph header = {g.left, g.top, g.width, g.height, g.advance, g.width * g.height};
        *out = header;
        if (g.coverage != 0 && header.size != 0u && header.size <= a[4]) {
            unsigned char *buffer = guest(s, a[3], header.size);
            if (buffer == 0) {
                return SXW_FAILED;
            }
            for (unsigned row = 0; row < g.height; ++row) {
                memcpy(buffer + row * g.width, g.coverage + row * g.pitch, g.width);
            }
        }
        return 0;
    }
    case SXW_SVC_FONT_FACE: {
        struct sxw_fonts *fonts = fonts_of(s);
        sxw_font_face_entry *out = guest(s, a[1], sizeof *out);
        sxw_font_face_entry e;
        if (fonts == 0 || out == 0 || !face_entry(fonts, a[0], &e)) {
            return SXW_FAILED;
        }
        *out = e;
        return 0;
    }
    default:
        return SXW_FAILED;
    }
}
