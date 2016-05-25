#include<stdio.h>
#include<stdlib.h>	
#include<string.h>	
#include"intset.h"
#include"zmalloc.h"
#include"endianconv.h"
#define INTSET_ENC_INT16 (sizeof(int16_t))
#define INTSET_ENC_INT32 (sizeof(int32_t))
#define INTSET_ENC_INT64 (sizeof(int64_t))

static uint8_t _intsetValueEncoding(int64_t v) {
	if (v<INT32_MIN || v>INT32_MAX)
		return INTSET_ENC_INT64;
	else if (v<INT16_MIN || v>INT16_MAX)
		return INTSET_ENC_INT32;
	else return INTSET_ENC_INT16;
}

static int64_t _intsetGetEncoded(intset *is, int pos, uint8_t enc) {
	int64_t v64;
	int32_t v32;
	int16_t v16;
	if (enc == INTSET_ENC_INT64) {
		memcpy(&v64, ((int64_t*)is->contents) + pos, sizeof(v64));
		memrev64ifbe(&v64);
		return v64;
	}
	else if (enc == INTSET_ENC_INT32) {
		memcpy(&v32, (uint32_t*)is->contents + pos, sizeof(v32));
		memrev32ifbe(&v32);
		return v32;
	}
	else {
		memcpy(&v16, (uint16_t*)is->contents + pos, sizeof(v16));
		memrev16ifbe(&v16);
		return v16;
	}
}

static int64_t _intsetGet(intset *is, int pos) {
	return _intsetGetEncoded(is, pos, intrev32ifbe(is->encoding));
}
static void _intsetSet(intset *is, int pos, int64_t value) {
	uint32_t encoding = intrev32ifbe(is->encoding);
	if (encoding == INTSET_ENC_INT64) {
		((int64_t*)is->contents)[pos] = value;
		memrev64ifbe(((int64_t*)is->contents) + pos);
	}
	else if (encoding == INTSET_ENC_INT32) {
		((int32_t*)is->contents)[pos] = value;
		memrev32ifbe(((int32_t*)is->contents) + pos);
	}
	else {
		((int16_t*)is->contents)[pos] = value;
		memrev16ifbe(((int16_t*)is->contents) + pos);
	}
}
intset *intsetNew(void) {
	intset *is = zmalloc(sizeof(intset));
	is->encoding = intrev32ifbe(INTSET_ENC_INT16);
	is->length = 0;
	return is;
}

static intset *intsetResize(intset *is, uint32_t len) {
	uint32_t size = len*intrev32ifbe(is->encoding);
	is = zrealloc(is, sizeof(intset) + size);
	return is;
}

static uint8_t intsetSearch(intset *is, int64_t value, uint32_t *pos) {
	int min = 0, max = intrev32ifbe(is->length) - 1, mid = -1;
	int64_t cur = -1;
	if (intrev32ifbe(is->length) == 0) {
		if (pos)*pos = 0;
		return 0;
	}
	else {
		if (value > _intsetGet(is, intrev32ifbe(is->length) - 1)) {
			if (pos) *pos = intrev32ifbe(is->length);
			return 0;
		}
		else if (value < _intsetGet(is, 0)) {
			if (pos) *pos = 0;
			return 0;
		}
	}
	while (max >= min) {
		mid = (min + max) / 2;
		cur = _intsetGet(is, mid);
		if (value > cur)
			min = mid + 1;
		else if (value < cur)
			max = mid - 1;
		else break;
	}
	if (value == cur) {
		if (pos) *pos = mid;
		return 1;
	}
	else {
		if (pos) *pos = min;
		return 0;
	}
}

static intset *intsetUpgradeAndAdd(intset *is, int64_t value){
	uint8_t curenc = intrev32ifbe(is->encoding);
	uint8_t newenc = _intsetValueEncoding(value);
	int length = intrev32ifbe(is->length);
	int prepend = value < 0 ? 1 : 0;
	is->encoding = intrev32ifbe(newenc);
	is = intsetResize(is, intrev32ifbe(is->length) + 1);
	while (length--) {
		_intsetSet(is, length + prepend, _intsetGetEncoded(is, length, curenc));
	}
	if (prepend) _intsetSet(is, 0, value);
	else _intsetSet(is, intrev32ifbe(is->length) + 1,value);
	is->length = intrev32ifbe(intrev32ifbe(is->length) + 1);
	return is;
}

static void intsetMoveTail(intset *is, uint32_t from, uint32_t to) {
	void *src, *dst;
	uint32_t bytes = intrev32ifbe(is->length) - from;
	uint32_t encoding = intrev32ifbe(is->encoding);
	if (encoding == INTSET_ENC_INT64) {
		src = (int64_t*)is->contents + from;
		dst = (int64_t*)is->contents + to;
		bytes *= sizeof(int64_t);
	}
	else if (encoding == INTSET_ENC_INT32) {
		src = (int32_t*)is->contents + from;
		dst = (int32_t*)is->contents + to;
		bytes *= sizeof(int32_t);
	}
	else {
		src = (int16_t*)is->contents + from;
		dst = (int16_t*)is->contents + to;
		bytes *= sizeof(int16_t);
	}
	memmove(dst, src, bytes);
}

intset *intsetAdd(intset *is, int64_t value, uint8_t *success) {
	uint8_t valenc = _intsetValueEncoding(value);
	uint32_t pos;
	if (success) *success = 1;
	if (valenc > intrev32ifbe(is->encoding))
		return intsetUpgradeAndAdd(is, value);
	else {
		if (intsetSearch(is, value, &pos)) {
			if (success) *success = 0;
			return is;
		}
		is = intsetResize(is, intrev32ifbe(is->length)+1);
		if (pos < intrev32ifbe(is->length)) intsetMoveTail(is, pos, pos + 1);
	}
	_intsetSet(is, pos, value);
	is->length = intrev32ifbe(intrev32ifbe(is->length) + 1);
	return is;
}

intset *intsetRemove(intset *is, int64_t value, int *success) {
	uint8_t valenc = _intsetValueEncoding(value);
	uint32_t pos;
	if (success) *success = 0;
	if (valenc <= intrev32ifbe(is->encoding) && intsetSearch(is, value, &pos)) {
		uint32_t len = intrev32ifbe(is->length);
		if (success) *success = 1;
		if (pos < (len - 1)) intsetMoveTail(is, pos + 1, pos);
		is = intsetResize(is, len - 1);
		is->length = intrev32ifbe(len - 1);
	}
	return is;
}

uint8_t intsetFind(intset *is, int64_t value) {
	uint8_t valenc = _intsetValueEncoding(value);
	return valenc <= intrev32ifbe(is->encoding) && intsetSearch(is, value, NULL);
}

int64_t intsetRandom(intset *is) {
	return _intsetGet(is, rand() % intrev32ifbe(is->length));
}

uint8_t intsetGet(intset *is, uint32_t pos, int64_t *value) {
	if (pos < intrev32ifbe(is->length)) {
		*value = _intsetGet(is, pos);
		return 1;
	}
	return 0;
}

uint32_t intsetLen(intset *is) {
	return intrev32ifbe(is->length);
}

size_t intsetBlobLen(intset *is) {
	return sizeof(intset) + intrev32ifbe(is->length)*intrev32ifbe(is->encoding);
}


