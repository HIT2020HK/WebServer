#include "webserver.h"

WebServer::WebServer()
{
    //http_conn类对象
    users = new http_conn[MAX_FD];

    //root文件夹路径
    char server_path[200];
    getcwd(server_path, 200);
    char root[6] = "/root";
    m_root = (char *)malloc(strlen(server_path) + strlen(root) + 1);
    strcpy(m_root, server_path);
    strcat(m_root, root);

    // 创建连接资源数组
    users_timer = new client_data[MAX_FD];
}

WebServer::~WebServer()
{
    close(m_epollfd);
    close(m_listenfd);
    close(m_pipefd[1]);
    close(m_pipefd[0]);
    delete[] users;
    delete[] users_timer;
    delete m_pool;
}

void WebServer::init(int port, string user, string passWord, string databaseName, int log_write, 
                     int opt_linger, int trigmode, int sql_num, int thread_num, int close_log, int actor_model)
{
    m_port = port;
    m_user = user;
    m_passWord = passWord;
    m_databaseName = databaseName;
    m_sql_num = sql_num;
    m_thread_num = thread_num;
    m_log_write = log_write;
    m_OPT_LINGER = opt_linger;
    m_TRIGMode = trigmode;
    m_close_log = close_log;
    m_actormodel = actor_model;
}

void WebServer::trig_mode()
{
    //LT + LT
    if (0 == m_TRIGMode)
    {
        m_LISTENTrigmode = 0;
        m_CONNTrigmode = 0;
    }
    //LT + ET
    else if (1 == m_TRIGMode)
    {
        m_LISTENTrigmode = 0;
        m_CONNTrigmode = 1;
    }
    //ET + LT
    else if (2 == m_TRIGMode)
    {
        m_LISTENTrigmode = 1;
        m_CONNTrigmode = 0;
    }
    //ET + ET
    else if (3 == m_TRIGMode)
    {
        m_LISTENTrigmode = 1;
        m_CONNTrigmode = 1;
    }
}

void WebServer::log_write()
{
    if (0 == m_close_log)
    {
        //初始化日志
        if (1 == m_log_write)
            Log::get_instance()->init("./ServerLog", m_close_log, 2000, 800000, 800);
        else
            Log::get_instance()->init("./ServerLog", m_close_log, 2000, 800000, 0);
    }
}

void WebServer::sql_pool()
{
    // 初始化数据库连接池
    m_connPool = connection_pool::GetInstance();
    m_connPool->init("localhost", m_user, m_passWord, m_databaseName, 3306, m_sql_num, m_close_log);

    // 初始化数据库读取表
    users->initmysql_result(m_connPool);
}

void WebServer::thread_pool()
{
    //线程池
    m_pool = new threadpool<http_conn>(m_actormodel, m_connPool, m_thread_num);
}

void WebServer::eventListen()
{
    // 网络编程基础步骤，创建监听socket文件描述符
    m_listenfd = socket(PF_INET, SOCK_STREAM, 0);
    assert(m_listenfd >= 0);

    // 优雅关闭连接
    if (0 == m_OPT_LINGER)
    {
        struct linger tmp = {0, 1};
        setsockopt(m_listenfd, SOL_SOCKET, SO_LINGER, &tmp, sizeof(tmp));
    }
    else if (1 == m_OPT_LINGER)
    {
        struct linger tmp = {1, 1};
        setsockopt(m_listenfd, SOL_SOCKET, SO_LINGER, &tmp, sizeof(tmp));
    }

    // 创建监听socket的TCP/IP的IPV4 socket地址 
    struct sockaddr_in address;
    bzero(&address, sizeof(address));
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_ANY);   // INADDR_ANY：将套接字绑定到所有可用的接口；完成32位无符号数的相互转换
    address.sin_port = htons(m_port);  // 网络字节序与主机字节序之间的转换函数:htons完成16位无符号数的相互转换

    // 设置端口复用：SO_REUSEADDR 允许端口被重复使用
    int reuse = 1;
    setsockopt(m_listenfd,SOL_SOCKET,SO_REUSEADDR,&reuse,sizeof(reuse));

    // 绑定监听的套接字
    int ret = 0;
    ret = bind(m_listenfd, (struct sockaddr *)&address, sizeof(address));
    assert(ret >= 0);
    // 创建监听队列以存放待处理的客户连接，在这些客户连接被accept()之前
    ret = listen(m_listenfd, 5);  
    assert(ret >= 0);

    utils.init(TIMESLOT);

    // 创建一个额外的文件描述符来唯一标识内核中的 epoll 事件表
    m_epollfd = epoll_create(5);
    // 用于存储epoll事件表中就绪事件的event数组
    epoll_event events[MAX_EVENT_NUMBER];
    assert(m_epollfd != -1);

    // 将监听的文件描述符添加到 epoll 对象中
    // 主线程往 epoll 内核事件表中注册监听socket事件，当listen到新的客户连接时，listenfd变为就绪事件
    utils.addfd(m_epollfd, m_listenfd, false, m_LISTENTrigmode);
    http_conn::m_epollfd = m_epollfd;

    // 创建管道套接字，项目中进程使用管道通信，注册pipefd[0]上的可读事件
    /* domain表示协议族，PF_UNIX或者AF_UNIX
       type表示协议，可以是SOCK_STREAM或者SOCK_DGRAM，SOCK_STREAM基于TCP，SOCK_DGRAM基于UDP
       protocol表示类型，只能为0
       sv[2] 表示套节字柄对，该两个句柄作用相同，均能进行读写双向操作
       返回结果， 0为创建成功，-1为创建失败 */
    ret = socketpair(PF_UNIX, SOCK_STREAM, 0, m_pipefd);
    assert(ret != -1);
    // 设置管道写端为非阻塞
    // send 是将信息发送给套接字缓冲区，如果缓冲区满了，则会阻塞，这时候会进一步增加信号处理函数的执行时间，为此，将其修改为非阻塞。
    utils.setnonblocking(m_pipefd[1]);
    // 设置管道读端为ET非阻塞，并添加到epoll内核事件表
    utils.addfd(m_epollfd, m_pipefd[0], false, 0);

    // 传递给主循环的信号值，这里只关注 SIGALRM 和 SIGTERM
    utils.addsig(SIGPIPE, SIG_IGN);
    utils.addsig(SIGALRM, utils.sig_handler, false);
    utils.addsig(SIGTERM, utils.sig_handler, false);

    // 每隔 TIMESLOT 时间触发 SIGALRM 信号
    alarm(TIMESLOT);

    //工具类,信号和描述符基础操作
    Utils::u_pipefd = m_pipefd;
    Utils::u_epollfd = m_epollfd;
}

void WebServer::timer(int connfd, struct sockaddr_in client_address)
{
    users[connfd].init(connfd, client_address, m_root, m_CONNTrigmode, m_close_log, m_user, m_passWord, m_databaseName);

    // 创建定时器，设置回调函数和超时时间，绑定用户数据，将定时器添加到链表中

    // 初始化 client_data 数据
    users_timer[connfd].address = client_address;
    users_timer[connfd].sockfd = connfd;

   
    util_timer *timer = new util_timer;      // 创建定时器临时变量   
    timer->user_data = &users_timer[connfd]; // 设置定时器对应的连接资源
    timer->cb_func = cb_func;                // 设置回调函数

    time_t cur = time(NULL);
    timer->expire = cur + 3 * TIMESLOT;      // 设置绝对超时时间
    users_timer[connfd].timer = timer;       // 创建该连接对应的定时器，初始化为前述临时变量
    utils.m_timer_lst.add_timer(timer);      // 将该定时器添加到链表中
}

// 若有数据传输，则将定时器往后延迟3个单位，并对新的定时器在链表上的位置进行调整
void WebServer::adjust_timer(util_timer *timer)
{
    time_t cur = time(NULL);
    timer->expire = cur + 3 * TIMESLOT;
    utils.m_timer_lst.adjust_timer(timer);

    LOG_INFO("%s", "adjust timer once");
}

void WebServer::deal_timer(util_timer *timer, int sockfd)
{
    timer->cb_func(&users_timer[sockfd]);
    if (timer)
    {
        utils.m_timer_lst.del_timer(timer);
    }

    LOG_INFO("close fd %d", users_timer[sockfd].sockfd);
}

bool WebServer::dealclinetdata()
{
    // 初始化客户端连接地址
    struct sockaddr_in client_address;
    socklen_t client_addrlength = sizeof(client_address);

    if (0 == m_LISTENTrigmode){
        // accept()返回一个新的 socket 文件描述符用于 send() 和 recv()
        int connfd = accept(m_listenfd, (struct sockaddr *)&client_address, &client_addrlength);
        if (connfd < 0)
        {
            LOG_ERROR("%s:errno is:%d", "accept error", errno);
            return false;
        }
        //目前连接数满了，给客户端写一个信息：服务器内部正忙
        if (http_conn::m_user_count >= MAX_FD)
        {
            utils.show_error(connfd, "Internal server busy");
            LOG_ERROR("%s", "Internal server busy");
            return false;
        }
        timer(connfd, client_address);
    }else{
        while (1)
        {
            int connfd = accept(m_listenfd, (struct sockaddr *)&client_address, &client_addrlength);
            if (connfd < 0)
            {
                LOG_ERROR("%s:errno is:%d", "accept error", errno);
                break;
            }
            if (http_conn::m_user_count >= MAX_FD)
            {
                utils.show_error(connfd, "Internal server busy");
                LOG_ERROR("%s", "Internal server busy");
                break;
            }
            timer(connfd, client_address);
        }
        return false;
    }
    return true;
}

bool WebServer::dealwithsignal(bool &timeout, bool &stop_server)
{
    int ret = 0;
    int sig;
    char signals[1024];

    // 从管道读端读出信号值，成功返回字节数，失败返回-1
    // 正常情况下，这里的ret返回值总是1，只有14和15两个ASCII码对应的字符
    ret = recv(m_pipefd[0], signals, sizeof(signals), 0);
    if (ret == -1){
        return false;
    }else if (ret == 0){
        return false;
    }else{  // 信号本身是整型数值，管道中传递的是ASCII码表中整型数值对应的字符
        for (int i = 0; i < ret; ++i){
            // 当 switch 的变量为字符时，case中可以是字符，也可以是字符对应的 ASCII 码。
            switch (signals[i]){
                case SIGALRM:{
                    timeout = true;
                    break;
                }
                    
                case SIGTERM:{  // SIGTERM（kill会触发，Ctrl+C）
                    stop_server = true;
                    break;
                }
                    
            }
        }
    }
    return true;
}

void WebServer::dealwithread(int sockfd)
{
    // 创建定时器临时变量，将该连接对应的定时器取出来
    util_timer *timer = users_timer[sockfd].timer;

    //reactor
    if (1 == m_actormodel)
    {
        if (timer)
        {
            adjust_timer(timer);
        }

        //若监测到读事件，将该事件放入请求队列
        m_pool->append(users + sockfd, 0);

        while (true)
        {
            if (1 == users[sockfd].improv)
            {
                if (1 == users[sockfd].timer_flag)
                {
                    deal_timer(timer, sockfd);
                    users[sockfd].timer_flag = 0;
                }
                users[sockfd].improv = 0;
                break;
            }
        }
    }
    else
    {
        //proactor
        if (users[sockfd].read_once()){        // 1、主线程从这一sockfd循环读取数据, 直到没有更多数据可读
            LOG_INFO("deal with the client(%s)", inet_ntoa(users[sockfd].get_address()->sin_addr));

            //若监测到读事件，将该事件放入请求队列
            m_pool->append_p(users + sockfd);  // 2、然后将读取到的数据封装成一个请求对象并插入请求队列

            if (timer)
            {
                adjust_timer(timer);
            }
        }else{
            deal_timer(timer, sockfd);
        }
    }
}

void WebServer::dealwithwrite(int sockfd)
{
    // 创建定时器临时变量，将该连接对应的定时器取出来
    util_timer *timer = users_timer[sockfd].timer;
    //reactor
    if (1 == m_actormodel)
    {
        if (timer)
        {
            adjust_timer(timer);
        }

        m_pool->append(users + sockfd, 1);

        while (true)
        {
            if (1 == users[sockfd].improv)
            {
                if (1 == users[sockfd].timer_flag)
                {
                    deal_timer(timer, sockfd);
                    users[sockfd].timer_flag = 0;
                }
                users[sockfd].improv = 0;
                break;
            }
        }
    }else{
        //proactor
        if (users[sockfd].write())
        {
            LOG_INFO("send data to the client(%s)", inet_ntoa(users[sockfd].get_address()->sin_addr));

            if (timer){
                adjust_timer(timer);
            }
        }else{
            deal_timer(timer, sockfd);
        }
    }
}

void WebServer::eventLoop()
{
    bool timeout = false;       // 超时标志
    bool stop_server = false;   // 循环条件

    while (!stop_server)
    {
        // 主线程调用 epoll_wait 等待一组文件描述符上的事件，并将当前所有就绪的 epoll_event 复制到 events 数组中
        int number = epoll_wait(m_epollfd, events, MAX_EVENT_NUMBER, -1);  // -1 代表超时等待时间是 无限
        if (number < 0 && errno != EINTR)
        {
            LOG_ERROR("%s", "epoll failure");
            break;
        }

        // 然后遍历这一数组以处理这些已经就绪的事件
        for (int i = 0; i < number; i++)
        {
            // 事件表中就绪的 socket 文件描述符
            int sockfd = events[i].data.fd;

            // 当 listen 到新的用户连接，listenfd 上则产生就绪事件
            if (sockfd == m_listenfd){
                bool flag = dealclinetdata();
                if (false == flag)
                    continue;
            }else if (events[i].events & (EPOLLRDHUP | EPOLLHUP | EPOLLERR)){
                // 对方异常断开或者错误等事件，服务器端关闭连接，并删除该用户的timer
                util_timer *timer = users_timer[sockfd].timer;
                deal_timer(timer, sockfd);
            }else if ((sockfd == m_pipefd[0]) && (events[i].events & EPOLLIN)){ //处理信号：管道读端对应文件描述符发生读事件
                bool flag = dealwithsignal(timeout, stop_server);
                if (false == flag)
                    LOG_ERROR("%s", "dealclientdata failure");
            }else if (events[i].events & EPOLLIN){  // 处理客户连接上接收到的数据
                dealwithread(sockfd);
            }else if (events[i].events & EPOLLOUT){ // 检测写事件
                dealwithwrite(sockfd);
            }
        }

        // 处理定时器为非必须事件，收到信号并不是立马处理
        // 完成读写事件后，再进行处理
        if (timeout)
        {
            utils.timer_handler();

            LOG_INFO("%s", "timer tick");

            timeout = false;
        }
    }
}
