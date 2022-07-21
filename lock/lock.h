#ifndef LOCKER_H
#define LOCKER_H

#include<pthread.h>
#include<exception>
#include<semaphore.h>
//线程同步机制封装类

//互斥锁类
class locker
{
private:
    pthread_mutex_t m_mutex;
public:
    locker(){
        // pthread_mutex_init函数用于初始化互斥锁
        if (pthread_mutex_init(&m_mutex,NULL)!=0)
        {
            throw std::exception();
        }
    }

    ~locker(){
        // pthread_mutex_destory函数用于销毁互斥锁
        pthread_mutex_destroy(&m_mutex);
    }

    bool lock(){  // 上锁
        // pthread_mutex_lock函数以原子操作方式给互斥锁加锁
        return pthread_mutex_lock(&m_mutex)==0;
    }

    bool unlock(){ // 解锁
        // pthread_mutex_unlock函数以原子操作方式给互斥锁解锁
        return pthread_mutex_unlock(&m_mutex)==0;
    }
    
    pthread_mutex_t* get(){
        return &m_mutex;
    }
};

// 条件变量类
// 条件变量提供了一种线程间的通知机制,当某个共享数据达到某个值时,唤醒等待这个共享数据的线程
class cond{
public:
    cond(){
        // pthread_cond_init函数用于初始化条件变量
        if(pthread_cond_init(&m_cond,NULL)!=0){
            throw std::exception();
        }
    }
    ~cond(){
        // pthread_cond_destory函数销毁条件变量
        pthread_cond_destroy(&m_cond);
    }
    bool wait(pthread_mutex_t *m_mutex){
        /* pthread_cond_wait函数用于等待目标条件变量.该函数调用时需要传入 mutex参数(加锁的互斥锁) ,
           函数执行时,先把调用线程放入条件变量的请求队列,然后将互斥锁mutex解锁,当函数成功返回为0时,互斥锁会再次被锁上 */
        int ret = 0;
        //pthread_mutex_lock(&m_mutex);
        ret = pthread_cond_wait(&m_cond, m_mutex);
        //pthread_mutex_unlock(&m_mutex);
        return ret == 0;
    }
    bool timewait(pthread_mutex_t *m_mutex,struct timespec t){//超时时间
        int ret = 0;
        //pthread_mutex_lock(&m_mutex);
        ret = pthread_cond_timedwait(&m_cond, m_mutex, &t);
        //pthread_mutex_unlock(&m_mutex);
        return ret == 0;
    }
    bool signal(){ // 条件变量增加,一个或者多个线程唤醒
        return pthread_cond_signal(&m_cond)==0;
    }
    bool broadcast(){  // 将所有的线程都唤醒
        // pthread_cond_broadcast函数以广播的方式唤醒所有等待目标条件变量的线程
        return pthread_cond_broadcast(&m_cond)==0;
    }
private:
    pthread_cond_t m_cond;
};

//信号量类
class sem{
private:
    sem_t m_sem;
public:
    sem(){
        if(sem_init(&m_sem,0,0) != 0){
            throw std::exception();
        }
    }
    sem(int num){
        if(sem_init(&m_sem,0,num) != 0){
            throw std::exception();
        }
    }
    ~sem(){
        // sem_destory函数用于销毁信号量
        sem_destroy(&m_sem);
    }
    bool wait(){   // 等待信号量
        // sem_wait函数将以原子操作方式将信号量减一,信号量为0时,sem_wait阻塞
        return sem_wait(&m_sem)==0;
    }
    bool post(){ // 增加信号量
        // sem_post函数以原子操作方式将信号量加一,信号量大于0时,唤醒调用sem_post的线程
        return sem_post(&m_sem)==0;
    }
};

#endif
