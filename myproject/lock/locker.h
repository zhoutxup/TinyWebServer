#ifndef __LOCKER_H
#define __LOCKER_H


#include <exception>
#include <pthread.h>
#include <semaphore.h>

class sem {//设置信号量，用于数据之间的同步
public:
    sem() {
        if(sem_init(&m_sem, 0, 0) != 0) {
            throw std::exception();
        }
    }

    sem(int num) {
        if(sem_init(&m_sem, 0, num)) {
            throw std::exception();
        }
    }

    ~sem() {
        sem_destroy(&m_sem);
    }

    bool wait() {//执行减一操作
        return sem_wait(&m_sem) == 0;
    }

    bool post() {//执行+1操作
        return sem_post(&m_sem) == 0;
    }


private:
    sem_t m_sem;
};

class locker {//给临界资源加/解锁
public:
    locker() {
        if(pthread_mutex_init(&m_mutex, NULL) != 0) {
            throw std::exception();
        }
        
    }

    ~locker() {
        pthread_mutex_destroy(&m_mutex);
    }

    bool lock() {
        return pthread_mutex_lock(&m_mutex) == 0;
    }

    bool unlock() {
        return pthread_mutex_unlock(&m_mutex) == 0;
    }

    pthread_mutex_t * get() {
        return &m_mutex;
    }
private:
    pthread_mutex_t m_mutex;
};

class cond {
public:
    cond() {
        if(pthread_cond_init(&m_cond, NULL) != 0) {
            throw std::exception();
        }
    }

    ~cond() {
        pthread_cond_destroy(&m_cond);
    }

    bool wait(pthread_mutex_t * m_mutex) {
        int ret = 0;

        ret = pthread_cond_wait(&m_cond, m_mutex);

        return ret == 0;
    }

    bool timewait(pthread_mutex_t * m_mutex, timespec t) {
        int ret = 0;

        ret = pthread_cond_timedwait(&m_cond, m_mutex, &t);

        return ret == 0;
    }

    bool signal() {
        return pthread_cond_signal(&m_cond) == 0;
    }

    bool broadcast() {
        return pthread_cond_broadcast(&m_cond) == 0;
    }

private:
    pthread_cond_t m_cond;
};



#endif