#pragma once

#define strcasecmp sx_strcasecmp
#define strncasecmp sx_strncasecmp

int sx_strcasecmp(const char* left, const char* right);
int sx_strncasecmp(const char* left, const char* right, unsigned long count);
