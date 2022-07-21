#ifndef THREADPOOL_H
#define THREADPOOL_H

#include <list>
#include <cstdio>
#include <exception>
#include <pthread.h>
#include "../lock/locker.h"
#include "../CGImysql/sql_connection_pool.h"

template <typename T>
class threadpool{
public:
    /*thread_number是线程池中线程的数量，max_requests是请求队列中最多允许的、等待处理的请求的数量*/
    threadpool(int actor_model, connection_pool *connPool, int thread_number = 8, int max_request = 10000);
    ~threadpool();
    bool append(T *request, int state);  // 添加任务
    bool append_p(T *request);

private:
    /*工作线程运行的函数，它不断从工作队列中取出任务并执行之*/
    static void *worker(void *arg);
    void run();

private:
    int m_thread_number;        // 线程池中的线程数
    int m_max_requests;         // 请求队列中允许的最大请求数
    pthread_t *m_threads;       // 描述线程池的数组，其大小为m_thread_number
    std::list<T *> m_workqueue; // 请求队列
    locker m_queuelocker;       // 保护请求队列的互斥锁
    sem m_queuestat;            // 信号量用来判断是否有任务需要处理
    
    int m_actor_model;          // 模型切换
    connection_pool *m_connPool;// 数据库
};

template <typename T>
threadpool<T>::threadpool( int actor_model, connection_pool *connPool, int thread_number, int max_requests) : 
        m_actor_model(actor_model),m_thread_number(thread_number), m_max_requests(max_requests), m_threads(NULL),m_connPool(connPool)
{
    if(thread_number <=0 || max_requests <=0 ){
        throw std::exception();
    }

    m_threads = new pthread_t[m_thread_number];  // 创建线程池数组
    if(!m_threads){   // 创建线程池数组失败
        throw std::exception();
    }

    // 创建 thread_number 个线程，并将它们设置为线程脱离（用完自己释放资源）
    for (int i = 0; i < thread_number; i++)
    {
        // printf("create the %dth thread\n",i);
        // worker是一个静态函数，this作为参数传递到静态函数中，用于访问非静态成员变量
        if(pthread_create(m_threads + i,NULL,worker,this) != 0){
            delete[] m_threads;
            throw std::exception();
        }
        // 将线程进行分离后，不用单独对工作线程进行回收
        if(pthread_detach(m_threads[i])){ 
            delete[] m_threads;
            throw std::exception();
        }
    }   
}

template <typename T>
threadpool<T>::~threadpool(){
    delete[] m_threads;
}

template <typename T>
bool threadpool<T>::append(T *request, int state){
    // 操作工作队列时一定要加锁，因为它被所有线程共享。
    m_queuelocker.lock();
    if(m_workqueue.size() >= m_max_requests){
        m_queuelocker.unlock();
        return false;
    }

    request->m_state = state;

    // 添加任务
    m_workqueue.push_back(request);
    m_queuelocker.unlock();

    // 信号量提醒有任务要处理，线程池增加一个，信号量相应的增加
    m_queuestat.post();      
    return true;
}

template <typename T>
bool threadpool<T>::append_p(T *request){
    m_queuelocker.lock();
    if (m_workqueue.size() >= m_max_requests){
        m_queuelocker.unlock();
        return false;
    }
    m_workqueue.push_back(request);
    m_queuelocker.unlock();
    m_queuestat.post();
    return true;
}

template <typename T>
void *threadpool<T>::worker(void *arg){
    threadpool *pool = (threadpool *)arg;
    pool->run();
    return pool;
}

template <typename T>
void threadpool<T>::run(){
    while (true){
        // 等待一个请求队列中待处理的 HTTP 请求，然后交给线程池中的空闲线程来处理
        m_queuestat.wait(); 

        m_queuelocker.lock();
        if(m_workqueue.empty()){
            m_queuelocker.unlock();
            continue;
        }
        // 从请求队列中取出第一个任务
        // 将任务从请求队列删除
        T* request = m_workqueue.front();
        m_workqueue.pop_front();
        m_queuelocker.unlock();
        if(!request){   // 没有获取到
            continue;
        }

        if (1 == m_actor_model){
            if (0 == request->m_state){
                if (request->read_once()){
                    request->improv = 1;
                    connectionRAII mysqlcon(&request->mysql, m_connPool);
                    request->process();
                }else{
                    request->improv = 1;
                    request->timer_flag = 1;
                }
            }else{
                if (request->write()){
                    request->improv = 1;
                }else{
                    request->improv = 1;
                    request->timer_flag = 1;
                }
            }
        }else{
            connectionRAII mysqlcon(&request->mysql, m_connPool);
            request->process();
        }
    }
}

#endif
