#ifndef HTTPCONNECTION_H
#define HTTPCONNECTION_H
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
#include <stdlib.h>
#include <sys/mman.h>   // 内存映射
#include <stdarg.h>
#include <errno.h>
#include <sys/wait.h>
#include <sys/uio.h>
#include <map>

#include "../lock/locker.h"
#include "../CGImysql/sql_connection_pool.h"
#include "../timer/lst_timer.h"
#include "../log/log.h"

class http_conn
{
public:
    static const int FILENAME_LEN = 200;        // 文件名的最大长度
    static const int READ_BUFFER_SIZE = 2048;   // 读缓冲区的大小
    static const int WRITE_BUFFER_SIZE = 1024;  // 写缓存区的大小
    // HTTP请求方法
    enum METHOD
    {
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
    /*
        解析客户端请求时，主状态机的状态
        CHECK_STATE_REQUESTLINE:  当前正在分析 请求行
        CHECK_STATE_HEADER:       当前正在分析 头部字段
        CHECK_STATE_CONTENT:      当前正在解析 请求体，仅用于解析 POST 请求
    */
    enum CHECK_STATE
    {
        CHECK_STATE_REQUESTLINE = 0,
        CHECK_STATE_HEADER,
        CHECK_STATE_CONTENT
    };
    // 从状态机的三种可能状态，即行的读取状态，分别表示
    // 0.读取到一个完整的行     1.报文语法错误     2.行数据尚且不完整
    enum LINE_STATUS
    {
        LINE_OK = 0,
        LINE_BAD,
        LINE_OPEN
    };
    /*
        服务器处理HTTP请求的可能结果，报文解析的结果
        NO_REQUEST          :   请求不完整，需要继续读取客户数据，跳转主线程继续监测读事件
        GET_REQUEST         :   表示获得了一个完成的客户请求，调用do_request完成请求资源映射
        BAD_REQUEST         :   表示客户请求语法错误，跳转process_write完成响应报文
        NO_RESOURCE         :   表示服务器没有资源，跳转process_write完成响应报文
        FORBIDDEN_REQUEST   :   表示客户对资源没有足够的访问权限，跳转process_write完成响应报文
        FILE_REQUEST        :   文件请求,获取文件成功，跳转process_write完成响应报文
        INTERNAL_ERROR      :   表示服务器内部错误，该结果在主状态机逻辑switch的default下，一般不会触发
        CLOSED_CONNECTION   :   表示客户端已经关闭连接了
    */
    //枚举 enum 是一个类型（class），可以保存一组由用户刻画的值。
    enum HTTP_CODE
    {
        NO_REQUEST,
        GET_REQUEST,
        BAD_REQUEST,
        NO_RESOURCE,
        FORBIDDEN_REQUEST,
        FILE_REQUEST,
        INTERNAL_ERROR,
        CLOSED_CONNECTION
    };
    
public:
    http_conn() {}
    ~http_conn() {}

public:
    void init(int sockfd, const sockaddr_in &addr, char *, int, int, string user, string passwd, string sqlname);
    void close_conn(bool real_close = true);    // 关闭连接
    void process();    // 各子线程通过process函数对任务进行处理，调用process_read函数和process_write函数分别完成报文解析与报文响应两个任务

    bool read_once();  // 非阻塞的读
    bool write();      // 非阻塞的写

    sockaddr_in *get_address()
    {
        return &m_address;
    }
    void initmysql_result(connection_pool *connPool);

    int timer_flag;
    int improv;


private:
    void init();    // 初始化连接

    HTTP_CODE process_read();            // 从 m_read_buff 读取， 并解析 http 请求
    bool process_write( HTTP_CODE ret ); // 向 m_write_buff 写入响应报文数据

    // 下面这一组函数被process_read调用以分析 http 请求
    HTTP_CODE parse_request_line( char* text );     // 主状态机解析报文中的请求行
    HTTP_CODE parse_headers( char* text );          // 主状态机解析报文中的请求头
    HTTP_CODE parse_content( char* text );          // 主状态机解析报文中的请求内容
    HTTP_CODE do_request();                         // 具体处理（生产响应报文）
    LINE_STATUS parse_line();                       // 从状态机解析具体某一行，分析是请求报文的哪一部分

    //get_line用于将指针向后偏移，指向未处理的字符。其中，m_start_line是已经解析的字符
    char *get_line() { return m_read_buf + m_start_line; };

    // 这一组函数被process_write调用以填充HTTP应答
    void unmap();
    //根据响应报文格式，生成对应8个部分，以下函数均由 do_request 调用
    bool add_response(const char *format, ...);   
    bool add_status_line(int status, const char *title);
    bool add_headers(int content_length);    
    bool add_content_length(int content_length);
    bool add_linger();
    bool add_blank_line();
    bool add_content(const char *content);
    bool add_content_type();

public:
    static int m_epollfd;                // 所有的 socket 上的事件都被注册到同一个 epoll 对象中，所以设置成静态的
    static int m_user_count;             // 统计用户的数量
    MYSQL *mysql;

    int m_state;                        // 读为0, 写为1

private:
    int m_sockfd;                       // 该 http 连接的 socket
    sockaddr_in m_address;              // 通信的 socket 地址 

    char m_read_buf[READ_BUFFER_SIZE];  // 读缓存区
    int m_read_idx;                     // 标识读缓冲区中以及读入的客户端数据的最后一个字节的下一个位置

    int m_checked_idx;                  // 当前正在分析的字符在读缓冲区中的位置
    int m_start_line;                   // 当前正在解析的行的起始位置

    CHECK_STATE m_check_state;          // 主状态机当前所处的状态

    METHOD m_method;                    // 请求方法

    //以下为解析请求报文中对应的6个变量，存储读取文件的名称
    char* m_url;                        // 客户请求的目标文件的文件名（m_url为请求报文中解析出的请求资源，以/开头，也就是/xxx）
    char* m_version;                    // HTTP协议版本号，我们仅支持HTTP1.1
    char* m_host;                       // 主机名
    bool m_linger;                      // HTTP请求是否要求保持连接
    int m_content_length;               // HTTP请求的消息总长度
    char m_real_file[ FILENAME_LEN ];   // 客户请求的目标文件的完整路径，其内容等于 doc_root + m_url, doc_root是网站根目录

    // stat 函数用于取得指定文件的文件属性，并将文件属性存储在结构体stat里，st_mode：文件类型和权限，st_size：文件大小，字节数
    struct stat m_file_stat;            // 目标文件的状态。通过它我们可以判断文件是否存在、是否为目录、是否可读，并获取文件大小等信息

    char m_write_buf[WRITE_BUFFER_SIZE];// 写缓冲区
    int m_write_idx;                    // 写缓冲区中待发送的字节数
    char* m_file_address;               // 客户请求的目标文件被mmap到内存中的起始位置

    // iovec 定义了一个向量元素，通常，这个结构用作一个多元素的数组。 iov_base指向数据的地址 ，iov_len表示数据的长度
    struct iovec m_iv[2];               // （io向量机制iovec）我们将采用writev来执行写操作，0：m_write_buf 和 1：m_file_address
    int m_iv_count;                     // m_iv_count表示被写内存块的数量。    

    int bytes_to_send;                  // 将要发送的数据的字节数
    int bytes_have_send;                // 已经发送的字节数

    int cgi;                            // 是否启用的POST
    char *m_string;                     // 存储请求头数据

    char *doc_root;

    map<string, string> m_users;
    int m_TRIGMode;
    int m_close_log;

    char sql_user[100];
    char sql_passwd[100];
    char sql_name[100];
};

#endif
