#include<stdint.h>

#ifndef __DICT_H
#define __DICT_H

#define DICT_OK 0
#define DICT_ERR 1

#define DICT_NOTUSED(V) ((void) V)

typedef struct dictEntry {
	void *key;
	union {
		void *val;
		uint64_t u64;
		int64_t s64;
	} v;
	struct dictEntry *next;
}dictEntry;
typedef struct dictType {
	//�����ϣֵ
	unsigned int(*hashFunction)(const void *key);
	//���Ƽ�
	void *(*keyDup)(void *privdata, const void *key);
	//����ֵ
	void *(*valDup)(void *privadata, const void *obj);
	//�Աȼ�
	int (*keyCompare)(void *privdata, const void *key1, const void *key2);
	//���ټ�
	void(*keyDestructor)(void *privdata, void *key);
	//����ֵ
	void(*valDestructor)(void *privdata, void *obj);
}dictType;

typedef struct dictht {
	//��ϣ������
	dictEntry **table;
	//��ϣ���С
	unsigned long size;
	//��ϣ���С���룬���ڼ�������ֵ�����ǵ���size-1
	unsigned long sizemask;
	//�ù�ϣ�����нڵ�����
	unsigned long used;
}dictht;

//�ֵ�
typedef struct dict {
	
	dictType *type;
	//˽������
	void *privdata;
	//��ϣ��
	dictht ht[2];
	int rehashidx;
	//Ŀǰ�������еİ�ȫ������������
	int iterators;
}dict;

/*
* �ֵ������
*
* ��� safe ���Ե�ֵΪ 1 ����ô�ڵ������еĹ����У�
* ������Ȼ����ִ�� dictAdd �� dictFind ���������������ֵ�����޸ġ�
*
* ��� safe ��Ϊ 1 ����ô����ֻ����� dictNext ���ֵ���е�����
* �������ֵ�����޸ġ�
*/
typedef struct dictIterator {
	//���������ֵ�
	dict *d;
	// table �����ڱ������Ĺ�ϣ����룬ֵ������ 0 �� 1 ��
	// index ����������ǰ��ָ��Ĺ�ϣ������λ�á�
	// safe ����ʶ����������Ƿ�ȫ
	int table, index, safe;
	// entry ����ǰ�������Ľڵ��ָ��
	// nextEntry ����ǰ�����ڵ����һ���ڵ�
	//             ��Ϊ�ڰ�ȫ����������ʱ�� entry ��ָ��Ľڵ���ܻᱻ�޸ģ�
	//             ������Ҫһ�������ָ����������һ�ڵ��λ�ã�
	//             �Ӷ���ָֹ�붪ʧ
	dictEntry *entry, *nextEntry;
	long long fingerprint;
}dictIterator;

typedef void (dictScanFunction)(void *privdata, const dictEntry* de);
//��ʼ��ϣ���С
#define DICT_HT_INITIAL_SIZE 4
// �ͷŸ����ֵ�ڵ��ֵ
#define dictFreeVal(d,entry)\
	if((d)->type->valDestructor)\
		(d)->type->valDestructor((d)->privdata,(entry)->v.val)
// ���ø����ֵ�ڵ��ֵ
#define dictSetVal(d,entry,_val_) do {\
	if((d)->type->valDup)\
		entry->v.val = (d)->type->valDup((d)->privdata,_val_);\
	else\
		entry->v.val = (_val_);\
} while(0)
// ��һ���з���������Ϊ�ڵ��ֵ
#define dictSetSignedIntegerVal(entry,_val_)\
	do { entry->v.s64 = _val_;} while(0)
// ��һ���޷���������Ϊ�ڵ��ֵ
#define dictSetUnsignedIntegerVal(entry, _val_) \
    do { entry->v.u64 = _val_; } while(0)
// �ͷŸ����ֵ�ڵ�ļ�
#define dictFreeKey(d, entry) \
    if ((d)->type->keyDestructor) \
        (d)->type->keyDestructor((d)->privdata, (entry)->key)
// ���ø����ֵ�ڵ�ļ�
#define dictSetKey(d, entry, _key_) do { \
    if ((d)->type->keyDup) \
        entry->key = (d)->type->keyDup((d)->privdata, _key_); \
    else \
        entry->key = (_key_); \
} while(0)
// �ȶ�������
#define dictCompareKeys(d, key1, key2) \
    (((d)->type->keyCompare) ? \
        (d)->type->keyCompare((d)->privdata, key1, key2) : \
        (key1) == (key2))
// ����������Ĺ�ϣֵ
#define dictHashKey(d,key) (d)->type->hashFunction(key)
// ���ػ�ȡ�����ڵ�ļ�
#define dictGetKey(he) ((he)->key)
// ���ػ�ȡ�����ڵ��ֵ
#define dictGetVal(he) ((he)->v.val)
// ���ػ�ȡ�����ڵ���з�������ֵ
#define dictGetSignedIntegerVal(he) ((he)->v.s64)
// ���ظ����ڵ���޷�������ֵ
#define dictGetUnsignedIntegerVal(he) ((he)->v.u64)
// ���ظ����ֵ�Ĵ�С
#define dictSlots(d) ((d)->ht[0].size+(d)->ht[1].size)
// �����ֵ�����нڵ�����
#define dictSize(d) ((d)->ht[0].used+(d)->ht[1].used)
// �鿴�ֵ��Ƿ����� rehash
#define dictIsRehashing(ht) ((ht)->rehashidx != -1)

/* API */
dict *dictCreate(dictType *type, void *privDataPtr);
int dictExpand(dict *d, unsigned long size);
int dictAdd(dict *d, void *key, void *val);
dictEntry *dictAddRaw(dict *d, void *key);
int dictReplace(dict *d, void *key, void *val);
dictEntry *dictReplaceRaw(dict *d, void *key);
int dictDelete(dict *d, const void *key);
int dictDeleteNoFree(dict *d, const void *key);
void dictRelease(dict *d);
dictEntry * dictFind(dict *d, const void *key);
void *dictFetchValue(dict *d, const void *key);
int dictResize(dict *d);
dictIterator *dictGetIterator(dict *d);
dictIterator *dictGetSafeIterator(dict *d);
dictEntry *dictNext(dictIterator *iter);
void dictReleaseIterator(dictIterator *iter);
dictEntry *dictGetRandomKey(dict *d);
int dictGetRandomKeys(dict *d, dictEntry **des, int count);
void dictPrintStats(dict *d);
unsigned int dictGenHashFunction(const void *key, int len);
unsigned int dictGenCaseHashFunction(const unsigned char *buf, int len);
void dictEmpty(dict *d, void(callback)(void*));
void dictEnableResize(void);
void dictDisableResize(void);
int dictRehash(dict *d, int n);
int dictRehashMilliseconds(dict *d, int ms);
void dictSetHashFunctionSeed(unsigned int initval);
unsigned int dictGetHashFunctionSeed(void);
unsigned long dictScan(dict *d, unsigned long v, dictScanFunction *fn, void *privdata);

extern dictType dictTypeHeapStringCopyKey;
extern dictType dictTypeHeapStrings;
extern dictType dictTypeHeapStringCopyKeyValue;
#endif