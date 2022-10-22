#include <string.h>
#include <stdio.h>
#include <mysql/mysql.h>
#include <iostream>
#include <unistd.h>
#include "CGImysql/sql_connection_pool.h"
#include "http/http_conn.h"
#include "log/log.h"
#include "log/block_queue.h"

using namespace std;
#define SYNLOG //同步写日志
// #define ASYLOG //异步写日志

int main() {
    #ifdef SYNLOG
    Log::get_instance()->init("ServerLog", 2000, 800000, 0); //同步日志模型
    #endif

    LOG_DEBUG("%s-%d", "test", 1);
    LOG_DEBUG("%s-%d", "test", 1);
}