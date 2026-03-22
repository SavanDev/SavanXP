#pragma once

struct passwd {
    char* pw_name;
    char* pw_dir;
};

#define getpwnam sx_getpwnam

struct passwd* sx_getpwnam(const char* name);

