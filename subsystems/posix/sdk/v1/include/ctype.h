#pragma once

#define isspace sx_isspace
#define isprint sx_isprint
#define isdigit sx_isdigit
#define isalpha sx_isalpha
#define isalnum sx_isalnum
#define islower sx_islower
#define isupper sx_isupper
#define isxdigit sx_isxdigit
#define tolower sx_tolower
#define toupper sx_toupper

int sx_isspace(int character);
int sx_isprint(int character);
int sx_isdigit(int character);
int sx_isalpha(int character);
int sx_isalnum(int character);
int sx_islower(int character);
int sx_isupper(int character);
int sx_isxdigit(int character);
int sx_tolower(int character);
int sx_toupper(int character);
