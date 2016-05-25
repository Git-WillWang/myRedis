#pragma once
typedef struct listNode {
	struct listNode *prev;
	struct listNode *next;
	void *value;
}listNode;

typedef struct listIter {
	listNode* next;
	int direction;
}listIter;

typedef struct list {
	listNode *head;
	listNode *tail;
	void *(*dup)(void *ptr);
	void(*free)(void *ptr);
	int(*match)(void *ptr, void *key);
	unsigned long len;
}list;

#define listLength(l) ((l)->len)

#define listFirst(l) ((l)->head)

#define listLast(l) ((l)->tail)

#define listPrevNode(n) ((n)->prev)

#define listNextNode(n) ((n)->next)

#define listNodeValue(n) ((n)->value)

#define listSetDupMethod(l,m)((l)->dup =(m))

#define listSetFreeMethod(l,m) ((l)->free = (m))
// 将链表的对比函数设置为 m
// T = O(1)
#define listSetMatchMethod(l,m) ((l)->match = (m))

// 返回给定链表的值复制函数
// T = O(1)
#define listGetDupMethod(l) ((l)->dup)
// 返回给定链表的值释放函数
// T = O(1)
#define listGetFree(l) ((l)->free)
// 返回给定链表的值对比函数
// T = O(1)
#define listGetMatchMethod(l) ((l)->match)

list *listCreate(void);
void listRelease(list *list);
list *listAddNodeHead(list *list, void *value);
list *listAddNodeTail(list *list, void *value);
list *listInsertNode(list *list, listNode *old_node, void *value, int after);
void listDelNode(list *list, listNode *node);
listIter *listGetIterator(list *list, int direction);
listNode *listNext(listIter *iter);
void listReleaseIterator(listIter *iter);
list *listDup(list *orig);
listNode *listSearchKey(list *list, void *key);
listNode *listIndex(list *list, long index);
void listRewind(list *list, listIter *li);
void listRewindTail(list *list, listIter *li);
void listRotate(list *list);

// 从表头向表尾进行迭代
#define AL_START_HEAD 0
// 从表尾到表头进行迭代
#define AL_START_TAIL 1