#ifndef EPOLLER_H
#define EPOLLER_H

#include <sys/epoll.h>
#include <vector>
#include <cassert>
#include <cerrno>


class Epoller {
public:
    //Epoller构造函数：对应原代码中注册内核时间表的部分！
    explicit Epoller(int max_events = 1024);
    ~Epoller();

    //禁止拷贝（epollfd 是独占资源）
    Epoller(const Epoller&) = delete;
    Epoller& operator=(const Epoller&) = delete;

    bool add_fd(int fd, uint32_t events);   // EPOLL_CTL_ADD
    bool mod_fd(int fd, uint32_t events);   // EPOLL_CTL_MOD
    bool del_fd(int fd);                    // EPOLL_CTL_DEL

    //传入timeout_ms: 默认为-1 表示永久阻塞，0 表示立即返回，>0 表示超时毫秒
    int wait(int timeout_ms = -1);

    //新接口：查询就绪事件
    int      get_event_fd(size_t i)   const; //第i个就绪 fd
    uint32_t get_events(size_t i)     const; //第i个就绪事件类型

    //暴露epollfd，供HttpConn静态成员初始化
    int epoll_fd() const { return epoll_fd_; }

private:
    int epoll_fd_;//epoll 实例
    std::vector<struct epoll_event> events_;//就绪事件数组关键优化：采用vector，保证内存安全，并解放1024个限制
};

#endif