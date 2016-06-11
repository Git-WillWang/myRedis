#pragma once
#ifndef __AE_H
#define __AE_H
#include <time.h>
/*
* 事件执行状态
*/
// 成功
#define AE_OK 0
//失败
#define AE_ERR -1

/*
* 文件事件状态
*/
// 未设置
#define AE_NONE 0
// 可读
#define AE_READABLE 1
// 可写
#define AE_WRITABLE 2

/*
* 时间处理器的执行 flags
*/
// 文件事件
#define AE_FILE_EVENTS 1
// 时间事件
#define AE_TIME_EVENTS 2
// 所有事件
#define AE_ALL_EVENTS (AE_FILE_EVENTS|AE_TIME_EVENTS)
// 不阻塞，也不进行等待
#define AE_DONT_WAIT 4

/*
* 决定时间事件是否要持续执行的 flag
*/
#define AE_NOMORE -1

/* Macros */
#define AE_NOTUSED(V) ((void) V)

/*
* 事件处理器状态
*/
struct aeEventLoop;
/* Types and data structures
*
* 事件接口
*/
typedef void aeFileProc(struct aeEventLoop *eventLoop, int fd, void *clientData, int mask);
typedef int aeTimeProc(struct aeEventLoop *eventLoop, long long id, void *clientData);
typedef void aeEventFinalizerProc(struct aeEventLoop *eventLoop, void *clientData);
typedef void aeBeforeSleepProc(struct aeEventLoop *eventLoop);
/* File event structure
*
* 文件事件结构
*/
typedef struct aeFileEvent {
	// 监听事件类型掩码，
	// 值可以是 AE_READABLE 1 或 AE_WRITABLE 2，
	// 或者 AE_READABLE | AE_WRITABLE 3
	int mask;/* one of AE_(READABLE|WRITABLE) */
	//读事件处理器
	aeFileProc *rfileProc;
	// 写事件处理器
	aeFileProc *wfileProc;
	// 多路复用库的私有数据
	void *clientData;
}aeFileEvent;
/* Time event structure
*
* 时间事件结构
*/
typedef struct aeTimeEvent {
	// 时间事件的唯一标识符
	long long id;
	// 事件的到达时间
	long when_sec; /* seconds */
	long when_ms; /* milliseconds */
	// 事件处理函数
	aeTimeProc *timeProc;

	// 事件释放函数
	aeEventFinalizerProc *finalizerProc;

	// 多路复用库的私有数据
	void *clientData;
	// 指向下个时间事件结构，形成链表
	struct aeTimeEvent *next;
}aeTimeEvent;
/* A fired event
*
* 已就绪事件
*/
typedef struct aeFiredEvent {
	// 已就绪文件描述符
	int fd;
	// 事件类型掩码，
	// 值可以是 AE_READABLE 或 AE_WRITABLE
	// 或者是两者的或
	int mask;
}aeFiredEvent;
/* State of an event based program 
 *
 * 事件处理器的状态
*/
typedef struct aeEventLoop {
	// 目前已注册的最大描述符
	int maxfd;/* highest file descriptor currently registered */
			  // 目前已追踪的最大描述符
	int setsize; /* max number of file descriptors tracked */

				 // 用于生成时间事件 id
	long long timeEventNextId;

	// 最后一次执行时间事件的时间
	time_t lastTime;     /* Used to detect system clock skew */

						 // 已注册的文件事件
	aeFileEvent *events; /* Registered events */

						 // 已就绪的文件事件
	aeFiredEvent *fired; /* Fired events */

						 // 时间事件
	aeTimeEvent *timeEventHead;

	// 事件处理器的开关
	int stop;

	// 多路复用库的私有数据
	void *apidata; /* This is used for polling API specific data */

	// 在处理事件前要执行的函数
	aeBeforeSleepProc *beforesleep;
}aeEventLoop;
/* Prototypes */
aeEventLoop *aeCreateEventLoop(int setsize);
void aeDeleteEventLoop(aeEventLoop *eventLoop);
void aeStop(aeEventLoop *eventLoop);
int aeCreateFileEvent(aeEventLoop *eventLoop, int fd, int mask,
	aeFileProc *proc, void *clientData);
void aeDeleteFileEvent(aeEventLoop *eventLoop, int fd, int mask);
int aeGetFileEvents(aeEventLoop *eventLoop, int fd);
long long aeCreateTimeEvent(aeEventLoop *eventLoop, long long milliseconds,
	aeTimeProc *proc, void *clientData,
	aeEventFinalizerProc *finalizerProc);
int aeDeleteTimeEvent(aeEventLoop *eventLoop, long long id);
int aeProcessEvents(aeEventLoop *eventLoop, int flags);
int aeWait(int fd, int mask, long long milliseconds);
void aeMain(aeEventLoop *eventLoop);
char *aeGetApiName(void);
void aeSetBeforeSleepProc(aeEventLoop *eventLoop, aeBeforeSleepProc *beforesleep);
int aeGetSetSize(aeEventLoop *eventLoop);
int aeResizeSetSize(aeEventLoop *eventLoop, int setsize);
#endif
