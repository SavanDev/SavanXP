/* The UTF-8 decoder of the text paths.
 *
 * It is a .inc rather than a .c so that its consumers include it the way they
 * include the font tables: the platform's text primitives (pixels.c) and the
 * host tests (tests/host/gfx2d_test.cpp), which compile it without linking the
 * runtime. One copy of the algorithm, however many binaries.
 *
 * It exists because the glyph tables are indexed by CODEPOINT (sx_noto_glyph,
 * sx_console_glyph) while the system's strings are UTF-8. Without it every
 * byte was taken as a glyph and an accented "o" came out as two.
 */
#ifndef SX_GFX_UTF8_INC
#define SX_GFX_UTF8_INC

/* Returns the codepoint at *cursor and moves the cursor past it.
 *
 * An invalid sequence consumes ONE byte and returns U+FFFD. Consuming one is
 * what guarantees that every loop over the string advances: garbage text shows
 * replacement characters, never a hang. */
static unsigned int sx_utf8_next(const char** cursor) {
    const unsigned char* s = (const unsigned char*)*cursor;
    unsigned int cp;
    int extra;
    int i;

    if (s[0] < 0x80u) {
        *cursor += 1;
        return s[0];
    }
    if ((s[0] & 0xE0u) == 0xC0u) { cp = s[0] & 0x1Fu; extra = 1; }
    else if ((s[0] & 0xF0u) == 0xE0u) { cp = s[0] & 0x0Fu; extra = 2; }
    else if ((s[0] & 0xF8u) == 0xF0u) { cp = s[0] & 0x07u; extra = 3; }
    else { *cursor += 1; return 0xFFFDu; }

    for (i = 1; i <= extra; ++i) {
        /* A continuation byte that is not one ends the sequence here. It is
         * not consumed: it may be the legitimate start of the next character. */
        if ((s[i] & 0xC0u) != 0x80u) {
            *cursor += 1;
            return 0xFFFDu;
        }
        cp = (cp << 6) | (unsigned int)(s[i] & 0x3Fu);
    }
    *cursor += extra + 1;
    return cp;
}

#endif /* SX_GFX_UTF8_INC */
