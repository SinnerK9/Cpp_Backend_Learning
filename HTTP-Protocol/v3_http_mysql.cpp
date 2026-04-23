#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <cstring>
#include <unistd.h>
#include <iostream>
#include <sys/epoll.h>
#include <fcntl.h> 
#include <errno.h>
#include <signal.h>
#include <thread>
#include <mutex>
#include <condition_variable>
#include <vector>
#include <queue>
#include <atomic>
#include <functional>
#include <future>
#include <sstream>
#include <sys/stat.h>
#include <sys/mman.h>
#include <sys/uio.h>
#include "mysql_pool.h"

using namespace std; 

enum CHECK_STATE { 
    CHECK_STATE_REQUESTLINE = 0, //请求行
    CHECK_STATE_HEADER, //头部
    CHECK_STATE_CONTENT //请求体
};

enum HTTP_CODE {
    NO_REQUEST,
    GET_REQUEST,
    BAD_REQUEST, 
    INTERNAL_ERROR 
};

class threadpool {
private:
    std::mutex mtx;
    std::condition_variable condition;
    std::atomic<bool> stop;
    std::vector<std::thread> workers;
    std::queue<std::function<void()>> tasks;  
    std::atomic<size_t> pending_tasks{0};

public:
    threadpool(size_t num_threads) : stop(false) {
        for (size_t i = 0; i < num_threads; i++) {
            workers.emplace_back([this] {
                while (true) {
                    std::function<void()> task; 
                    {
                        std::unique_lock<std::mutex> lock(mtx);
                        condition.wait(lock, [this] {
                            return stop || !tasks.empty();
                        });
                        if (stop && tasks.empty()) {
                            return;
                        }
                        task = std::move(this->tasks.front());
                        tasks.pop();
                    } 
                    task();
                    pending_tasks--;
                }
            });
        }
    }

    template<typename F, typename ...Args> 
    auto submit(F &&f , Args && ...args) 
    -> std::future<decltype(f(args...))>{   
        using return_type = decltype(f(args...)); 
        auto task = std::make_shared<std::packaged_task<return_type()>>(std::bind(std::forward<F>(f),std::forward<Args>(args)...));
        std::future<return_type> result = task -> get_future();

        {
            std::lock_guard<std::mutex> lock(mtx);
            if(stop){
                std::cout << "线程池已停止，不再提交" << std:: endl;
                throw std::runtime_error("线程池已停止");
            }
        
            tasks.push([task](){
                (*task)();
            });
            pending_tasks++;
        }
        condition.notify_one();
        return result;
    }

    ~threadpool(){
        {
            std::lock_guard<std::mutex> lock(mtx);
            stop = true;
        }
        condition.notify_all();
        while(pending_tasks > 0){
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
        for(auto& worker : workers){
            if(worker.joinable()){
                worker.join();
            }
        }

        std:: cout << "线程池工作结束" << std::endl;
    }
};


struct ClientState {
    int fd;
    std::string buffer;
    CHECK_STATE check_state; 
    int checked_idx; 
    std::string post_data; //增加对POST请求传入内容的存储
    std::string method;
    std::string url;
    std::string version;
    bool keep_alive;
    int content_length;

    void init(int client_fd){
        fd = client_fd;
        buffer.clear();
        check_state = CHECK_STATE_REQUESTLINE;
        checked_idx = 0;
        keep_alive = false;
        content_length = 0;
        method.clear();
        url.clear();
        version.clear();
        post_data.clear();
    }
};
ClientState users[65536];


// 从状态机逻辑实现：字符串切分
bool get_line(ClientState& client, std::string& line) {
    size_t pos = client.buffer.find("\r\n", client.checked_idx);
    if (pos == std::string::npos) {
        return false;
    }
    line = client.buffer.substr(client.checked_idx, pos - client.checked_idx); 
    client.checked_idx = pos + 2; 
    return true;
}

//核心：主状态机逻辑实现
HTTP_CODE parse_http_request(ClientState& client){
    std::string line;
    while( get_line(client,line) || client.check_state == CHECK_STATE_CONTENT){

        switch(client.check_state){
            case CHECK_STATE_REQUESTLINE:{
                std::stringstream ss(line);
                ss >> client.method >> client.url >> client.version;
                if(client.method.empty() || client.url.empty()){
                    return BAD_REQUEST;
                }
                client.check_state = CHECK_STATE_HEADER; 
                break;
            }
            case CHECK_STATE_HEADER:{
                if(line.empty()){
                    if(client.content_length == 0){
                        return GET_REQUEST; 
                    }
                    client.check_state = CHECK_STATE_CONTENT;
                    break;
                }
                if(line.find("Connection:") != std::string::npos){
                    if(line.find("keep-alive") != std::string::npos){
                        client.keep_alive = true;
                    }
                }
                if(line.find("Content-Length:")!= std::string::npos){
                    client.content_length = std::stoi(line.substr(15)); //请求体长度：Content-Length:后面的子字符串转为数字
                }
                break;
            }
            case CHECK_STATE_CONTENT:{ 
                if(client.buffer.size() - client.checked_idx >= client.content_length){
                    client.post_data = client.buffer.substr(client.checked_idx,client.content_length); //更新：增加CONTENT解析逻辑，针对POST类报文
                    return GET_REQUEST;
                }
                return NO_REQUEST; 
            }
            default:
                return INTERNAL_ERROR; 
        }
    }
    return NO_REQUEST; 
}
//新增逻辑：从post_data中解析出真正username和password
//HOST报文这部分的形式是固定的：user=admin&password=123456
bool parse_post_data(ClientState& client,std::string& username,std::string& password){
    size_t user_pos = client.post_data.find("user=");
    size_t pwd_pos = client.post_data.find("&password=");
    if(user_pos == std::string::npos || pwd_pos == std::string::npos){
        return false;
    }
    username = client.post_data.substr(user_pos + 5,pwd_pos - (user_pos + 5));
    password = client.post_data.substr(pwd_pos + 10);
    return true;
}

bool check_login(const std::string& user, const std::string& password){
    //静态对象属于整个类！！
    MySQLPool* pool = MySQLPool::get_instance();
    MYSQL* conn = pool->get_conn();
    if(conn == NULL){
        return false;
    }
    char sql[1024];
    sprintf(sql,"SELECT * FROM user WHERE username='%s' AND password='%s'",user.c_str(),password.c_str());
    //mysql关键API：mysql_query为通过连接执行传入的SQL语句，传入参数为连接和语句，返回值为0表示成功，非0失败
    if(mysql_query(conn,sql)){
        pool->return_conn(conn);//一定要先归还连接再return，否则会泄露
        return false; //返回值非0，执行失败了，回收链接
    }
    //关键API：储存查询结果集的mysql_store_result，传入连接，传出一个指向结果集结构体的指针，包含查询到的所有行数据
    MYSQL_RES* res = mysql_store_result(conn);
    bool success = mysql_num_rows(res) > 0; //宏:mysql_num_rows：查询出来的结果有多少行？>0表示确实查询到了结果
    mysql_free_result(res); //结果没用了，丢掉
    pool->return_conn(conn);//用完了，归还链接
    return success;
}

//更新：根据是否登录是否成功发送动态响应
void send_login_response(int fd,bool success){
    std::string html;
    if(success){
        html = html = "<html><head><meta charset='UTF-8'></head>"
               "<body style='text-align:center; padding-top:100px; font-family:Arial;'>"
               "<h1 style='color:green;'>✅ 登录成功！账号密码完全正确！</h1>"
               "<p>MySQL 数据库验证通过！</p>"
               "<a href='/index.html' style='font-size:20px;'>🔙 返回首页</a>" 
               "</body></html>";
    }else{
        html = "<html><head><meta charset='UTF-8'></head>"
               "<body style='text-align:center; padding-top:100px; font-family:Arial;'>"
               "<h1 style='color:red;'>❌ 登录失败！</h1>"
               "<p>账号或密码与 MySQL 数据库不匹配，请重试。</p>"
               "<a href='/login.html' style='font-size:20px;'>🔄 重新登录</a>"
               "</body></html>";
    }

    std::string header = "HTTP/1.1 200 OK\r\n";
    header += "Content-Type: text/html; charset=utf-8\r\n";
    header += "Content-length:" + std::to_string(html.size()) + "\r\n";
    header += "Connection:keep-alive\r\n\r\n";
    send(fd,(header + html).c_str(),header.size() + html.size(),0);
}

int setnonblocking(int fd){
    int old_flag = fcntl(fd,F_GETFL);
    int new_flag = old_flag | O_NONBLOCK;
    fcntl(fd,F_SETFL,new_flag);
    return old_flag;
}

void handle_client(int fd,int epoll_fd){
    ClientState& client = users[fd];
    HTTP_CODE ret = parse_http_request(client);
    //请求尚不完整，重置EPOLLONESHOT
    if(ret == NO_REQUEST){
        struct epoll_event ev;
        ev.data.fd = fd;
        ev.events = EPOLLIN | EPOLLONESHOT | EPOLLRDHUP |EPOLLET;
        epoll_ctl(epoll_fd,EPOLL_CTL_MOD,fd,&ev);
        return;
    //请求语法有问题，关闭连接
    }else if(ret == BAD_REQUEST){
        std::string response;
        response = "HTTP/1.1 400 Bad Request\r\nContent-Length: " + std::to_string(strlen("Bad Request")) + "\r\n\r\n";
        send(fd,response.c_str(),response.size(),0);
        close(fd);
        return;
    }else if(ret == GET_REQUEST){
        std::cout << "方法为： " << client.method << "  路径为： " << client.url << " 版本为： " << client.version << " 长连接： " << client.keep_alive << std::endl;
        if(client.method == "POST" && client.url == "/login"){
            std::string user;
            std::string password;
            bool if_parse = parse_post_data(client,user,password);
            std::cout << "1. post_data长度: " << client.post_data.size() << std::endl;
            std::cout << "2. post_data内容: [" << client.post_data << "]" << std::endl;
            std::cout << "3. 账号: [" << user << "]" << std::endl;
            std::cout << "4. 密码: [" << password << "]" << std::endl;
            bool if_login = if_parse ? check_login(user, password) : false;
            send_login_response(fd,if_login);
        }else{
            if(client.url == "/"){
                client.url = "/index.html"; 
            }
            std::string filepath = "./resources" + client.url; 
            struct stat file_stat;
            if(stat(filepath.c_str(),&file_stat) < 0){
                std::string body = "<h1>404 Not Found</h1>";
                std::string header = "HTTP/1.1 404 Not Found\r\nContent-Length: " + std::to_string(body.size()) + "\r\n\r\n";
                send(fd,header.c_str(),header.size(),0); 
                send(fd,body.c_str(),body.size(),0);
            }else{
                std::string header = "HTTP/1.1 200 OK\r\n";
                if(client.keep_alive == true){
                    header +=  "Connection: keep-alive\r\n";
                }else{
                    header += "Connection: close\r\n";
                }
                if(client.url.find(".jpg") != std::string::npos || client.url.find(".png") != std::string::npos) {
                    header += "Content-Type: image/jpeg\r\n"; // 图片类型
                } else {
                    header += "Content-Type: text/html; charset=utf-8\r\n"; // 网页类型
                }
                header += "Content-Length: "+ std::to_string(file_stat.st_size) + "\r\n\r\n"; //stat返回=0，找到了
                int src_fd = open(filepath.c_str(),O_RDONLY); 
                char* file_address = (char*) mmap(NULL,file_stat.st_size,PROT_READ,MAP_PRIVATE,src_fd,0);
                close(src_fd); 
                struct iovec iv[2];
                iv[0].iov_base = (void*)header.c_str(); 
                iv[0].iov_len = header.size();
                iv[1].iov_base = file_address;
                iv[1].iov_len = file_stat.st_size;
                writev(fd,iv,2); 
                munmap(file_address, file_stat.st_size); 
            }
        }
        
        if(client.keep_alive){
            client.init(fd);
            struct epoll_event ev;
            ev.data.fd = fd;
            ev.events = EPOLLIN | EPOLLONESHOT | EPOLLRDHUP |EPOLLET;
            epoll_ctl(epoll_fd,EPOLL_CTL_MOD,fd,&ev);
        }else{
            epoll_ctl(epoll_fd,EPOLL_CTL_DEL,fd,NULL);
            close(fd);
        }

    }
}

int main() {
    MySQLPool::get_instance();//新增：先初始化连接池
    signal(SIGPIPE, SIG_IGN); 
    threadpool pool(8);
    int listenfd = socket(PF_INET, SOCK_STREAM, 0);
    if (listenfd < 0) {
        perror("Socket Error!");
        return -1;
    }
    setnonblocking(listenfd);
    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;      
    addr.sin_addr.s_addr = htonl(INADDR_ANY);  
    addr.sin_port = htons(8080);

    int opt = 1;
    setsockopt(listenfd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt)); 
    int ret = bind(listenfd, (struct sockaddr*)&addr, sizeof(addr));
    if (ret < 0) {
        perror("Bind Error!");
        return -1;
    }

    ret = listen(listenfd, 5);
    if (ret < 0) {
        perror("Listen Error!");
        return -1;
    }

    cout << "waiting..." << endl;

    int epoll_fd = epoll_create1(0);
    if(epoll_fd < 0){
        perror("Epoll Error!");
        close(listenfd);
        return -1;
    }

    struct epoll_event ev;
    ev.events = EPOLLIN | EPOLLET | EPOLLRDHUP; 
    ev.data.fd = listenfd; 

    if(epoll_ctl(epoll_fd,EPOLL_CTL_ADD,listenfd,&ev) < 0){
        perror("epoll_ctl:listenfd error");
        close(listenfd);
        return -1;
    }

    struct epoll_event events[1024];

    while(true){ 
        int n = epoll_wait(epoll_fd,events,1024,-1);
        if(n < 0){
            perror("epoll_wait");
            continue;
        }

        for(int i = 0; i < n; i++){
            int curr_fd = events[i].data.fd; 
            if (events[i].events & (EPOLLRDHUP | EPOLLHUP | EPOLLERR)) {
                cout << "fd: " << curr_fd << " 异常断开，已清理" << endl;
                epoll_ctl(epoll_fd, EPOLL_CTL_DEL, curr_fd, NULL); 
                close(curr_fd);                                
                continue;                                       
            }

            if(curr_fd == listenfd){
                while(true){ 
                    sockaddr_in client_addr;
                    socklen_t client_addr_len = sizeof(client_addr);
                    int client_fd = accept(listenfd,(sockaddr*) &client_addr,&client_addr_len);

                    if(client_fd < 0){
                        if(errno == EAGAIN||errno == EWOULDBLOCK) break; 
                        perror("Accept Error");
                        break;
                    }
                    setnonblocking(client_fd);
                    users[client_fd].init(client_fd);
                    std::cout << "新连接：" << client_fd << std::endl;
                    std::cout << "新连接IP:" << inet_ntoa(client_addr.sin_addr) << std::endl;
                    struct epoll_event client_ev;
                    client_ev.events = EPOLLIN | EPOLLET | EPOLLRDHUP | EPOLLONESHOT; 
                    client_ev.data.fd = client_fd;
                    if(epoll_ctl(epoll_fd, EPOLL_CTL_ADD, client_fd, &client_ev) < 0){
                        perror("epoll_ctl: client_fd");
                        close(client_fd);
                    }
                }
            }else{
                while(true){ 
                    char buf[1024] = {0};
                    int byte_read = recv(curr_fd,buf,sizeof(buf)-1,0);
                    if(byte_read > 0){
                        users[curr_fd].buffer += buf; 
                    }else if(byte_read == 0){
                        cout << "fd: " << curr_fd << "断开连接" << endl;
                        epoll_ctl(epoll_fd, EPOLL_CTL_DEL, curr_fd, NULL);
                        close(curr_fd);
                        users[curr_fd].buffer.clear(); 
                        break;
                    }else if(byte_read < 0){
                        if(errno == EAGAIN||errno == EWOULDBLOCK){
                            if(!users[curr_fd].buffer.empty()){
                            pool.submit(handle_client,curr_fd,epoll_fd);                    
                            }else{
                                struct epoll_event ev;
                                ev.data.fd = curr_fd;
                                ev.events = EPOLLIN | EPOLLET | EPOLLRDHUP | EPOLLONESHOT;
                                epoll_ctl(epoll_fd, EPOLL_CTL_MOD, curr_fd, &ev);
                            }
                            break;
                        }
                        perror("recv");
                        epoll_ctl(epoll_fd, EPOLL_CTL_DEL, curr_fd, NULL);
                        close(curr_fd); 
                        users[curr_fd].buffer.clear();
                        break;
                    }
                }
            }
        }
    }
    close(listenfd);
    close(epoll_fd);
    return 0;
}

