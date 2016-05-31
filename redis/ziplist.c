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
#define ZIP_INT_16B (0xc0 | 0<<4) //11000000|00000000=11000000 2字节整数
#define ZIP_INT_32B (0xc0 | 1<<4) //11000000|00010000=11010000 4字节整数
#define ZIP_INT_64B (0xc0 | 2<<4) //11000000|00100000=11100000 8字节整数
#define ZIP_INT_24B (0xc0 | 3<<4) //11000000|00110000=11110000 3字节整数
#define ZIP_INT_8B 0xfe //11111110 介于0至12的无符号整数

/* 4 bit integer immediate encoding
*
* 4 位整数编码的掩码和类型
*/
#define ZIP_INT_IMM_MASK 0x0f
#define ZIP_INT_IMM_MIN 0xf1    /* 11110001 */
#define ZIP_INT_IMM_MAX 0xfd    /* 11111101 */
#define ZIP_INT_IMM_VAL(v) (v & ZIP_INT_IMM_MASK) //取4位整数具体的值

/*
* 24 位整数的最大值和最小值
*/
#define INT24_MAX 0x7fffff
#define INT24_MIN (-INT24_MAX - 1)

//判断是否是字符串
#define ZIP_IS_STR(enc) (((enc)& ZIP_STR_MASK)<ZIP_STR_MASK)
// 定位到 ziplist 的 bytes 属性，该属性记录了整个 ziplist 所占用的内存字节数
// 用于取出 bytes 属性的现有值，或者为 bytes 属性赋予新值	
#define ZIPLIST_BYTES(zl) (*((uint32_t*)(zl)))
// 定位到 ziplist 的 offset 属性，该属性记录了到达表尾节点的偏移量
// 用于取出 offset 属性的现有值，或者为 offset 属性赋予新值
#define ZIPLIST_TAIL_OFFSET(zl) (*((uint32_t*)(zl) + sizeof(uint32_t)))
// 定位到 ziplist 的 length 属性，该属性记录了 ziplist 包含的节点数量
// 用于取出 length 属性的现有值，或者为 length 属性赋予新值
#define ZIPLIST_LENGTH(zl) (*((uint16_t*)((zl)+sizeof(uint32_t)*2)))
///返回 ziplist 表头的大小
#define ZIPLIST_HEADER_SIZE (sizeof(uint32_t)*2+sizeof(uint16_t))
// 返回指向 ziplist 第一个节点（的起始位置）的指针
#define ZIPLIST_ENTRY_HEAD(zl) ((zl)+ZIPLIST_HEADER_SIZE)
// 返回指向 ziplist 最后一个节点（的起始位置）的指针
#define ZIPLIST_ENTRY_TAIL(zl) ((zl)+intrev32ifbe(ZIPLIST_TAIL_OFFSET(zl)))
// 返回指向 ziplist 末端 ZIP_END （的起始位置）的指针
#define ZIPLIST_ENTRY_END(zl) ((zl)+intrev32ifbe(ZIPLIST_BYTES(zl))-1)

//增加 ziplist 的节点数
#define ZIPLIST_INCR_LENGTH(zl,incr){\
	if (ZIPLIST_LENGTH(zl)<UINT16_MAX) \
		ZIPLIST_LENGTH(zl) = intrev16ifbe(intrev16ifbe(ZIPLIST_LENGTH(zl))+incr);\
}
//保存 ziplist 节点信息的结构
typedef struct zlentry {
	// prevrawlen ：前置节点的长度
	// prevrawlensize ：编码 prevrawlen 所需的字节大小
	unsigned int prevrawlensize, prevrawlen;
	// len ：当前节点值的长度
	// lensize ：编码 len 所需的字节大小
	unsigned int lensize, len;
	// 当前节点 header 的大小 = prevrawlensize + lensize
	unsigned int headersize;
	// 当前节点值所使用的编码类型	
	unsigned char encoding;
	unsigned char* p;
}zlentry;

//如果encoding < ZIP_STR_MASK 说明是字符编码，则掩掉其他位，
//使其他位为0，剩下编码位（00000000/01000000/10000000）
#define ZIP_ENTRY_ENCODING(ptr,encoding) do{\
	(encoding) = (ptr[0]);\
	if((encoding)<ZIP_STR_MASK) (encoding)&= ZIP_STR_MASK;\
} while(0)

//返回整数编码方式所对应的类型的大小
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

/*根据rawlen长度将rawlen编码进位置p
 if rawlen<=00111111 then 00000000|rawlen
 else if rawlen<= 0011111111111111 then 高八位=(01000000|(rawlen>>8 & 00111111))
 低八位=rawlen&11111111
 else 
*/
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
//反斜线后不能有空格
//根据encoding解出ptr位置上存储的长度len，以及存储len占用的字节数lensize
#define ZIP_DECODE_LENGTH(ptr,encoding,lensize,len) do{\
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
			(len) = ((ptr)[1] << 24) | ((ptr)[2] << 16) | ((ptr)[3] << 8) | ((ptr)[4]); \
		} else { \
			assert(NULL); \
		} \
	}else{ \
		(lensize) = 1; \
		(len) = zipIntSize(encoding); \
	} \
} while (0);
/*对前置节点的长度 len 进行编码，并将它写入到 p 中，
* 然后返回编码 len 所需的字节数量。
*/
static unsigned int zipPrevEncodeLength(unsigned char *p, unsigned int len) {
	if (p == NULL) return (len < ZIP_BIGLEN) ? 1 : sizeof(len) + 1;
	else {
		//如果小于254，则用1字节存储
		if (len < ZIP_BIGLEN) {
			p[0] = len;
			return 1;
		}
		//大于254，则第一字节放置ZIP_BIGLEN，为5字节长度标识，随后的字节放置len
		else {
			p[0] = ZIP_BIGLEN;
			memcpy(p + 1, &len, sizeof(len));
			memrev32ifbe(p + 1);
			return 1 + sizeof(len);
		}
	}
}
//将原本只需要 1 个字节来保存的前置节点长度 len 编码至一个 5 字节长的 header 中。
static void zipPrevEncodeLengthForceLarge(unsigned char *p, unsigned int len) {
	if (p == NULL) return;
	p[0] = ZIP_BIGLEN;
	memcpy(p + 1, &len, sizeof(len));
	memrev32ifbe(p + 1);
}
/*解码 ptr 指针，
 * 取出编码前置节点长度所需的字节数，并将它保存到 prevlensize 变量中。
 */
#define ZIP_DECODE_PREVLENSIZE(ptr,prevlensize) do{\
	if ((ptr)[0] < ZIP_BIGLEN) {\
		(prevlensize) = 1;\
	}\
	else {\
		(prevlensize) = 5;\
	}\
}while (0)

 /* 解码 ptr 指针，
  * 取出编码前置节点长度所需的字节数，
  * 并将这个字节数保存到 prevlensize 中。
  *
  * 然后根据 prevlensize ，从 ptr 中取出前置节点的长度值，
  * 并将这个长度值保存到 prevlen 变量中。
  */
#define ZIP_DECODE_PREVLEN(ptr,prevlensize,prevlen) do{\
	ZIP_DECODE_PREVLENSIZE(ptr, prevlensize);\
	if ((prevlensize) == 1) {\
		(prevlen) = (ptr)[0];\
	}\
	else if ((prevlensize) == 5) {\
		assert((sizeof(prevlensize)) == 4);\
		memcpy(&(prevlen), ((char*)(ptr)) + 1, 4);\
		memrev32ifbe(&prevlen);\
	}\
}while (0)
/*计算编码新的前置节点长度 len 所需的字节数，
* 减去编码 p 原来的前置节点长度所需的字节数之差。
*/
static int zipPrevLenByteDiff(unsigned char *p, unsigned int len) {
	unsigned int prevlensize;
	ZIP_DECODE_PREVLENSIZE(p, prevlensize);
	return zipPrevEncodeLength(NULL, len) - prevlensize;
}
//返回指针 p 所指向的节点占用的字节数总和。
static unsigned int zipRawEntryLength(unsigned char *p) {
	unsigned int prevlensize, encoding, lensize, len;
	ZIP_DECODE_PREVLENSIZE(p, prevlensize);
	ZIP_DECODE_LENGTH(p + prevlensize, encoding, lensize, len);
	return prevlensize + lensize + len;
}

/*检查 entry 中指向的字符串能否被编码为整数。
*
* 如果可以的话，
* 将编码后的整数保存在指针 v 的值中，并将编码的方式保存在指针 encoding 的值中。
*/
static int zipTryEncoding(unsigned char *entry, unsigned int entryLen, long long *v, unsigned char *encoding) {
	long long value;
	//长度超过32位或者为0的忽略
	if (entryLen >= 32 || entryLen == 0) return 0;
	//将字符串转换成long long并保存到value中
	if (string2ll((char*)entry, entryLen, &value)) {
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
//以 encoding 指定的编码方式，将整数值 value 写入到 p
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
		memcpy(p, ((uint8_t*)&i32) + 1, sizeof(i32) - sizeof(uint8_t));
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
/*
以 encoding 指定的编码方式，读取并返回指针 p 中的整数值
*/
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
/*
将 p 所指向的列表节点的信息全部保存到 zlentry 中，并返回该 zlentry 。
*/
static zlentry zipEntry(unsigned char *p) {
	zlentry e;
	ZIP_DECODE_PREVLEN(p, e.prevrawlensize, e.prevrawlen);
	ZIP_DECODE_LENGTH(p + e.prevrawlensize, e.encoding, e.lensize, e.len);
	e.headersize = e.prevrawlensize + e.lensize;
	e.p = p;
	return e;
}

unsigned char *ziplistNew(void) {
	unsigned int bytes = ZIPLIST_HEADER_SIZE + 1;//包含zip_end的空间
	unsigned char *zl = zmalloc(bytes);
	ZIPLIST_BYTES(zl) = intrev32ifbe(bytes);
	//因为zlentry的个数为0，因此header之后便是tail
	ZIPLIST_TAIL_OFFSET(zl) = intrev32ifbe(ZIPLIST_HEADER_SIZE);
	zl[bytes - 1] = ZIP_END;
	return zl;
}

/*
调整 ziplist 的大小为 len 字节。
* 当 ziplist 原有的大小小于 len 时，扩展 ziplist 不会改变 ziplist 原有的元素。
*/
static unsigned char *ziplistResize(unsigned char *zl, unsigned int len) {
	zl = zrealloc(zl, len);
	ZIPLIST_BYTES(zl) = intrev32ifbe(len);
	zl[len - 1] = ZIP_END;//调整大小后调整zip_end
	return zl;
}
/*
对插入p之后的节点进行调整
*/
static unsigned char *__ziplistCascadeUpdate(unsigned char *zl, unsigned char *p) {
	size_t curlen = intrev32ifbe(ZIPLIST_BYTES(zl)), rawlen, rawlensize;
	size_t offset, noffset, extra;
	unsigned char *np;
	zlentry cur, next;

	while (p[0] != ZIP_END) {
		cur = zipEntry(p);
		// 当前节点的长度
		rawlen = cur.headersize + cur.len;
		// 编码当前节点的长度所需的字节数
		rawlensize = zipPrevEncodeLength(NULL, rawlen);
		if (p[rawlen] == ZIP_END) break;
		//取得p节点的下一个节点
		next = zipEntry(p + rawlen);
		//如果下一个节点的prevrawlen等于目前节点所需长度，则说明后续无需再调整
		if (next.prevrawlen == rawlen) break;
		if (next.prevrawlensize < rawlensize) {
			//取得p节点到zl起始位置的距离
			offset = p - zl;
			extra = rawlensize - next.prevrawlensize;//取得需要扩展的长度大小
			zl = ziplistResize(zl, curlen + extra);//扩展所需大小
			p = zl + offset;
			np = p + rawlen;
			noffset = np - zl;
			if ((zl + intrev32ifbe(ZIPLIST_TAIL_OFFSET(zl))) != np) {
				ZIPLIST_TAIL_OFFSET(zl) = intrev32ifbe(intrev32ifbe(ZIPLIST_TAIL_OFFSET(zl)) + extra); 
			}
			memmove(np + rawlensize, np + next.prevrawlensize, curlen - noffset - next.prevrawlensize - 1);
			zipPrevEncodeLength(np, rawlen);
			p += rawlen;
			curlen += extra;
		}else{
			if (next.prevrawlensize > rawlensize)
				zipPrevEncodeLengthForceLarge(p + rawlen, rawlen);
			else zipPrevEncodeLength(p + rawlen, rawlen);
			break;
		}
	}
	return zl;
}

static unsigned char *__ziplistDelete(unsigned char *zl, unsigned char* p, unsigned int num) {
	unsigned int i, totlen, deleted = 0;
	size_t offset;
	int nextdiff = 0;
	zlentry first, tail;
	first = zipEntry(p);
	for (i = 0; p[0] != ZIP_END&&i < num; ++i) {
		p += zipRawEntryLength(p);
		deleted++;
	}
	totlen = p - first.p;
	if (totlen > 0) {
		if (p[0] != ZIP_END) {
			nextdiff = zipPrevLenByteDiff(p, first.prevrawlen);
			p -= nextdiff;
			zipPrevEncodeLength(p, first.prevrawlen);
			ZIPLIST_TAIL_OFFSET(zl) = intrev32ifbe(intrev32ifbe(ZIPLIST_TAIL_OFFSET(zl))-totlen);
			tail = zipEntry(p);
			if (p[tail.headersize + tail.len] != ZIP_END) {
				ZIPLIST_TAIL_OFFSET(zl) = intrev32ifbe(intrev32ifbe(ZIPLIST_TAIL_OFFSET(zl)) + nextdiff);
			}
			memmove(first.p, p, intrev32ifbe(ZIPLIST_BYTES(zl)) - (p - zl) - 1);
		}
		else {
			ZIPLIST_TAIL_OFFSET(zl) = intrev32ifbe((first.p - zl) - first.prevrawlen);
		}
		offset = first.p - zl;
		zl = ziplistResize(zl, intrev32ifbe(ZIPLIST_BYTES(zl)) - totlen + nextdiff);
		ZIPLIST_INCR_LENGTH(zl, -deleted);
		p = zl + offset;
		if (nextdiff != 0)
			zl = __ziplistCascadeUpdate(zl, p);
	}
	return zl;
}
/*根据指针 p 所指定的位置，将长度为 slen 的字符串 s 插入到 zl 中。
*
* 函数的返回值为完成插入操作之后的 ziplist
*/
static unsigned char *__ziplistInsert(unsigned char *zl, unsigned char *p, unsigned char *s, unsigned int slen) {
	size_t curlen = intrev32ifbe(ZIPLIST_BYTES(zl)), reqlen, prevlen;
	size_t offset;
	int nextdiff = 0;
	unsigned char encoding = 0;
	long long value = 123456789;
	zlentry entry, tail;
	if (p[0] != ZIP_END) {
		entry = zipEntry(p);
		prevlen = entry.prevrawlen;
	}
	else {
		unsigned char *ptail = ZIPLIST_ENTRY_TAIL(zl);
		if (ptail[0] != ZIP_END) {
			prevlen = zipRawEntryLength(ptail);
		}
	}
	if (zipTryEncoding(s, slen, &value, &encoding)) {
		reqlen = zipIntSize(encoding);
	}
	else {
		reqlen = slen;
	}
	reqlen += zipPrevEncodeLength(NULL, prevlen);
	reqlen += zipEncodeLength(NULL, encoding, slen);
	nextdiff = (p[0] != ZIP_END) ? zipPrevLenByteDiff(p, reqlen) : 0;
	offset = p - zl;
	zl = ziplistResize(zl, curlen + reqlen + nextdiff);
	p = zl + offset;
	if (p[0] != ZIP_END) {
		memmove(p + reqlen, p - nextdiff, curlen - offset - 1 + nextdiff);
		zipPrevEncodeLength(p + reqlen, reqlen);
		ZIPLIST_TAIL_OFFSET(zl) = intrev32ifbe(intrev32ifbe(ZIPLIST_TAIL_OFFSET(zl)) + reqlen);
		tail = zipEntry(p + reqlen);
		if (p[reqlen + tail.headersize + tail.len] != ZIP_END) {
			ZIPLIST_TAIL_OFFSET(zl) = intrev32ifbe(intrev32ifbe(ZIPLIST_TAIL_OFFSET(zl)) + nextdiff);
		}
	}
	else {
		ZIPLIST_TAIL_OFFSET(zl) = intrev32ifbe(p - zl);
	}
	if (nextdiff != 0) {
		offset = p - zl;
		zl = __ziplistCascadeUpdate(zl, p + reqlen);
		p = zl + offset;
	}
	p += zipPrevEncodeLength(p, prevlen);
	p += zipEncodeLength(p, encoding, slen);
	if (ZIP_IS_STR(encoding)) {
		memcpy(p, s, slen);
	}
	else {
		zipSaveInteger(p, value, encoding);
	}
	ZIPLIST_INCR_LENGTH(zl, 1);
	return zl;
}

unsigned char *ziplistPush(unsigned char *zl, unsigned char *s, unsigned int slen, int where) {
	unsigned char *p;
	p = (where == ZIPLIST_HEAD) ? ZIPLIST_ENTRY_HEAD(zl) : ZIPLIST_ENTRY_END(zl);
	return __ziplistInsert(zl, p, s, slen);
}
unsigned char *ziplistIndex(unsigned char *zl, int index) {

	unsigned char *p;

	zlentry entry;

	// 处理负数索引
	if (index < 0) {

		// 将索引转换为正数
		index = (-index) - 1;

		// 定位到表尾节点
		p = ZIPLIST_ENTRY_TAIL(zl);

		// 如果列表不为空，那么。。。
		if (p[0] != ZIP_END) {

			// 从表尾向表头遍历
			entry = zipEntry(p);
			// T = O(N)
			while (entry.prevrawlen > 0 && index--) {
				// 前移指针
				p -= entry.prevrawlen;
				// T = O(1)
				entry = zipEntry(p);
			}
		}

		// 处理正数索引
	}
	else {

		// 定位到表头节点
		p = ZIPLIST_ENTRY_HEAD(zl);

		// T = O(N)
		while (p[0] != ZIP_END && index--) {
			// 后移指针
			// T = O(1)
			p += zipRawEntryLength(p);
		}
	}

	// 返回结果
	return (p[0] == ZIP_END || index > 0) ? NULL : p;
}

unsigned char *ziplistNext(unsigned char *zl, unsigned char *p) {
	((void)zl);
	if (p[0] == ZIP_END)
		return NULL;
	p += zipRawEntryLength(p);
	if (p[0] == ZIP_END)
		return NULL;
	return p;
}


unsigned char *ziplistPrev(unsigned char *zl, unsigned char *p) {
	zlentry entry;

	/* Iterating backwards from ZIP_END should return the tail. When "p" is
	* equal to the first element of the list, we're already at the head,
	* and should return NULL. */

	// 如果 p 指向列表末端（列表为空，或者刚开始从表尾向表头迭代）
	// 那么尝试取出列表尾端节点
	if (p[0] == ZIP_END) {
		p = ZIPLIST_ENTRY_TAIL(zl);
		// 尾端节点也指向列表末端，那么列表为空
		return (p[0] == ZIP_END) ? NULL : p;

		// 如果 p 指向列表头，那么说明迭代已经完成
	}
	else if (p == ZIPLIST_ENTRY_HEAD(zl)) {
		return NULL;

		// 既不是表头也不是表尾，从表尾向表头移动指针
	}
	else {
		// 计算前一个节点的节点数
		entry = zipEntry(p);
		assert(entry.prevrawlen > 0);
		// 移动指针，指向前一个节点
		return p - entry.prevrawlen;
	}
}

unsigned int ziplistGet(unsigned char *p, unsigned char **sstr, unsigned int *slen, long long *sval) {
	zlentry entry;
	if (p == NULL || p[0] == ZIP_END) return 0;
	if (sstr) *sstr = NULL;
	entry = zipEntry(p);
	if (ZIP_IS_STR(entry.encoding)) {
		if (sstr) {
			*slen = entry.len;
			*sstr = p + entry.headersize;
		}
	}else{
		if (sval) {
			*sval = zipLoadInteger(p + entry.headersize, entry.encoding);
		}
	}
	return 1;
}

unsigned char *ziplistInsert(unsigned char *zl, unsigned char *p, unsigned char *s, unsigned int slen) {
	return __ziplistInsert(zl, p, s, slen);
}

unsigned char *ziplistDelete(unsigned char *zl, unsigned char **p) {

	// 因为 __ziplistDelete 时会对 zl 进行内存重分配
	// 而内存重分配可能会改变 zl 的内存地址
	// 所以这里需要记录到达 *p 的偏移量
	// 这样在删除节点之后就可以通过偏移量来将 *p 还原到正确的位置
	size_t offset = *p - zl;
	zl = __ziplistDelete(zl, *p, 1);

	/* Store pointer to current element in p, because ziplistDelete will
	* do a realloc which might result in a different "zl"-pointer.
	* When the delete direction is back to front, we might delete the last
	* entry and end up with "p" pointing to ZIP_END, so check this. */
	*p = zl + offset;

	return zl;
}

unsigned char *ziplistDeleteRange(unsigned char *zl, unsigned int index, unsigned int num) {

	// 根据索引定位到节点
	// T = O(N)
	unsigned char *p = ziplistIndex(zl, index);

	// 连续删除 num 个节点
	// T = O(N^2)
	return (p == NULL) ? zl : __ziplistDelete(zl, p, num);
}

unsigned int ziplistCompare(unsigned char *p, unsigned char *sstr, unsigned int slen) {
	zlentry entry;
	unsigned char sencoding;
	long long zval, sval;
	if (p[0] == ZIP_END) return 0;

	// 取出节点
	entry = zipEntry(p);
	if (ZIP_IS_STR(entry.encoding)) {

		// 节点值为字符串，进行字符串对比

		/* Raw compare */
		if (entry.len == slen) {
			// T = O(N)
			return memcmp(p + entry.headersize, sstr, slen) == 0;
		}
		else {
			return 0;
		}
	}
	else {

		// 节点值为整数，进行整数对比

		/* Try to compare encoded values. Don't compare encoding because
		* different implementations may encoded integers differently. */
		if (zipTryEncoding(sstr, slen, &sval, &sencoding)) {
			// T = O(1)
			zval = zipLoadInteger(p + entry.headersize, entry.encoding);
			return zval == sval;
		}
	}

	return 0;
}

unsigned char *ziplistFind(unsigned char *p, unsigned char *vstr, unsigned int vlen, unsigned int skip) {
	int skipcnt = 0;
	unsigned char vencoding = 0;
	long long vll = 0;

	// 只要未到达列表末端，就一直迭代
	// T = O(N^2)
	while (p[0] != ZIP_END) {
		unsigned int prevlensize, encoding, lensize, len;
		unsigned char *q;

		ZIP_DECODE_PREVLENSIZE(p, prevlensize);
		ZIP_DECODE_LENGTH(p + prevlensize, encoding, lensize, len);
		q = p + prevlensize + lensize;

		if (skipcnt == 0) {

			/* Compare current entry with specified entry */
			// 对比字符串值
			// T = O(N)
			if (ZIP_IS_STR(encoding)) {
				if (len == vlen && memcmp(q, vstr, vlen) == 0) {
					return p;
				}
			}
			else {
				/* Find out if the searched field can be encoded. Note that
				* we do it only the first time, once done vencoding is set
				* to non-zero and vll is set to the integer value. */
				// 因为传入值有可能被编码了，
				// 所以当第一次进行值对比时，程序会对传入值进行解码
				// 这个解码操作只会进行一次
				if (vencoding == 0) {
					if (!zipTryEncoding(vstr, vlen, &vll, &vencoding)) {
						/* If the entry can't be encoded we set it to
						* UCHAR_MAX so that we don't retry again the next
						* time. */
						vencoding = UCHAR_MAX;
					}
					/* Must be non-zero by now */
					assert(vencoding);
				}

				/* Compare current entry with specified entry, do it only
				* if vencoding != UCHAR_MAX because if there is no encoding
				* possible for the field it can't be a valid integer. */
				// 对比整数值
				if (vencoding != UCHAR_MAX) {
					// T = O(1)
					long long ll = zipLoadInteger(q, encoding);
					if (ll == vll) {
						return p;
					}
				}
			}

			/* Reset skip count */
			skipcnt = skip;
		}
		else {
			/* Skip entry */
			skipcnt--;
		}

		/* Move to next entry */
		// 后移指针，指向后置节点
		p = q + len;
	}

	// 没有找到指定的节点
	return NULL;
}

unsigned int ziplistLen(unsigned char *zl) {

	unsigned int len = 0;

	// 节点数小于 UINT16_MAX
	// T = O(1)
	if (intrev16ifbe(ZIPLIST_LENGTH(zl)) < UINT16_MAX) {
		len = intrev16ifbe(ZIPLIST_LENGTH(zl));

		// 节点数大于 UINT16_MAX 时，需要遍历整个列表才能计算出节点数
		// T = O(N)
	}
	else {
		unsigned char *p = zl + ZIPLIST_HEADER_SIZE;
		while (*p != ZIP_END) {
			p += zipRawEntryLength(p);
			len++;
		}

		/* Re-store length if small enough */
		if (len < UINT16_MAX) ZIPLIST_LENGTH(zl) = intrev16ifbe(len);
	}

	return len;
}

size_t ziplistBlobLen(unsigned char *zl) {
	return intrev32ifbe(ZIPLIST_BYTES(zl));
}

void ziplistRepr(unsigned char *zl) {
	unsigned char *p;
	int index = 0;
	zlentry entry;

	printf(
		"{total bytes %d} "
		"{length %u}\n"
		"{tail offset %u}\n",
		intrev32ifbe(ZIPLIST_BYTES(zl)),
		intrev16ifbe(ZIPLIST_LENGTH(zl)),
		intrev32ifbe(ZIPLIST_TAIL_OFFSET(zl)));
	p = ZIPLIST_ENTRY_HEAD(zl);
	while (*p != ZIP_END) {
		entry = zipEntry(p);
		printf(
			"{"
			"addr 0x%08lx, "
			"index %2d, "
			"offset %5ld, "
			"rl: %5u, "
			"hs %2u, "
			"pl: %5u, "
			"pls: %2u, "
			"payload %5u"
			"} ",
			(long unsigned)p,
			index,
			(unsigned long)(p - zl),
			entry.headersize + entry.len,
			entry.headersize,
			entry.prevrawlen,
			entry.prevrawlensize,
			entry.len);
		p += entry.headersize;
		if (ZIP_IS_STR(entry.encoding)) {
			if (entry.len > 40) {
				if (fwrite(p, 40, 1, stdout) == 0) perror("fwrite");
				printf("...");
			}
			else {
				if (entry.len &&
					fwrite(p, entry.len, 1, stdout) == 0) perror("fwrite");
			}
		}
		else {
			printf("%lld", (long long)zipLoadInteger(p, entry.encoding));
		}
		printf("\n");
		p += entry.len;
		index++;
	}
	printf("{end}\n\n");
}







