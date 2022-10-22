#ifndef __CONNECTION_POOL__
#define __CONNECTION_POOL__


#include <stdio.h>
#include <list>
#include <mysql/mysql.h>
#include <error.h>
#include <string.h>
#include <iostream>
#include <string>
#include "../lock/locker.h"

using namespace std;

class connection_pool {
public:
    MYSQL * GetConnection();//获取数据库连接
    bool ReleaseConnection(MYSQL * conn);//释放连接
    int GetFreeConn();//获取连接
    void DestoryPool();//销毁所有连接

    static connection_pool * GetInstance();

    void init(string url, string user, string password, string databasename, int port, unsigned int maxConn);

    connection_pool();
    ~connection_pool();


private:
    unsigned int MaxConn;//最大连接数
    unsigned int CurConn;//当前已使用的连接数
    unsigned int FreeConn;//当前空闲的连接数

private:
    locker lock;
    list<MYSQL *> connlist;//数据库的连接池
    sem reserve;

private:
    string Url;
    int Port;
    string User;
    string PassWord;
    string DataBaseName;

};

class connectionRAII {
    public:
        connectionRAII(MYSQL ** con, connection_pool * connpol);
        ~connectionRAII();

    private:
        MYSQL * conRAII;
        connection_pool * poolRAII;
};













#endif