#include "WebServer.h"
#include <cstring>
#include <iostream>
#include <cassert>

//构造/析构
WebServer::WebServer(int port, int thread_num)
    : port_(port)
    , listen_fd_(-1)
    , is_running_(false)
    , epoller_(MAX_EVENTS)         // Epoller 最多返回 1024 个事件
    , thread_pool_(thread_num)     // 创建线程池
    , users_(new HttpConn[MAX_FD]) // 堆分配连接数组
{
    //关键步骤：把epollfd注入到HttpConn的静态成员
    HttpConn::m_epollfd = epoller_.epoll_fd();
    // 初始化信号处理
    init_signal_();
}

WebServer::~WebServer() {
    stop();
    if (listen_fd_ >= 0) {
        close(listen_fd_);
    }
    delete[] users_;
}


//忽略SIGPIPE：前置知识，向已断连的客户端发送消息会产生默认导致服务端崩溃的SIGPIPE信号，专门设置忽略
void WebServer::init_signal_() {
    signal(SIGPIPE, SIG_IGN);
}

//listen socket初始化
bool WebServer::init_socket_() {
    // ---- 1. socket ----
    listen_fd_ = socket(PF_INET, SOCK_STREAM, 0);
    if (listen_fd_ < 0) {
        perror("WebServer: socket() failed");
        return false;
    }

    //设为非阻塞（ET 模式要求），复用HttpConn的静态工具方法
    HttpConn::set_nonblocking(listen_fd_);

    //SO_REUSEADDR：重启后立即复用端口，避开TIME_WAIT
    int opt = 1;
    setsockopt(listen_fd_, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    //bind
    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family      = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    addr.sin_port        = htons(port_);
    if (bind(listen_fd_, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
        perror("WebServer: bind() failed");
        close(listen_fd_);
        return false;
    }

    //listen
    if (listen(listen_fd_, 5) < 0) {
        perror("WebServer: listen() failed");
        close(listen_fd_);
        return false;
    }
    
    //注册到 Epoller:注意要反复用，不能EPOLLONESHOT
    epoller_.add_fd(listen_fd_, EPOLLIN | EPOLLET | EPOLLRDHUP);
    std::cout << "[WebServer] Listening on port " << port_
              << " (Reactor Mode, " << "ET + ThreadPool)" << std::endl;
    return true;
}


// start()：真正的事件循环
//对应原代码的while(true)
void WebServer::start() {
    if (!init_socket_()) {
        std::cerr << "[WebServer] Socket initialization failed!" << std::endl;
        return;
    }

    is_running_ = true;
    std::cout << "[WebServer] Reactor event loop started." << std::endl;

    //REACTOR主循环
    while (is_running_) {
        int n = epoller_.wait(-1);  //阻塞等待事件

        if (n < 0) {
            if (errno == EINTR) continue;  //被信号中断，重试
            perror("WebServer: epoll_wait error");
            break;
        }

        //遍历所有就绪事件
        for (int i = 0; i < n; i++) {
            int fd = epoller_.get_event_fd(i);
            uint32_t events = epoller_.get_events(i);

            //新连接
            if (fd == listen_fd_) {
                handle_listen_();
                continue;
            }

            //连接异常（对端关闭/RST/错误）
            if (events & (EPOLLRDHUP | EPOLLHUP | EPOLLERR)) {
                handle_close_(fd);
                continue;
            }

            //数据可读
            if (events & EPOLLIN) {
                handle_read_(fd);
                continue;
            }
            //可写（响应已生成，等待发送）
            if (events & EPOLLOUT) {
                handle_write_(fd);
                continue;
            }
        }
    }

    std::cout << "[WebServer] Reactor event loop stopped." << std::endl;
}


void WebServer::stop() {
    is_running_ = false;
    //线程池会在析构时自动等待所有任务完成
}

//handle_listen_()：接受所有新连接，ET模式循环accept
void WebServer::handle_listen_() {
    while (true) {
        struct sockaddr_in client_addr;
        socklen_t client_addr_len = sizeof(client_addr);
        int client_fd = accept(listen_fd_,
                               (struct sockaddr*)&client_addr,
                               &client_addr_len);

        if (client_fd < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                //没有更多连接了，退出循环
                break;
            }
            perror("WebServer: accept() error");
            break;
        }
        //交给HttpConn初始化
        add_client_(client_fd, client_addr);
    }
}

//交给HttpConn初始化
void WebServer::add_client_(int fd, const sockaddr_in& addr) {
    if (fd >= MAX_FD) {
        std::cerr << "[WebServer] Too many connections, close fd: " << fd << std::endl;
        close(fd);
        return;
    }

    users_[fd].init(fd, addr);

    std::cout << "[WebServer] New connection: fd=" << fd
              << " IP=" << users_[fd].get_ip()
              << " Port=" << users_[fd].get_port()
              << " Total=" << HttpConn::m_user_count << std::endl;
}


void WebServer::handle_read_(int fd) {
    HttpConn& conn = users_[fd];

    //从socket读数据到m_read_buf（由主线程完成！）
    if (!conn.read()) {
        //read() 返回 false → 对端关闭或出错
        handle_close_(fd);
        return;
    }

    //读到数据了，把CPU密集的解析+响应生成提交到线程池
    thread_pool_.submit([this, fd]() {
        users_[fd].process();
    });
}

void WebServer::handle_write_(int fd) {
    HttpConn& conn = users_[fd];
    //特殊性：write由主线程完成，目前的文件大小不足以阻塞
    if (conn.write()) {
        //发送完成
        if (conn.is_keep_alive()) {
            //是长连接！重置状态，继续监听下一个请求
            conn.reset();
            epoller_.mod_fd(fd, EPOLLIN | EPOLLET | EPOLLRDHUP | EPOLLONESHOT);
        } else {
            //短连接，直接关了
            handle_close_(fd);
        }
    } else {
        //还没发完重新注册
        epoller_.mod_fd(fd, EPOLLOUT | EPOLLET | EPOLLRDHUP | EPOLLONESHOT);
    }
}

//handle_close_()关闭连接
void WebServer::handle_close_(int fd) {
    std::cout << "[WebServer] Closing fd: " << fd << std::endl;
    users_[fd].close_conn(true);
}