/*
 * text.c -- The RichEdit control's text: memory, the conversions in and out
 * of its one-CR paragraphs, and word breaks, each as Windows XP's
 * msftedit.dll answered fixtures/richedit.
 */
#include "msftedit_private.h"

void *rich_alloc(u32 size) {
    return SxwHeapAllocate(size == 0 ? 1u : size, 1);
}

void rich_free(void *p) {
    if (p != 0) {
        SxwHeapFree(p);
    }
}

/* Room for `length` characters and a terminator. */
int rich_reserve(rich *r, int length) {
    if (length + 1 <= r->capacity) {
        return 1;
    }
    int capacity = r->capacity == 0 ? 64 : r->capacity;
    while (capacity < length + 1) {
        capacity *= 2;
    }
    WCHAR *grown = rich_alloc((u32)capacity * sizeof(WCHAR));
    if (grown == 0) {
        return 0;
    }
    for (int i = 0; i < r->length; ++i) {
        grown[i] = r->text[i];
    }
    grown[r->length] = 0;
    rich_free(r->text);
    r->text = grown;
    r->capacity = capacity;
    return 1;
}

int rich_count_crs(const WCHAR *text, int length) {
    int n = 0;
    for (int i = 0; i < length; ++i) {
        n += text[i] == '\r';
    }
    return n;
}

/* Text as a program gives it, into the control's form, stopping at a NUL or
 * after n characters (n < 0: at the NUL). CR LF and a lone LF are a
 * paragraph's CR; CR CR LF, the soft line break Windows writes, is a space.
 * A single-line control keeps what comes before the first line break.
 * Returns the length written; `out` may be NULL to measure. */
int rich_convert_in(const WCHAR *in, int n, WCHAR *out, int multiline) {
    int k = 0;
    for (int i = 0; n < 0 ? in[i] != 0 : i < n && in[i] != 0; ++i) {
        WCHAR c = in[i];
        const int more = n < 0 ? 1 : 0;
        const int has1 = more ? in[i + 1] != 0 : i + 1 < n && in[i + 1] != 0;
        const int has2 = has1 && (more ? in[i + 2] != 0 : i + 2 < n && in[i + 2] != 0);
        if (c == '\r' || c == '\n') {
            if (!multiline) {
                break;
            }
            if (c == '\r' && has2 && in[i + 1] == '\r' && in[i + 2] == '\n') {
                c = ' ';
                i += 2;
            } else if (c == '\r' && has1 && in[i + 1] == '\n') {
                c = '\r';
                ++i;
            } else {
                c = '\r';
            }
        }
        if (out != 0) {
            out[k] = c;
        }
        ++k;
    }
    return k;
}

/* rich_convert_in into a new block (rich_free it). */
WCHAR *rich_import(const WCHAR *in, int n, int multiline, int *out_length) {
    const int length = in == 0 ? 0 : rich_convert_in(in, n, 0, multiline);
    WCHAR *out = rich_alloc((u32)(length + 1) * sizeof(WCHAR));
    if (out == 0) {
        *out_length = 0;
        return 0;
    }
    if (in != 0) {
        rich_convert_in(in, n, out, multiline);
    }
    out[length] = 0;
    *out_length = length;
    return out;
}

/* The control's text with each CR as CR LF, into at most capacity - 1
 * characters and a terminator. Returns the characters written. */
int rich_crlf_copy(const WCHAR *text, int length, WCHAR *out, int capacity) {
    int k = 0;
    if (capacity <= 0) {
        return 0;
    }
    for (int i = 0; i < length; ++i) {
        if (text[i] == '\r') {
            if (k + 2 > capacity - 1) {
                break;
            }
            out[k++] = '\r';
            out[k++] = '\n';
        } else {
            if (k + 1 > capacity - 1) {
                break;
            }
            out[k++] = text[i];
        }
    }
    out[k] = 0;
    return k;
}

/* ---- Word breaks ------------------------------------------------------------------------- */

#define CLASS_WORD 0x00
#define CLASS_PUNCT 0x01
#define CLASS_HYPHEN 0x41 /* WBF_BREAKAFTER | 1 */
#define CLASS_SPACE 0x32  /* WBF_BREAKLINE | WBF_ISWHITE | 2 */
#define CLASS_EOP 0x14    /* WBF_ISWHITE | 4 */

static int is_punct(WCHAR c) {
    return (c >= '!' && c <= '/') || (c >= ':' && c <= '@') || (c >= '[' && c <= '`') || (c >= '{' && c <= '~');
}

/* What EM_FINDWORDBREAK's WB_CLASSIFY answers: the document's final
 * paragraph mark classifies as a CR, and anything after it as a word. */
int rich_word_class(const rich *r, int cp) {
    if (cp < 0 || cp > r->length) {
        return CLASS_WORD;
    }
    if (cp == r->length) {
        return CLASS_EOP;
    }
    const WCHAR c = r->text[cp];
    if (c == '\r') {
        return CLASS_EOP;
    }
    if (c == ' ' || c == '\t') {
        return CLASS_SPACE;
    }
    if (c == '-') {
        return CLASS_HYPHEN;
    }
    return is_punct(c) ? CLASS_PUNCT : CLASS_WORD;
}

static int is_space_at(const rich *r, int cp) {
    return cp >= 0 && cp < r->length && rich_word_class(r, cp) == CLASS_SPACE;
}

static int group_of(int word_class) {
    return word_class & 0x0f;
}

/* Where a line may break: the start of a run of spaces, a CR, and the
 * position after a hyphen. */
static int is_break(const rich *r, int b) {
    if (b < 0 || b >= r->length) {
        return 0;
    }
    const int c = rich_word_class(r, b);
    if (c == CLASS_EOP) {
        return 1;
    }
    if (c == CLASS_SPACE && !is_space_at(r, b - 1)) {
        return 1;
    }
    return b > 0 && (rich_word_class(r, b - 1) & 0x40) != 0;
}

/* The start of a word that follows spaces. */
static int is_word_after_space(const rich *r, int b) {
    return b > 0 && b < r->length && !is_space_at(r, b) && rich_word_class(r, b) != CLASS_EOP && is_space_at(r, b - 1);
}

static int is_hyphen_break(const rich *r, int b) {
    return b > 0 && b <= r->length && (rich_word_class(r, b - 1) & 0x40) != 0;
}

int rich_find_word_break(const rich *r, int code, int cp) {
    const int n = r->length;
    switch (code) {
    case WB_ISDELIMITER:
        return is_space_at(r, cp);
    case WB_CLASSIFY:
        return rich_word_class(r, cp);
    case WB_MOVEWORDRIGHT: {
        if (cp >= n) {
            return n + 1;
        }
        if (cp < 0) {
            cp = 0;
        }
        const int c = rich_word_class(r, cp);
        if (c == CLASS_EOP) {
            return cp + 1;
        }
        if (c != CLASS_SPACE) {
            const int g = group_of(c);
            while (cp < n && rich_word_class(r, cp) != CLASS_SPACE && rich_word_class(r, cp) != CLASS_EOP &&
                   group_of(rich_word_class(r, cp)) == g) {
                ++cp;
            }
        }
        while (is_space_at(r, cp)) {
            ++cp;
        }
        return cp;
    }
    case WB_MOVEWORDLEFT: {
        if (cp > n + 1) {
            cp = n + 1;
        }
        int p = cp;
        while (p > 0 && is_space_at(r, p - 1)) {
            --p;
        }
        if (p > 0) {
            const int c = rich_word_class(r, p - 1);
            if (c == CLASS_EOP) {
                return p - 1;
            }
            const int g = group_of(c);
            while (p > 0 && !is_space_at(r, p - 1) && rich_word_class(r, p - 1) != CLASS_EOP &&
                   group_of(rich_word_class(r, p - 1)) == g) {
                --p;
            }
        }
        return p;
    }
    case WB_LEFT:
    case WB_LEFTBREAK: {
        if (cp > n) {
            return cp;
        }
        int best = 0;
        for (int b = cp; b > 0; --b) {
            if (is_break(r, b)) {
                best = b;
                break;
            }
        }
        if (code == WB_LEFT) {
            for (int b = cp - 1; b > best; --b) {
                if (is_word_after_space(r, b)) {
                    best = b;
                    break;
                }
            }
        }
        return best;
    }
    case WB_RIGHT:
    case WB_RIGHTBREAK:
        if (cp == 0) {
            return 0;
        }
        if (cp >= n) {
            return n + 1;
        }
        for (int b = cp + 1; b < n; ++b) {
            if (code == WB_RIGHTBREAK && (is_break(r, b) || is_word_after_space(r, b))) {
                return b;
            }
            if (code == WB_RIGHT &&
                (is_word_after_space(r, b) || is_hyphen_break(r, b) || rich_word_class(r, b) == CLASS_EOP)) {
                return b;
            }
        }
        return n + 1;
    default:
        return 0;
    }
}

/* Replaces [cp, cp + remove) with insert. The caller has checked the
 * range; on a failed allocation nothing changes. */
void rich_splice(rich *r, int cp, int remove, const WCHAR *insert, int insert_length) {
    const int length = r->length - remove + insert_length;
    if (!rich_reserve(r, length)) {
        return;
    }
    if (insert_length != remove) {
        if (insert_length > remove) {
            for (int i = r->length - 1; i >= cp + remove; --i) {
                r->text[i + insert_length - remove] = r->text[i];
            }
        } else {
            for (int i = cp + remove; i < r->length; ++i) {
                r->text[i + insert_length - remove] = r->text[i];
            }
        }
    }
    for (int i = 0; i < insert_length; ++i) {
        r->text[cp + i] = insert[i];
    }
    r->length = length;
    r->text[length] = 0;
}
