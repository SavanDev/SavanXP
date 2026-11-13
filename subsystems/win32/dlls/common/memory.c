/*
 * memory.c -- memcpy and memset for the DLLs that copy structures: with no C
 * runtime linked, the compiler's own calls for a large struct copy or
 * initializer need them. Not exported.
 */
typedef __SIZE_TYPE__ size_t;

void *memcpy(void *destination, const void *source, size_t count) {
    unsigned char *d = destination;
    const unsigned char *s = source;
    while (count-- != 0) {
        *d++ = *s++;
    }
    return destination;
}

void *memset(void *destination, int value, size_t count) {
    unsigned char *d = destination;
    while (count-- != 0) {
        *d++ = (unsigned char)value;
    }
    return destination;
}
