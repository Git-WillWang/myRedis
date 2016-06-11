#include "redis.h"
struct redisServer server;
struct redisCommand *commandTable;

struct redisCommand redisCommandTable[] = {
	{"get",getCommand,2,"r",0,NULL,1,1,1,0,0},
	{}
};