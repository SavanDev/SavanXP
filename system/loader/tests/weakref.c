/* A library with a weak reference nothing defines (ldfailtest): the loader has
 * to load it and resolve the reference to zero, as ELF says, rather than fail
 * the load over a symbol the code is prepared to do without. */

extern int sx_weak_nobody_defines(void) __attribute__((weak));
int weakref_probe(void);

int weakref_probe(void) {
    return sx_weak_nobody_defines == 0 ? 1 : 2;
}
