# MyLittleWebserver

> 从零构建的高并发 HTTP 服务器 — C++ 网络编程的完整学习与实践

[![C++17](https://img.shields.io/badge/C%2B%2B-17-blue.svg)](https://en.cppreference.com/w/cpp/17)
[![Linux](https://img.shields.io/badge/Platform-Linux-orange.svg)](https://www.kernel.org/)
[![License](https://img.shields.io/badge/License-MIT-green.svg)](LICENSE)

## 项目简介

**MyLittleWebserver** 是一个基于 **Reactor 模式 + Epoll 边沿触发 + 线程池** 的高性能 HTTP 服务器，参考 [TinyWebServer](https://github.com/qinguoyi/TinyWebServer) 的设计思想，用 C++17 从零构建。

这不是一个课程作业的复制粘贴，而是一份完整的**学习记录**：从 Socket 编程零基础开始，逐步理解每一层抽象的必要性，最终将 1600+ 行工程代码拆分为 7 个独立解耦的模块。`Docs/` 目录下有超过 1800 行的笔记，详细记录了每一步的原理认知、踩坑经历和修复方案。

### 功能特性

| 功能 | 说明 |
|------|------|
| HTTP GET / POST | 主从状态机解析，支持任意 URL 路由 |
| 静态文件服务 | HTML / 图片 / 图标，mmap + writev 零拷贝 |
| 用户登录验证 | MySQL 数据库查询，连接池 + RAII 守卫 |
| Keep-Alive 长连接 | 请求结束后复用 TCP 连接 |
| 连接超时清理 | 升序链表定时器，SIGALRM 信号管道统一事件源 |
| 日志系统 | 单例同步日志，微秒级时间戳，自动刷盘 |

## 架构概览

```
                       ┌──────────────────────────────────┐
                       │           WebServer              │
                       │        (Reactor 主循环)           │
                       │                                  │
        Clients ──▶ listen_fd ──▶ Epoller ──▶ 事件分发    │
                       │              │                   │
                       │     ┌────────┼────────┐          │
                       │     │        │        │          │
                       │  EPOLLIN  EPOLLOUT  Pipe[0]     │
                       │     │        │        │          │
                       │  read()   write()  Timer.tick()  │
                       │     │        │        │          │
                       │  ┌──▼──┐     │   超时 fd[]       │
                       │  │线程池│     │   ▶ close         │
                       │  │process│    │                  │
                       │  └──────┘     │                  │
                       └──────────────────────────────────┘
```

**核心数据流**：主线程通过 epoll 统一监听网络 IO 和定时器管道 → 读事件触发后将 CPU 密集的 HTTP 解析提交给线程池 → 线程池完成业务处理后切换 fd 为 EPOLLOUT → 主线程统一执行数据发送。

## 快速开始

### 环境要求

- **Linux** 操作系统（依赖 epoll、mmap 等 Linux 特有系统调用）
- **g++** 7.0+（需要 C++17 支持）
- **MySQL** 5.7+ 及开发库

### 安装依赖

```bash
sudo apt install g++ make mysql-server libmysqlclient-dev
```

### 配置数据库

```sql
CREATE DATABASE webserver;
USE webserver;
CREATE TABLE user (
    id INT AUTO_INCREMENT PRIMARY KEY,
    username VARCHAR(50) NOT NULL,
    password VARCHAR(50) NOT NULL
);
INSERT INTO user (username, password) VALUES ('admin', '123456');
```

修改 `WebServer_Proj/MySQL_Pool/MySQL_Pool.cpp` 中的连接参数（主机、用户名、密码、数据库名），确保与本地 MySQL 环境一致。

### 编译运行

```bash
cd WebServer_Proj
make        # 编译
./webserver # 启动服务器
```

浏览器访问 `http://localhost:8080/` 即可看到首页。

> 详细的构建指南、测试方法和常见问题见 [Docs/03_Build_and_Deploy.md](Docs/03_Build_and_Deploy.md)

## 模块设计

最终工程 `WebServer_Proj/` 包含 7 个独立模块，总计约 1600 行代码：

| 模块 | 文件 | 代码行数 | 职责 |
|------|------|----------|------|
| **WebServer** | `WebServer/` | ~300 | Reactor 事件循环，统一调度所有模块 |
| **HttpConn** | `HttpConn/` | ~550 | HTTP 主从状态机解析，mmap + writev 响应 |
| **Epoller** | `Epoller/` | ~110 | epoll 系统调用的 RAII 封装 |
| **ThreadPool** | `Thread_Pool/` | ~125 | Header-only，生产者-消费者模型 |
| **MySQL_Pool** | `MySQL_Pool/` | ~190 | 单例连接池 + RAII ConnGuard 守卫 |
| **Timer** | `Timer/` | ~200 | 升序链表定时器，信号管道统一事件源 |
| **Logger** | `Logger/` | ~100 | Meyers Singleton，微秒级时间戳 |

模块依赖单向：`main → WebServer → {Epoller, ThreadPool, Timer, HttpConn} → {MySQL_Pool, Logger}`。下层完全不感知上层。

## 核心技术决策

### Reactor 模式：主线程只做 IO，业务交给线程池

```cpp
// WebServer::handle_read_()
conn.read();                                    // 主线程：尽快读完数据
thread_pool_.submit([this, fd] {
    users_[fd].process();                       // 线程池：CPU 密集的解析与响应
});
```

ET 模式下数据只通知一次 — 如果 read 也交给线程池，调度延迟会直接导致数据丢失。主线程负责"收信"，线程池负责"读信和回信"，边界清晰。

### EPOLLONESHOT：用内核机制替代应用层锁

每个 client_fd 注册时带上 `EPOLLONESHOT`，事件触发一次后自动失效。处理完毕后手动 `mod_fd` 重新激活。保证同一 fd 不会被多个线程并发处理，无需额外加锁。

### mmap + writev：零拷贝文件服务

```cpp
// HttpConn::serve_static_file()
m_mmap_addr = mmap(nullptr, file_size, PROT_READ, MAP_PRIVATE, src_fd, 0);
close(src_fd);                                  // mmap 后可立即关闭 fd
m_iv[0] = {header_buf, header_len};             // 响应头
m_iv[1] = {m_mmap_addr, file_size};             // 响应体（内核直接从页缓存拷贝）
writev(sockfd, m_iv, 2);                        // 一次系统调用，聚合发送
```

用内存映射代替 `read()` 循环读取文件，用 `writev` 聚合发送响应头和响应体 — 将多次系统调用合并为一次。

### 信号管道：异步信号同步化

```
SIGALRM 到达 → sig_handler() → send(pipefd[1], signal)
    ↓
epoll 检测 pipefd[0] 可读 → 主循环 handle_timer_()
    ↓
消费管道数据 → timer.tick() → 返回超时 fd 列表 → 逐个 close
```

信号处理函数只做一件事：向管道写入信号值。定时器事件变成普通的 epoll 读事件，主循环用完全统一的方式处理网络 IO 和定时任务。

### RAII 贯穿始终

锁（`std::lock_guard`）、数据库连接（`ConnGuard`）、内存映射（`unmap()`）、epoll 句柄（`Epoller` 析构）全部由对象生命周期自动管理，禁止拷贝、支持移动，杜绝资源泄漏。

## 项目结构

```
cpp_proj1/
├── Base/                         # 基础练习
│   ├── Mutex/                    #   互斥锁、条件变量、生产者-消费者
│   ├── Thread/                   #   线程创建与 detach/join
│   └── Threadpool/               #   线程池原型（Header-only）
├── Network/                      # TCP 服务器演进
│   ├── TCP_Server.cpp            #   单线程阻塞版本
│   ├── TCP_Server_Thread.cpp     #   多线程（每连接一线程）
│   └── TCP_Server_Threadpool.cpp #   线程池版本
├── IO-Models/                    # IO 多路复用
│   ├── TCP_Server_Select.cpp     #   select 模型
│   ├── TCP_Server_Poll.cpp       #   poll 模型
│   ├── TCP_Server_Epoll.cpp      #   epoll 基础（ET + 非阻塞）
│   └── Webserver_Basic.cpp       #   完整雏形（epoll + 线程池 + HTTP）
├── HTTP-Protocol/                # HTTP 协议处理
│   ├── v1_http_basic.cpp         #   基础 HTTP 请求解析
│   ├── v2_http_parser.cpp        #   主从状态机重构
│   └── v3_http_mysql.cpp         #   MySQL 登录集成
├── Timer/                        # 定时器原型
│   └── timer_test.cpp            #   升序链表定时器验证
├── WebServer_Proj/               # ★ 最终工程项目
│   ├── main.cpp                  #   程序入口
│   ├── Makefile                  #   构建配置（g++ C++17）
│   ├── WebServer/                #   核心调度器（Reactor 事件循环）
│   ├── HttpConn/                 #   HTTP 连接处理
│   ├── Epoller/                  #   epoll RAII 封装
│   ├── Thread_Pool/              #   线程池（Header-only）
│   ├── MySQL_Pool/               #   MySQL 单例连接池
│   ├── Timer/                    #   定时器（升序链表 + 信号管道）
│   ├── Logger/                   #   日志系统（单例同步日志）
│   └── resources/                #   静态资源（html / jpg / ico）
└── Docs/                         # 完整文档
    ├── 00_Project_Overview.md    #   项目总览
    ├── 01_Architecture.md        #   架构设计与关键决策
    ├── 02_Component_Reference.md #   组件 API 参考
    └── 03_Build_and_Deploy.md    #   构建、部署与测试
```

## 学习路径

以下是从 Socket 编程零基础到完成本项目的完整演进过程。每个阶段都有对应的练习代码和详细笔记：

```
Socket 编程基础 (TCP 三次握手/四次挥手, socket/bind/listen/accept)
    ↓
多线程与同步机制 (std::thread, mutex, condition_variable, RAII 锁守卫)
    ↓
线程池 (生产者-消费者模型, std::packaged_task + std::future 异步结果)
    ↓
IO 多路复用 (select → poll → epoll, 理解"同时监听多个 fd"的本质)
    ↓
非阻塞 IO + ET 模式 + EPOLLONESHOT (为什么必须循环读, 为什么需要 ONESHOT)
    ↓
HTTP 协议解析 (请求行/请求头/请求体, 主从状态机, GET vs POST)
    ↓
零拷贝优化 (mmap 内存映射替代 read, writev 聚合写替代多次 write)
    ↓
MySQL 集成 (libmysqlclient, 连接池单例模式, RAII ConnGuard 防泄漏)
    ↓
模块化重构 (7 个独立组件, 单向依赖, 头文件只暴露必要接口)
    ↓
定时器 (升序链表, SIGALRM 信号 + 管道统一事件源, 连接超时自动清理)
    ↓
日志系统 (Meyers Singleton, 微秒级时间戳, 线程安全, 宏接口简化调用)
```

## 参考与致谢

- [TinyWebServer](https://github.com/qinguoyi/TinyWebServer) — 核心参考项目

## License

MIT License
