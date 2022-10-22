#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <stdio.h>
#include <unistd.h>
#include <errno.h>
#include <fcntl.h>
#include <stdlib.h>
#include <cassert>
#include <sys/epoll.h>
#include "./lock/locker.h"
#include "./threadpool/threadpool.h"
#include "./timer/lst_timer.h"
#include "./http/http_conn.h"
#include "./log/log.h"
#include "./CGImysql/sql_connection_pool.h"

// 最大文件描述符数
#define MAX_FD 65536

// 最大事件数
#define MAX_EVENT_NUMBER 10000

// 最小超时单位
#define TIMESLOT 5


#define SYNLOG //同步写日志
// #define ASYLOG //异步写日志

#define listenfdLT //水平触发阻塞
// #define listenfdET //边缘触发非阻塞

extern int addfd(int epollfd, int fd, bool one_shot);
extern int remove(int epollfd, int fd);
extern int setnonblocking(int fd);

// 设置定时器相关参数
static int pipefd[2];
static sort_timer_lst timer_lst;//存放定时器的容器
static int epollfd = 0;

// 信号处理函数
void sig_handler(int sig) {
    int save_errno = errno;
    int msg = sig;
    send(pipefd[1], (char *)&msg, 1, 0);
    errno = save_errno;
}

// 设置信号函数
void addsig(int sig, void(handler)(int), bool restart = true) {
    struct sigaction sa;
    memset(&sa, '\0', sizeof(sa));

    sa.sa_handler = handler; //设置信号的处理函数
    if(restart) {
        sa.sa_flags |= SA_RESTART;
    }
    sigfillset(&sa.sa_mask);

    assert(sigaction(sig, &sa, NULL) != -1);
}

// 定时处理任务，重新定时用来不断触发sigalrm信号

void timer_handler() {
    timer_lst.tick();//每隔timeslot个时间调整一次
    alarm(TIMESLOT);
}

// 定时器的回调函数，是你出非活动连接在socket上的注册事件，并关闭
void cb_func(client_data * user_data) {
    epoll_ctl(epollfd, EPOLL_CTL_DEL, user_data->sockfd, 0);//将监听的文件描述符移出
    assert(user_data);
    close(user_data->sockfd);
    http_conn::m_user_count--;
    LOG_INFO("close fd %d", user_data->sockfd);
    Log::get_instance()->flush();
    // 定时器的回调函数十分简单，按时删除任务即可
}

void show_error(int connfd, const char * info) {
    printf("%s", info);
    send(connfd, info, strlen(info), 0);
    close(connfd);
}

int main(int argc, char * argv[]) {
        
    #ifdef ASYNLOG
        Log::get_instance()->init("ServerLog", 2000, 800000, 8); //异步日志模型
    #endif

    #ifdef SYNLOG
        Log::get_instance()->init("ServerLog", 2000, 800000, 0); //同步日志模型
    #endif
    
    if(argc <= 1) {
        printf("usage: %s ip_address port_number\n", basename(argv[0]));
        return 1;
    }
    
    int port = atoi(argv[1]);

    addsig(SIGPIPE, SIG_IGN);//忽略管道信号
    
    // 创建数据连接池，使用connctRaii类从数据连接池中获取一个数据连接
    connection_pool * connPool = connection_pool::GetInstance();
    connPool->init("localhost", "root", "000", "zhou", 3306, 8);

    // 创建线程池，线程池处理的任务为http_conn类任务
    threadpool<http_conn> * pool = NULL;
   
    try {
        pool = new threadpool<http_conn>(connPool);
    }
    catch (...) {
        LOG_ERROR("%s", "create poll error");
        return 1;
    }
    
    
    http_conn * users = new http_conn[MAX_FD];

    assert(users);
    

    //初始化数据库读取表
    users->initmysql_result(connPool);//此时内部map已经导入了数据库的passwd以及对应的username
    

    int listenfd = socket(PF_INET, SOCK_STREAM, 0);
    assert(listenfd >= 0);

    int ret = 0;
    struct sockaddr_in address;
    bzero(&address, sizeof(address));
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_ANY);
    address.sin_port = htons(port);

    int flag = 1;
    setsockopt(listenfd, SOL_SOCKET, SO_REUSEADDR, &flag, sizeof(flag));
    ret = bind(listenfd, (struct sockaddr *)&address, sizeof(address));
    assert(ret >= 0);
    ret = listen(listenfd, 5);
    assert(ret >= 0);

    // 创建内核事件表
    epoll_event events[MAX_EVENT_NUMBER];
    epollfd = epoll_create(5);
    assert(epollfd != -1);

    addfd(epollfd, listenfd, false);
    http_conn::m_epollfd = epollfd;//便于http_conn内的数据的监听

    // 创建管道
    ret = socketpair(PF_UNIX, SOCK_STREAM, 0, pipefd);
    assert(ret != -1);
    setnonblocking(pipefd[1]);
    addfd(epollfd, pipefd[0], false);

    addsig(SIGALRM, sig_handler, false);
    addsig(SIGTERM, sig_handler, false);
    bool stop_server = false;

    client_data * users_timer = new client_data[MAX_FD];

    bool timeout = false;
    alarm(TIMESLOT);

    while(!stop_server) {
        int number = epoll_wait(epollfd, events, MAX_EVENT_NUMBER, -1);
        if(number < 0 && errno != EINTR) {
            LOG_ERROR("%s", "epoll failure");
            break;
        }

        for(int i = 0; i < number; ++i) {
            int sockfd = events[i].data.fd;

            // 处理新到的客户连接
            if(sockfd == listenfd) {
                struct sockaddr_in client_address;
                socklen_t client_addrlength = sizeof(client_address);
#ifdef listenfdLT
        int connfd = accept(listenfd, (struct sockaddr *)&client_address, &client_addrlength);
        if(connfd < 0) {
            LOG_ERROR("%s:errno is:%d", "accept error", errno);
            continue;
        }
        if(http_conn::m_user_count >= MAX_FD) {
            show_error(connfd, "Internal server busy");
            LOG_ERROR("%s", "Internal server busy");
            continue;
        }

        users[connfd].init(connfd, client_address);

        // 初始化client_data数据
        // 创建定时器，设置回调函数和超时时间，绑定用户数据，将定时器添加到链表中
        users_timer[connfd].address = client_address;
        users_timer[connfd].sockfd = connfd;
        util_timer * timer = new util_timer;
        timer->user_data = &users_timer[connfd];
        timer->cb_func = cb_func;
        time_t cur = time(NULL);
        timer->expire = cur + 3 * TIMESLOT;
        users_timer[connfd].timer = timer;
        timer_lst.add_timer(timer);
#endif

#ifdef listenfdET
        while(1) {
            int connfd = accept(listenfd, (struct aockaddr *)&client_address, &client_addrlength);
            if(connfd < 0) {
                LOG_ERROR("%s:errno is:%d", "accept error", errno);
                break;
            }
            if(http_conn::m_user_count >= MAX_FD) {
                show_error(connfd, "Internal server busy");
                LOG_ERROR("%s", "Internal server busy");
                break;
            }

            users_timer[connfd].init(connfd, client_address);

            users_timer[connfd].sockfd = connfd;
            util_timer * timer = new util_timer;
            timer->user_data = &users_timer[connfd];
            timer->cb_func = cb_func;
            time_t cur = time(NULL);
            timer->expire = cur + 3 * TIMESLOT;
            users_timer[connfd].timer = timer;
            timer_lst.add_timer(timer);
        }
        continue;
#endif
            }
            else if(events[i].events & (EPOLLRDHUP | EPOLLHUP | EPOLLERR)) {
                // 服务器端关闭连接，移除对应的定时器
                util_timer * timer = users_timer[sockfd].timer;
                timer->cb_func(&users_timer[sockfd]);
                if(timer) {
                    timer_lst.del_timer(timer);
                }
            }

            else if((sockfd == pipefd[0]) && (events[i].events & EPOLLIN)) {
                int sig;
                char signals[1024];
                ret = recv(pipefd[0], signals, sizeof(signals), 0);
                if(ret == -1) {
                    continue;
                }
                else if(ret == 0) {
                    continue;
                }
                else {
                    for(int i = 0; i < ret; ++i) {
                        switch(signals[i]) {
                            case SIGALRM: {
                                timeout = true;
                                break;
                            }
                            case SIGTERM: {
                                stop_server = true;
                            }
                        }
                    }
                }
            }

            // 客户连接上接收到了数据
            else if(events[i].events & EPOLLIN) {
                util_timer * timer = users_timer[sockfd].timer;
                if(users[sockfd].read_once()) {
                    LOG_INFO("deal with the client(%s)", inet_ntoa(users[sockfd].get_address()->sin_addr));
                    Log::get_instance()->flush();

                    // 检测到读事件，将该事件放入请求列表
                    pool->append(users + sockfd);

                    if(timer) {
                        time_t cur = time(NULL);
                        timer->expire = cur + 3 * TIMESLOT;
                        LOG_INFO("%s", "adjust time once");
                        Log::get_instance()->flush();
                        timer_lst.adjust_timer(timer);
                    }
                }
                else {
                    timer->cb_func(&users_timer[sockfd]);
                    if(timer) {
                        timer_lst.del_timer(timer);
                    }
                }
            }

            else if(events[i].events & EPOLLOUT) {
                util_timer * timer = users_timer[sockfd].timer;
                if(users[sockfd].write()) {
                    LOG_INFO("Send data to the client(%s)", inet_ntoa(users[sockfd].get_address()->sin_addr));
                    Log::get_instance()->flush();

                    // 若有数据传输，则将定时器往后延迟三个单位
                    // 并对新的定时器在链表上的位置进行调整
                    if(timer) {
                        time_t cur = time(NULL);
                        timer->expire = cur + 3 * TIMESLOT;
                        LOG_INFO("%s", "adjust timer once");
                        Log::get_instance()->flush();
                        timer_lst.adjust_timer(timer);
                    }
                }
                else {
                    timer->cb_func(&users_timer[sockfd]);
                    if(timer) {
                        timer_lst.del_timer(timer);
                    }
                }
            }
        }
        if(timeout) {
            timer_handler();
            timeout = false;
        }
    }
    close(epollfd);
    close(listenfd);
    close(pipefd[1]);
    close(pipefd[0]);
    delete [] users;
    delete [] users_timer;
    delete pool;
    return 0;

}   

