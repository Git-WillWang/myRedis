#pragma once
#include "sds.h"
typedef struct zskiplistNode {
	robj *obj;
	double score;
	struct zskiplistNode *backward;
	struct zskiplistLevel {
		struct zskiplistNode *forward;
		unsigned int span;
	}level[];
}zskiplistNode;

typedef struct zskiplist {
	struct zskiplistNode *header, *tail;
	unsigned long length;
	int level;
}zskiplist;

zskiplist *zslCreate(void);
void zslFree(zskiplist *zsl);
zskiplistNode *zslInsert(zskiplist *zsl, double score, robj *obj);
unsigned char *zzlInsert(unsigned char *zl, robj *ele, double score);
int zslDelete(zskiplist *zsl, double score, robj *obj);
zskiplistNode *zslFirstInRange(zskiplist *zsl, zrangespec *range);
zskiplistNode *zslLastInRange(zskiplist *zsl, zrangespec *range);