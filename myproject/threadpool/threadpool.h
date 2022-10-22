#ifndef __THREADPOOL_H
#define __THREADPOOL_H

#include <list>
#include <cstdio>
#include <exception>
#include <pthread.h>
#include "../lock/locker.h"
#include "../CGImysql/sql_connection_pool.h"

template <typename T>
class threadpool {
    public:
        threadpool(connection_pool * connpool, int thread_number = 8, int max_request = 10000);
        ~threadpool();
        bool append(T * request);

    private:

        static void * worker(void * arg);
        void run();
    private:

        int m_thread_number;//线程池中的连接数
        int m_max_requests;//请求队列中允许的最大请求数
        pthread_t * m_threads;//描述线程池的数组，其大小为m_thread_number
        std::list<T *> m_workqueue;//请求队列
        locker m_queuelocker;//保护请求队列的互斥锁
        sem m_queuestat;//是否有任务需要处理，信号量
        bool m_stop;//是否结束线程
        connection_pool * m_connPool;//数据库
};

template <typename T>
threadpool<T>::threadpool(connection_pool * connpool, int thread_number, int max_request): m_thread_number(thread_number),
                                                                                           m_stop(false),
                                                                                           m_max_requests(max_request),
                                                                                           m_threads(NULL),
                                                                                           m_connPool(connpool)
{
    if(thread_number <= 0 || max_request <= 0) {
        throw std::exception();
    }

    m_threads = new pthread_t[m_thread_number];//创建线程池
    
    if(!m_threads) {
        throw std::exception();
    }
    
    for(int i = 0; i < thread_number; ++i) {//开始初始化线程池
        if(pthread_create(m_threads + i, NULL, worker, this) != 0) {
            delete [] m_threads;
            throw std::exception();
        }

        if(pthread_detach(m_threads[i])) {
            delete [] m_threads;
            throw std::exception();
        }
    }
    
}

template <typename T>
threadpool<T>::~threadpool() {
    delete [] m_threads;
    m_stop = true;
}

template <typename T>
bool threadpool<T>::append(T * quest) {//向任务队列中添加T类型的请求
    m_queuelocker.lock();

    if(m_workqueue.size() > m_max_requests) {
        m_queuelocker.unlock();
        return false;
    }

    m_workqueue.push_back(quest);

    m_queuelocker.unlock();

    m_queuestat.post();

    return true;
}


template <typename T>
void * threadpool<T>::worker(void * arg) {
    threadpool * pool = (threadpool *)(arg);
    pool->run();
    return pool;
}

template <typename T>
void threadpool<T>::run() {
    while(!m_stop) {//运行线程池
        m_queuestat.wait();
        m_queuelocker.lock();

        if(m_workqueue.empty()) {
            m_queuelocker.unlock();
            continue ;
        }

        T * request = m_workqueue.front();
        m_workqueue.pop_front();
        m_queuelocker.unlock();

        if(!request) {
            continue;
        }

        connectionRAII mysqlcon(&request->mysql, m_connPool);

        request->process();
    }
}







#endif