#include "http_conn.h"

#include <mysql/mysql.h>
#include <fstream>

// 定义 http 响应的一些状态信息
const char *ok_200_title = "OK";
const char *error_400_title = "Bad Request";
const char *error_400_form = "Your request has bad syntax or is inherently impossible to staisfy.\n";
const char *error_403_title = "Forbidden";
const char *error_403_form = "You do not have permission to get file form this server.\n";
const char *error_404_title = "Not Found";
const char *error_404_form = "The requested file was not found on this server.\n";
const char *error_500_title = "Internal Error";
const char *error_500_form = "There was an unusual problem serving the request file.\n";

// 将数据库中的用户名和密码载入到服务器的map中，将表中的用户名和密码放入 map
locker m_lock;
map<string, string> users;

// 同步线程 初始化数据库读取表
void http_conn::initmysql_result(connection_pool *connPool)
{
    // 先从连接池中取一个连接
    MYSQL *mysql = NULL;
    connectionRAII mysqlcon(&mysql, connPool);

    // 在 user 表中检索 username，passwd 数据，浏览器端输入
    if (mysql_query(mysql, "SELECT username,passwd FROM user"))
    {
        LOG_ERROR("SELECT error:%s\n", mysql_error(mysql));
    }

    // 从表中检索完整的结果集
    MYSQL_RES *result = mysql_store_result(mysql);

    // 返回结果集中的列数
    int num_fields = mysql_num_fields(result);

    // 返回所有字段结构的数组
    MYSQL_FIELD *fields = mysql_fetch_fields(result);

    // 从结果集中获取下一行，将对应的用户名和密码，存入 map 中
    while (MYSQL_ROW row = mysql_fetch_row(result))
    {
        string temp1(row[0]);
        string temp2(row[1]);
        users[temp1] = temp2;
    }
}

// 对文件描述符设置非阻塞
int setnonblocking(int fd)
{
    int old_option = fcntl(fd, F_GETFL);
    int new_option = old_option | O_NONBLOCK;
    fcntl(fd, F_SETFL, new_option);
    return old_option;
}

// 将内核事件表注册读事件，ET模式(边缘触发模式)，选择开启EPOLLONESHOT（避免出现两个线程同时操作一个 socket 的局面）
// 将fd上的EPOLLIN和EPOLLET事件注册到 epollfd 指示的 epoll 内核事件中
void addfd(int epollfd, int fd, bool one_shot, int TRIGMode)
{
    epoll_event event;
    event.data.fd = fd;

    if (1 == TRIGMode)
        event.events = EPOLLIN | EPOLLET | EPOLLRDHUP;
    else
        event.events = EPOLLIN | EPOLLRDHUP;

    if (one_shot)
        event.events |= EPOLLONESHOT;  // 防止同一个通信被不同的线程处理
    // 追加文件描述符
    epoll_ctl(epollfd, EPOLL_CTL_ADD, fd, &event);
    // 采用 ET 模式需要一次性将所有的数据读出，采用非阻塞模式，故需要设置文件描述符为非阻塞
    setnonblocking(fd);
}

//从内核时间表删除描述符
void removefd(int epollfd, int fd)
{
    epoll_ctl(epollfd, EPOLL_CTL_DEL, fd, 0);
    close(fd);
}

// 修改文件描述符，将事件重置为EPOLLONESHOT，以确保下一次可读时，EPOLLIN 能被触发
void modfd(int epollfd, int fd, int ev, int TRIGMode)
{
    epoll_event event;
    event.data.fd = fd;

    if (1 == TRIGMode)
        event.events = ev | EPOLLET | EPOLLONESHOT | EPOLLRDHUP;
    else
        event.events = ev | EPOLLONESHOT | EPOLLRDHUP;

    epoll_ctl(epollfd, EPOLL_CTL_MOD, fd, &event);
}

int http_conn::m_user_count = 0;  // 所有的客户数
int http_conn::m_epollfd = -1;    // 所有socket上的事件都被注册到同一个epoll内核事件中，所以设置成静态的

//关闭连接，关闭一个连接，客户总量减一
void http_conn::close_conn(bool real_close)
{
    if (real_close && (m_sockfd != -1))
    {
        printf("close %d\n", m_sockfd);
        removefd(m_epollfd, m_sockfd);
        m_sockfd = -1;
        m_user_count--;
    }
}

//初始化连接,外部调用初始化套接字地址，函数内部会调用私有方法 init()
void http_conn::init(int sockfd, const sockaddr_in &addr, char *root, int TRIGMode,
                     int close_log, string user, string passwd, string sqlname)
{
    m_sockfd = sockfd;
    m_address = addr;

    //添加到 epoll 对象中
    addfd(m_epollfd, sockfd, true, m_TRIGMode);
    m_user_count++;   // 总用户数 +1

    //当浏览器出现连接重置时，可能是网站根目录出错或http响应格式出错或者访问的文件中内容完全为空
    doc_root = root;
    m_TRIGMode = TRIGMode;
    m_close_log = close_log;

    strcpy(sql_user, user.c_str());
    strcpy(sql_passwd, passwd.c_str());
    strcpy(sql_name, sqlname.c_str());

    init();      // 注意两个初始化分开
}

// 初始化新接受的连接
// check_state默认为分析请求行状态
void http_conn::init()
{
    m_check_state = CHECK_STATE_REQUESTLINE;  // 初始状态为解析请求首行
    m_linger = false;      // 默认不保持链接  Connection : keep-alive保持连接
    m_method = GET;        // 默认请求方式为GET
    mysql = NULL;
    cgi = 0;               // 默认不启用的POST

    m_url = 0;
    m_version = 0;
    m_content_length = 0;

    m_host = 0;
    m_start_line = 0;
    m_checked_idx = 0;
    m_read_idx = 0;
    m_write_idx = 0;

    m_state = 0;
    timer_flag = 0;
    improv = 0;

    memset(m_read_buf, '\0', READ_BUFFER_SIZE);   // 读缓冲数据清空
    memset(m_write_buf, '\0', WRITE_BUFFER_SIZE);
    memset(m_real_file, '\0', FILENAME_LEN);
    /* bzero(m_read_buf, READ_BUFFER_SIZE);     
       bzero(m_write_buf, READ_BUFFER_SIZE);
       bzero(m_real_file, FILENAME_LEN);      */

    bytes_to_send = 0;
    bytes_have_send = 0;  
}

// 解析一行，判断依据\r\n。从状态机，用于分析出一行内容，返回值为行的读取状态，有LINE_OK,LINE_BAD,LINE_OPEN
http_conn::LINE_STATUS http_conn::parse_line()
{
    char temp;
    for (; m_checked_idx < m_read_idx; ++m_checked_idx)
    {
        // temp 为将要分析的字节
        temp = m_read_buf[m_checked_idx];

        // 如果当前是 \r 字符，则有可能会读取到完整行
        if (temp == '\r')
        {
            // 下一个字节达到了buffer末尾，表示buffer还需要继续接收，返回LINE_OPEN
            if ((m_checked_idx + 1) == m_read_idx)   
                return LINE_OPEN;
            else if (m_read_buf[m_checked_idx + 1] == '\n')
            {
                m_read_buf[m_checked_idx++] = '\0';
                m_read_buf[m_checked_idx++] = '\0';
                return LINE_OK;                     // 完整的解析一行(将m_checked_idx指向下一行的开头，则返回LINE_OK)
            }
            return LINE_BAD;
        }
        // 如果当前字符是\n，也有可能读取到完整行
        // 一般是上次读取到\r就到buffer末尾了，没有接收完整，再次接收时会出现这种情况
        else if (temp == '\n')
        {
            // 前一个字符是 \r，则接收完整
            if (m_checked_idx > 1 && m_read_buf[m_checked_idx - 1] == '\r')
            {               
                m_read_buf[m_checked_idx - 1] = '\0';
                m_read_buf[m_checked_idx++] = '\0';
                return LINE_OK;
            }
            return LINE_BAD;
        }
    }

    // 当前字节既不是\r，也不是\n : 表示接收不完整，需要继续接收，返回LINE_OPEN
    return LINE_OPEN;      
}

//循环读取客户数据，直到无数据可读或对方关闭连接
//非阻塞ET工作模式下，需要一次性将数据读完
bool http_conn::read_once()
{
    if (m_read_idx >= READ_BUFFER_SIZE) // 缓存区已经满了
    {
        return false;
    }
    int bytes_read = 0;          // 读取到的字节

    // LT 读取数据
    if (0 == m_TRIGMode)
    {
        bytes_read = recv(m_sockfd, m_read_buf + m_read_idx, READ_BUFFER_SIZE - m_read_idx, 0);
        m_read_idx += bytes_read;

        if (bytes_read <= 0)
        {
            return false;
        }

        return true;
    }
    // ET 读数据
    else
    {
        while (true)
        {
            // 从m_read_buf + m_read_idx索引出开始保存数据，大小是READ_BUFFER_SIZE - m_read_idx
            bytes_read = recv(m_sockfd, m_read_buf + m_read_idx, READ_BUFFER_SIZE - m_read_idx, 0);
            if (bytes_read == -1){
                if (errno == EAGAIN || errno == EWOULDBLOCK){
                    break;  // 没有数据
                }
                return false;
            }else if (bytes_read == 0){ // 对方关闭连接
                return false;
            }
            m_read_idx += bytes_read;
        }
        // printf("读取到了数据：%s\n",m_read_buf);
        return true;
    }
}

// 对内存映射区执行 munmap 操作，释放资源
void http_conn::unmap(){
    if (m_file_address)
    {
        munmap(m_file_address, m_file_stat.st_size);
        m_file_address = 0;
    }
}

// 写 HTTP 响应
bool http_conn::write()
{
    int temp = 0;
    
    // 将要发送的字节为0，表示响应报文为空，一般不会出现这种情况
    if ( bytes_to_send == 0 ) {       
        modfd( m_epollfd, m_sockfd, EPOLLIN, m_TRIGMode); 
        init();
        return true;
    }

    // 将响应报文的状态行、消息头、空行和响应正文发送给浏览器端
    while(1) {
        /* writev函数用于在一次函数调用中写多个非连续缓冲区，
           若成功则返回已写的字节数，若出错则返回-1。writev以顺序iov[0]，iov[1]至iov[iovcnt-1]从缓冲区中聚集输出数据。
           writev返回输出的字节总数，通常，它应等于所有缓冲区长度之和。   */
        temp = writev(m_sockfd, m_iv, m_iv_count);   // 分散写

        if ( temp < 0 ) {
            // 如果TCP写缓冲没有空间，则等待下一轮EPOLLOUT事件，虽然在此期间，
            // 服务器无法立即接收到同一客户的下一个请求，但可以保证连接的完整性。
            if( errno == EAGAIN ) {   // 判断缓冲区是否满了
                // 重新注册写事件
                modfd( m_epollfd, m_sockfd, EPOLLOUT, m_TRIGMode );
                return true;
            }
            // 如果发送失败，但不是缓冲区问题，取消映射
            unmap();
            return false;
        }

        // temp > 0 正常发送，temp 为发送的字节数
        bytes_have_send += temp;        // 更新已发送字节数
        bytes_to_send -= temp;          // 更新将要发送字节数

        // 第一个 iovec 头部信息的数据已发送完，发送第二个 iovec 数据
        if (bytes_have_send >= m_iv[0].iov_len){
            // 不再继续发送头部信息
            m_iv[0].iov_len = 0;
            m_iv[1].iov_base = m_file_address + (bytes_have_send - m_write_idx);
            m_iv[1].iov_len = bytes_to_send;
        }else{
            // 继续发送第一个 iovec 头部信息的数据
            m_iv[0].iov_base = m_write_buf + bytes_have_send;
            m_iv[0].iov_len = m_iv[0].iov_len - temp;
        }

        // 判断条件，数据已全部发送完
        if (bytes_to_send <= 0)
        {
            unmap();
            // 在 epoll 树上重置 EPOLLONESHOT 事件
            modfd(m_epollfd, m_sockfd, EPOLLIN, m_TRIGMode);

            // 浏览器的请求为长连接
            if (m_linger){
                init();
                // 重新初始化 HTTP 对象
                return true;
            }else{
                return false;
            }
        }
    }  
}

// 主状态机，解析请求
http_conn::HTTP_CODE http_conn::process_read() {
    LINE_STATUS line_status = LINE_OK;   // 定义初始状态
    HTTP_CODE ret = NO_REQUEST;          // 定义解析的结果
    char* text = 0;                      // 需要获取的一行数据

    // 需要逐行解析，故使用 while，LINE_OK 表示正常的
    // 解析一行完整的数据，或者解析请求体，也是完整的数据
    while (((m_check_state == CHECK_STATE_CONTENT) && (line_status == LINE_OK))
                || ((line_status = parse_line()) == LINE_OK)) {       
        // 获取一行数据
        text = get_line();

        // m_start_line是每一个数据行在 m_read_buf 中的起始位置
        // m_checked_idx表示从状态机在  m_read_buf 中读取的位置
        m_start_line = m_checked_idx;

        // printf( "got 1 http line: %s\n", text );
        LOG_INFO("%s", text);
        // Log::get_instance()->flush();

        switch ( m_check_state ) {
            case CHECK_STATE_REQUESTLINE: {
                ret = parse_request_line( text );
                if ( ret == BAD_REQUEST ) {
                    return BAD_REQUEST;
                }
                break;
            }
            case CHECK_STATE_HEADER: {
                ret = parse_headers( text );
                if ( ret == BAD_REQUEST ) {
                    return BAD_REQUEST;
                } else if ( ret == GET_REQUEST ) {
                    return do_request();
                }
                break;
            }
            case CHECK_STATE_CONTENT: {
                ret = parse_content( text );
                if ( ret == GET_REQUEST ) {
                    return do_request();
                }
                // 解析完消息体即完成报文解析，避免再次进入循环，更新line_status
                line_status = LINE_OPEN;  
                break;
            }
            default: {
                return INTERNAL_ERROR;
            }
        }
    }
    return NO_REQUEST;
}

// 根据服务器处理 HTTP 请求的结果，决定返回给客户端的内容
bool http_conn::process_write(HTTP_CODE ret) {
    switch (ret)
    {
        case INTERNAL_ERROR:
            add_status_line( 500, error_500_title );
            add_headers( strlen( error_500_form ) );
            if ( ! add_content( error_500_form ) ) {
                return false;
            }
            break;
        case BAD_REQUEST:
            add_status_line( 400, error_400_title );
            add_headers( strlen( error_400_form ) );
            if ( ! add_content( error_400_form ) ) {
                return false;
            }
            break;
        case NO_RESOURCE:
            add_status_line( 404, error_404_title );
            add_headers( strlen( error_404_form ) );
            if ( ! add_content( error_404_form ) ) {
                return false;
            }
            break;
        case FORBIDDEN_REQUEST:
            add_status_line( 403, error_403_title );
            add_headers(strlen( error_403_form));
            if ( ! add_content( error_403_form ) ) {
                return false;
            }
            break;
        case FILE_REQUEST:    //文件存在，200
            add_status_line(200, ok_200_title );

            // 如果请求的资源存在，将响应写到 connfd 的写缓存 m_write_buf 中
            if (m_file_stat.st_size != 0){
                add_headers(m_file_stat.st_size);
                // 第一个 iovec 指针指向响应报文缓冲区，长度指向 m_write_idx
                m_iv[0].iov_base = m_write_buf;
                m_iv[0].iov_len = m_write_idx;
                // 第二个 iovec 指针指向 mmap 返回的文件指针，长度指向文件大小
                m_iv[1].iov_base = m_file_address;
                m_iv[1].iov_len = m_file_stat.st_size;
                m_iv_count = 2;

                // 发送的全部数据为响应报文头部信息和文件大小
                bytes_to_send = m_write_idx + m_file_stat.st_size;
                return true;
            }else{
                // 如果请求的资源大小为 0，则返回空白 html 文件
                const char *ok_string = "<html><body></body></html>";
                add_headers(strlen(ok_string));
                if (!add_content(ok_string))
                return false;
            }
        default:
            return false;
    }

    // 除 FILE_REQUEST 状态外，其余状态只申请一个iovec，指向响应报文缓冲区
    m_iv[ 0 ].iov_base = m_write_buf;
    m_iv[ 0 ].iov_len = m_write_idx;
    m_iv_count = 1;
    bytes_to_send = m_write_idx;

    return true;
}

// 正则表达式 或者 常规方法：解析 HTTP 请求行，获得请求方法，目标 URL ,以及 HTTP 版本号
// 请求行中最重要的就是 URL 部分，将会保留下来用于后面的 HTTP 响应
http_conn::HTTP_CODE http_conn::parse_request_line(char *text){
    // GET /index.html HTTP/1.1
    m_url = strpbrk(text, " \t"); // 判断第二个参数中的字符（空格 和 \t），哪个在text中最先出现
    // 如果没有空格或\t，则报文格式有误
    if (! m_url) { 
        return BAD_REQUEST;
    }
    // GET\0/index.html HTTP/1.1
    *m_url++ = '\0';    // 置位空字符，字符串结束符

    //取出数据，并通过与GET和POST比较，以确定请求方式
    char* method = text;
    if ( strcasecmp(method, "GET") == 0 ) { // 忽略大小写比较
        m_method = GET;
    } else if (strcasecmp(method, "POST") == 0){
        m_method = POST;
        cgi = 1;
    }else {
        return BAD_REQUEST;
    }

    // /index.html HTTP/1.1
    // 检索字符串 str1 中第一个不在字符串 str2 中出现的字符下标。
    m_url += strspn(m_url, " \t");

    m_version = strpbrk( m_url, " \t" );
    if (!m_version) {
        return BAD_REQUEST;
    }
    // /index.html\0HTTP/1.1
    *m_version++ = '\0';

    m_version += strspn(m_version, " \t");

    if (strcasecmp( m_version, "HTTP/1.1") != 0 ) {
        return BAD_REQUEST;
    }

    // http://192.168.57.128:10000/index.html
    if (strncasecmp(m_url, "http://", 7) == 0 ) {   
        m_url += 7;   // 192.168.57.128:10000/index.html
        // 在参数 str 所指向的字符串中搜索第一次出现字符（一个无符号字符）的位置。
        m_url = strchr( m_url, '/' );  // /index.html
    }

    if (strncasecmp(m_url, "https://", 8) == 0)
    {
        m_url += 8;
        m_url = strchr(m_url, '/');
    }

    // 一般的不会带有上述两种符号，直接是单独的/ 或 /后面带访问资源
    if ( !m_url || m_url[0] != '/' ) {
        return BAD_REQUEST;
    }

    //当url为 / 时，显示欢迎界面
    if (strlen(m_url) == 1){
        strcat(m_url, "judge.html");
    }

    m_check_state = CHECK_STATE_HEADER; // 主状态机：检查状态变成检查请求头
    return NO_REQUEST;
}

// 解析 http 请求的一个头部信息：请求行以下、空行以上的部分
http_conn::HTTP_CODE http_conn::parse_headers(char *text){
    // 遇到空行，表示 请求头部 解析完毕
    if( text[0] == '\0' ) {
        // 如果HTTP请求有消息体，则还需要读取m_content_length字节的消息体，
        // 如果不是0，表明是POST请求，则状态转移到CHECK_STATE_CONTENT
        if ( m_content_length != 0 ) {
            m_check_state = CHECK_STATE_CONTENT;
            return NO_REQUEST;
        }
        // 否则说明是GET请求，我们已经得到了一个完整的HTTP请求
        return GET_REQUEST;
    } else if ( strncasecmp( text, "Connection:", 11 ) == 0 ) {
        // 处理Connection 头部字段  Connection: keep-alive
        text += 11;
        text += strspn( text, " \t" );     // 跳过空格和\t字符
        if ( strcasecmp( text, "keep-alive" ) == 0 ) {
            // 如果是长连接，则将linger标志设置为true
            m_linger = true;
        }
    } else if ( strncasecmp( text, "Content-Length:", 15 ) == 0 ) {
        // 处理Content-Length头部字段
        text += 15;
        text += strspn( text, " \t" );
        m_content_length = atol(text);
    } else if ( strncasecmp( text, "Host:", 5 ) == 0 ) {
        // 处理Host头部字段
        text += 5;
        text += strspn( text, " \t" );
        m_host = text;
    } else {
        // printf( "oop! unknow header %s\n", text );
        LOG_INFO("oop!unknow header: %s", text);
        // Log::get_instance()->flush();
    }
    return NO_REQUEST;
}

// 判断http请求是否被完整读入
/* 解析请求数据，对于GET来说这部分是空的，因为这部分内容已经以明文的方式包含在了请求行中的URL部分了；
   只有POST的这部分是有数据的，项目中的这部分数据为用户名和密码，
   我们会根据这部分内容做登录和校验，并涉及到与数据库的连接。    */
http_conn::HTTP_CODE http_conn::parse_content(char *text){
    if (m_read_idx >= (m_content_length + m_checked_idx))
    {
        text[m_content_length] = '\0';

        //POST请求中最后为输入的用户名和密码
        m_string = text;

        return GET_REQUEST;
    }
    return NO_REQUEST;
}

// 当得到一个完整、正确的 HTTP 请求时，我们就分析目标文件的属性，
// 如果目标文件存在、对所有用户可读，且不是目录，则使用 mmap 将其
// 映射到内存地址 m_file_address 处，并告诉调用者获取文件成功
http_conn::HTTP_CODE http_conn::do_request(){
    // "/home/wf/webserver/resources"
    // 将初始化的m_real_file赋值为网站根目录
    strcpy( m_real_file, doc_root );
    int len = strlen( doc_root );

    // 找到 m_url 中 / 的位置，根据 / 后的第一个字符判断是登录还是注册校验
    const char *p = strrchr(m_url, '/');  // 查找在s字符串中最后一次出现字符c的位置,如果s中存在字符c,返回出现c的位置的指针；否则返回NULL。

    //处理cgi：2是登录校验，3是注册校验
    if (cgi == 1 && (*(p + 1) == '2' || *(p + 1) == '3')){

        // 根据标志判断是登录检测还是注册检测
        char flag = m_url[1];

        char *m_url_real = (char *)malloc(sizeof(char) * 200);
        strcpy(m_url_real, "/");
        strcat(m_url_real, m_url + 2);
        strncpy(m_real_file + len, m_url_real, FILENAME_LEN - len - 1);
        free(m_url_real);

        // 将用户名和密码提取出来
        // user=123&passwd=123
        char name[100], password[100];
        int i;
        // 以 & 为分隔符，前面的为用户名
        for (i = 5; m_string[i] != '&'; ++i)
            name[i - 5] = m_string[i];
        name[i - 5] = '\0';

        // 以 & 为分隔符，后面的是密码
        int j = 0;
        for (i = i + 10; m_string[i] != '\0'; ++i, ++j)
            password[j] = m_string[i];
        password[j] = '\0';

        // 同步线程注册校验
        if (*(p + 1) == '3')
        {
            // 如果是注册，先检测数据库中是否有重名的
            // 没有重名的，进行增加数据
            char *sql_insert = (char *)malloc(sizeof(char) * 200);
            strcpy(sql_insert, "INSERT INTO user(username, passwd) VALUES(");
            strcat(sql_insert, "'");
            strcat(sql_insert, name);
            strcat(sql_insert, "', '");
            strcat(sql_insert, password);
            strcat(sql_insert, "')");

            // 判断 map 中能否找到重复的用户名
            if (users.find(name) == users.end()){
                // 向数据库中插入数据时，需要通过锁来同步数据
                m_lock.lock();
                int res = mysql_query(mysql, sql_insert);
                users.insert(pair<string, string>(name, password));
                m_lock.unlock();

                if (!res){   // 校验成功，跳转登录页面
                    strcpy(m_url, "/log.html");
                }else{       // 校验失败，跳转注册失败页面
                    strcpy(m_url, "/registerError.html");
                }
            }else{
                strcpy(m_url, "/registerError.html");
            }
        }
        //如果是登录，直接判断
        //若浏览器端输入的用户名和密码在表中可以查找到，返回1，否则返回0
        else if (*(p + 1) == '2'){
            if (users.find(name) != users.end() && users[name] == password){
                strcpy(m_url, "/welcome.html");
            }else{
                strcpy(m_url, "/logError.html");
            }
        }
    }

    
    if (*(p + 1) == '0'){          // 如果请求资源为 /0，表示跳转注册界面
        char *m_url_real = (char *)malloc(sizeof(char) * 200);
        strcpy(m_url_real, "/register.html");
        // 将网站目录和 /log.html 进行拼接，更新到 m_real_file 中
        strncpy(m_real_file + len, m_url_real, strlen(m_url_real));

        free(m_url_real);
    }else if (*(p + 1) == '1'){    // 如果请求资源为 /1，表示跳转登录界面
        char *m_url_real = (char *)malloc(sizeof(char) * 200);
        strcpy(m_url_real, "/log.html");
        strncpy(m_real_file + len, m_url_real, strlen(m_url_real));

        free(m_url_real);
    }else if (*(p + 1) == '5'){
        char *m_url_real = (char *)malloc(sizeof(char) * 200);
        strcpy(m_url_real, "/picture.html");
        strncpy(m_real_file + len, m_url_real, strlen(m_url_real));

        free(m_url_real);
    }else if (*(p + 1) == '6'){
        char *m_url_real = (char *)malloc(sizeof(char) * 200);
        strcpy(m_url_real, "/video.html");
        strncpy(m_real_file + len, m_url_real, strlen(m_url_real));

        free(m_url_real);
    }else if (*(p + 1) == '7'){
        char *m_url_real = (char *)malloc(sizeof(char) * 200);
        strcpy(m_url_real, "/fans.html");
        strncpy(m_real_file + len, m_url_real, strlen(m_url_real));

        free(m_url_real);
    }else{
        // 否则发送 url 实际请求的文件
        strncpy( m_real_file + len, m_url, FILENAME_LEN - len - 1 );   // "/home/wf/webserver/resources/index.html"
    }

    // 获取m_real_file文件的相关的状态信息，-1失败，失败返回NO_RESOURCE状态，表示资源不存在；0成功，则将信息更新到m_file_stat结构体
    // stat 获取文件属性，存储在 statbuf 中
    if ( stat( m_real_file, &m_file_stat ) < 0 ) {
        return NO_RESOURCE;
    }

    // 判断访问权限
    if ( ! ( m_file_stat.st_mode & S_IROTH ) ) {
        return FORBIDDEN_REQUEST;
    }

    // 判断是否是目录
    if ( S_ISDIR( m_file_stat.st_mode ) ) {
        return BAD_REQUEST;
    }

    // 以只读方式打开文件，通过mmap将该文件映射到内存中
    int fd = open( m_real_file, O_RDONLY );
    // 若目标文件存在、对所有用户可读且不是目录时，则使用mmap将其映射到内存地址 m_file_address 处，并告诉调用者获取文件成功FILE_REQUEST。
    /*  mmap 用于将一个文件或其他对象映射到内存，提高文件的访问速度。
        start：映射区的开始地址，设置为0时表示由系统决定映射区的起始地址
        length：映射区的长度
        prot：期望的内存保护标志，不能与文件的打开模式冲突
            PROT_READ 表示页内容可以被读取
        flags：指定映射对象的类型，映射选项和映射页是否可以共享
            MAP_PRIVATE 建立一个写入时拷贝的私有映射，内存区域的写入不会影响到原文件
        fd：有效的文件描述符，一般是由open()函数返回
        off_toffset：被映射对象内容的起点  */
    m_file_address = ( char* )mmap( 0, m_file_stat.st_size, PROT_READ, MAP_PRIVATE, fd, 0 );
    close( fd );           // 避免文件描述符的浪费和占用
    return FILE_REQUEST;   // 表示请求文件存在，且可以访问
}


// 往写缓冲中写入待发送的数据
bool http_conn::add_response(const char *format, ...)
{
    // 如果写入内容超出 m_write_buf 大小则报错
    if (m_write_idx >= WRITE_BUFFER_SIZE)
        return false;
    va_list arg_list;              // 定义可变参数列表
    va_start(arg_list, format);    // 将变量 arg_list 初始化为传入参数
    // 将数据 format 从可变参数列表写入缓冲区写，返回写入数据的长度
    int len = vsnprintf(m_write_buf + m_write_idx, WRITE_BUFFER_SIZE - 1 - m_write_idx, format, arg_list);
    // 如果写入的数据长度超过缓冲区剩余空间，则报错
    if (len >= (WRITE_BUFFER_SIZE - 1 - m_write_idx))
    {
        va_end(arg_list);

        return false;
    }
    m_write_idx += len;   // 更新 m_write_idx 位置
    va_end(arg_list);     // 清空可变参列表

    LOG_INFO("request:%s", m_write_buf);
    // Log::get_instance()->flush();

    return true;
}

// 添加状态行：http/1.1 状态码 状态消息
bool http_conn::add_status_line(int status, const char *title){
    return add_response("%s %d %s\r\n", "HTTP/1.1", status, title);
}

// 添加消息报头，具体的添加文本长度、连接状态和空行，内部调用add_content_length和add_linger函数
bool http_conn::add_headers(int content_len){
    return add_content_length(content_len) && add_linger() && add_blank_line();
}

// 记录响应报文长度，用于浏览器端判断服务器是否发送完数据
bool http_conn::add_content_length(int content_len){
    return add_response("Content-Length:%d\r\n", content_len);
}

// 添加文本类型，这里是 html
bool http_conn::add_content_type(){
    return add_response("Content-Type:%s\r\n", "text/html");
}

// 记录连接状态，通知浏览器端是保持连接还是关闭
bool http_conn::add_linger(){
    return add_response("Connection:%s\r\n", (m_linger == true) ? "keep-alive" : "close");
}

// 添加空行
bool http_conn::add_blank_line(){
    return add_response("%s", "\r\n");
}

// 添加文本 content
bool http_conn::add_content(const char *content){
    return add_response("%s", content);
}

void http_conn::process()
{
    HTTP_CODE read_ret = process_read();
    // NO_REQUEST，表示请求不完整，需要继续接收请求数据
    if (read_ret == NO_REQUEST)
    {
        modfd(m_epollfd, m_sockfd, EPOLLIN, m_TRIGMode);   // 注册并监听读事件
        return;
    }

    // 调用 process_write 完成报文响应
    bool write_ret = process_write(read_ret);
    if (!write_ret)
    {
        close_conn();
    }
    modfd(m_epollfd, m_sockfd, EPOLLOUT, m_TRIGMode);      // 注册并监听写事件
}
