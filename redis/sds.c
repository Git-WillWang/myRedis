#pragma once

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <assert.h>
#include "sds.h"
//#include "zmalloc.h"

sds sdsnewlen(const void* init, size_t initlen) {
	struct sdshdr *sh;
	if (init) {
		sh = zmalloc(sizeof(struct sdshdr) + initlen + 1);
	}
	else {
		sh = zcalloc(sizeof(struct sdshdr) + initlen + 1);
	}
	if (sh == NULL) return NULL;
	sh->len = initlen;
	sh->free = 0;
	if (initlen && init)
		memcpy(sh->buf, init, initlen);
	sh->buf[initlen] = '\0';
	return (char*)sh->buf;
}

sds sdsempty(void) {
	return sdsnewlen("", 0);
}
sds sdsnew(const char* init) {
	size_t initlen = (init == NULL) ? 0 : strlen(init);
	return sdsnewlen(init, initlen);
}

sds sdsdup(const sds s) {
	return sdsnewlen(s, sdslen(s));
}
void sdsfree(sds s) {
	if (s == NULL)return;
	zfree(s - sizeof(struct sdshdr));
}

void sdsclear(sds s) {
	struct sdshdr *sh = (void*)(s - (sizeof(struct sdshdr)));
	sh->free += sh->len;
	sh->buf[0] = '\0';
}

sds sdsMakeRoomFor(sds s, size_t addlen) {
	size_t free = sdsavail(s);
	size_t len, newlen;
	if (free >= addlen) return s;
	len = sdslen(s);
	struct sdshdr *sh = (void*)(s - (sizeof(struct sdshdr)));
	newlen = (len + addlen);
	if (newlen < SDS_MAX_PREALLOC)
		newlen *= 2;
	else newlen += SDS_MAX_PREALLOC;
	struct sdshdr *newsh = zrealloc(sh, sizeof(struct sdshdr) + newlen + 1);
	if (newsh == NULL) return NULL;
	newsh->free = newlen - len;
	return newsh->buf;
}

sds sdsRemoveFreeSpace(sds s) {
	struct sdshdr *sh = (void*)(s - (sizeof(struct sdshdr)));
	sh = zrealloc(sh, sizeof(struct sdshdr) + sh->len + 1);
	sh->free = 0;
	return sh->buf;
}

size_t sdsAllocSize(sds s) {
	struct sdshdr *sh = (void*)(s - (sizeof(struct sdshdr)));
	return sizeof(*sh) + sh->len + sh->free + 1;
}

void sdsIncrLen(sds s, int incr) {
	struct sdshdr *sh = (void*)(s - (sizeof(struct sdshdr)));
	assert(sh->free >= incr);
	sh->len += incr;
	sh->free -= incr;
	assert(sh->free >= 0);
	s[sh->len] = '\0';
}

sds sdsgrowzero(sds s, size_t len) {
	struct sdshdr *sh = (void*)(s - (sizeof(struct sdshdr)));
	size_t totlen, curlen = sh->len;
	if (len <= curlen) return s;
	s = sdsMakeRoomFor(s, len - curlen);
	if (s == NULL) return NULL;
	sh = (void*)(s - (sizeof(struct sdshdr)));
	memset(s + curlen, 0, (len - curlen + 1));
	totlen = sh->len + sh->free;
	sh->len = len;
	sh->free = totlen - sh->len;
	return s;
}

sds sdscatlen(sds s, const void* t, size_t len) {
	size_t curlen = sdslen(s);
	s = sdsMakeRoomFor(s, len);
	if (s == NULL) return NULL;
	struct sdshdr *sh = (void*)(s - (sizeof(struct sdshdr)));
	memcpy(s + curlen, t, len);
	sh->len = curlen + len;
	sh->free = sh->free - len;
	s[curlen + len] = '\0';
	return s;
}

sds sdscat(sds s, const char* t) {
	return sdscatlen(s, t, strlen(t));
}
sds sdscatsds(sds s, const sds t) {
	return sdscatlen(s, t, sdslen(t));
}

sds sdscpylen(sds s, const char *t, size_t len) {
	struct sdshdr *sh = (void*)(s - (sizeof(struct sdshdr)));
	size_t totlen = sh->free + sh->len;
	if (totlen < len) {
		s = sdsMakeRoomFor(s, len - sh->len);
		if (s == NULL) return NULL;
		sh = (void*)(s - (sizeof(struct sdshdr)));
		totlen = sh->free + sh->len;
	}
	memcpy(s, t, len);
	s[len] = len;
	sh->free = totlen - len;
	return s;
}

sds sdscpy(sds s, const char* t) {
	return sdscpylen(s, t, strlen(t));
}

#define SDS_LISTR_SIZE 21
int sdsll2str(char *s, long long value) {
	unsigned long long v = value < 0 ? -value : value;
	char *p = s;
	do {
		*p++ = '0' + (v % 10);
		v /= 10;
	} while (v);
	if (value < 0) *p++ = '-';
	int l = p - s;
	*p = '\0';
	--p;
	char* aux;
	while (s < p) {
		aux = *s;
		*s = *p;
		*p = aux;
		++s; --p;
	}
	return l;
}

int sdsull2str(char *s, unsigned long long v) {
	char *p = *s;
	do {
		*p++ = '0' + (v % 10);
		v /= 10;
	} while (v);
	int l = p - s;
	*p = '\0';
	--p;
	char* aux;
	while (s < p) {
		aux = *s;
		*s = *p;
		*p = aux;
		++s; --p;
	}
	return l;
}
sds sdsfromlonglong(long long value) {
	char buf[SDS_LISTR_SIZE];
	int len = sdsll2str(buf, value);
	return sdsnewlen(buf, len);
}

sds sdscatvprintf(sds s, const char *fmt, va_list ap) {

}

sds sdscatprintf(sds s, const char *fmt, ...) {
	va_list ap;
	char *t;
	va_start(ap, fmt);
	t = sdscatvprintf(s, fmt, ap);
	va_end(ap);
	return t;
}

sds sdscatfmt(sds s, char const *fmt, ...) {
	struct sdshdr *sh = (void*)(s - (sizeof(struct sdshdr)));
	size_t initlen = sdslen(s);
	const char *f = fmt;
	int i;
	va_list ap;
	va_start(ap, fmt);
	f = fmt;
}