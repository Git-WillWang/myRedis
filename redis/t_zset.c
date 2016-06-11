#include "redis.h"
#include <math.h>

static int zslLexValueGteMin(robj *value, zlexrangespec *spec);
static int zslLexValueLteMax(robj *value, zlexrangespec *spec);

/*
* ����һ������Ϊ level ����Ծ��ڵ㣬
* �����ڵ�ĳ�Ա��������Ϊ obj ����ֵ����Ϊ score ��
*
* ����ֵΪ�´�������Ծ��ڵ�
*
* T = O(1)
*/
zskiplistNode *zslCreateNode(int level, double score, robj *obj) {
	zskiplistNode *zn = zmalloc(sizeof(*zn) + level * sizeof(struct zskiplistLevel));
	zn->score = score;
	zn->obj = obj;
	return zn;
}

zskiplist *zslCreate(void) {
	int j;
	zskiplist *zsl;
	zsl = zmalloc(sizeof(*zsl));
	zsl->level = 1;
	zsl->length = 0;
	zsl->header = zslCreateNode(ZSKIPLIST_MAXLEVEL, 0, NULL);
	for (j = 0; j < ZSKIPLIST_MAXLEVEL; ++j) {
		zsl->header->level[j].forward = NULL;
		zsl->header->level[j].span = 0;
	}
	zsl->header->backward = NULL;
	zsl->tail = NULL;
	return zsl;
}

void zslFree(zskiplist *zsl) {
	zskiplistNode *node = zsl->header->level[0].forward, *next;
	zfree(zsl->header);
	while (node) {
		next = node->level[0].forward;
		zslFreeNode(node);
		node = next;
	}
	zfree(zsl);
}

int zslRandomLevel(void) {
	int level = 1;
	while ((random() & 0xFFFF) < (ZSKIPLIST_P * 0xFFFF))
		++level;
	return (level < ZSKIPLIST_MAXLEVEL) ? level : ZSKIPLIST_MAXLEVEL;
}

zskiplistNode *zslInsert(zskiplist *zsl,double score,robj *obj){
	zskiplistNode *update[ZSKIPLIST_MAXLEVEL], *x;
	unsigned int rank[ZSKIPLIST_MAXLEVEL];
	int i, level;
	redisAssert(!isnan(score));
	x = zsl->header;

	for (i = zsl->level - 1; i >= 0; --i) {
		rank[i] = i == (zsl->level - 1) ? 0 : rank[i + 1];
		while (x->level[i].forward && 
			(x->level[i].forward->score < score ||
			(x->level[i].forward->score == score&&compareStringObjects(x->level[i].forward->obj, obj) < 0))) {
			rank[i] += x->level[i].span;
			x = x->level[i].forward;
		}
		update[i] = x;
	}
	level = zslRandomLevel();
	if (level > zsl->level) {
		for (i = zsl->level; i < level; ++i) {
			rank[i] = 0;
			update[i] = zsl->header;
			update[i]->level[i].span = zsl->length;
		}
		zsl->level = level;
	}
	x = zslCreateNode(level, score, obj);
	for (i = 0; i < level; ++i) {
		x->level[i].forward = update[i]->level[i].forward;
		update[i]->level[i].forward = x;
		x->level[i].span = update[i]->level[i].span - (rank[0] - rank[i]);	
		update[i]->level[i].span = (rank[0] - rank[i]) + 1;
	}
	for (i = level; i < zsl->length; ++i)
		update[i]->level[i].span++;
	x->backward = (update[0] == zsl->header) ? NULL : update[0];
	if (x->level[0].forward)
		x->level[0].forward->backward = x;
	else zsl->tail = x;
	zsl->length++;
	return x;
}

void zslDeleteNode(zskiplist *zsl, zskiplistNode *x, zskiplistNode **update) {
	int i;
	for (i = 0; i < zsl->level; ++i) {
		if (update[i]->level[i].forward == x) {
			update[i]->level[i].span += x->level[i].span - 1;
			update[i]->level[i].forward = x->level[i].forward;
		}
		else {
			update[i]->level[i].span -= 1;
		}
	}
	if (x->level[0].forward) {
		x->level[0].forward->backward = x->backward;
	}
	else
		zsl->tail = x->backward;
	while (zsl->level > 1 && zsl->header->level[zsl->level - 1].forward == NULL)
		zsl->level--;
	zsl->length--;
}

int zslDelete(zskiplist *zsl, double score, robj *obj) {
	zskiplistNode *update[ZSKIPLIST_MAXLEVEL], *x;
	int i;
	x = zsl->header;
	for (i = zsl->level - 1; i >= 0; --i) {
		while (x->level[i].forward &&
			(x->level[i].forward->score < score ||
			(x->level[i].forward->score == score&&
				compareStringObjects(x->level[i].forward->obj, obj) < 0)))
			x = x->level[i].forward;
		update[i] = x;
	}
	x = x->level[0].forward;
	if (x&&score == x->score&&equalStringObjects(x->obj, obj)) {
		zslDeleteNode(zsl, x, update);
		zslFreeNode(x);
		return 1;
	}
	else
		return 0;
	return 0;
}

static int zslValueGetMin(double value, zrangespec *spec) {
	return spec->minex ? (value > spec->min) : (value >= spec->min);
}

static int zslValueLteMax(double value, zrangespec *spec) {
	return spec->maxex ? (value < spec->max) : (value <= spec->max);
}

int zslIsInRange(zskiplist *zsl, zrangespec *range) {
	zskiplistNode *x;
	if (range->min > range->max || (range->min == range->max && (range->minex || range->maxex)))
		return 0;
	x = zsl->tail;
	if (x == NULL || !zslValueGetMin(x->score, range))
		return 0;
	x = zsl->header->level[0].forward;
	if (x == NULL || !zslValueLteMax(x->score, range))
		return 0;
	return 1;
}

zskiplistNode *zslFirstInRange(zskiplist *zsl, zrangespec *range) {
	zskiplistNode *x;
	int i;
	x = zsl->header;
	for (i = zsl->level - 1; i >= 0; --i) {
		while (x->level[i].forward &&
			!zslValueGetMin(x->level[i].forward->score, range))
			x = x->level[i].forward;
	}
	x = x->level[0].forward;
	redisAssert(x != NULL);
	if (!zslValueLteMax(x->score, range)) return NULL;
	return x;
}

zskiplistNode *zslLastInRange(zskiplist *zsl, zrangespec *range) {
	zskiplistNode *x;
	int i;

	/* If everything is out of range, return early. */
	// ��ȷ����Ծ����������һ���ڵ���� range ָ���ķ�Χ��
	// ����ֱ��ʧ��
	// T = O(1)
	if (!zslIsInRange(zsl, range)) return NULL;

	// ������Ծ�����ҷ��Ϸ�Χ max ��Ľڵ�
	// T_wrost = O(N), T_avg = O(log N)
	x = zsl->header;
	for (i = zsl->level - 1; i >= 0; i--) {
		/* Go forward while *IN* range. */
		while (x->level[i].forward &&
			zslValueLteMax(x->level[i].forward->score, range))
			x = x->level[i].forward;
	}

	/* This is an inner range, so this node cannot be NULL. */
	redisAssert(x != NULL);

	/* Check if score >= min. */
	// ���ڵ��Ƿ���Ϸ�Χ�� min ��
	// T = O(1)
	if (!zslValueGteMin(x->score, range)) return NULL;

	// ���ؽڵ�
	return x;
}

unsigned long zslDeleteRangeByScore(zskiplist *zsl, zrangespec *range, dict *dict) {
	zskiplistNode *update[ZSKIPLIST_MAXLEVEL], *x;
	unsigned long removed = 0;
	int i;
	x = zsl->header;
	for (i = zsl->level - 1; i >= 0; --i) {
		while (x->level[i].forward && (range->minex ?
			x->level[i].forward->score <= range->min :
			x->level[i].forward->score < range->min))
			x = x->level[i].forward;
		update[i] = x;
	}
	x = x->level[0].forward;
	while (x && (range->maxex ? x->score < range->max : x->score <= range->max)) {
		zskiplistNode *next = x->level[0].forward;
		zslDeleteNode(zsl, x, update);
		dictDelete(dict, x->obj);
		zslFreeNode(x);
		removed++;
		x = next;
	}
	return removed;
}

unsigned long zslDeleteRangeByLex(zskiplist *zsl, zlexrangespec *range, dict *dict) {
	zskiplistNode *update[ZSKIPLIST_MAXLEVEL], *x;
	unsigned long removed = 0;
	int i;


	x = zsl->header;
	for (i = zsl->level - 1; i >= 0; i--) {
		while (x->level[i].forward &&
			!zslLexValueGteMin(x->level[i].forward->obj, range))
			x = x->level[i].forward;
		update[i] = x;
	}

	/* Current node is the last with score < or <= min. */
	x = x->level[0].forward;

	/* Delete nodes while in range. */
	while (x && zslLexValueLteMax(x->obj, range)) {
		zskiplistNode *next = x->level[0].forward;

		// ����Ծ����ɾ����ǰ�ڵ�
		zslDeleteNode(zsl, x, update);
		// ���ֵ���ɾ����ǰ�ڵ�
		dictDelete(dict, x->obj);
		// �ͷŵ�ǰ��Ծ��ڵ�Ľṹ
		zslFreeNode(x);

		// ����ɾ��������
		removed++;

		// ���������¸��ڵ�
		x = next;
	}

	// ���ر�ɾ���ڵ������
	return removed;
}

unsigned long zslDeleteRangeByRank(zskiplist *zsl, unsigned int start, unsigned int end, dict *dict) {
	zskiplistNode *update[ZSKIPLIST_MAXLEVEL], *x;
	unsigned long traversed = 0, removed = 0;
	int i;
	x = zsl->header;
	for (i = zsl->level - 1; i >= 0; --i) {
		while (x->level[i].forward && (traversed + x->level[i].span) < start) {
			traversed += x->level[i].span;
			x = x->level[i].forward;
		}
		update[i] = x;
	}
	traversed++;
	x = x->level[0].forward;
	while (x&&traversed <= end) {
		zskiplistNode *next = x->level[0].forward;
		zslDeleteNode(zsl, x, update);
		dictDelete(dict, x->obj);
		zslFreeNode(x);
		removed++;
		traversed++;
		x = next;
	}
	return removed;
}
unsigned long zslGetRank(zskiplist *zsl, double score, robj *o) {
	zskiplistNode *x;
	unsigned long rank = 0;
	int i;

	// ����������Ծ��
	x = zsl->header;
	for (i = zsl->level - 1; i >= 0; i--) {

		// �����ڵ㲢�Ա�Ԫ��
		while (x->level[i].forward &&
			(x->level[i].forward->score < score ||
				// �ȶԷ�ֵ
			(x->level[i].forward->score == score &&
				// �ȶԳ�Ա����
				compareStringObjects(x->level[i].forward->obj, o) <= 0))) {

			// �ۻ���Խ�Ľڵ�����
			rank += x->level[i].span;

			// ����ǰ��ָ�������Ծ��
			x = x->level[i].forward;
		}

		/* x might be equal to zsl->header, so test if obj is non-NULL */
		// ����ȷ��������ֵ��ȣ����ҳ�Ա����ҲҪ���
		// T = O(N)
		if (x->obj && equalStringObjects(x->obj, o)) {
			return rank;
		}
	}

	// û�ҵ�
	return 0;
}

zskiplistNode* zslGetElementByRank(zskiplist *zsl, unsigned long rank) {
	zskiplistNode *x;
	unsigned long traversed = 0;
	int i;

	// T_wrost = O(N), T_avg = O(log N)
	x = zsl->header;
	for (i = zsl->level - 1; i >= 0; i--) {

		// ������Ծ���ۻ�Խ���Ľڵ�����
		while (x->level[i].forward && (traversed + x->level[i].span) <= rank)
		{
			traversed += x->level[i].span;
			x = x->level[i].forward;
		}

		// ���Խ���Ľڵ������Ѿ����� rank
		// ��ô˵���Ѿ�����Ҫ�ҵĽڵ�
		if (traversed == rank) {
			return x;
		}
	}
	// û�ҵ�Ŀ��ڵ�
	return NULL;
}

static int zslParseRange(robj *min, robj *max, zrangespec *spec) {
	char *eptr;
	spec->minex = spec->maxex = 0;
	if (min->encoding == REDIS_ENCODING_INT) {
		spec->min = (long)min->ptr;
	}
	else {
		if (((char*)min->ptr)[0] == '(') {
			spec->min = strtod((char*)min->ptr + 1, &eptr);
			if (eptr[0] != '\0' || isnan(spec->min)) return REDIS_ERR;
			spec->minex = 1;
		}else{
			spec->min = strtod((char*)min->ptr, &eptr);
			if (eptr[0] != '\0' || isnan(spec->min)) return REDIS_ERR;
		}
	}
	if (max->encoding == REDIS_ENCODING_INT) {
		spec->max = (long)max->ptr;
	}
	else {
		if (((char*)max->ptr)[0] == '(') {
			spec->max = strtod((char*)max->ptr + 1, &eptr);
			if (eptr[0] != '\0' || isnan(spec->max)) return REDIS_ERR;
			spec->maxex = 1;
		}
		else {
			// T = O(N)
			spec->max = strtod((char*)max->ptr, &eptr);
			if (eptr[0] != '\0' || isnan(spec->max)) return REDIS_ERR;
		}
	}
	return REDIS_OK;
}

int zslParseLexRangeItem(robj *item, robj **dest, int *ex) {
	char *c = item->ptr;
	switch (c[0]) {
	case '+':
		if (c[1] != '\0') return REDIS_ERR;
		*ex = 0;
		*dest = shared.maxstring;
		incrRefCount(shared.maxstring);
		return REDIS_OK;
	case '-':
		if (c[1] != '\0') return REDIS_ERR;
		*ex = 0;
		*dest = shared.minstring;
		incrRefCount(shared.minstring);
		return REDIS_OK;
	case '[':
		*ex = 1;
		*dest = createStringObject(c + 1, sdslen(c) - 1);
		return REDIS_OK;
	default:
		return REDIS_ERR;
	}
}

static int zslParseLexRange()