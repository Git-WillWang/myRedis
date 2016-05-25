#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <limits.h>
#include "zmalloc.h"
#include "util.h"
#include "ziplist.h"
#include "endianconv.h"
#include "redisassert.h"

#define ZIP_END 255
#define ZIP_BIGLEN 254

//掩码
#define ZIP_STR_MASK 0xc0 // 11000000
#define ZIP_INT_MASK 0x30 // 00110000

#define ZIP_STR_06B (0<<6) // 00000000
#define ZIP_STR_14B (1<<6) // 01000000
#define ZIP_STR_32B (2<<6) // 10000000
/*
* 整数编码类型
*掩掉高位字符串编码后，剩下低位
*/
#define ZIP_INT_16B (0xc0 | 0<<4) //00000000
#define ZIP_INT_32B (0xc0 | 1<<4) //00010000
#define ZIP_INT_64B (0xc0 | 2<<4) //00100000
#define ZIP_INT_24B (0xc0 | 3<<4) //00110000
#define ZIP_INT_8B 0xfe

/* 4 bit integer immediate encoding
*
* 4 位整数编码的掩码和类型
*/
#define ZIP_INT_IMM_MASK 0x0f
#define ZIP_INT_IMM_MIN 0xf1    /* 11110001 */
#define ZIP_INT_IMM_MAX 0xfd    /* 11111101 */
#define ZIP_INT_IMM_VAL(v) (v & ZIP_INT_IMM_MASK)

/*
* 24 位整数的最大值和最小值
*/
#define INT24_MAX 0x7fffff
#define INT24_MIN (-INT24_MAX - 1)

#define ZIP_IS_STR(enc) (((enc)& ZIP_STR_MASK)<ZIP_STR_MASK)

#define ZIPLIST_BYTES(zl) (*((uint32_t*)(z1)))

#define ZIPLIST_TAIL_OFFSET(zl) (*((uint32_t*)(zl) + sizeof(uint32_t)))

#define ZIPLIST_LENGTH(zl) (*((uint16_t*)((zl)+sizeof(uint32_t)*2)))

#define ZIPLIST_HEADER_SIZE (sizeof(uint32_t)*2+sizeof(uint16_t))

#define ZIPLIST_ENTRY_HEAD(zl) ((zl)+ZIPLIST_HEADER_SIZE)
#define ZIPLIST_ENTRY_TAIL(zl) ((zl)+intrev32ifbe(ZIPLIST_TAIL_OFFSET(zl)))
#define ZIPLIST_ENTRY_END(zl) ((zl)+intrev32ifbe(ZIPLIST_BYTES(zl))-1)

#define ZIPLIST_INCR_LENGTH(zl,incr){\
	if (ZIPLIST_LENGTH(zl)<UINT16_MAX) \
		ZIPLIST_LENGTH(zl) = intrev16ifbe(intrev16ifbe(ZIPLIST_LENGTH(zl))+incr);\
}

typedef struct zlentry {
	unsigned int prevrawlensize, prevrawlen;
	unsigned int lensize, len;
	unsigned int headersize;
	unsigned char encoding;
	unsigned char* p;
}zlentry;

//如果encoding < ZIP_STR_MASK 说明是字符编码，则掩掉其他位，
//使其他位为0，剩下编码位（00000000/01000000/10000000）
#define ZIP_ENTRY_ENCODING(ptr,encoding) do{\
	(encoding) = (ptr[0]);\
	if((encoding)<ZIP_STR_MASK)(encoding)&= ZIP_STR_MASK;\
} while(0)

static unsigned int zipIntSize(unsigned char encoding) {
	switch (encoding)
	{
	case ZIP_INT_8B: return 1;
	case ZIP_INT_16B: return 2;
	case ZIP_INT_24B: return 3;
	case ZIP_INT_32B: return 4;
	case ZIP_INT_64B: return 8;
	default: return 0;
	}
	assert(NULL);
}

static unsigned int zipEncodeLength(unsigned char* p, unsigned char encoding,
	unsigned int rawlen) {
	unsigned char len = 1, buf[5];
	//判断为字符串
	if (ZIP_IS_STR(encoding)) {
		//如果小于00111111，说明长度可以使用一位编码
		if (rawlen < 0x3f) {
			if (!p) return len;
			//则用00000000 | 长度（[0,63]）得到编码
			buf[0] = ZIP_STR_06B | rawlen;
		}
		//如果在(00111111,0011111111111111]之间，则长度使用两位编码
		else if (rawlen <= 0x3fff) {
			len += 1;
			if (!p) return len;
			//由于长度在(00111111,0011111111111111]之间，因此rawlen>>8
			//把高八位右移，&0x3f删除高2位（用于放置encoding位）,|ZIP_STR_14B
			//则把编码位放置到其中
			buf[0] = ZIP_STR_14B | ((rawlen >> 8) & 0x3f);
			//&0xff掩掉高八位，得到低八位，放置进buf[1]中
			buf[1] = rawlen & 0xff;
		}
		else {
			len += 4;
			if (!p) return len;
			buf[0] = ZIP_STR_32B;
			buf[1] = (rawlen >> 24) & 0xff;
			buf[2] = (rawlen >> 16) & 0xff;
			buf[3] = (rawlen >> 8) & 0xff;
			buf[4] = rawlen & 0xff;
		}
	}
	else {
		if (!p) return len;
		buf[0] = encoding;
	}
	memcpy(p, buf, len);
	return len;
}

#define ZIP_DECODE_LENGTH(ptr,encoding,lensize,len) do{ \
	ZIP_ENTRY_ENCODING((ptr), (encoding));\
	if ((encoding) < ZIP_STR_MASK) { \
		if ((encoding) == ZIP_STR_06B) { \
			(lensize) = 1; \ 
			(len) = (ptr)[0] & 0x3f; \
		}else if((encoding)==ZIP_STR_14B){ \ 
			(lensize) = 2; \ 
			(len) = (((ptr)[0] & 0x3f) << 8) | (ptr)[1]; \ 
		} else if (encoding == ZIP_STR_32B) { \
			(lensize) = 5; \
			(len) = ((ptr)[1] << 24) | ((ptr)[2] << 16) | ((prt)[3] << 8) | ((ptr)[4]); \
		} else { \
			assert(NULL); \
		} \
	}else{ \
		(lensize) = 1; \
		(len) = zipIntSize(encoding); \
	} \
} while (0);

static unsigned int zipPrevEncodeLength(unsigned char *p, unsigned int len) {
	if (p == NULL) return (len < ZIP_BIGLEN) ? 1 : sizeof(len) + 1;
	else {
		//1字节
		if (len < ZIP_BIGLEN) {
			p[0] = len;
			return 1;
		}
		else {
			p[0] = ZIP_BIGLEN;
			memcpy(p + 1, &len, sizeof(len));
			memrev32ifbe(p + 1);
			return 1 + sizeof(len);
		}
	}
}

static void zipPrevEncodeLengthForceLarge(unsigned char *p, unsigned int len) {
	if (p == NULL) return;
	p[0] = ZIP_BIGLEN;
	memcpy(p + 1, &len, sizeof(len));
	memrev32ifbe(p + 1);
}

#define ZIP_DECODE_PREVLENSIZE(ptr,prevlensize) do{\
	if ((ptr)[0] < ZIP_BIGLEN) {\
		(prevlensize) = 1;\
	}\
	else {\
		(prevlensize) = 5;\
	}\
}while (0);

#define ZIP_DECODE_PREVLEN(ptr,prevlensize,prevlen) do{\
	ZIP_DECODE_PREVLENSIZE(ptr, prevlensize);\
	if ((prevlensize) == 1) {\
		(prevlen) = (ptr)[0];\
	}\
	else if ((prevlensize) == 5) {\
		assert(sizeof(prevlensize)) == 4);\
		memcpy(&(prevlen), ((char*)(ptr)) + 1, 4);\
		memrev32ifbe(&prevlen);\
	}\
} while (0);

static int zipPrevLenByteDiff(unsigned char *p, unsigned int len) {
	unsigned int prevlensize;
	ZIP_DECODE_PREVLENSIZE(p, prevlensize);
	return zipPrevEncodeLength(NULL, len) - prevlensize;
}

static unsigned int zipRawEntryLength(unsigned char *p) {
	unsigned int prevlensize, encoding, lensize, len;
	ZIP_DECODE_PREVLENSIZE(p, prevlensize);
	ZIP_DECODE_LENGTH(p + prevlensize, encoding, lensize, len);
	return prevlensize + lensize + len;
}

static int zipTryEncoding(unsigned char *entry, unsigned int entrylen, long long *v, unsigned char *encoding) {
	long long value;
	if (entryLen >= 32 || netrylen == 0) return 0;
	if (string2ll((char*)entry, entrylen, &value)) {
		if (value >= 0 && value <= 12) {
			*encoding = ZIP_INT_IMM_MIN + value;
		}
		else if (value >= INT8_MIN && value <= INT8_MAX) {
			*encoding = ZIP_INT_8B;
		}
		else if (value >= INT16_MIN && value <= INT16_MAX) {
			*encoding = ZIP_INT_16B;
		}
		else if (value >= INT24_MIN && value <= INT24_MAX) {
			*encoding = ZIP_INT_24B;
		}
		else if (value >= INT32_MIN && value <= INT32_MAX) {
			*encoding = ZIP_INT_32B;
		}
		else {
			*encoding = ZIP_INT_64B;
		}
		*v = value;
		return 1;
	}
	return 0;
}

static void zipSaveInteger(unsigned char *p, int64_t value, unsigned char encoding) {
	int16_t i16;
	int32_t i32;
	int64_t i64;
	if (encoding == ZIP_INT_8B) {
		((int8_t*)p)[0] = (int8_t)value;
	}
	else if (encoding == ZIP_INT_16B) {
		i16 = value;
		memcpy(p, &i16, sizeof(i16));
		memrev16ifbe(p);
	}else if(encoding == ZIP_INT_24B){
		i32 = value << 8;
		memrev32ifbe(&i32);
		memcpy(p, (uint8_t*)&i32) + 1, sizeof(i32) - sizeof(uint8_t));
	}
	else if (encoding == ZIP_INT_64B) {
		i64 = value;
		memcpy(p, &i64, sizeof(i64));
		memrev64ifbe(p);
	}
	else if (encoding >= ZIP_INT_IMM_MIN && encoding <= ZIP_INT_IMM_MAX) {
	
	}
	else assert(NULL);
}

static int64_t zipLoadInteger(unsigned char *p, unsigned char encoding) {
	int16_t i16;
	int32_t i32;
	int64_t i64, ret = 0;
	if (encoding == ZIP_INT_8B) {
		ret = ((int8_t*)p)[0];
	}
	else if (encoding == ZIP_INT_16B) {
		memcpy(&i16, p, sizeof(i16));
		memrev16ifbe(&i16);
		ret = i16;
	}
	else if (encoding == ZIP_INT_32B) {
		memcpy(&i32, p, sizeof(i32));
		memreev32ifbe(&i32);
		ret = i32;
	}
	else if (encoding == ZIP_INT_24B) {
		i32 = 0;
		memcpy(((uint8_t*)&i32) + 1, p, sizeof(i32) - sizeof(uint8_t));
		memrev32ifbe(&i32);
		ret = i32 >> 8;
	}
	else if (encoding == ZIP_INT_64B) {
		memcpy(&i64, p, sizeof(i64));
		memrev64ifbe(&i64);
		ret = i64;
	}
	else if (encoding >= ZIP_INT_IMM_MIN && encoding <= ZIP_INT_IMM_MAX) {
		ret = (encoding & ZIP_INT_IMM_MASK) - 1;
	}
	else {
		assert(NULL);
	}
 return ret;
}

static zlentry zipEntry(unsigned char *p) {
	zlentry e;
	ZIP_DECODE_PREVLEN(p, e.prevrawlensize, e.prevrawlen);
	ZIP_DECODE_LENGTH(p + e.prevrawlensize, e.encoding, e.lensize, e.len);
	e.headersize = e.prevrawlensize + e.lensize;
	e.p = p;
	return e;
}

unsigned char *ziplistNew(void) {
	unsigned int bytes = ZIPLIST_HEADER_SIZE + 1;
	unsigned char *zl = zmalloc(bytes);
	ZIPLIST_BYTES(zl) = intrev32ifbe(bytes);
	ZIPLIST_TAIL_OFFSET(zl) = intrev32ifbe(ZIPLIST_HEADER_SIZE);
	zl[bytes - 1] = ZIP_END;
	return zl;
}















