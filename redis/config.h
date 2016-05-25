#ifndef BYTE_ORDER
#ifdef __BYTE_ORDER
#if defined(__LITTLE_ENDIAN) && defined(__BIG_ENDIAN)
#ifndef LITTLE_ENDIAN
#define LITTLE_ENDIAN __LITTLE_ENDIAN
#endif // !LITTLE_ENDIAN
#ifndef BIG_ENDIAN
#define BIG_ENDIAN __BIG_ENDIAN
#endif // !BIG_ENDIAN
#if (__BYTE_ORDER == __LITTLE_ENDIAN)
#define BYTE_ORDER LITTLE_ENDIAN
#else
#define BYTE_ORDER BIG_ENDIAN
#endif
#endif
#endif
#endif // !BYTE_ORDER

#if !defined(BYTE_ORDER) || (BYTE_ORDER!=BIG_ENDIAN && BYTE_ORDER !=LITTLE_ENDIAN)
#error "Undefined or invalid BYTE_ORDER"
#endif