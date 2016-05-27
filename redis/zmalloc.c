#include <stdio.h>
#include <stdlib.h>
/*在包含tcmalloc等其他非标准分配器之前除出现，这个函数被用于对被
backtrace_symbols()函数获取的实例的结果的释放
*/
void zlibc_free(void *ptr) {
	free(ptr);
}

#include <string.h>
#include <pthread.h>
#include "config.h"
#include "zmalloc.h"

/*
	PREFIX_SIZE用于记录malloc已分配内存大小
	tc_malloc/jemalloc/Mac平台分别采用tc_malloc_size/je_malloc_size_usable_size/malloc_size
	表示分配内存大小，不使用PREFIX_SIZE，设置为0
	linux/sun平台分别采用sizeof(size_t)=8字节和sizeof(long long)定长字段记录
*/
#ifdef HAVE_MALLOC_SIZE
#define PREFIX_SIZE (0)
#else
#if defined(__sun) || defined(__sparc) || defined(__sparc__)
#define PREFIX_SIZE (sizeof(long long))
#else
#define PREFIX_SIZE (sizeof(size_t))
#endif
#endif

//使用tc_malloc接口覆盖malloc接口
#if defined(USE_TCMALLOC)
#define malloc(size) tc_malloc(size)
#define calloc(count,size) tc_malloc(count,size)
#define realloc(ptr,size) tc_realloc(ptr,size)
#define free(ptr) tc_free(ptr)
//使用je_malloc接口覆盖malloc接口
#elif defined(USE_JEMALLOC)
#define malloc(size) je_malloc(size)
#define calloc(count,size) je_calloc(count,size)
#define realloc(ptr,size) je_realloc(ptr,size)
#define free(ptr) je_free(ptr)
#endif

#ifdef HAVE_ATOMIC
#define update_zmalloc_stat_add(__n) __sync_add_and_fetch(&used_memory, (__n))
#define update_zmalloc_stat_sub(__n) __sync_sub_and_fetch(&used_memory, (__n))
#else
#define update_zmalloc_stat_add(__n)do{\
	pthread_mutex_lock(&used_memory_mutex);\
	used_memory+=(__n);\
	pthread_mutex_unlock(&used_memory_mutex);\
}while(0)

#define update_zmalloc_stat_sub(__n) do {\
	pthread_mutex_lock(&used_memory_mutex);\
	used_memory -=(__n);\
	pthread_mutex_unlock(&used_memory_mutex);\
}while(0)
#endif

//_n&(sizeof(long)-1)将_n大于sizeof(long)位数的部分掩掉，并加上余值
//使其满足最小内存分配单元
#define update_zmalloc_stat_alloc(__n) do{\
	size_t _n = (__n);\
	if (_n & (sizeof(long) - 1)) _n += sizeof(long) - (_n&(sizeof(long) - 1));\
	if(zmalloc_thread_safe){\
		update_zmalloc_stat_add(_n);\
	}else {\
		used_memory+=_n;\
	}\
} while(0)

#define update_zmalloc_stat_free(__n) do{\
	size_t _n= (__n);\
	if(_n&(sizeof(long)-1)) _n += sizeof(long)-(_n&(sizeof(long)-1));\
	if(zmalloc_thread_safe){\
		update_zmalloc_stat_sub(_n);\
	}else{\
		used_memory -=_n;\
	}\
} while(0)

static size_t used_memory = 0;
static int zmalloc_thread_safe = 0;
pthread_mutex_t used_memory_mutex = PTHREAD_MUTEX_INITIALIZER;

//Out of memory
static void zmalloc_default_oom(size_t size) {
	fprintf(stderr, "zmalloc: Out of memroy trying to allocate %zu bytes\n", size);
	fflush(stderr);
	abort();
}

static void(*zmalloc_oom_handler)(size_t) = zmalloc_default_oom;

void *zmalloc(size_t size) {
	//分配size+PREFIX_SIZE的空间，PREFIX_SIZE用于存放其他
	void *ptr = malloc(size + PREFIX_SIZE);
	if (!ptr) zmalloc_oom_handler(size);
//如果存在malloc_size函数用于判断内存分配情况，此时PREFIX_SIZE=0
#ifdef HAVE_MALLOC_SIZE
	update_zmalloc_stat_alloc(zmalloc_size(ptr));//使用zmalloc_size函数确定具体分配情况，并更新到used_memory
	return ptr;
#else//如果不存在
	//只知道分配内存大小，没有malloc_size函数获得具体内存分配大小，在分配空间的起始位置存储变量size
	*((size_t*)ptr) = size;
	update_zmalloc_alloc(size + PREFIX_SIZE);
	//将指针后移PREFIX_SIZE，因为前PREFIX_SIZE用于保存size_t size
	return (char*)ptr + PREFIX_SIZE;
#endif
}

void *zcalloc(size_t size) {
	void *ptr = calloc(1, size + PREFIX_SIZE);
#ifdef HAVE_MALLOC_SIZE
	update_zmalloc_stat_alloc(zmalloc_size(ptr));
	return ptr;
#else
	*((size_t*)ptr) = size;
	update_zmalloc_stat_alloc(size + PREFIX_SIZE);
	return (char*)ptr + PREFIX_SIZE;
#endif
}

void *zrealloc(void *ptr, size_t size) {
#ifndef HAVE_MALLOC_SIZE
	void *realptr;
#endif;
	size_t oldsize;
	void *newptr;
	if (ptr == NULL) return zmalloc(size);
//如果有malloc_size函数返回当前内存分配量
#ifdef HAVE_MALLOC_SIZE
	oldsize = zmalloc_size(ptr);
	newptr = realloc(ptr, size);
	if (!newptr) zmalloc_oom_handler(size);
	update_zmalloc_stat_free(oldsize);
	update_zmalloc_stat_alloc(zmalloc_size(newptr));
	return newptr;
#else
	realptr = (char*)ptr - PREFIX_SIZE;
	oldsize = *((size_t*)realptr);
	newptr = realloc(realptr, size + PREFIX_SIZE);
	if (!newptr) zmalloc_oom_handler(size);
	*((size_t*)newptr) = size;
	update_zmalloc_stat_free(oldsize);
	update_zmalloc_stat_alloc(size);
	return (char*)newptr + PREFIX_SIZE;
#endif
}

#ifndef HAVE_MALLOC_SIZE
size_t zmalloc_size(void *ptr) {
	void *realptr = (char*)ptr - PREFIX_SIZE;
	size_t size = *((size_t*)realptr);
	if (size&(sizeof(long) - 1)) size += sizeof(long) - (size&(sizeof(long) - 1));
	return size + PREFIX_SIZE;
}
#endif

void zfree(void *ptr) {
#ifndef HAVE_MALLOC_SIZE
	void *realptr;
	size_t oldsize;
#endif // !HAVE_MALLOC_SIZE
	if (ptr == NULL) return;
#ifdef HAVE_MALLOC_SIZE
	update_zmalloc_stat_free(zmalloc_size(ptr));
	free(ptr);
#else
	realptr = (char*)ptr - PREFIX_SIZE;
	oldsize = *((size_t*)realptr);
	update_zmalloc_stat_free(oldsize + PREFIX_SIZE);
	free(realptr);
#endif // HAVE_MALLOC_SIZE
}

char *zstrdup(const char *s) {
	size_t l = strlen(s) + 1;
	char *p = zmalloc(l);
	memcpy(p, s, l);
	return p;
}

//返回已使用内存大小
size_t zmalloc_used_memory(void) {
	size_t um;
	if (zmalloc_thread_safe) {
#ifdef HAVE_ATOMIC
		um = __sync_add_and_fetch(&used_memory, 0);
#else
		pthread_mutex_lock(&used_memory_mutex);
		um = used_memory;
		pthread_mutex_unlock(&used_memory_mutex);
#endif
	}
	else {
		um = used_memory;
	}
	return um;
}
void zmalloc_enable_thread_safeness(void) {
	zmalloc_thread_safe = 1;
}
void zmalloc_set_oom_handler(void(*oom_handler(size_t))) {
	zmalloc_oom_handler = oom_handler;
}
