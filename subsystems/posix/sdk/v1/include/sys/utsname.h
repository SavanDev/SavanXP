#pragma once

struct utsname {
    char sysname[65];
    char nodename[65];
    char release[65];
    char version[65];
    char machine[65];
};

int sx_uname(struct utsname* value);

#define uname sx_uname
