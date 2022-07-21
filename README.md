
# WebServer

Linux下C++轻量级Web服务器的主要工作：

    ·使用 线程池 + 非阻塞socket + epoll(ET和LT均实现) + 事件处理(Reactor和模拟Proactor均实现) 的并发模型
    ·使用状态机解析HTTP请求报文，支持解析GET和POST请求
    ·访问服务器数据库实现web端用户注册、登录功能，可以请求服务器图片和视频文件
    ·实现同步/异步日志系统，记录服务器运行状态
    ·经Webbench压力测试可以实现上万的并发连接数据交换


# 目录
概述 	框架 	Demo演示 	压力测试 	更新日志 	源码下载 	快速运行 	个性化运行 	庖丁解牛 	CPP11实现 	致谢

# 框架
! [This is an image]（）
Demo演示

        注册演示

        登录演示

        请求图片文件演示(6M)

        请求视频文件演示(39M)

压力测试

在关闭日志后，使用Webbench对服务器进行压力测试，对listenfd和connfd分别采用ET和LT模式，均可实现上万的并发连接，下面列出的是两者组合后的测试结果.

        Proactor，LT + LT，93251 QPS

        Proactor，LT + ET，97459 QPS

        Proactor，ET + LT，80498 QPS

        Proactor，ET + ET，92167 QPS

        Reactor，LT + ET，69175 QPS

        并发连接总数：10500
        访问服务器时间：5s
        所有访问均成功

注意： 使用本项目的webbench进行压测时，若报错显示webbench命令找不到，将可执行文件webbench删除后，重新编译即可。
更新日志

    解决请求服务器上大文件的Bug
    增加请求视频文件的页面
    解决数据库同步校验内存泄漏
    实现非阻塞模式下的ET和LT触发，并完成压力测试
    完善lock.h中的封装类，统一使用该同步机制
    改进代码结构，更新局部变量懒汉单例模式
    优化数据库连接池信号量与代码结构
    使用RAII机制优化数据库连接的获取与释放
    优化代码结构，封装工具类以减少全局变量
    编译一次即可，命令行进行个性化测试更加友好
    main函数封装重构
    新增命令行日志开关，关闭日志后更新压力测试结果
    改进编译方式，只配置一次SQL信息即可
    新增Reactor模式，并完成压力测试

源码下载

目前有两个版本，版本间的代码结构有较大改动，文档和代码运行方法也不一致。重构版本更简洁，原始版本(raw_version)更大保留游双代码的原汁原味，从原始版本更容易入手.

如果遇到github代码下载失败，或访问太慢，可以从以下链接下载，与Github最新提交同步.

    重构版本下载地址 : BaiduYun
        提取码 : vsqq
    原始版本(raw_version)下载地址 : BaiduYun
        提取码 : 9wye
        原始版本运行请参考原始文档

快速运行

    服务器测试环境
        Ubuntu版本16.04
        MySQL版本5.7.29

    浏览器测试环境
        Windows、Linux均可
        Chrome
        FireFox
        其他浏览器暂无测试

    测试前确认已安装MySQL数据库

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

    修改main.cpp中的数据库初始化信息

    //数据库登录名,密码,库名
    string user = "root";
    string passwd = "root";
    string databasename = "yourdb";

    build

    sh ./build.sh

    启动server

    ./server

    浏览器端

    ip:9006

个性化运行

./server [-p port] [-l LOGWrite] [-m TRIGMode] [-o OPT_LINGER] [-s sql_num] [-t thread_num] [-c close_log] [-a actor_model]

温馨提示:以上参数不是非必须，不用全部使用，根据个人情况搭配选用即可.

    -p，自定义端口号
        默认9006
    -l，选择日志写入方式，默认同步写入
        0，同步写入
        1，异步写入
    -m，listenfd和connfd的模式组合，默认使用LT + LT
        0，表示使用LT + LT
        1，表示使用LT + ET
        2，表示使用ET + LT
        3，表示使用ET + ET
    -o，优雅关闭连接，默认不使用
        0，不使用
        1，使用
    -s，数据库连接数量
        默认为8
    -t，线程数量
        默认为8
    -c，关闭日志，默认打开
        0，打开日志
        1，关闭日志
    -a，选择反应堆模型，默认Proactor
        0，Proactor模型
        1，Reactor模型

测试示例命令与含义

./server -p 9007 -l 1 -m 0 -o 1 -s 10 -t 10 -c 1 -a 1

    端口9007
    异步写入日志
    使用LT + LT组合
    使用优雅关闭连接
    数据库连接池内有10条连接
    线程池内有10条线程
    关闭日志
    Reactor反应堆模型
致谢

Linux高性能服务器编程，游双著.
