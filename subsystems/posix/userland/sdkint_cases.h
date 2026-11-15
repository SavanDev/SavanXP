#pragma once
#include <errno.h>
#include <inttypes.h>
#include <limits.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

_Static_assert(sizeof(long) == 4 && sizeof(void*) == 4 && sizeof(size_t) == 4, "i386 data model");
_Static_assert(sizeof(intmax_t) == 8 && sizeof(int64_t) == 8, "wide integer types");
_Static_assert(sizeof(imaxdiv_t) == 16 && offsetof(imaxdiv_t, rem) == 8, "imaxdiv return layout");
_Static_assert(LONG_MAX == 2147483647L && LONG_MIN == (-2147483647L - 1), "long limits");
_Static_assert(ULONG_MAX == 4294967295UL && LLONG_MAX == INT64_MAX, "unsigned and wide limits");
#define SDK_CHECK(x) do { if (!(x)) { printf("SDK integer failure at line %d\n", __LINE__); return 1; } } while (0)

static int sdk_parse_cases(void) {
    char* end;
    const struct { const char* text; int base; int64_t value; const char* rest; int error; } signed_cases[] = {
        {"0",0,0,"",0}, {"-0",0,0,"",0}, {"  +123tail",10,123,"tail",0},
        {"077!",0,63,"!",0}, {"0x7f!",0,127,"!",0}, {"0X7f",16,127,"",0},
        {"0x",0,0,"x",0}, {"0xg",16,0,"xg",0}, {"09",0,0,"9",0},
        {"zZ",36,1295,"",0}, {"101010",2,42,"",0},
        {"4294967297!",10,INT64_C(4294967297),"!",0},
        {"9223372036854775807",10,INT64_MAX,"",0},
        {"-9223372036854775808",10,INT64_MIN,"",0},
        {"9223372036854775808999end",10,INT64_MAX,"end",ERANGE},
        {"-9223372036854775809!",10,INT64_MIN,"!",ERANGE},
        {"+",10,0,"+",EINVAL}, {"   no",10,0,"   no",EINVAL}, {"",10,0,"",EINVAL},
        {"10",1,0,"10",EINVAL}, {"10",37,0,"10",EINVAL}, {"10",-2,0,"10",EINVAL},
    };
    for (unsigned i = 0; i < sizeof(signed_cases)/sizeof(signed_cases[0]); ++i) {
        errno = EDOM;
        SDK_CHECK(strtoll(signed_cases[i].text, &end, signed_cases[i].base) == signed_cases[i].value);
        SDK_CHECK(strcmp(end,signed_cases[i].rest) == 0 && errno == (signed_cases[i].error ? signed_cases[i].error : EDOM));
        errno = EDOM;
        SDK_CHECK(strtoimax(signed_cases[i].text, &end, signed_cases[i].base) == signed_cases[i].value);
        SDK_CHECK(strcmp(end,signed_cases[i].rest) == 0 && errno == (signed_cases[i].error ? signed_cases[i].error : EDOM));
    }
    const struct { const char* text; uint64_t value; const char* rest; int error; } unsigned_cases[] = {
        {"0",0,"",0}, {"+4294967297",UINT64_C(4294967297),"",0},
        {"18446744073709551615",UINT64_MAX,"",0}, {"-1",UINT64_MAX,"",0},
        {"-18446744073709551615",1,"",0},
        {"18446744073709551616!",UINT64_MAX,"!",ERANGE},
        {"-18446744073709551616x",UINT64_MAX,"x",ERANGE},
    };
    for (unsigned i = 0; i < sizeof(unsigned_cases)/sizeof(unsigned_cases[0]); ++i) {
        errno = EDOM;
        SDK_CHECK(strtoull(unsigned_cases[i].text,&end,10) == unsigned_cases[i].value);
        SDK_CHECK(strcmp(end,unsigned_cases[i].rest) == 0 && errno == (unsigned_cases[i].error ? unsigned_cases[i].error : EDOM));
        errno = EDOM;
        SDK_CHECK(strtoumax(unsigned_cases[i].text,&end,10) == unsigned_cases[i].value);
        SDK_CHECK(strcmp(end,unsigned_cases[i].rest) == 0 && errno == (unsigned_cases[i].error ? unsigned_cases[i].error : EDOM));
    }
    errno = 0; SDK_CHECK(strtol("2147483647",&end,10) == LONG_MAX && !errno && !*end);
    errno = 0; SDK_CHECK(strtol("2147483648!",&end,10) == LONG_MAX && errno == ERANGE && *end == '!');
    errno = 0; SDK_CHECK(strtol("-2147483648",&end,10) == LONG_MIN && !errno);
    errno = 0; SDK_CHECK(strtol("-2147483649",&end,10) == LONG_MIN && errno == ERANGE);
    errno = 0; SDK_CHECK(strtoul("4294967295",&end,10) == ULONG_MAX && !errno);
    errno = 0; SDK_CHECK(strtoul("4294967296",&end,10) == ULONG_MAX && errno == ERANGE);
    errno = 0; SDK_CHECK(strtoul(" -1",&end,10) == ULONG_MAX && !errno);
    SDK_CHECK(atoll("4294967297") == INT64_C(4294967297));
    SDK_CHECK(imaxabs(-INT64_C(4294967297)) == INT64_C(4294967297));
    imaxdiv_t div = imaxdiv(-INT64_C(4294967303),3);
    SDK_CHECK(div.quot == -INT64_C(1431655767) && div.rem == -2);
    div = imaxdiv(INTMAX_MIN,3);
    SDK_CHECK(div.quot == -INT64_C(3074457345618258602) && div.rem == -2);
    // Exercise every supported base above 32 bits, using independently built digits.
    for (int base = 2; base <= 36; ++base) {
        const uint64_t expected = UINT64_C(0xfedcba9876543210);
        uint64_t left = expected;
        char reversed[65], text[65];
        unsigned length = 0;
        do { unsigned digit = left % (unsigned)base; reversed[length++] = "0123456789abcdefghijklmnopqrstuvwxyz"[digit]; left /= (unsigned)base; } while (left);
        for (unsigned j = 0; j < length; ++j) text[j] = reversed[length-1-j];
        text[length] = 0; errno = 0;
        SDK_CHECK(strtoumax(text,&end,base) == expected && !*end && !errno);
    }
    return 0;
}

// Compare parsed values with the original inputs and guard bytes. A formatter
// and scanner that both truncate to 32 bits cannot accidentally pass this test.
#define SDK_ROUNDTRIP(type, print_format, scan_format, expected) do { \
    struct { uint64_t before; type value; unsigned char after[8]; } slot; \
    _Static_assert(offsetof(__typeof__(slot),after) == offsetof(__typeof__(slot),value)+sizeof(type), "adjacent guards"); \
    memset(&slot,0xa5,sizeof(slot)); \
    char text[128]; unsigned tail=0; \
    SDK_CHECK(snprintf(text,sizeof(text),"%" print_format "|%u",(type)(expected),77u) > 0); \
    SDK_CHECK(sscanf(text,"%" scan_format "|%u",&slot.value,&tail) == 2); \
    SDK_CHECK(slot.value == (type)(expected) && tail == 77 && slot.before == UINT64_C(0xa5a5a5a5a5a5a5a5)); \
    for (unsigned guard=0;guard<sizeof(slot.after);++guard) SDK_CHECK(slot.after[guard] == 0xa5); \
} while (0)

static int sdk_format_cases(void) {
    char text[256];
    int count = snprintf(text,sizeof(text),"%lld|%d|%llu|%u",LLONG_MIN,17,ULLONG_MAX,23u);
    SDK_CHECK(count == 47 && strcmp(text,"-9223372036854775808|17|18446744073709551615|23") == 0);
    SDK_CHECK(snprintf(text,sizeof(text),"%jd %ju %td %zu",INTMAX_MIN,UINTMAX_MAX,(ptrdiff_t)-7,(size_t)19) > 0);
    SDK_CHECK(strcmp(text,"-9223372036854775808 18446744073709551615 -7 19") == 0);
    SDK_CHECK(snprintf(text,sizeof(text),"%016llx %.20llu %hhd %hhu %hd %hu",UINT64_C(0x123456789abcdef0),UINT64_C(4294967297),(int)257,(unsigned)257,(int)65537,(unsigned)65537) > 0);
    SDK_CHECK(strcmp(text,"123456789abcdef0 00000000004294967297 1 1 1 1") == 0);
    intmax_t consumed = -1; ptrdiff_t consumed_t = -1; __PTRDIFF_TYPE__ consumed_z = -1;
    SDK_CHECK(snprintf(text,sizeof(text),"abc%jn%tn%zn",&consumed,&consumed_t,&consumed_z) == 3);
    SDK_CHECK(consumed == 3 && consumed_t == 3 && consumed_z == 3);
    long long wide; unsigned long long u; int sentinel=0;
    SDK_CHECK(sscanf("-9223372036854775808 18446744073709551615 91","%lld %llu %d",&wide,&u,&sentinel) == 3);
    SDK_CHECK(wide == LLONG_MIN && u == ULLONG_MAX && sentinel == 91);
    consumed = -1; consumed_t = -1; consumed_z = -1;
    SDK_CHECK(sscanf("12345!","%*5lld%jn%tn%zn",&consumed,&consumed_t,&consumed_z) == 0);
    SDK_CHECK(consumed == 5 && consumed_t == 5 && consumed_z == 5);
    SDK_CHECK(sscanf("1234567890123!","%10lld%n",&wide,&sentinel) == 1 && wide == INT64_C(1234567890) && sentinel == 10);
    void* pointer = 0;
    SDK_CHECK(sscanf("0x12345678", "%p", &pointer) == 1 && (uintptr_t)pointer == UINT32_C(0x12345678));
    SDK_CHECK(snprintf(text,sizeof(text),"%p",pointer) == 10 && strcmp(text,"0x12345678") == 0);
    char tiny[7];
    SDK_CHECK(snprintf(tiny,sizeof(tiny),"%" PRIu64,UINT64_MAX) == 20 && strcmp(tiny,"184467") == 0);
    SDK_ROUNDTRIP(int8_t,PRId8,SCNd8,INT8_MIN);
    SDK_ROUNDTRIP(int16_t,PRId16,SCNd16,INT16_MIN);
    SDK_ROUNDTRIP(int32_t,PRId32,SCNd32,INT32_MIN);
    SDK_ROUNDTRIP(int64_t,PRId64,SCNd64,INT64_MIN);
    SDK_ROUNDTRIP(uint8_t,PRIu8,SCNu8,UINT8_MAX);
    SDK_ROUNDTRIP(uint16_t,PRIu16,SCNu16,UINT16_MAX);
    SDK_ROUNDTRIP(uint32_t,PRIu32,SCNu32,UINT32_MAX);
    SDK_ROUNDTRIP(uint64_t,PRIu64,SCNu64,UINT64_MAX);
    SDK_ROUNDTRIP(int_least8_t,PRIdLEAST8,SCNdLEAST8,INT_LEAST8_MIN);
    SDK_ROUNDTRIP(int_least16_t,PRIdLEAST16,SCNdLEAST16,INT_LEAST16_MIN);
    SDK_ROUNDTRIP(int_least32_t,PRIdLEAST32,SCNdLEAST32,INT_LEAST32_MIN);
    SDK_ROUNDTRIP(int_least64_t,PRIdLEAST64,SCNdLEAST64,INT_LEAST64_MIN);
    SDK_ROUNDTRIP(uint_least64_t,PRIuLEAST64,SCNuLEAST64,UINT_LEAST64_MAX);
    SDK_ROUNDTRIP(int_fast8_t,PRIdFAST8,SCNdFAST8,INT_FAST8_MIN);
    SDK_ROUNDTRIP(int_fast16_t,PRIdFAST16,SCNdFAST16,INT_FAST16_MIN);
    SDK_ROUNDTRIP(int_fast32_t,PRIdFAST32,SCNdFAST32,INT_FAST32_MIN);
    SDK_ROUNDTRIP(int_fast64_t,PRIdFAST64,SCNdFAST64,INT_FAST64_MIN);
    SDK_ROUNDTRIP(uint_fast64_t,PRIuFAST64,SCNuFAST64,UINT_FAST64_MAX);
    SDK_ROUNDTRIP(intmax_t,PRIdMAX,SCNdMAX,INTMAX_MIN);
    SDK_ROUNDTRIP(uintmax_t,PRIuMAX,SCNuMAX,UINTMAX_MAX);
    SDK_ROUNDTRIP(intptr_t,PRIdPTR,SCNdPTR,INTPTR_MIN);
    SDK_ROUNDTRIP(uintptr_t,PRIuPTR,SCNuPTR,UINTPTR_MAX);
    SDK_ROUNDTRIP(uint64_t,PRIx64,SCNx64,UINT64_MAX);
    SDK_ROUNDTRIP(uint64_t,PRIX64,SCNx64,UINT64_C(0x1234abcdef7890));
    SDK_ROUNDTRIP(uint64_t,PRIo64,SCNo64,UINT64_MAX);
    SDK_ROUNDTRIP(int64_t,PRIi64,SCNi64,INT64_MIN);
    return 0;
}
static int sdk_integer_cases(void) {
    if (sdk_parse_cases() || sdk_format_cases()) return 1;
    return 0;
}
