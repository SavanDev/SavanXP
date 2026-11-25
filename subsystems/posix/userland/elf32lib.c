// Relocation fixture: initialized pointers have implicit nonzero REL addends,
// mutable globals must remain private, and the executable supplies a callback.
extern int elf32_callback(void);
extern int elf32_absent(void) __attribute__((weak));
int elf32_data[2] = {11, 37};
int* elf32_second = elf32_data + 1;
static int counter;
int elf32_probe(void) {
    if (elf32_absent || *elf32_second != 37) return -100;
    return ++counter + elf32_callback();
}
// Force a PC-relative data relocation, independent of compiler code generation.
__asm__(".data\n.global elf32_pc\nelf32_pc:\n.long elf32_callback - .\n");
