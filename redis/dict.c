#include "fmacros.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <limits.h>
#include <time.h>
#include <ctype.h>

#include "dict.h"
#include "zmalloc.h"
#include "redisassert.h"

// 指示字典是否启用 rehash 的标识
static int dict_can_resize = 1;
// 强制 rehash 的比率
static unsigned int dict_force_resize_ratio = 5;

static int _dictExpandIfNeeded(dict *ht);
static unsigned long _dictNextPower(unsigned long size);
static int _dictKeyIndex(dict *ht, const void *key);
static int _dictInit(dict *ht, dictType *type, void *privDataPtr);

unsigned int dictIntHashFunction(unsigned int key) {
	key+=~
}

unsigned int dictIdentityHashFunction(unsigned int key) {
	return key;
}

static uint32_t dict_hash_function_seed = 5381;
//设置种子
void dictSetHashFunctionSeed(uint32_t seed) {
	dict_hash_function_seed = seed;
}
uint32_t dictGetHashFunctionSeed(void) {
	return dict_hash_function_seed;
}
/* MurmurHash2, by Austin Appleby
* Note - This code makes a few assumptions about how your machine behaves -
* 1. We can read a 4-byte value from any address without crashing
* 2. sizeof(int) == 4
*
* And it has a few limitations -
*
* 1. It will not work incrementally.
* 2. It will not produce the same results on little-endian and big-endian
*    machines.
*/
unsigned int dictGenHashFunction(const void *key, int len) {
	/* 'm' and 'r' are mixing constants generated offline.
	They're not really 'magic', they just happen to work well.  */
	uint32_t seed = dict_hash_function_seed;
	const uint32_t m = 0x5bd1e995;
	const int r = 24;

	/* Initialize the hash to a 'random' value */
	uint32_t h = seed ^ len;

	/* Mix 4 bytes at a time into the hash */
	const unsigned char *data = (const unsigned char *)key;

	while (len >= 4) {
		uint32_t k = *(uint32_t*)data;

		k *= m;
		k ^= k >> r;
		k *= m;

		h *= m;
		h ^= k;

		data += 4;
		len -= 4;
	}

	/* Handle the last few bytes of the input array  */
	switch (len) {
	case 3: h ^= data[2] << 16;
	case 2: h ^= data[1] << 8;
	case 1: h ^= data[0]; h *= m;
	};

	/* Do a few final mixes of the hash to ensure the last few
	* bytes are well-incorporated. */
	h ^= h >> 13;
	h *= m;
	h ^= h >> 15;

	return (unsigned int)h;
}

/* And a case insensitive hash function (based on djb hash) */
unsigned int dictGenCaseHashFunction(const unsigned char *buf, int len) {
	unsigned int hash = (unsigned int)dict_hash_function_seed;

	while (len--)
		hash = ((hash << 5) + hash) + (tolower(*buf++)); /* hash * 33 + c */
	return hash;
}

static void _dictReset(dictht *ht) {
	ht->table = NULL;
	ht->size = 0;
	ht->sizemask = 0;
	ht->used = 0;
}

dict *dictCreate(dictType *type, void *privDataPtr) {
	dict *d = zmalloc(sizeof(*d));
	_dictInit(d, type, privDataPtr);
	return d;
}

int _dictInit(dict *d, dictType *type, void *privDataPtr) {
	_dictReset(&d->ht[0]);
	_dictReset(&d->ht[1]);
	d->type = type;
	d->privdata = privDataPtr;
	d->rehashidx = -1;
	d->iterators = 0;
	return DICT_OK;
}

int dictResize(dict *d) {
	int minimal;
	if (!dict_can_resize || dictIsRehashing(d)) return DICT_ERR;
	minimal = d->ht[0].used;
	if (minimal < DICT_HT_INITIAL_SIZE)
		minimal = DICT_HT_INITIAL_SIZE;
	return dictExpand(d, minimal);
}
int dictExpand(dict *d, unsigned long size) {
	dictht n;
	unsigned long realsize = _dictNextPower(size);
	if (dictIsRehashing(d) || d->ht[0].used > size)
		return DICT_ERR;
	n.size = realsize;
	n.sizemask = realsize - 1;
	n.table = zcalloc(realsize * sizeof(dictEntry*));
	n.used = 0;

	if (d->ht[0].table == NULL) {
		d->ht[0] = n;
		return DICT_OK;
	}
	d->ht[1] = n;
	d->rehashidx = 0;
	return DICT_OK;
}
int dictRehash(dict *d, int n) {
	if (!dictIsRehashing(d)) return 0;
	while (n--) {
		dictEntry *de, *nextde;
		if (d->ht[0].used == 0) {
			zfree(d->ht[0].table);
			d->ht[0] = d->ht[1];
			_dictReset(&d->ht[1]);
			d->rehashidx = -1;
			return 0;
		}
		assert(d->ht[0].size > (unsigned)d->rehashidx);
		while (d->ht[0].table[d->rehashidx] == NULL)d->rehashidx++;
		de = d->ht[0].table[d->rehashidx];
		while (de) {
			unsigned int h;
			nextde = de->next;
			h = dictHashKey(d, de->key)&d->ht[1].sizemask;
			de->next = d->ht[1].table[h];
			d->ht[1].table[h] = de;
			d->ht[0].used--;
			d->ht[1].used++;
			de = nextde;
		}
		d->ht[0].table[d->rehashidx] = NULL;
		d->rehashidx++;
	}
	return 1;
}

long long timeInMillseconds(void) {
	struct timeval tv;
	gettimeofday(&tv, NULL);
	return (((long long)tv.tv_sec) * 1000) + (tv.tv_usec / 1000);
}
/* Rehash for an amount of time between ms milliseconds and ms+1 milliseconds */
/*
* 在给定毫秒数内，以 100 步为单位，对字典进行 rehash，如果使用时间超过ms，则退出
* rehash，返回所使用的时间。
*
* T = O(N)
*/
int dictRehashMillseconds(dict *d, int ms) {
	long long start = timeInMillseconds();
	int rehashes = 0;
	while (dictRehash(d, 100)) {
		rehashes += 100;
		if (timeInMillseconds() - start > ms) break;
	}
	return rehashes;
}

static void _dictRehashStep(dict *d) {
	if (d->iterators == 0) dictRehash(d, 1);
}

int dictAdd(dict *d, void *key, void *val) {
	dictEntry *entry = dictAddRaw(d, key);
	if (!entry) return DICT_ERR;
	dictSetVal(d, entry, val);
	return DICT_OK;
}

dictEntry *dictAddRaw(dict *d,void *key){
	int index;
	dictEntry *entry;
	dictht *ht;
	if (dictIsRehashing(d)) _dictRehashStep(d);
	if ((index = _dictKeyIndex(d, key)) == -1)
		return NULL;
	ht = dictIsRehashing(d) ? &d->ht[1] : &d->ht[0];
	entry = zmalloc(sizeof(*entry));
	entry->next = ht->table[index];
	ht->table[index] = entry;
	ht->used++;
	dictSetKey(d, entry, key);
	return entry;
}

int dictReplace(dict *d, void *key, void *val) {
	dictEntry *entry, auxentry;
	if (dictAdd(d, key, val) == DICT_OK)
		return 1;
	entry = dictFind(d, key);
	auxentry = *entry;
	dictSetVal(d, entry, val);
	dictFreeVal(d, &auxentry);
	return 0;
}

dictEntry *dictReplaceRaw(dict *d, void *key) {
	dictEntry *entry = dictFind(d, key);
	return entry ? entry : dictAddRaw(d, key);
}

/* Search and remove an element */
/*
* 查找并删除包含给定键的节点
*
* 参数 nofree 决定是否调用键和值的释放函数
* 0 表示调用，1 表示不调用
*
* 找到并成功删除返回 DICT_OK ，没找到则返回 DICT_ERR
*
* T = O(1)
*/
static int dictGenericDelete(dict *d, const void *key, int nofree) {
	unsigned int h, idx;
	dictEntry *he, *prevHe;
	int table;
	if (d->ht[0].size == 0) return DICT_ERR;
	if (dictIsRehashing(d)) _dictRehashStep(d);
	h = dictHashKey(d, key);
	for (table = 0; table <= 1; table++) {
		idx = h&d->ht[table].sizemask;
		he = d->ht[table].table[idx];
		prevHe = NULL;
		while (he) {
			if (dictCompareKeys(d, key, he->key)) {
				if (prevHe)
					prevHe->next = he->next;
				else d->ht[table].table[idx] = he->next;
				if (!nofree) {
					dictFreeKey(d, he);
					dictFreeVal(d, he);
				}
				zfree(he);
				d->ht[table].used--;
				return DICT_OK;
			}
			prevHe = he;
			he = he->next;
		}
		if (!dictIsRehashing(d))break;
	}
	return DICT_ERR;
}

int dictDelete(dict *ht, const void *key) {
	return dictGenericDelete(ht, key, 0);
}

int dictDeleteNoFree(dict *ht, void *key) {
	return dictGenericDelete(ht, key, 1);
}

int _dictClear(dict *d, dictht *ht, void(callback)(void*)) {
	unsigned long i;
	for (i = 0; i < ht->size&&ht->used>0; ++i) {
		dictEntry *he, *nextHe;
		//如果回调函数存在而且i==0或者i!=65536的n倍
		if (callback && (i & 65535) == 0) callback(d->privdata);
		if ((he = ht->table[i]) == NULL) continue;
		while (he) {
			nextHe = he->next;
			dictFreeKey(d, he);
			dictFreeVal(d, he);
			zfree(he);
			ht->used--;
			ht = nextHe;
		}
	}
	zfree(ht->table);
	_dictReset(ht);
	return DICT_OK;
}

void dictRelease(dict *d) {
	_dictClear(d, &d->ht[0], NULL);
	_dictClear(d, &d->ht[1], NULL);
	zfree(d);
}

dictEntry *dictFind(dict *d, const void *key) {
	dictEntry *he;
	unsigned int h, idx, table;
	if (d->ht[0].size == 0) return NULL;
	if (dictIsRehashing(d)) _dictRehashStep(d);
	h = dictHashKey(d, key);
	for (table = 0; table <= 1; table++) {
		idx = h&d->ht[table].sizemask;
		he = d->ht[table].table[idx];
		while (he) {
			if (dictCompareKeys(d, key, he->key)) return he;
			he = he->next;
		}
		// 如果程序遍历完 0 号哈希表，仍然没找到指定的键的节点
		// 那么程序会检查字典是否在进行 rehash ，
		// 然后才决定是直接返回 NULL ，还是继续查找 1 号哈希表
		if (!dictRehashing(d)) return NULL;
	}
	return NULL;
}

void *dictFetchValue(dict *d, const void *key) {
	dictEntry *he;
	he = dictFind(d, key);
	return he ? dictGetVal(he) : NULL;
}
/* 一个fingerprint为一个64位数值,用以表示某个时刻dict的状态, 它由dict的一些属性通过位操作(xord	)计算得到.
* 场景: 一个非安全迭代器初始后, 会产生一个fingerprint值. 在该迭代器被释放时会重新检查这个fingerprint值.
* 如果前后两个fingerprint值不一致,说明迭代器的使用者在迭代时执行了某些非法操作（改变了dict状态）. */
long long dictFingerprint(dict *d) {
	long long integers[6], hash = 0;
	int j;
	integers[0] = (long)d->ht[0].table;
	integers[1] = d->ht[0].size;
	integers[2] = d->ht[0].used;
	integers[3] = (long)d->ht[1].table;    
	integers[4] = d->ht[1].size;
	integers[5] = d->ht[1].used;
	for (j = 0; j < 6; ++j) {
		hash = (~hash) + (hash << 21); // hash = (hash << 21) - hash - 1;
		hash = hash ^ (hash >> 24);
		hash = (hash + (hash << 3)) + (hash << 8); // hash * 265
		hash = hash ^ (hash >> 14);
		hash = (hash + (hash << 2)) + (hash << 4); // hash * 21
		hash = hash ^ (hash >> 28);
		hash = hash + (hash << 31);
	}
	return hash;
}

dictIterator *dictGetIterator(dict *d) {
	dictIterator *iter = zmalloc(sizeof(*iter));
	iter->d = d;
	iter->table = 0;
	iter->index = -1;
	iter->safe = 0;
	iter->entry = NULL;
	iter->nextEntry = NULL;
	return iter;
}

dictIterator *dictGetSafeIterator(dict *d) {
	dictIterator *i = dictGetIterator(d);
	i->safe = 1;
	return i;
}

dictEntry *dictNext(dictIterator *iter) {
	while (1) {
		if (iter->entry == NULL) {
			dictht *ht = &iter->d->ht[iter->table];
			if (iter->index == -1 && iter->table == 0) {
				if (iter->safe)
					iter->d->iterators++;
				else iter->fingerprint = dictFingerprint(iter->d);
			}
			iter->index++;
			if (iter->index >= (signed)ht->size) {
				if (dictIsRehashing(iter->d) && iter->table == 0) {
					iter->table++;
					iter->index = 0;
					ht = &iter->d->ht[1];
				}
				else break;
			}
			iter->entry = ht->table[iter->index];
		}
		else {
			iter->entry = iter->nextEntry;
		}
		if (iter->entry) {
			iter->nextEntry = iter->entry->next;
			return iter->entry;
		}
	}
	return NULL;
}

void dictReleaseIterator(dictIterator *iter) {
	if (!(iter->index == -1 && iter->table == 0)) {
		if (iter->safe) iter->d->iterators--;
		else assert(iter->fingerprint == dictFingerprint(iter->d));
	}
	zfree(iter);
}

dictEntry *dictGetRandomKey(dict *d) {
	dictEntry *he, *orighe;
	unsigned int h;
	int listlen, listele;
	if (dictSize(d) == 0) return 0;
	if (dictIsRehashing(d)) _dictRehashStep(d);
	if (dictIsRehashing(d)) {
		do {
			h = random() % (d->ht[0].size + d->ht[1].size);
			he = (h >= d->ht[0].size) ? d->ht[1].table[h - d->ht[0].size] : d->ht[0].size;
		} while (he == NULL);
	}
	else {
		do {
			h = random()&d->ht[0].sizemask;
			he = d->ht[0].table[h];
		} while (he == NULL);
	}
	listlen = 0;
	orighe = he;
	while (he) {
		he = he->next;
		listlen++;
	}
	listele = random() % listlen;
	he = orighe;
	while (listele--) he = he->next;
	return he;
}

int dictGetRandomKeys(dict *d, dictEntry **des, int count) {
	int j;
	int stored = 0;
	if (dictSize(d) < count) count = dictSize(d);
	while (stored < count) {
		for (j = 0; j < 2; ++j) {
			unsigned int i = random()&d->ht[j].sizemask;
			int size = d->ht[j].size;
			while (size--) {
				dictEntry *he = d->ht[j].table[i];
				while (he) {
					*des = he;
					des++;
					he = he->next;
					stored++;
					if (stored == count) return stored;
				}
				i = (i + 1)&d->ht[j].sizemask;
			}
			assert(dictIsRehashing(d) != 0);
		}
	}
	return stored;
}

//将v的二进制位翻转
static unsigned long rev(unsigned long v) {
	unsigned long s = 8 * sizeof(v);
	unsigned long mask = ~0;
	while ((s >> 1) > 0) {
		mask ^= (mask << s);
		v = ((v >> s)&mask) | ((v << s)&~mask);
	}
	return v;
}
/* dictScan() is used to iterate over the elements of a dictionary.
*
* dictScan() 函数用于迭代给定字典中的元素。
*
* Iterating works in the following way:
*
* 迭代按以下方式执行：
*
* 1) Initially you call the function using a cursor (v) value of 0.
*    一开始，你使用 0 作为游标来调用函数。
* 2) The function performs one step of the iteration, and returns the
*    new cursor value that you must use in the next call.
*    函数执行一步迭代操作，
*    并返回一个下次迭代时使用的新游标。
* 3) When the returned cursor is 0, the iteration is complete.
*    当函数返回的游标为 0 时，迭代完成。
*/
unsigned long dictScan(dict *d, unsigned long v, dictScanFunction *fn, void *privdata) {
	dictht *t0, *t1;
	const dictEntry *de;
	unsigned long m0, m1;
	if (dictSize(d) == 0) return 0;
	if (!dictIsRehashing(d)) {
		t0 = &(d->ht[0]);
		m0 = t0->sizemask;
		de = t0->table[v&m0];
		while (de) {
			fn(privdata, de);
			de = de->next;
		}
	}
	else {
		t0 = &d->ht[0];
		t1 = &d->ht[1];
		if (t0->size > t1->size) {
			t0 = &d->ht[1];
			t1 = &d->ht[0];
		}
		m0 = t0->sizemask;
		m1 = t1->sizemask;
		de = t0->table[v&m0];
		while (de) {
			fn(privdata, de);
			de = de->next;
		}
		do {
			de = t1->table[v&m1];
			while (de) {
				fn(privdata, de);
				de = de->next;
			}
			v = (((v | m0) + 1)&~m0) | (v&m0);
		} while (v&(m0^m1));
	}
	v | ~m0;
	v = rev(v);
	++v; 
	v = rev(v);
	return v;	
}

static int _dictExpandIfNeeded(dict *d) {
	if (dictIsRehashing(d)) return DICT_OK;
	if (d->ht[0].size == 0) return dictExpand(d, DICT_HT_INITIAL_SIZE);
	if (d->ht[0].used >= d->ht[0].size && (dict_can_resize || d->ht[0].used / d->ht[0].size
	> dict_force_resize_ratio)) {
		return dictExpand(d, d->ht[0].used * 2);
	}
	return DICT_OK;
}

static unsigned long _dictNextPower(unsigned long size) {
	unsigned long i = DICT_HT_INITIAL_SIZE;
	if (size >= LONG_MAX) return LONG_MAX;
	while (1) {
		if (i >= size) return i;
		i *= 2;
	}
}

static int _dictKeyIndex(dict *d, const void *key) {
	unsigned int h, idx, table;
	dictEntry *he;
	if (_dictExpandIfNeeded(d) == DICT_ERR) return -1;
	h = dictHashKey(d, key);
	for (table = 0; table <= 1; table++) {
		idx = h&d->ht[table].sizemask;
		he = d->ht[table].table[idx];
		while (he) {
			if (dictCompareKeys(d, key, he->key)) return -1;
			he = he->next;
		}
		if (!dictIsRehashing(d)) break;
	}
	return idx;
}

void dictEmptry(dict *d, void (callback)(void*)) {
	_dictClear(d, &d->ht[0], callback);
	_dictClear(d, &d->ht[1], callback);
	d->rehashidx = -1;
	d->iterators = 0;
}

void dictEnableResize(void) {
	dict_can_resize = 1;
}

void dictDisableResize(void) {
	dict_can_resize = 0;
}