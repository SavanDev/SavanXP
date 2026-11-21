/*
 * resource.h -- Finding a resource in a mapped image: what FindResourceEx
 * does, shared by kernel32 (FindResource and friends) and user32 (strings,
 * menus, dialogs, bitmaps, icons, accelerators). Compiled into each DLL
 * that looks resources up; not exported.
 */
#ifndef SXW_DLLS_RESOURCE_H
#define SXW_DLLS_RESOURCE_H

/* Any language: the first one the directory lists. */
#define SXW_ANY_LANGUAGE 0xffffu

/* The IMAGE_RESOURCE_DATA_ENTRY of the resource of this type and name in the
 * module mapped at base, or 0. type and name are ids (below 0x10000) or
 * UTF-16 names compared without case, "#123" meaning id 123. language picks
 * the language entry; one that is not there, or SXW_ANY_LANGUAGE, takes the
 * first. The entry's first word is the data's RVA, its second the size. */
const unsigned char *sxw_resource_entry(unsigned int base, const unsigned short *type,
                                        const unsigned short *name, unsigned int language);

#endif
