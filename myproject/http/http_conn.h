#ifndef __HTTPCONNECTION_H
#define __HTTPCONNECTION_H
#include <unistd.h>
#include <signal.h>
#include <sys/types.h>
#include <sys/epoll.h>
#include <fcntl.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <assert.h>
#include <sys/stat.h>
#include <string.h>
#include <pthread.h>
#include <stdio.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/mman.h>
#include <stdarg.h>
#include <errno.h>
#include <sys/wait.h>
#include <sys/uio.h>
#include "../lock/locker.h"
#include "../CGImysql/sql_connection_pool.h"

using namespace std;

class http_conn {
    public:
        static const int FILENAME_LEN = 200;
        static const int READ_BUFFER_SIZE = 2048;
        static const int WRITE_BUFFER_SIZE = 1024;

        enum METHOD {
            GET = 0,
            POST,
            HEAD,
            PUT,
            DELETE,
            TRACE,
            OPTIONS,
            CONNECT,
            PATH
        };

        enum CHECK_STATE {
            CHECK_STATE_REQUESTLINE = 0,//请求行，主状态机正在解析请求行
            CHECK_STATE_HEADER,//头
            CHECK_STATE_CONTENT//正文
        };

        enum HTTP_CODE {//服务器http解析之后返回的信息
            NO_REQUEST,//表示请求不完整，需要继续读取客户数据
            GET_REQUEST,//表示获得了一个完整的请求
            BAD_REQUEST,//请求语法出错
            NO_RESOUCE,
            FORBIDDEN_REQUEST,//没有权限
            FILE_REQUEST,
            INTERNAL_ERROE,//服务器内部错误
            CLOSED_CONNECTION//客户端已经关闭了连接
        };

        enum LINE_STATUS {//解析报文的每一行的结果，既从状态机的三种状态，既行的读取状态
            LINE_OK = 0,
            LINE_BAD,
            LINE_OPEN
        };

    public:
        http_conn() {}
        ~http_conn() {}

    public:
        void init(int sockfd, const sockaddr_in & addr);
        void close_conn(bool real_close = true);
        void process();
        bool read_once();
        bool write();
        sockaddr_in * get_address() {
            return & m_address;
        }
        void initmysql_result(connection_pool * connPool);
    private:
        void init();
        HTTP_CODE process_read();
        bool process_write(HTTP_CODE ret);
        HTTP_CODE parse_request_line(char * text);
        HTTP_CODE parse_headers(char * text);
        HTTP_CODE pares_content(char * text);
        HTTP_CODE do_request();
        char * get_line() {
            return m_read_buf + m_start_line;
        }
        LINE_STATUS parse_line();
        void unmap();
        bool add_response(const char * format, ...);
        bool add_content(const char * content);
        bool add_status_line(int status, const char * title);
        bool add_headers(int content_length);
        bool add_content_type();
        bool add_content_length(int content_length);
        bool add_linger();
        bool add_blank_line();
    public:
        static int m_epollfd;
        static int m_user_count;
        MYSQL * mysql;

    private:
        int m_sockfd;
        sockaddr_in m_address;
        char m_read_buf[READ_BUFFER_SIZE];
        int m_read_idx;
        int m_checked_idx;
        int m_start_line;
        char m_write_buf[WRITE_BUFFER_SIZE];
        int m_write_idx;
        CHECK_STATE m_check_state;
        METHOD m_method;
        char m_real_file[FILENAME_LEN];
        char * m_url;
        char * m_version;
        char * m_host;
        int m_content_length;
        bool m_linger;
        char * m_file_address;
        struct stat m_file_stat;
        iovec m_iv[2];
        int m_iv_count;
        int cgi; //是否启用post;
        char * m_string;//存储请求头数据
        int bytes_to_send;
        int bytes_have_send;
};







#endif