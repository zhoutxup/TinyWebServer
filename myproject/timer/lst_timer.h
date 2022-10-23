#ifndef __LST_TIMER_H
#define __LST_TIMER_H

#include <time.h>
#include "../log/log.h"
#include <sys/socket.h>
#include <netinet/in.h>

class util_timer;

struct client_data {
    sockaddr_in address;
    int sockfd;
    util_timer * timer;
};

class util_timer {//定时器
    public:
        util_timer():prev(nullptr), next(nullptr){}
    public:
        time_t expire;//超时时间
        void (*cb_func)(client_data *);//超时后执行的回调函数
        client_data * user_data;//该定时器对应的用户信息
        util_timer * prev;//上一个定时器
        util_timer * next;//下一个定时器
};

class sort_timer_lst {//存放定时器的容器，基于升序链表，只用维护两根指针就可以
    public:
        sort_timer_lst():head(nullptr), tail(nullptr){}
        ~sort_timer_lst() {
            util_timer * temp = head;
            while(temp) {
                head = temp->next;
                delete temp;
                temp = head;
            }
        }

        void add_timer(util_timer * timer) {
            if(!timer) return ;

            if(!head) {
                head = timer;
                tail = timer;
                return;
            }

            if(timer->expire < head->expire) {
                timer->next = head;
                head->prev = timer;
                head = timer;
                return ;
            }

            add_timer(timer, head);
        }

        void adjust_timer(util_timer * timer) {//调整定时器timer在升序链表中的位置（向后调整）
            if(!timer) return ;
            util_timer * temp = timer->next;

            if(!temp || timer->expire < temp->expire) {//如果下一个定时器为空，或者定时时间小于下一个定时器那么不做任何调整操作
                return ;
            }

            if(timer == head) {
                head = head->next;
                head->prev = nullptr;
                timer->next = nullptr;
                add_timer(timer, head);
            }
            else {
                timer->prev->next = timer->next;
                timer->next->prev = timer->prev;
                add_timer(timer, timer->next);
            }
        }

        void del_timer(util_timer * timer) {
            if(!timer) return ;
            if((timer == head) && (timer == tail)) {
                delete timer;
                head = nullptr;
                tail = nullptr;
                return ;
            }

            if(timer == head) {
                head = head->next;
                head->prev = nullptr;
                delete timer;
                return;
            }

            if(timer == tail) {
                tail = tail->prev;
                tail->next = nullptr;
                delete timer;
                return ;
            }

            timer->prev->next = timer->next;
            timer->next->prev = timer->prev;
            delete timer;
        }

        void tick() {
            if(!head) return ;
            LOG_INFO("%s", "timer tick");
            Log::get_instance()->flush();
            time_t cur = time(nullptr);
            util_timer * temp = head;
            while(temp) {
                if(cur < temp->expire) {//定时时间未到，那么接下来的时间也肯定不会到达
                    break;
                }
                //定时时间到了
                temp->cb_func(temp->user_data);
                head = temp->next;
                if(head) {
                    head->prev = nullptr;
                }

                delete temp;
                temp = head;
            }
        }

    private:
        void add_timer(util_timer * timer, util_timer * lst_head) {
            util_timer * prev = lst_head;
            util_timer * temp = prev->next;
            while(temp) {
                if(timer->expire < temp->expire) {//找到可以插入的位置了
                    prev->next = timer;
                    timer->next = temp;
                    temp->prev = timer;
                    timer->prev = prev;
                }

                prev = temp;
                temp = temp->next;
            }

            if(!temp) {//遍历整个链表都没有找到，那么就加在最后
                prev->next = timer;
                timer->prev = prev;
                timer->next = nullptr;
                tail = timer;
            }
        }

    private:
        util_timer * head;
        util_timer * tail;

};


#endif
