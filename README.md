
# WebServer

Linux下C++轻量级Web服务器的主要工作：
- 使用多线程充分利用多核CPU，增加并行服务数量，并使用**线程池**避免线程频繁创建销毁的开销；
- 使用**非阻塞socket+epoll (LT和ET均实现)+事件处理(Reactor和Proactor均实现)** 的并发模型；
- 使用**状态机**解析 HTTP 请求报文，支持**GET/POST**请求，支持长/短连接；
- 采用基于升序双向链表的**定时器**，定时检测并断开非活跃用户；
- 实现**同步/异步日志系统**，记录服务器运行状态； 
- 访问服务器数据库实现 web 端用户**注册、登录**功能，可以请求服务器**图片和视频**文件;
- 经Webbench压力测试可以实现**上万的并发连接**数据交换 


# 目录

|[框架](#框架)|[Demo演示](https://github.com/HIT2020HK/WebServer/blob/web/README.md#demo%E6%BC%94%E7%A4%BA)|[快速运行](https://github.com/HIT2020HK/WebServer/blob/web/README.md#%E5%BF%AB%E9%80%9F%E8%BF%90%E8%A1%8C)|[个性化运行](https://github.com/HIT2020HK/WebServer/blob/web/README.md#%E4%B8%AA%E6%80%A7%E5%8C%96%E8%BF%90%E8%A1%8C) | [压力测试](https://github.com/HIT2020HK/WebServer/blob/web/README.md#%E5%8E%8B%E5%8A%9B%E6%B5%8B%E8%AF%95)|[致谢](https://github.com/HIT2020HK/WebServer/blob/web/README.md#%E8%87%B4%E8%B0%A2)|
|:--------:|:--------:|:--------:|:--------:|:--------:|:--------:|

# 框架

![frame](https://user-images.githubusercontent.com/86244913/180124295-b56ceddc-03bc-465d-b5b0-15f20484c6d6.jpg)

# Demo演示

- 欢迎界面

![图片](https://user-images.githubusercontent.com/86244913/180174879-1b0d9f7b-bf5d-4d0f-b93f-81c43272c3d1.png)

- 注册界面

![图片](https://user-images.githubusercontent.com/86244913/180174802-c772999d-27d0-4738-a9d4-2885a3553b5b.png)
- 登陆界面

![图片](https://user-images.githubusercontent.com/86244913/180175048-377397a8-3f30-4bce-92a9-aa4232efa01b.png)
- 登录成功界面

![图片](https://user-images.githubusercontent.com/86244913/180175476-76633cc2-dfad-4eaf-926c-c2770e4f9dfc.png)
- 校训界面

![图片](https://user-images.githubusercontent.com/86244913/180175607-8c9da003-389b-4961-9fe8-f90f3a6d272b.png)
- 招生界面

![图片](https://user-images.githubusercontent.com/86244913/180175690-4a092467-e9b8-43cf-9375-a07068505528.png)
- 宣传片界面（视频）

![图片](https://user-images.githubusercontent.com/86244913/180175779-6788e33c-acad-437d-b070-c135b4b4c011.png)

        
# 快速运行
- 服务器测试环境
    ```C++
        Ubuntu版本18.04
        MySQL版本5.7.38  
    ```
- 浏览器测试环境
    ```C++
        Linux
        FireFox
    ```
- 测试前已安装MySQL数据库

    ```C++
    // 建立yourdb库
    create database yourdb;
    // 创建user表
    USE yourdb;
    CREATE TABLE user(
        username char(50) NULL,
        passwd char(50) NULL
    )ENGINE=InnoDB;
    // 添加数据
    INSERT INTO user(username, passwd) VALUES('name', 'passwd');
    ```
    
- 修改main.cpp中的数据库初始化信息
    ```C++
    //数据库登录名,密码,库名
    string user = "root";
    string passwd = "root";
    string databasename = "yourdb";
    ```
- build
    ```C++
    sh ./build.sh
    ```
- 启动server
   ```C++
   ./server
   ```
- 浏览器端
    ```C++
    ip:9006
    ```
# 个性化运行
```C++
./server [-p port] [-l LOGWrite] [-m TRIGMode] [-o OPT_LINGER] [-s sql_num] [-t thread_num] [-c close_log] [-a actor_model]
```

- -p，自定义端口号
     * 默认9006
- -l，选择日志写入方式，默认同步写入
     * 0，同步写入
     * 1，异步写入
- -m，listenfd和connfd的模式组合，默认使用LT + LT
     * 0，表示使用LT + LT
     * 1，表示使用LT + ET
     * 2，表示使用ET + LT
     * 3，表示使用ET + ET
- -o，优雅关闭连接，默认不使用
     * 0，不使用
     * 1，使用
- -s，数据库连接数量
     * 默认为8
- -t，线程数量
     * 默认为8
- -c，关闭日志，默认打开
     * 0，打开日志
     * 1，关闭日志
- -a，选择反应堆模型，默认Proactor
      * 0，Proactor模型
      * 1，Reactor模型

测试示例命令与含义
```C++
./server -p 9007 -l 1 -m 0 -o 1 -s 10 -t 10 -c 1 -a 1
```
- [x] 端口9007
- [x] 异步写入日志
- [x] 使用LT + LT组合
- [x] 使用优雅关闭连接
- [x] 数据库连接池内有10条连接
- [x] 线程池内有10条线程
- [x] 关闭日志
- [x] Reactor反应堆模型

# 压力测试

在关闭日志后，使用Webbench对服务器进行压力测试，对listenfd和connfd分别采用ET和LT模式，均可实现上万的并发连接，下面列出的是两者组合后的测试结果.

- Proactor，LT + LT，25023 QPS
   
   ![图片](https://user-images.githubusercontent.com/86244913/180144660-6116e00a-1d09-4d13-ae74-aecc1ebd6c31.png)
        
- Proactor，LT + ET，16876 QPS
   
   ![图片](https://user-images.githubusercontent.com/86244913/180148527-28bb3cdd-72f2-45b2-bf78-c48234f440e0.png)
   
- Proactor，ET + LT，17943 QPS
   
   ![图片](https://user-images.githubusercontent.com/86244913/180148576-8b8ab576-a298-4c57-a70d-ad6c53db8397.png)
    
- Proactor，ET + ET，17428 QPS
    
   ![图片](https://user-images.githubusercontent.com/86244913/180148617-db9901a9-ddf7-48e7-b1a0-941ae684ac97.png)
   
- Reactor，LT + ET，8624 QPS
   
   ![图片](https://user-images.githubusercontent.com/86244913/180148655-e6244b43-e6f0-4799-8684-661c037130db.png)

- 并发连接总数：10000
- 访问服务器时间：5s
- 所有访问均成功

# 致谢

《Linux高性能服务器编程》游双著
