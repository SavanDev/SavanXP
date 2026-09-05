/* Decodificador UTF-8 compartido por los caminos de texto.
 *
 * Vive en un .inc y no en un .c porque los dos consumidores son runtimes
 * distintos que no se linkean entre si -- el SDK posix (gfx_impl.inc) y el
 * subsistema nativo (sx_text.c) -- exactamente como ya comparten los datos de
 * la fuente. Una sola copia del algoritmo, dos binarios.
 *
 * Existe porque las tablas de glifos se indexan por CODEPOINT (sx_noto_glyph,
 * sx_console_glyph) mientras que las cadenas del sistema son UTF-8. Sin esto
 * cada byte se tomaba como un glifo y una "o" acentuada salia como dos.
 */
#ifndef SX_GFX_UTF8_INC
#define SX_GFX_UTF8_INC

/* Devuelve el codepoint en *cursor y avanza el cursor mas alla de el.
 *
 * Una secuencia invalida consume UN solo byte y devuelve U+FFFD. Consumir uno
 * es lo que garantiza que todo bucle sobre la cadena avance: con texto basura
 * se ven caracteres de reemplazo, nunca un cuelgue. */
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
        /* Un continuador que no lo es corta la secuencia acá. No se consume:
         * puede ser el arranque legitimo del proximo caracter. */
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
