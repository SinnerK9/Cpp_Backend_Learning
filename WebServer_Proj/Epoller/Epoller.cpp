#include "Epoller.h"
#include <unistd.h>
#include <iostream>

// 构造函数
Epoller::Epoller(int max_events)
    : epoll_fd_(epoll_create1(0))
    , events_(max_events)
{
    if (epoll_fd_ < 0) {
        perror("Epoller: epoll_create1 failed");
        throw std::runtime_error("Epoller initialization failed");
    }
}

//析构函数
Epoller::~Epoller() {
    if (epoll_fd_ >= 0) {
        close(epoll_fd_);
    }
}


//add_fd:用于注册新fd，封装EPOLL_CTL_ADD
bool Epoller::add_fd(int fd, uint32_t events) {
    struct epoll_event ev;
    ev.data.fd = fd;
    ev.events  = events;
    return epoll_ctl(epoll_fd_, EPOLL_CTL_ADD, fd, &ev) == 0;
}


//mod_fd():修改已注册fd的监听事件（主要用来重置EPOLLONESHOT）
bool Epoller::mod_fd(int fd, uint32_t events) {
    struct epoll_event ev;
    ev.data.fd = fd;
    ev.events  = events;
    return epoll_ctl(epoll_fd_, EPOLL_CTL_MOD, fd, &ev) == 0;
}

//del_fd()：从epoll注销fd，一般是连接关闭了
bool Epoller::del_fd(int fd) {
    return epoll_ctl(epoll_fd_, EPOLL_CTL_DEL, fd, nullptr) == 0;
}


//wait()：等待事件
//优化：更加严谨，考虑errno为EINTR（信号打断）这种不是错误的错误
int Epoller::wait(int timeout_ms) {
    int n = epoll_wait(epoll_fd_,
                       events_.data(),
                       static_cast<int>(events_.size()),
                       timeout_ms);
    if (n < 0 && errno != EINTR) {
        perror("Epoller: epoll_wait failed");
    }
    return n;
}

// 查询接口
int Epoller::get_event_fd(size_t i) const {
    assert(i < events_.size() && "Epoller: event index out of range");
    return events_[i].data.fd;
}

uint32_t Epoller::get_events(size_t i) const {
    assert(i < events_.size() && "Epoller: event index out of range");
    return events_[i].events;
}