#ifndef WEBSERVER_H
#define WEBSERVER_H

#include <netinet/in.h>
#include <arpa/inet.h>
#include <sys/socket.h>
#include <signal.h>
#include <vector>
#include <memory>
#include <string>
#include <atomic>
#include "../Thread_Pool/Thread_Pool.h"
#include "../Epoller/Epoller.h"
#include "../HttpConn/HttpConn.h"

class WebServer {
public:
    //构造/析构
    WebServer(int port, int thread_num);
    ~WebServer();

    //禁止拷贝
    WebServer(const WebServer&) = delete;
    WebServer& operator=(const WebServer&) = delete;

    void start();//运行事件循环
    void stop();//关闭

private:
    //初始化
    bool init_socket_(); //socket() + bind() + listen()
    bool init_threadpool_();//创建线程池
    void init_signal_();//SIGPIPE

    //事件处理
    void handle_listen_();         //处理新连接
    void handle_read_(int fd);     //处理读事件
    void handle_write_(int fd);    //处理写事件
    void handle_close_(int fd);    //处理关闭/错误

    //连接管理
    void add_client_(int fd, const sockaddr_in& addr);
    void close_client_(int fd);

    //成员变量
    int  port_;
    int  listen_fd_;
    std::atomic<bool> is_running_; //atomic，防止竞争乱改

    //核心组件，生命周期由WebServer管理
    Epoller    epoller_;
    ThreadPool thread_pool_;

    //堆分配连接数组，防止栈溢出
    static const int MAX_FD = 65536;
    HttpConn* users_; 

    static const int MAX_EVENTS = 1024;
};

#endif