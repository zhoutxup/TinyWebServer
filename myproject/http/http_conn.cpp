#include "http_conn.h"
#include <map>
#include <mysql/mysql.h>
#include "../log/log.h"

// #define connfdET //边缘触发非阻塞
#define connfdLT //水平触发阻塞

// #define listenfdET //边缘触发非阻塞
#define listenfdLT //水平触发阻塞

// 定义http响应的一些状态信息
const char * ok_200_title = "OK";
const char * error_400_title = "Bad Request";
const char * error_400_form = "Your request has bad syntax or is inherently impossible to staisfy.\n";
const char * error_403_title = "Forbidden";
const char * error_403_form = "You do not have permission to get file from this server.\n";
const char * error_404_title = "Not Found";
const char * error_404_form = "The requested file was not found on this server.\n";
const char * error_500_title = "Internal Error";
const char * error_500_form = "There was an unusual problem serving the request file.\n";

const char * doc_root = "/home/zhou/Project/myproject/root";

map<string, string> users;//将表中的用户名和密码放入map

locker m_lock;

void http_conn::initmysql_result(connection_pool * connPool) {//这一步将存在数据中的数据全部读到users中
    MYSQL * mysql = NULL;
    connectionRAII mysqlcon(&mysql, connPool);//从连接池中取出一个mysql连接，保存到mysql中
    
    //在user表中检索username，passwd数据
    if(mysql_query(mysql, "SELECT username,passwd FROM user")) {
        printf("Process have been here\n");
        LOG_ERROR("SELECT error:%s\n", mysql_errno(mysql));
    }
    
    //从表中检索完整的结果集
    MYSQL_RES * result = mysql_store_result(mysql);

    //返回结果集中的列数
    int num_fileds = mysql_num_fields(result);

    //返回所有字段结构的数组
    MYSQL_FIELD * fileds = mysql_fetch_field(result);
    
    // 从结果集中获取下一行，将对应的用户名和密码存入map中
    while(MYSQL_ROW row = mysql_fetch_row(result)) {
        string temp1(row[0]);
        string temp2(row[1]);
        users[temp1] = temp2;
    }
    
}

int setnonblocking(int fd) {//改变文件描述符的属性，使其为非阻塞
    int old_option = fcntl(fd, F_GETFL);
    int new_option = old_option | O_NONBLOCK;
    fcntl(fd, F_SETFL, new_option);
    return old_option;
}

void addfd(int epollfd, int fd, bool one_shot) {//将内核事件表注册为读事件，ET模式，选择开启EPOLLONESHOT
    epoll_event event;
    event.data.fd = fd;

    #ifdef connfdET
        event.events = EPOLLIN | EPOLLET | EPOLLRDHUP;
    #endif

    #ifdef connfdLT
        event.events = EPOLLIN | EPOLLRDHUP;
    #endif

    #ifdef listenfdET
        event.events = EPOLLIN | EPOLLET | EPOLLRDHUP;
    #endif

    #ifdef listenfdLT
        event.events = EPOLLIN | EPOLLRDHUP;
    #endif

    if(one_shot) {
        event.events |= EPOLLONESHOT;
    }

    epoll_ctl(epollfd, EPOLL_CTL_ADD, fd, &event);
    setnonblocking(fd);
}

// 从内核事件表删除文件描述符
void removefd(int epollfd, int fd) {
    epoll_ctl(epollfd, EPOLL_CTL_DEL, fd, 0);
    close(fd);
}

// 将事件重置为EPOLLONESHOT
void modfd(int epollfd, int fd, int ev) {
    epoll_event event;
    event.data.fd = fd;

    #ifdef connfdET
        event.events = ev | EPOLLET | EPOLLONESHOT | EPOLLRDHUP;
    #endif

    #ifdef connfdLT
        event.events = ev | EPOLLONESHOT | EPOLLRDHUP;
    #endif

    epoll_ctl(epollfd, EPOLL_CTL_MOD, fd, &event);
}

int http_conn::m_user_count = 0;
int http_conn::m_epollfd = -1;

void http_conn::close_conn(bool real_close) {//关闭连接，关闭一个连接，客户总量减一
    if(real_close && (m_sockfd != -1)) {
        removefd(m_epollfd, m_sockfd);
        m_sockfd = -1;
        m_user_count--;
    } 
}

void http_conn::init(int sockfd, const sockaddr_in & addr) {
    m_sockfd = sockfd;
    m_address = addr;

    // int reuse = 1;
    // setsockopt(m_sockfd, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));

    addfd(m_epollfd, sockfd, true);
    m_user_count++;
    init();
}

void http_conn::init() {
    mysql = nullptr;
    bytes_to_send = 0;
    bytes_have_send = 0;
    m_check_state = CHECK_STATE_REQUESTLINE;
    m_linger = false;
    m_method = GET;
    m_url = 0;
    m_version = 0;
    m_content_length = 0;
    m_host = 0;
    m_start_line = 0;
    m_checked_idx = 0;
    m_read_idx = 0;
    m_write_idx = 0;
    cgi = 0;
    memset(m_read_buf, '\0', READ_BUFFER_SIZE);
    memset(m_write_buf, '\0', WRITE_BUFFER_SIZE);
    memset(m_read_buf, '\0', FILENAME_LEN);
}

// 从状态机，用于分析出一行内容
// 返回值为行读取状态
http_conn::LINE_STATUS http_conn::parse_line() {
    char temp;
    for(; m_checked_idx < m_read_idx; ++m_checked_idx) {
        temp = m_read_buf[m_checked_idx];
        if(temp == '\r') {
            if((m_checked_idx + 1) == m_read_idx) {
                return LINE_OK;
            }
            else if(m_read_buf[m_checked_idx++] == '\n') {
                m_read_buf[m_checked_idx++] = '\0';
                m_read_buf[m_checked_idx++] = '\0';
                return LINE_OK;
            }
            return LINE_BAD;
        }
        else if(temp == '\n') {
            if(m_checked_idx > 1 && m_read_buf[m_checked_idx - 1] == '\r') {
                m_read_buf[m_checked_idx - 1] = '\0';
                m_read_buf[m_checked_idx++] = '\0';
                return LINE_OK;
            }
            return LINE_BAD;
        }
    }
    return LINE_OPEN;
}

// 循环读取客户数据，直到无数据可读或对方关闭连接
// 非阻塞ET工作模式下，需要一次性将数据读完
bool http_conn::read_once() {//将sockfd中的数据读入自己的读缓冲区中
    if(m_read_idx >= READ_BUFFER_SIZE) {
        return false;
    }
    int bytes_read = 0;

    #ifdef connfdLT
        bytes_read = recv(m_sockfd, m_read_buf + m_read_idx, READ_BUFFER_SIZE - m_read_idx, 0);
        m_read_idx += bytes_read;

        if(bytes_read <= 0) {
            return false;
        }
        return true;
    #endif

    #ifdef connfdET
        while(true) {
            bytes_read = recv(m_sockfd, m_read_buf + m_read_idx, READ_BUFFER_SIZE - m_read_idx, 0);
            if(bytes_read == -1) {
                if(errno = EAGAIN || errno == EWOULDBLOCK) {
                    break;
                }
                return false;
            }
            else if(bytes_read == 0) {
                return false;
            }
            m_read_idx += bytes_read;
        }
        retrun true;
    #endif
}

// 解析http请求行，获得请求方法，目标url以及http版本号
http_conn::HTTP_CODE http_conn::parse_request_line(char * text) {
    m_url = strpbrk(text, " \t");
    if(!m_url) {
        return BAD_REQUEST;
    }
    *m_url++ = '\0';
    char * method = text;
    if(strcasecmp(method, "GET") == 0) {
        m_method = GET;
    }
    else if(strcasecmp(method, "POST") == 0) {
        m_method = POST;
        cgi = 1;
    }
    else {
        return BAD_REQUEST;
    }

    m_url += strspn(m_url, " \t");
    m_version = strpbrk(m_url, " \t");
    if(!m_version) {
        return BAD_REQUEST;
    }  
    *m_version++ = '\0';
    m_version += strspn(m_version, " \t");
    if(strcasecmp(m_version, "HTTP/1.1") != 0) {
        return BAD_REQUEST;
    }
    if(strncasecmp(m_url, "http://", 7) == 0) {
        m_url += 8;
        m_url = strchr(m_url, '/');//url定位到最后一级目录
    }

    if(!m_url || m_url[0] != '/') {
        return BAD_REQUEST;
    }

    //当url为/时，显示判断界面
    if(strlen(m_url) == 1) {
        strcat(m_url, "judge.html");//把judeje.html字符串添加到m_url末尾
    }
    m_check_state = CHECK_STATE_HEADER;
    return NO_REQUEST;
}

http_conn::HTTP_CODE http_conn::parse_headers(char * text) {
    if(text[0] == '\0') {
        if(m_content_length != 0) {
            m_check_state = CHECK_STATE_CONTENT;
            return NO_REQUEST;
        }
        return GET_REQUEST;
    }
    else if(strncasecmp(text, "Connection:", 11) == 0) {
        text += 11;
        text += strspn(text, " \t");
        if(strcasecmp(text, "keep-alive") == 0) {
            m_linger = true;
        }
    }
    else if(strncasecmp(text, "Content-length:", 15) == 0) {
        text += 15;
        text += strspn(text, " \t");
        m_content_length = atol(text);
    }
    else if(strncasecmp(text, "Host:", 5) == 0) {
        text += 5;
        text += strspn(text, " \t");
        m_host = text;
    }
    else {
        LOG_INFO("oop!unknow header: %s", text);
        Log::get_instance()->flush();
    }
    return NO_REQUEST; 
}

// 判断http请求是否被完整读入
http_conn::HTTP_CODE http_conn::pares_content(char * text) {
    if(m_read_idx >= (m_content_length + m_checked_idx)) {
        text[m_content_length] = '\0';
        // post请求中最后为输入的用户名和密码
        m_string = text;
        return GET_REQUEST;
    }
    return NO_REQUEST;
}

http_conn::HTTP_CODE http_conn::process_read() {
    LINE_STATUS line_status = LINE_OK;
    HTTP_CODE ret = NO_REQUEST;
    char * text = 0;

    while((m_check_state == CHECK_STATE_CONTENT && line_status == LINE_OK) || ((line_status = parse_line()) == LINE_OK)) {
        text = get_line();
        m_start_line = m_checked_idx;
        LOG_INFO("%s", text);
        Log::get_instance()->flush();
        switch(m_check_state) {
            case CHECK_STATE_REQUESTLINE:
                {
                    ret = parse_request_line(text);
                    if(ret == BAD_REQUEST) return BAD_REQUEST;
                    break;
                }
            case CHECK_STATE_HEADER:
                {
                    ret = parse_headers(text);
                    if(ret == BAD_REQUEST) return BAD_REQUEST;
                    else if(ret == GET_REQUEST) {
                        return do_request();
                    }
                    break;
                }
            case CHECK_STATE_CONTENT:
                {
                    ret = pares_content(text);
                    if(ret == GET_REQUEST) {
                        return do_request();
                    }
                    line_status = LINE_OPEN;
                    break;
                }
            default:
                return INTERNAL_ERROE;
        }
    }
    return NO_REQUEST;
}

http_conn::HTTP_CODE http_conn::do_request() {
    strcpy(m_real_file, doc_root);
    int len = strlen(doc_root);

    const char * p = strrchr(m_url, '/');

    // 处理cgi
    if(cgi == 1 && (*(p + 1) == '2' || *(p + 1) == '3')) {
        // 根据标志判断是登录检测还是注册检测
        char flag = m_url[1];

        char * m_url_real = (char *)malloc(sizeof(char) * 200);
        strcpy(m_url_real, "/");
        strcat(m_url_real, m_url + 2);
        strncpy(m_real_file + len, m_url_real, FILENAME_LEN - len - 1);
        free(m_url_real);

        // 将用户名和密码提取出来
        char name[100], passwd[100];
        int i;
        for(i = 5; m_string[i] != '&'; ++i) {
            name[i - 5] = m_string[i];
        }
        name[i - 5] = '\0';

        int j = 0;
        for(int j = i + 10; m_string[i] != '\0'; ++i, ++j) {
            passwd[j] = m_string[i];
        }
        passwd[j] = '\0';

        // 同步线程登录校验
        if(*(p + 1) == '3') {
            // 如果是注册数据，将检测数据库中是否有重名
            // 如果无重名，先进行增加数据
            char * sql_insert = (char *) malloc(sizeof(char) * 200);
            strcpy(sql_insert, "INSERT INTO user(username, passwd) VALUES(");
            strcat(sql_insert, "'");
            strcat(sql_insert, name);
            strcat(sql_insert, "', '");
            strcat(sql_insert, passwd);
            strcat(sql_insert, "')");

            if(users.find(name) == users.end()) {
                m_lock.lock();
                int res = mysql_query(mysql, sql_insert);
                users.insert(pair<string, string>(name, passwd));
                m_lock.unlock();

                if(!res) {
                    strcpy(m_url, "/log.html");
                }
                else {
                    strcpy(m_url, "/registerError.html");
                }
            }
            else strcpy(m_url, "/registerError.html");
        }
        //如果是登录，直接判断
        // 若浏览器输入的用户名和密码在表中可以查找到，返回1，否则返回0

        else if(*(p + 1) == '2') {
            if(users.find(name) != users.end() && users[name] == passwd) {
                strcpy(m_url, "/welcome.html");
            }
            else {
                strcpy(m_url, "/logError.html");
            }
        }
    }

    if(*(p + 1) == '0') {
        char * m_url_real = (char *)malloc(sizeof(char) * 200);
        strcpy(m_url_real, "/register.html");
        strncpy(m_real_file + len, m_url_real, strlen(m_url_real));

        free(m_url_real);
    }
    else if(*(p + 1) == '1') {
        char * m_url_real = (char *)malloc(sizeof(char) * 200);
        strcpy(m_url_real, "/log.html");
        strncpy(m_real_file + len, m_url_real, strlen(m_url_real));

        free(m_url_real);
    }
    else if(*(p + 1) == '5') {
        char * m_url_real = (char *)malloc(sizeof(char) * 200);
        strcpy(m_url_real, "/picture.html");
        strncpy(m_real_file + len, m_url_real, strlen(m_url_real));

        free(m_url_real);
    }
    else if(*(p + 1) == '6') {
        char * m_url_real = (char *)malloc(sizeof(char) * 200);
        strcpy(m_url_real, "/video.html");
        strncpy(m_real_file + len, m_url_real, strlen(m_url_real));

        free(m_url_real);
    }
    else if(*(p + 1) == '7') {
        char * m_url_real = (char *)malloc(sizeof(char) * 200);
        strcpy(m_url_real, "/fans.html");
        strncpy(m_real_file + len, m_url_real, strlen(m_url_real));

        free(m_url_real);
    }
    else {
        strncpy(m_real_file + len, m_url, FILENAME_LEN - len - 1);
    }

    if(stat(m_real_file, &m_file_stat) < 0) {
        return NO_REQUEST;
    }
    if(!(m_file_stat.st_mode & S_IROTH)) {
        return FORBIDDEN_REQUEST;
    }
    if(S_ISDIR(m_file_stat.st_mode)) {
        return BAD_REQUEST;
    }

    int fd = open(m_real_file, O_RDONLY);
    m_file_address = (char *)mmap(0, m_file_stat.st_size, PROT_READ, MAP_PRIVATE, fd, 0);
    close(fd);
    return FILE_REQUEST;
}

void http_conn::unmap() {
    if(m_file_address) {
        munmap(m_file_address, m_file_stat.st_size);
        m_file_address = 0;
    }
}

bool http_conn::write() {
    int temp = 0;

    if(bytes_to_send == 0) {
        modfd(m_epollfd, m_sockfd, EPOLLIN);
        init();
        return true;
    }

    while(1) {
        temp = writev(m_sockfd, m_iv, m_iv_count);

        if(temp < 0) {
            if(errno == EAGAIN) {
                modfd(m_epollfd, m_sockfd, EPOLLOUT);
                return true;
            }
            unmap();
            return false;
        }

        bytes_have_send += temp;
        bytes_to_send -= temp;

        if(bytes_have_send >= m_iv[0].iov_len) {
            m_iv[0].iov_len = 0;
            m_iv[1].iov_base = m_file_address + (bytes_have_send - m_write_idx);
            m_iv[1].iov_len = bytes_to_send;
        }
        else {
            m_iv[0].iov_base = m_write_buf + bytes_have_send;
            m_iv[0].iov_len = m_iv[0].iov_len - bytes_have_send;
        }

        if(bytes_to_send <= 0) {
            unmap();
            modfd(m_epollfd, m_sockfd, EPOLLIN);

            if(m_linger) {
                init();
                return true;
            }
            else {
                return false;
            }
        }
    }
}

bool http_conn::add_response(const char * format, ...) {
    if(m_write_idx >= WRITE_BUFFER_SIZE) return false;

    va_list arg_list;
    va_start(arg_list, format);

    int len = vsnprintf(m_write_buf + m_write_idx, WRITE_BUFFER_SIZE - 1- m_write_idx, format, arg_list);

    if(len >= (WRITE_BUFFER_SIZE - 1 - m_write_idx)) {
        va_end(arg_list);
        return false;
    }

    m_write_idx += len;
    va_end(arg_list);

    LOG_INFO("request:%s", m_write_buf);
    Log::get_instance()->flush();
    return true;
}

bool http_conn::add_headers(int content_len) {
    add_content_length(content_len);
    add_linger();
    add_blank_line();
}

bool http_conn::add_content_length(int content_len) {
    return add_response("Content-Length:%d\r\n", content_len);
}

bool http_conn::add_content_type() {
    return add_response("Content-Type:%s\r\n", "text/html");
}

bool http_conn::add_linger() {
    return add_response("Connection:%s\r\n", (m_linger == true) ? "keep-alive": "close");
}

bool http_conn::add_blank_line() {
    return add_response("%s", "\r\n");
}

bool http_conn::add_content(const char * content) {
    return add_response("%s", content);
}

bool http_conn::process_write(HTTP_CODE ret) {
    switch(ret) {
        case INTERNAL_ERROE:
        {
            http_conn::add_status_line(500, error_500_title);
            //add_status_line(500, error_500_title);
            add_headers(strlen(error_500_form));
            if(!add_content(error_500_form)) return false;
            break;
        }

        case BAD_REQUEST: {
            add_status_line(400, error_404_title);
            add_headers(strlen(error_404_form));
            if(!add_content(error_404_form)) return false;
            break;
        }

        case FORBIDDEN_REQUEST: {
            add_status_line(400, error_403_title);
            add_headers(strlen(error_403_form));
            if(!add_content(error_403_form)) return false;
            break;
        }

        case FILE_REQUEST: {
            add_status_line(200, ok_200_title);
            if(m_file_stat.st_size != 0) {
                add_headers(m_file_stat.st_size);
                m_iv[0].iov_base = m_write_buf;
                m_iv[0].iov_len = m_write_idx;
                m_iv[1].iov_base = m_file_address;
                m_iv[1].iov_len = m_file_stat.st_size;
                m_iv_count = 2;
                bytes_to_send = m_write_idx + m_file_stat.st_size;
                return true;
            }
            else {
                const char * ok_string = "<html><body></body></html>";
                add_headers(strlen(ok_string));
                if(!add_content(ok_string)) return false;
            }
        }
        default:
            return false;
    }

    m_iv[0].iov_base = m_write_buf;
    m_iv[0].iov_len = m_write_idx;

    m_iv_count = 1;
    bytes_to_send = m_write_idx;
    return true;
}

bool http_conn::add_status_line(int status, const char * title) {
    return add_response("%s %d %s\r\n", "HTTP/1.1", status, title);
}

void http_conn::process() {
    HTTP_CODE read_ret = process_read();
    if(read_ret == NO_REQUEST) {
        modfd(m_epollfd, m_sockfd, EPOLLIN);
        return ;
    }

    bool write_ret = process_write(read_ret);
    if(!write_ret) {
        close_conn();
    }
    modfd(m_epollfd, m_sockfd, EPOLLOUT);
}