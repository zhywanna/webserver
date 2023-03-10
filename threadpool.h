#ifndef THREADPOOL_H
#define THREADPOOL_H

#include <pthread.h>
#include <list>
#include "locker.h"
#include <exception>
#include <cstdio>


// 线程池+工作队列 T是任务类
template<class T>
class threadpool {
public:
    threadpool(int thread_num = 8, int max_requests = 10000) :
    m_thread_num(thread_num), m_max_requests(max_requests),
    m_stop(false), m_threads(nullptr) {
        if (m_thread_num <= 0 || m_max_requests <= 0) {
            throw std::exception();
        }

        m_threads = new pthread_t[m_thread_num];
        if (!m_threads) throw std::exception();

        // 创建线程并设置线程分离
        for (int i = 0; i < m_thread_num; ++i) {
            printf("creating %dth thread\n", i);
            if (pthread_create(&m_threads[i], nullptr, worker, this)) {
                delete [] m_threads;
                throw std::exception();
            }
            
            // 主线程与子线程分离，子线程结束时资源自动回收，不产生僵尸线程
            if (pthread_detach(m_threads[i])) {
                delete [] m_threads;
                throw std::exception();
            }

        }

    }

    ~threadpool() {
        delete [] m_threads;
        m_stop = true;
    }

    // 添加任务
    bool append(T* request) {
        m_queuelocker.lock();
        if (m_workqueue.size() > m_max_requests) {
            m_queuelocker.unlock();
            return false;
        }

        m_workqueue.push_back(request);
        m_queuelocker.unlock();
        m_queuestat.post(); // 信号量增加
        return true;
    }

private:
    static void* worker(void* arg) {
        // 在pthread_create时和worker一起传递的arg是当前对象的this指针
        threadpool* pool = (threadpool*) arg;
        pool->run();
        return pool;
    }

    void run() {
        while (!m_stop) {
            // 将信号量-1 如果 < 0 就阻塞，初始状态下线程都阻塞在这个位置
            m_queuestat.wait();

            // 到这里说明队列中有需要处理的任务，否则会阻塞在wait处
            m_queuelocker.lock();
            if (m_workqueue.empty()) {
                // 再加强一下判断，实际可能是没必要的
                m_queuelocker.unlock();
                continue;
            }

            T* request = m_workqueue.front();
            m_workqueue.pop_front();
            m_queuelocker.unlock();

            if (request) {
                request->process();
            }
        }
    }

private:
    // 线程池数量
    int m_thread_num;

    // 线程池数组，大小为m_thread_num
    pthread_t* m_threads;

    // 请求队列最多允许等待的数量
    int m_max_requests;

    // 请求队列
    std::list<T*> m_workqueue;

    // 互斥锁
    locker m_queuelocker;

    // 信号量用来判断请求队列中是否有任务需要处理
    sem m_queuestat;

    // 是否结束线程
    bool m_stop;    
};


#endif