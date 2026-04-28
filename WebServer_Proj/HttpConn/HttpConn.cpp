#include "HttpConn.h"
#include "MySQL_Pool/MySQL_Pool.h"

int HttpConn::m_epollfd    = -1;
int HttpConn::m_user_count = 0;

//构造函数与析构函数
HttpConn::HttpConn()
    : m_sockfd(-1)
    , m_check_state(CHECK_STATE_REQUESTLINE)
    , m_checked_idx(0)
    , m_content_length(0)
    , m_keep_alive(false)
    , m_mmap_addr(nullptr)
    , m_iv_count(0)
    , m_bytes_to_send(0)
    , m_bytes_have_send(0)
{
    memset(&m_addr, 0, sizeof(m_addr));
    memset(&m_file_stat, 0, sizeof(m_file_stat));
    memset(m_iv, 0, sizeof(m_iv));
}

HttpConn::~HttpConn() {
    close_conn(true);
}

int HttpConn::set_nonblocking(int fd) {
    int old_flag = fcntl(fd, F_GETFL, 0);
    int new_flag = old_flag | O_NONBLOCK;
    fcntl(fd, F_SETFL, new_flag);
    return old_flag;
}

const char* HttpConn::get_ip() const {
    static char ip[INET_ADDRSTRLEN];
    inet_ntop(AF_INET, &m_addr.sin_addr, ip, sizeof(ip));
    return ip;
}

int HttpConn::get_port() const {
    return ntohs(m_addr.sin_port);
}

//init()：接纳一个新连接
//init()的特殊之处：需要用到epoll_ctl操作，但是m_sockfd和m_addr都是httpconn持有的内容
void HttpConn::init(int sockfd, const sockaddr_in& addr) {
    m_sockfd = sockfd;
    m_addr = addr;
    set_nonblocking(m_sockfd);
    // 注册到 epoll（使用共享的静态 epollfd）
    struct epoll_event ev;
    ev.data.fd = m_sockfd;
    ev.events  = EPOLLIN | EPOLLET | EPOLLRDHUP | EPOLLONESHOT;
    epoll_ctl(m_epollfd, EPOLL_CTL_ADD, m_sockfd, &ev);

    //重置所有状态（用于长连接复用）
    reset();
    m_user_count++;
}

//reset()长连接重置
void HttpConn::reset() {
    m_read_buf.clear();
    m_write_buf.clear();
    m_check_state   = CHECK_STATE_REQUESTLINE;
    m_checked_idx   = 0;
    m_content_length = 0;
    m_keep_alive    = false;
    m_method.clear();
    m_url.clear();
    m_version.clear();
    m_post_data.clear();
    m_bytes_to_send  = 0;
    m_bytes_have_send = 0;
    m_iv_count = 0;
    memset(m_iv, 0, sizeof(m_iv));

    //m_mmap_addr的释放独立到unmap()中
}

// close_conn():统一关闭接口
void HttpConn::close_conn(bool real_close) {
    if (real_close && m_sockfd != -1) {
        epoll_ctl(m_epollfd, EPOLL_CTL_DEL, m_sockfd, nullptr);
        close(m_sockfd);
        m_sockfd = -1;
        m_user_count--;
    }
    unmap();  //释放mmap（不论是否 real_close，都要释放）
}

//unmap():释放内存映射
void HttpConn::unmap() {
    if (m_mmap_addr) {
        munmap(m_mmap_addr, m_file_stat.st_size);
        m_mmap_addr = nullptr;
    }
}

// read():循环读取
bool HttpConn::read() {
    //ET 模式：必须循环读直到 EAGAIN，否则可能丢失事件
    while (true) {
        char buf[4096];
        int byte_read = recv(m_sockfd, buf, sizeof(buf) - 1, 0);

        if (byte_read > 0) {
            // 追加到读缓冲区
            buf[byte_read] = '\0';
            m_read_buf += buf;
        }
        //客户端自己断连了，没读着，返回false
        else if (byte_read == 0) {
            return false;
        }
        //没读着的情况:读完了/错误了
        else { 
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                //没读着，不过是因为已经读完了，可以准备把任务丢到线程池里面找人干了
                break;
            }
            //其他错误
            return false;
        }
    }
    //保证读到了数据
    return !m_read_buf.empty();
}

//ET模式循环写入、
//优化：当写缓冲区已满的时候，原来的代码不会再继续写
bool HttpConn::write() {
    // 还有数据没发完
    if (m_bytes_to_send == 0) {
        return true;  //没有要发的，视为完成
    }

    while (true) {
        ssize_t bytes_written = writev(m_sockfd, m_iv, m_iv_count);

        if (bytes_written > 0) {
            m_bytes_have_send += bytes_written;

            //优化：更新iovec：跳过已发送的部分
            //头发完了体没发完
            while (m_iv_count > 0 && static_cast<size_t>(bytes_written) >= m_iv[0].iov_len) {
                bytes_written -= m_iv[0].iov_len;
                m_iv_count--;
                //修复：考虑只有一个响应头的情况
                if (m_iv_count > 0) {
                    m_iv[0] = m_iv[1];  //修复bug:当聚合写的内存段只有一段的时候原代码会越界读（不存在m_iv[1]，里面全是脏数据）
                }
            }
            //最后一个 iov 部分发送
            if (m_iv_count > 0 && bytes_written > 0) {
                m_iv[0].iov_base = (char*)m_iv[0].iov_base + bytes_written;
                m_iv[0].iov_len  -= bytes_written;
            }

            // 全部发完
            if (m_bytes_have_send >= m_bytes_to_send) {
                unmap();               //可以释放mmap了！
                m_bytes_to_send  = 0;
                m_bytes_have_send = 0;
                return true;
            }
        }
        else if (bytes_written < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                // 发送缓冲区满，回去重新注册事件，等待下次 EPOLLOUT
                return false;
            }
            //真正的错误
            unmap();
            return false;
        }
    }
}

//process()：线程池任务入口
void HttpConn::process() {
    //解析HTTP请求
    HTTP_CODE code = parse_request();

    if (code == NO_REQUEST) {
        //数据不完整，重新注册读事件，等待更多数据
        struct epoll_event ev;
        ev.data.fd = m_sockfd;
        ev.events  = EPOLLIN | EPOLLET | EPOLLRDHUP | EPOLLONESHOT;
        epoll_ctl(m_epollfd, EPOLL_CTL_MOD, m_sockfd, &ev);
        return;
    }

    if (code == BAD_REQUEST) {
        send_error(400, "Bad Request");
        struct epoll_event ev;
        ev.data.fd = m_sockfd;
        //注意：此处要改成EPOLLOUT，准备把400的响应发给客户端
        ev.events  = EPOLLOUT | EPOLLET | EPOLLRDHUP | EPOLLONESHOT;
        epoll_ctl(m_epollfd, EPOLL_CTL_MOD, m_sockfd, &ev);
        return;
    }
    //生成响应
    make_response();
    //等待发送：切换到EPOLLOUT，等主线程调用write()
    struct epoll_event ev;
    ev.data.fd = m_sockfd;
    ev.events  = EPOLLOUT | EPOLLET | EPOLLRDHUP | EPOLLONESHOT;
    epoll_ctl(m_epollfd, EPOLL_CTL_MOD, m_sockfd, &ev);
    //write()这个方法由谁来调动非常重要，是在这里线程池里直接write了，还是这里先不write，让主线程统一write？这反映的是不同架构的区别
}


// get_line() 从读缓冲区切出一行（从状态机逻辑）
bool HttpConn::get_line(std::string& line) {
    size_t pos = m_read_buf.find("\r\n", m_checked_idx);
    if (pos == std::string::npos) {
        return false;
    }
    line = m_read_buf.substr(m_checked_idx, pos - m_checked_idx);
    m_checked_idx = pos + 2;  // 跳过 \r\n
    return true;
}

// parse_request()：主状态机
HTTP_CODE HttpConn::parse_request() {
    std::string line;

    while (get_line(line) || m_check_state == CHECK_STATE_CONTENT) {

        switch (m_check_state) {
            //请求行
            case CHECK_STATE_REQUESTLINE: {
                std::stringstream ss(line);
                ss >> m_method >> m_url >> m_version;
                if (m_method.empty() || m_url.empty()) {
                    return BAD_REQUEST;
                }
                m_check_state = CHECK_STATE_HEADER;
                break;
            }
            //请求头
            case CHECK_STATE_HEADER: {
                if (line.empty()) {
                    //空行则请求头部分结束
                    if (m_content_length == 0) {
                        return GET_REQUEST;//GET类，读完了
                    }
                    //有content，不是GET类
                    m_check_state = CHECK_STATE_CONTENT;
                    break;
                }
                //解析Connection
                if (line.find("Connection:") != std::string::npos) {
                    if (line.find("keep-alive") != std::string::npos) {
                        m_keep_alive = true;
                    }
                }
                //解析Content-Length
                if (line.find("Content-Length:") != std::string::npos) {
                    m_content_length = std::stoi(line.substr(15));
                }
                break;
            }

            //请求体
            case CHECK_STATE_CONTENT: {
                if (m_read_buf.size() - m_checked_idx >= m_content_length) {
                    m_post_data = m_read_buf.substr(m_checked_idx, m_content_length);
                    return GET_REQUEST;  //POST请求体完整接收
                }
                return NO_REQUEST;  //请求体还没收全
            }

            default:
                return INTERNAL_ERROR;
        }
    }
    return NO_REQUEST;
}


//提取用户名/密码
bool HttpConn::parse_post_data(std::string& user, std::string& pwd) {
    size_t user_pos = m_post_data.find("user=");
    size_t pwd_pos  = m_post_data.find("&password=");

    if (user_pos == std::string::npos || pwd_pos == std::string::npos) {
        return false;
    }
    user = m_post_data.substr(user_pos + 5, pwd_pos - (user_pos + 5));
    pwd  = m_post_data.substr(pwd_pos + 10);
    return true;
}


//check_login() 数据库验证
bool HttpConn::check_login(const std::string& user, const std::string& pwd) {
    ConnGuard guard; //修复:使用Connguard保证连接安全
    MYSQL* conn = guard.conn();
    if(!guard) return false; //退出作用域自动析构

    char sql[1024];
    snprintf(sql, sizeof(sql),
             "SELECT * FROM user WHERE username='%s' AND password='%s'",
             user.c_str(), pwd.c_str());

    if (mysql_query(conn, sql)) {
        return false;//析构
    }

    MYSQL_RES* res = mysql_store_result(conn);
    bool success = (mysql_num_rows(res) > 0);
    mysql_free_result(res);
    return success; //同样析构
}
//使用连接守卫，保证在每个return离开都会调用return_conn

// make_response() 响应分发器
void HttpConn::make_response() {
    //登录请求
    if (m_method == "POST" && m_url == "/login") {
        std::string user, pwd;
        bool if_parse = parse_post_data(user, pwd);
        bool if_login = if_parse ? check_login(user, pwd) : false;
        send_login_response(if_login);
        return;
    }

    //不是登录请求，那就是发静态文件
    serve_static_file();
}


//serve_static_file()：零拷贝静态文件服务
void HttpConn::serve_static_file() {
    //默认首页
    if (m_url == "/") {
        m_url = "/index.html";
    }

    std::string filepath = "./resources" + m_url;

    //检查文件是否存在
    if (stat(filepath.c_str(), &m_file_stat) < 0) {
        send_error(404, "Not Found");
        return;
    }

    //构建响应头
    m_write_buf  = "HTTP/1.1 200 OK\r\n";

    //根据文件后缀设置Content-Type
    if (m_url.find(".jpg") != std::string::npos ||
        m_url.find(".png") != std::string::npos) {
        m_write_buf += "Content-Type: image/jpeg\r\n";
    } else {
        m_write_buf += "Content-Type: text/html; charset=utf-8\r\n";
    }
    //keep-alive/close
    if (m_keep_alive) {
        m_write_buf += "Connection: keep-alive\r\n";
    } else {
        m_write_buf += "Connection: close\r\n";
    }

    m_write_buf += "Content-Length: " +
                   std::to_string(m_file_stat.st_size) + "\r\n\r\n";

    //mmap内存映射
    int src_fd = open(filepath.c_str(), O_RDONLY);
    m_mmap_addr = (char*)mmap(nullptr, m_file_stat.st_size,
                              PROT_READ, MAP_PRIVATE, src_fd, 0);
    close(src_fd);  //mmap后立即关闭，不影响映射，且能节省资源

    //设置writev的iovec：一个是响应头，一个是发出的文件
    m_iv[0].iov_base = (void*)m_write_buf.data();
    m_iv[0].iov_len  = m_write_buf.size();
    m_iv[1].iov_base = m_mmap_addr;
    m_iv[1].iov_len  = m_file_stat.st_size;
    m_iv_count       = 2;
    m_bytes_to_send  = m_write_buf.size() + m_file_stat.st_size;
    m_bytes_have_send = 0;
}

//send_error()：不同连接可能产生不同错误，提供统一接口封装响应逻辑，将所有的发送准备做好
void HttpConn::send_error(int code, const char* msg) {
    std::string body = "<html><body><h1>" + std::to_string(code) +
                       " " + msg + "</h1></body></html>";

    m_write_buf  = "HTTP/1.1 " + std::to_string(code) + " " + msg + "\r\n";
    m_write_buf += "Content-Type: text/html; charset=utf-8\r\n";
    m_write_buf += "Content-Length: " + std::to_string(body.size()) + "\r\n";
    m_write_buf += "Connection: close\r\n\r\n";
    m_write_buf += body;

    //做好发送准备，只等一个EPOLLOUT
    m_iv[0].iov_base = (void*)m_write_buf.data();
    m_iv[0].iov_len  = m_write_buf.size();
    m_iv_count       = 1;
    m_bytes_to_send  = m_write_buf.size();
    m_bytes_have_send = 0;
    m_mmap_addr      = nullptr;  //错误响应不使用 mmap
    m_keep_alive     = false;    //出错直接关闭
}


void HttpConn::send_login_response(bool success) {
    std::string html;
    if (success) {
        html = "<html><head><meta charset='UTF-8'></head>"
               "<body style='text-align:center; padding-top:100px; font-family:Arial;'>"
               "<h1 style='color:green;'>✅ 登录成功！账号密码完全正确！</h1>"
               "<p>MySQL 数据库验证通过！</p>"
               "<a href='/index.html' style='font-size:20px;'>🔙 返回首页</a>"
               "</body></html>";
        m_keep_alive = true;
    } else {
        html = "<html><head><meta charset='UTF-8'></head>"
               "<body style='text-align:center; padding-top:100px; font-family:Arial;'>"
               "<h1 style='color:red;'>❌ 登录失败！</h1>"
               "<p>账号或密码与 MySQL 数据库不匹配，请重试。</p>"
               "<a href='/login.html' style='font-size:20px;'>🔄 重新登录</a>"
               "</body></html>";
        m_keep_alive = true;  //失败也保持连接，让用户重试
    }

    m_write_buf  = "HTTP/1.1 200 OK\r\n";
    m_write_buf += "Content-Type: text/html; charset=utf-8\r\n";
    m_write_buf += "Content-Length: " + std::to_string(html.size()) + "\r\n";
    if (m_keep_alive) {
        m_write_buf += "Connection: keep-alive\r\n";
    } else {
        m_write_buf += "Connection: close\r\n";
    }
    m_write_buf += "\r\n";
    m_write_buf += html;

    m_iv[0].iov_base = (void*)m_write_buf.data();
    m_iv[0].iov_len  = m_write_buf.size();
    m_iv_count       = 1;
    m_bytes_to_send  = m_write_buf.size();
    m_bytes_have_send = 0;
    m_mmap_addr      = nullptr;
}