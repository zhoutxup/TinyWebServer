#include "../lock/locker.h"
#include <iostream>
#include "sql_connection_pool.h"
#include <mysql/mysql.h>
#include <stdio.h>
#include <string>
#include <string.h>
#include <stdlib.h>
#include <list>
#include <pthread.h>

connection_pool::connection_pool() {//构造函数，初始化池内连接的数量
    this->CurConn = 0;
    this->FreeConn = 0;
}

connection_pool * connection_pool::GetInstance() {
    static connection_pool connPool;//内部自动调用构造函数
    return &connPool;//返回全局唯一的数据库连接池
}

void connection_pool::init(string url, string user, string password, string databasename, int port, unsigned int maxConn) {
    this->Url = url;
    this->Port = port;
    this->User = user;
    this->PassWord = password;
    this->DataBaseName = databasename;

    lock.lock();//接下来要对连接池进行操作，必须保护数据
    
    for(int i = 0; i < maxConn; ++i) {
        MYSQL * con = NULL;
        con = mysql_init(con);
        if(con == nullptr) {
            cout << "Error: " << mysql_error(con);
            exit(1);
        }

        //建立一个与数据库的连接，失败返回null，成功返回con值
        con = mysql_real_connect(con, Url.c_str(), User.c_str(), PassWord.c_str(), DataBaseName.c_str(), Port, NULL, 0);

        if(con == nullptr) {
            cout << "Error: " << mysql_error(con);
            exit(1);
        }

        connlist.push_back(con);
        ++FreeConn;
    }

    reserve = sem(FreeConn);

    this->MaxConn = FreeConn;

    lock.unlock();
}

MYSQL * connection_pool::GetConnection() { //当有请求时，从数据库连接池中返回一个可用连接，更新使用和空闲连接数
    MYSQL * con = nullptr;

    if(connlist.size() == 0) {
        return nullptr;
    }

    reserve.wait();//设置信号量执行-1操作

    lock.lock();//对池的操作需要进行保护

    con = connlist.front();
    connlist.pop_front();//从连接池中取出头部

    --FreeConn;
    ++CurConn;

    lock.unlock();

    return con;
}

bool connection_pool::ReleaseConnection(MYSQL * conn) {
    if(NULL == conn) {
        return false;
    }

    lock.lock();

    connlist.push_back(conn);
    ++FreeConn;
    --CurConn;

    lock.unlock();
    reserve.post();
    return true;
}

void connection_pool::DestoryPool() {
    lock.lock();
    if(connlist.size() > 0) {
        list<MYSQL *>::iterator  it;
        for(it = connlist.begin(); it != connlist.end(); ++it) {
            MYSQL * con = *it;

            mysql_close(con);
        }
        CurConn = 0;
        FreeConn = 0;
        connlist.clear();
        lock.unlock();
    }

    lock.unlock();
}

int connection_pool::GetFreeConn() {
    return this->FreeConn;
}

connection_pool::~connection_pool() {
    DestoryPool();
}

connectionRAII::connectionRAII(MYSQL ** SQL, connection_pool * connpool) {
    *SQL = connpool->GetConnection();

    conRAII = *SQL;

    poolRAII = connpool;
}

connectionRAII::~connectionRAII() {
    poolRAII->ReleaseConnection(conRAII);
}