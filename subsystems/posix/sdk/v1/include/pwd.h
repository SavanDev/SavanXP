#pragma once

struct passwd {
    char* pw_name;
    char* pw_dir;
};


struct passwd* getpwnam(const char* name);

