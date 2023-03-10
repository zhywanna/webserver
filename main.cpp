#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <arpa/inet.h> // 包含了socket包
#include <unistd.h>
#include <errno.h>
#include <fcntl.h>
#include <sys/epoll.h>
#include <signal.h>
#include "locker.h"
#include "threadpool.h"
#include "http_conn.h"

#define MAX_FD 65535 // 最大的文件描述符个数,实际可能支持不了这么高的并发
#define MAX_EVENT_NUM 10000 // 同时监听的最大数量

// 添加信号捕捉 
void addsig(int sig, void (*handler)(int)) {
    struct sigaction sa;
    memset(&sa, '\0', sizeof(sa));
    sa.sa_handler = handler; // 
    sigfillset(&sa.sa_mask); // 把所有临时阻塞的信号集全部置为1（把信号全部添加到阻塞信号集中，表示全部阻塞）
    sigaction(sig, &sa, NULL);
}

// 添加文件描述符到epoll
extern void addfd(int epollfd, int fd, bool one_shot);

// 修改文件描述符
extern void modfd(int epollfd, int fd, int ev);

// 删除文件描述符
extern void removefd(int epollfd, int fd);

int main(int argc, char* argv[]) {
    // 主线程

    if (argc <= 1) {
        printf("命令行输入缺少端口号\n");
        exit(-1); // 程序退出并将异常值返回给os
    }

    // 获取端口号 acsii to integer
    int port = atoi(argv[1]);

    // 对sigpipe做处理
    addsig(SIGPIPE, SIG_IGN);

    // 创建线程池 http_connection
    threadpool<http_conn> *pool = nullptr;
    try {
        pool = new threadpool<http_conn>;
    } catch(...) {
        exit(-1);
    }

    // 所有客户端的连接请求
    http_conn* users = new http_conn[MAX_FD]; // 已连接的客户端

    // 创建监听的套接字
    int listenfd = socket(PF_INET, SOCK_STREAM, 0);

    // 设置端口复用
    int reuse = 1;
    setsockopt(listenfd, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));

    // 绑定
    struct sockaddr_in address;
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = INADDR_ANY; // 监听所有网卡
    address.sin_port = htons(port);
    bind(listenfd, (struct sockaddr*)&address, sizeof(address)); // listen套接字将要监听的是这个地址


    // 监听
    listen(listenfd, 5);

    // 创建epoll对象 IO 多路复用
    epoll_event events[MAX_EVENT_NUM]; // ready list
    int epollfd = epoll_create(1); // 任意正数，不影响实际创建多大

    // 将监听的文件描述符添加
    addfd(epollfd, listenfd, false);
    http_conn::m_epollfd = epollfd;

    while (1) {
        int request_num = epoll_wait(epollfd, events, MAX_EVENT_NUM, -1);
        if (errno != EINTR && request_num < 0) { // 不是被中断的
            printf("epoll failed!\n");
            break;
        }

        // 遍历事件数组
        for (int i = 0; i < request_num; i++) {
            int sockfd = events[i].data.fd;
            if (sockfd == listenfd) {
                // listen触发说明有新的客户端连接
                struct sockaddr_in client_address;
                socklen_t client_address_len = sizeof(client_address);
                int connfd = accept(listenfd, (struct sockaddr*)&client_address, &client_address_len);

                if (http_conn::m_user_count >= MAX_FD) {
                    /*
                        TODO:
                        给客户端回写信息：服务器正忙
                    */
                    close(connfd);
                    continue;
                }

                // 将新客户的数据初始化
                users[connfd].init(connfd, client_address);

            } else if (events[i].events & (EPOLLRDHUP | EPOLLHUP | EPOLLERR)) {
                // 对方异常断开等错误事件
                users[sockfd].close_conn();

            } else if (events[i].events & EPOLLIN) {
                if (users[sockfd].read()) {
                    // 一次把数据都读完
                    pool->append(&users[sockfd]);
                } else {
                    users[sockfd].close_conn();
                }
            } else if (events[i].events & EPOLLOUT) {
                if (!users[sockfd].write()) {
                    users[sockfd].close_conn();
                }
            }
        }
    }

    close(epollfd);
    close(listenfd);
    delete [] users; // 释放用户池
    delete pool; // 释放线程池
    return 0;
}