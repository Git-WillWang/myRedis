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

//����
#define ZIP_STR_MASK 0xc0 // 11000000
#define ZIP_INT_MASK 0x30 // 00110000

#define ZIP_STR_06B (0<<6) // 00000000
#define ZIP_STR_14B (1<<6) // 01000000
#define ZIP_STR_32B (2<<6) // 10000000
/*
* ������������
*�ڵ���λ�ַ��������ʣ�µ�λ
*/
#define ZIP_INT_16B (0xc0 | 0<<4) //11000000|00000000=11000000 2�ֽ�����
#define ZIP_INT_32B (0xc0 | 1<<4) //11000000|00010000=11010000 4�ֽ�����
#define ZIP_INT_64B (0xc0 | 2<<4) //11000000|00100000=11100000 8�ֽ�����
#define ZIP_INT_24B (0xc0 | 3<<4) //11000000|00110000=11110000 3�ֽ�����
#define ZIP_INT_8B 0xfe //11111110 ����0��12���޷�������

/* 4 bit integer immediate encoding
*
* 4 λ������������������
*/
#define ZIP_INT_IMM_MASK 0x0f
#define ZIP_INT_IMM_MIN 0xf1    /* 11110001 */
#define ZIP_INT_IMM_MAX 0xfd    /* 11111101 */
#define ZIP_INT_IMM_VAL(v) (v & ZIP_INT_IMM_MASK) //ȡ4λ���������ֵ

/*
* 24 λ���������ֵ����Сֵ
*/
#define INT24_MAX 0x7fffff
#define INT24_MIN (-INT24_MAX - 1)

//�ж��Ƿ����ַ���
#define ZIP_IS_STR(enc) (((enc)& ZIP_STR_MASK)<ZIP_STR_MASK)
// ��λ�� ziplist �� bytes ���ԣ������Լ�¼������ ziplist ��ռ�õ��ڴ��ֽ���
// ����ȡ�� bytes ���Ե�����ֵ������Ϊ bytes ���Ը�����ֵ	
#define ZIPLIST_BYTES(zl) (*((uint32_t*)(zl)))
// ��λ�� ziplist �� offset ���ԣ������Լ�¼�˵����β�ڵ��ƫ����
// ����ȡ�� offset ���Ե�����ֵ������Ϊ offset ���Ը�����ֵ
#define ZIPLIST_TAIL_OFFSET(zl) (*((uint32_t*)(zl) + sizeof(uint32_t)))
// ��λ�� ziplist �� length ���ԣ������Լ�¼�� ziplist �����Ľڵ�����
// ����ȡ�� length ���Ե�����ֵ������Ϊ length ���Ը�����ֵ
#define ZIPLIST_LENGTH(zl) (*((uint16_t*)((zl)+sizeof(uint32_t)*2)))
///���� ziplist ��ͷ�Ĵ�С
#define ZIPLIST_HEADER_SIZE (sizeof(uint32_t)*2+sizeof(uint16_t))
// ����ָ�� ziplist ��һ���ڵ㣨����ʼλ�ã���ָ��
#define ZIPLIST_ENTRY_HEAD(zl) ((zl)+ZIPLIST_HEADER_SIZE)
// ����ָ�� ziplist ���һ���ڵ㣨����ʼλ�ã���ָ��
#define ZIPLIST_ENTRY_TAIL(zl) ((zl)+intrev32ifbe(ZIPLIST_TAIL_OFFSET(zl)))
// ����ָ�� ziplist ĩ�� ZIP_END ������ʼλ�ã���ָ��
#define ZIPLIST_ENTRY_END(zl) ((zl)+intrev32ifbe(ZIPLIST_BYTES(zl))-1)

//���� ziplist �Ľڵ���
#define ZIPLIST_INCR_LENGTH(zl,incr){\
	if (ZIPLIST_LENGTH(zl)<UINT16_MAX) \
		ZIPLIST_LENGTH(zl) = intrev16ifbe(intrev16ifbe(ZIPLIST_LENGTH(zl))+incr);\
}
//���� ziplist �ڵ���Ϣ�Ľṹ
typedef struct zlentry {
	// prevrawlen ��ǰ�ýڵ�ĳ���
	// prevrawlensize ������ prevrawlen ������ֽڴ�С
	unsigned int prevrawlensize, prevrawlen;
	// len ����ǰ�ڵ�ֵ�ĳ���
	// lensize ������ len ������ֽڴ�С
	unsigned int lensize, len;
	// ��ǰ�ڵ� header �Ĵ�С = prevrawlensize + lensize
	unsigned int headersize;
	// ��ǰ�ڵ�ֵ��ʹ�õı�������	
	unsigned char encoding;
	unsigned char* p;
}zlentry;

//���encoding < ZIP_STR_MASK ˵�����ַ����룬���ڵ�����λ��
//ʹ����λΪ0��ʣ�±���λ��00000000/01000000/10000000��
#define ZIP_ENTRY_ENCODING(ptr,encoding) do{\
	(encoding) = (ptr[0]);\
	if((encoding)<ZIP_STR_MASK) (encoding)&= ZIP_STR_MASK;\
} while(0)

//�����������뷽ʽ����Ӧ�����͵Ĵ�С
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

/*����rawlen���Ƚ�rawlen�����λ��p
 if rawlen<=00111111 then 00000000|rawlen
 else if rawlen<= 0011111111111111 then �߰�λ=(01000000|(rawlen>>8 & 00111111))
 �Ͱ�λ=rawlen&11111111
 else 
*/
static unsigned int zipEncodeLength(unsigned char* p, unsigned char encoding,
	unsigned int rawlen) {
	unsigned char len = 1, buf[5];
	//�ж�Ϊ�ַ���
	if (ZIP_IS_STR(encoding)) {
		//���С��00111111��˵�����ȿ���ʹ��һλ����
		if (rawlen < 0x3f) {
			if (!p) return len;
			//����00000000 | ���ȣ�[0,63]���õ�����
			buf[0] = ZIP_STR_06B | rawlen;
		}
		//�����(00111111,0011111111111111]֮�䣬�򳤶�ʹ����λ����
		else if (rawlen <= 0x3fff) {
			len += 1;
			if (!p) return len;
			//���ڳ�����(00111111,0011111111111111]֮�䣬���rawlen>>8
			//�Ѹ߰�λ���ƣ�&0x3fɾ����2λ�����ڷ���encodingλ��,|ZIP_STR_14B
			//��ѱ���λ���õ�����
			buf[0] = ZIP_STR_14B | ((rawlen >> 8) & 0x3f);
			//&0xff�ڵ��߰�λ���õ��Ͱ�λ�����ý�buf[1]��
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
//��б�ߺ����пո�
//����encoding���ptrλ���ϴ洢�ĳ���len���Լ��洢lenռ�õ��ֽ���lensize
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
/*��ǰ�ýڵ�ĳ��� len ���б��룬������д�뵽 p �У�
* Ȼ�󷵻ر��� len ������ֽ�������
*/
static unsigned int zipPrevEncodeLength(unsigned char *p, unsigned int len) {
	if (p == NULL) return (len < ZIP_BIGLEN) ? 1 : sizeof(len) + 1;
	else {
		//���С��254������1�ֽڴ洢
		if (len < ZIP_BIGLEN) {
			p[0] = len;
			return 1;
		}
		//����254�����һ�ֽڷ���ZIP_BIGLEN��Ϊ5�ֽڳ��ȱ�ʶ�������ֽڷ���len
		else {
			p[0] = ZIP_BIGLEN;
			memcpy(p + 1, &len, sizeof(len));
			memrev32ifbe(p + 1);
			return 1 + sizeof(len);
		}
	}
}
//��ԭ��ֻ��Ҫ 1 ���ֽ��������ǰ�ýڵ㳤�� len ������һ�� 5 �ֽڳ��� header �С�
static void zipPrevEncodeLengthForceLarge(unsigned char *p, unsigned int len) {
	if (p == NULL) return;
	p[0] = ZIP_BIGLEN;
	memcpy(p + 1, &len, sizeof(len));
	memrev32ifbe(p + 1);
}
/*���� ptr ָ�룬
 * ȡ������ǰ�ýڵ㳤��������ֽ��������������浽 prevlensize �����С�
 */
#define ZIP_DECODE_PREVLENSIZE(ptr,prevlensize) do{\
	if ((ptr)[0] < ZIP_BIGLEN) {\
		(prevlensize) = 1;\
	}\
	else {\
		(prevlensize) = 5;\
	}\
}while (0)

 /* ���� ptr ָ�룬
  * ȡ������ǰ�ýڵ㳤��������ֽ�����
  * ��������ֽ������浽 prevlensize �С�
  *
  * Ȼ����� prevlensize ���� ptr ��ȡ��ǰ�ýڵ�ĳ���ֵ��
  * �����������ֵ���浽 prevlen �����С�
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
/*��������µ�ǰ�ýڵ㳤�� len ������ֽ�����
* ��ȥ���� p ԭ����ǰ�ýڵ㳤��������ֽ���֮�
*/
static int zipPrevLenByteDiff(unsigned char *p, unsigned int len) {
	unsigned int prevlensize;
	ZIP_DECODE_PREVLENSIZE(p, prevlensize);
	return zipPrevEncodeLength(NULL, len) - prevlensize;
}
//����ָ�� p ��ָ��Ľڵ�ռ�õ��ֽ����ܺ͡�
static unsigned int zipRawEntryLength(unsigned char *p) {
	unsigned int prevlensize, encoding, lensize, len;
	ZIP_DECODE_PREVLENSIZE(p, prevlensize);
	ZIP_DECODE_LENGTH(p + prevlensize, encoding, lensize, len);
	return prevlensize + lensize + len;
}

/*��� entry ��ָ����ַ����ܷ񱻱���Ϊ������
*
* ������ԵĻ���
* ������������������ָ�� v ��ֵ�У���������ķ�ʽ������ָ�� encoding ��ֵ�С�
*/
static int zipTryEncoding(unsigned char *entry, unsigned int entryLen, long long *v, unsigned char *encoding) {
	long long value;
	//���ȳ���32λ����Ϊ0�ĺ���
	if (entryLen >= 32 || entryLen == 0) return 0;
	//���ַ���ת����long long�����浽value��
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
//�� encoding ָ���ı��뷽ʽ��������ֵ value д�뵽 p
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
�� encoding ָ���ı��뷽ʽ����ȡ������ָ�� p �е�����ֵ
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
�� p ��ָ����б�ڵ����Ϣȫ�����浽 zlentry �У������ظ� zlentry ��
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
	unsigned int bytes = ZIPLIST_HEADER_SIZE + 1;//����zip_end�Ŀռ�
	unsigned char *zl = zmalloc(bytes);
	ZIPLIST_BYTES(zl) = intrev32ifbe(bytes);
	//��Ϊzlentry�ĸ���Ϊ0�����header֮�����tail
	ZIPLIST_TAIL_OFFSET(zl) = intrev32ifbe(ZIPLIST_HEADER_SIZE);
	zl[bytes - 1] = ZIP_END;
	return zl;
}

/*
���� ziplist �Ĵ�СΪ len �ֽڡ�
* �� ziplist ԭ�еĴ�СС�� len ʱ����չ ziplist ����ı� ziplist ԭ�е�Ԫ�ء�
*/
static unsigned char *ziplistResize(unsigned char *zl, unsigned int len) {
	zl = zrealloc(zl, len);
	ZIPLIST_BYTES(zl) = intrev32ifbe(len);
	zl[len - 1] = ZIP_END;//������С�����zip_end
	return zl;
}
/*
�Բ���p֮��Ľڵ���е���
*/
static unsigned char *__ziplistCascadeUpdate(unsigned char *zl, unsigned char *p) {
	size_t curlen = intrev32ifbe(ZIPLIST_BYTES(zl)), rawlen, rawlensize;
	size_t offset, noffset, extra;
	unsigned char *np;
	zlentry cur, next;

	while (p[0] != ZIP_END) {
		cur = zipEntry(p);
		// ��ǰ�ڵ�ĳ���
		rawlen = cur.headersize + cur.len;
		// ���뵱ǰ�ڵ�ĳ���������ֽ���
		rawlensize = zipPrevEncodeLength(NULL, rawlen);
		if (p[rawlen] == ZIP_END) break;
		//ȡ��p�ڵ����һ���ڵ�
		next = zipEntry(p + rawlen);
		//�����һ���ڵ��prevrawlen����Ŀǰ�ڵ����賤�ȣ���˵�����������ٵ���
		if (next.prevrawlen == rawlen) break;
		if (next.prevrawlensize < rawlensize) {
			//ȡ��p�ڵ㵽zl��ʼλ�õľ���
			offset = p - zl;
			extra = rawlensize - next.prevrawlensize;//ȡ����Ҫ��չ�ĳ��ȴ�С
			zl = ziplistResize(zl, curlen + extra);//��չ�����С
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
/*����ָ�� p ��ָ����λ�ã�������Ϊ slen ���ַ��� s ���뵽 zl �С�
*
* �����ķ���ֵΪ��ɲ������֮��� ziplist
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

	// ����������
	if (index < 0) {

		// ������ת��Ϊ����
		index = (-index) - 1;

		// ��λ����β�ڵ�
		p = ZIPLIST_ENTRY_TAIL(zl);

		// ����б�Ϊ�գ���ô������
		if (p[0] != ZIP_END) {

			// �ӱ�β���ͷ����
			entry = zipEntry(p);
			// T = O(N)
			while (entry.prevrawlen > 0 && index--) {
				// ǰ��ָ��
				p -= entry.prevrawlen;
				// T = O(1)
				entry = zipEntry(p);
			}
		}

		// ������������
	}
	else {

		// ��λ����ͷ�ڵ�
		p = ZIPLIST_ENTRY_HEAD(zl);

		// T = O(N)
		while (p[0] != ZIP_END && index--) {
			// ����ָ��
			// T = O(1)
			p += zipRawEntryLength(p);
		}
	}

	// ���ؽ��
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

	// ��� p ָ���б�ĩ�ˣ��б�Ϊ�գ����߸տ�ʼ�ӱ�β���ͷ������
	// ��ô����ȡ���б�β�˽ڵ�
	if (p[0] == ZIP_END) {
		p = ZIPLIST_ENTRY_TAIL(zl);
		// β�˽ڵ�Ҳָ���б�ĩ�ˣ���ô�б�Ϊ��
		return (p[0] == ZIP_END) ? NULL : p;

		// ��� p ָ���б�ͷ����ô˵�������Ѿ����
	}
	else if (p == ZIPLIST_ENTRY_HEAD(zl)) {
		return NULL;

		// �Ȳ��Ǳ�ͷҲ���Ǳ�β���ӱ�β���ͷ�ƶ�ָ��
	}
	else {
		// ����ǰһ���ڵ�Ľڵ���
		entry = zipEntry(p);
		assert(entry.prevrawlen > 0);
		// �ƶ�ָ�룬ָ��ǰһ���ڵ�
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

	// ��Ϊ __ziplistDelete ʱ��� zl �����ڴ��ط���
	// ���ڴ��ط�����ܻ�ı� zl ���ڴ��ַ
	// ����������Ҫ��¼���� *p ��ƫ����
	// ������ɾ���ڵ�֮��Ϳ���ͨ��ƫ�������� *p ��ԭ����ȷ��λ��
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

	// ����������λ���ڵ�
	// T = O(N)
	unsigned char *p = ziplistIndex(zl, index);

	// ����ɾ�� num ���ڵ�
	// T = O(N^2)
	return (p == NULL) ? zl : __ziplistDelete(zl, p, num);
}

unsigned int ziplistCompare(unsigned char *p, unsigned char *sstr, unsigned int slen) {
	zlentry entry;
	unsigned char sencoding;
	long long zval, sval;
	if (p[0] == ZIP_END) return 0;

	// ȡ���ڵ�
	entry = zipEntry(p);
	if (ZIP_IS_STR(entry.encoding)) {

		// �ڵ�ֵΪ�ַ����������ַ����Ա�

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

		// �ڵ�ֵΪ���������������Ա�

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

	// ֻҪδ�����б�ĩ�ˣ���һֱ����
	// T = O(N^2)
	while (p[0] != ZIP_END) {
		unsigned int prevlensize, encoding, lensize, len;
		unsigned char *q;

		ZIP_DECODE_PREVLENSIZE(p, prevlensize);
		ZIP_DECODE_LENGTH(p + prevlensize, encoding, lensize, len);
		q = p + prevlensize + lensize;

		if (skipcnt == 0) {

			/* Compare current entry with specified entry */
			// �Ա��ַ���ֵ
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
				// ��Ϊ����ֵ�п��ܱ������ˣ�
				// ���Ե���һ�ν���ֵ�Ա�ʱ�������Դ���ֵ���н���
				// ����������ֻ�����һ��
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
				// �Ա�����ֵ
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
		// ����ָ�룬ָ����ýڵ�
		p = q + len;
	}

	// û���ҵ�ָ���Ľڵ�
	return NULL;
}

unsigned int ziplistLen(unsigned char *zl) {

	unsigned int len = 0;

	// �ڵ���С�� UINT16_MAX
	// T = O(1)
	if (intrev16ifbe(ZIPLIST_LENGTH(zl)) < UINT16_MAX) {
		len = intrev16ifbe(ZIPLIST_LENGTH(zl));

		// �ڵ������� UINT16_MAX ʱ����Ҫ���������б���ܼ�����ڵ���
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







