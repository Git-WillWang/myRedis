#ifndef __REDIS_FMACRO_H
#define __REDIS_FMACRO_H
#define _BSD_SOURCE
#if defined(__LINUX__)
#define _GNU_SOURCE
#endif

#if defined(__LINUX__)||defined(__OpenBSD__)
#define _XOPEN_SOURCE 700
#elif !defined(__NetBSD__)
#define _XOPEN_SOURCE
#endif
#define _LARGEFILE_SOURCE
#define _FILE_OFFSET_BITS 64

#endif