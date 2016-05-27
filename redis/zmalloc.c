#include <stdio.h>
#include <stdlib.h>
/*�ڰ���tcmalloc�������Ǳ�׼������֮ǰ�����֣�������������ڶԱ�
backtrace_symbols()������ȡ��ʵ���Ľ�����ͷ�
*/
void zlibc_free(void *ptr) {
	free(ptr);
}

#include <string.h>
#include <pthread.h>
#include "config.h"
#include "zmalloc.h"

/*
	PREFIX_SIZE���ڼ�¼malloc�ѷ����ڴ��С
	tc_malloc/jemalloc/Macƽ̨�ֱ����tc_malloc_size/je_malloc_size_usable_size/malloc_size
	��ʾ�����ڴ��С����ʹ��PREFIX_SIZE������Ϊ0
	linux/sunƽ̨�ֱ����sizeof(size_t)=8�ֽں�sizeof(long long)�����ֶμ�¼
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

//ʹ��tc_malloc�ӿڸ���malloc�ӿ�
#if defined(USE_TCMALLOC)
#define malloc(size) tc_malloc(size)
#define calloc(count,size) tc_malloc(count,size)
#define realloc(ptr,size) tc_realloc(ptr,size)
#define free(ptr) tc_free(ptr)
//ʹ��je_malloc�ӿڸ���malloc�ӿ�
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

//_n&(sizeof(long)-1)��_n����sizeof(long)λ���Ĳ����ڵ�����������ֵ
//ʹ��������С�ڴ���䵥Ԫ
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
	//����size+PREFIX_SIZE�Ŀռ䣬PREFIX_SIZE���ڴ������
	void *ptr = malloc(size + PREFIX_SIZE);
	if (!ptr) zmalloc_oom_handler(size);
//�������malloc_size���������ж��ڴ�����������ʱPREFIX_SIZE=0
#ifdef HAVE_MALLOC_SIZE
	update_zmalloc_stat_alloc(zmalloc_size(ptr));//ʹ��zmalloc_size����ȷ�������������������µ�used_memory
	return ptr;
#else//���������
	//ֻ֪�������ڴ��С��û��malloc_size������þ����ڴ�����С���ڷ���ռ����ʼλ�ô洢����size
	*((size_t*)ptr) = size;
	update_zmalloc_alloc(size + PREFIX_SIZE);
	//��ָ�����PREFIX_SIZE����ΪǰPREFIX_SIZE���ڱ���size_t size
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
//�����malloc_size�������ص�ǰ�ڴ������
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

//������ʹ���ڴ��С
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