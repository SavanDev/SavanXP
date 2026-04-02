#pragma once

#define FNM_NOESCAPE 0x01
#define FNM_PATHNAME 0x02
#define FNM_PERIOD 0x04

int sx_fnmatch(const char* pattern, const char* string, int flags);

#define fnmatch sx_fnmatch
