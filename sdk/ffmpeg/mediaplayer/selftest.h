#pragma once

/* Modos sin ventana del reproductor (selftest.c). */
int selftest_main(int argc, char** argv);
int probe_main(const char* path);
int gpu_hold_main(unsigned long hold_ms, const char* path);
