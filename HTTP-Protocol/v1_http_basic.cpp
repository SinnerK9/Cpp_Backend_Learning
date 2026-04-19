#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <cstring>
#include <unistd.h>
#include <iostream>
#include <sys/epoll.h>
#include <fcntl.h> //优化1：用于设置非阻塞的头文件
#include <errno.h>
#include <signal.h> //优化4：用于设置忽略SIGPIPE逻辑 
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

using namespace std; 

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
};
ClientState users[65536];


int setnonblocking(int fd){
    int old_flag = fcntl(fd,F_GETFL);
    int new_flag = old_flag | O_NONBLOCK;
    fcntl(fd,F_SETFL,new_flag);
    return old_flag;
}

void handle_client(int fd,int epoll_fd){
    std::string& request = users[fd].buffer;
    size_t first_line_end = request.find("\r\n");  //利用find函数和每行末尾特征将第一行找出来！
    if(first_line_end == string::npos) return; //string::pos表示no position（没有找到期望位置），请求可能不完整
    string first_line = request.substr(0, first_line_end); //提取出第一行
    stringstream ss(first_line);
    string method,url,version; //用stringstream进行最简单的分割，用空格将第一行分割为请求方法/资源地址/http版本
    ss >> method >> url >> version;
    std::cout << "[解析请求] 方法:" << method << " 路径:" << url << " 协议:" << version << std::endl;
    //判断请求的路径是什么，给出相应的响应
    if(url == "/"){
        url = "/index.html"; //默认主页
    }
    std::string filepath = "./resources" + url; //要读的东西都存在resources，拼出路径
    struct stat file_stat;
    //使用stat方法查询相应路径文件是否存在，注意path方法不接受string，需要转化为char*
    if(stat(filepath.c_str(),&file_stat) < 0){
        std::string body = "<h1>404 Not Found</h1>";
        std::string header = "HTTP/1.1 404 Not Found\r\nContent-Length: " + std::to_string(file_stat.st_size) + "\r\n\r\n";
        send(fd,header.c_str(),header.size(),0); 
        send(fd,body.c_str(),body.size(),0);
    }else{
        std::string header = "HTTP/1.1 200 OK\r\nContent-Length: " + std::to_string(file_stat.st_size) + "\r\n\r\n"; //stat返回=0，找到了
        int src_fd = open(filepath.c_str(),O_RDONLY); //READ_ONLY打开
        //进行内存映射。传入参数：映射虚拟内存地址（NULL自动分配），映射内存长度，映射权限（只读），映射类型（私有映射），映射的文件描述符，文件偏移量（0表示开头开始映射）
        //返回值是映射后的内存首地址，直接靠这个地址即可访问文件
        char* file_address = (char*) mmap(NULL,file_stat.st_size,PROT_READ,MAP_PRIVATE,src_fd,0);
        close(src_fd); //mmap建立后不需要文件描述符了，关掉释放资源

        //writev专用的数据块描述结构体，描述不连续内存，包含内存首地址和数据长度
        struct iovec iv[2];
        iv[0].iov_base = (void*)header.c_str(); //第一块内存：响应头
        iv[0].iov_len = header.size();
        iv[1].iov_base = file_address; //第二块内存：mmap出的内容
        iv[1].iov_len = file_stat.st_size;

        writev(fd,iv,2); //聚合写：传入客户端socket描述符，iovec数组和要传送的数据块数量
        munmap(file_address, file_stat.st_size); //解除映射，释放资源
    }
    //清空buffer，重置EPOLLONESHOT
    users[fd].buffer.clear(); 
    struct epoll_event ev;
    ev.events = EPOLLIN | EPOLLET | EPOLLONESHOT| EPOLLRDHUP;  
    ev.data.fd= fd;
    epoll_ctl(epoll_fd,EPOLL_CTL_MOD,fd,&ev); 
}

int main() {

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
                    users[client_fd].fd = client_fd; 
                    users[client_fd].buffer = ""; 
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

