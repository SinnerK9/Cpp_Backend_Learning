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
#include "Threadpool.h"

using namespace std; 

//优化：新增一个结构体：全局大数组记录断续传来的数据并组合在一起，防止局部数组导致的丢数据
struct ClientState {
    int fd;
    std::string buffer;//用string记录数据，方便断续传来的数据组合起来
};
ClientState users[65536];

//新增函数：setnonblocking用于给描述符fd加上非阻塞属性！
int setnonblocking(int fd){
    int old_flag = fcntl(fd,F_GETFL);
    int new_flag = old_flag | O_NONBLOCK;
    fcntl(fd,F_SETFL,new_flag);
    return old_flag;
}

void handle_client(int fd,int epoll_fd){
    std::cout << "fd " << fd << " 的完整请求内容: \n" << users[fd].buffer << endl;
    const char* response = "HTTP/1.1 200 OK\r\n\r\n<h1>Hello</h1>";
    send(fd, response, strlen(response), 0);
    users[fd].buffer.clear(); 
    //关键步骤：在第一次请求之后，EPOLLONESHOT屏蔽了该socket的后续请求，它的事件在事件表里不再被触发
    //必须把这个fd的事件在事件表里重新激活，换一套新的状态参数
    struct epoll_event ev;
    ev.events = EPOLLIN | EPOLLET | EPOLLONESHOT| EPOLLRDHUP;   //下一次它还是需要EPOLLONESHOT
    ev.data.fd= fd;
    epoll_ctl(epoll_fd,EPOLL_CTL_MOD,fd,&ev); //用EPOLL_CTL_MOD修改参数，而非增加，它一直在红黑树里
}

int main() {

    signal(SIGPIPE, SIG_IGN); //设置忽略SIGPIPE
    threadpool pool(8);//创建线程池
    int listenfd = socket(PF_INET, SOCK_STREAM, 0);
    if (listenfd < 0) {
        perror("Socket Error!");
        return -1;
    }
    setnonblocking(listenfd); //优化1：设置listenfd为非阻塞
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
    ev.events = EPOLLIN | EPOLLET | EPOLLRDHUP; //LT->ET:规定电平触发（位或运算）
    ev.data.fd = listenfd; 

    if(epoll_ctl(epoll_fd,EPOLL_CTL_ADD,listenfd,&ev) < 0){
        perror("epoll_ctl:listenfd error");
        close(listenfd);
        return -1;
    }

    struct epoll_event events[1024];//存储返回的就绪事件：注意，跟红黑树里的事件完全不是一回事，不能用fd下标访问对应事件

    while(true){ 
        int n = epoll_wait(epoll_fd,events,1024,-1);
        if(n < 0){
            perror("epoll_wait");
            continue;
        }

        for(int i = 0; i < n; i++){
            int curr_fd = events[i].data.fd; 
            //优化：在处理前增加对EPOLLRDHUP | EPOLLHUP | EPOLLERR等其他异常标志的检测，并对异常连接进行安全关闭
            //通过位运算，检测event中是否存在这三异常个事件中的其中一个
            if (events[i].events & (EPOLLRDHUP | EPOLLHUP | EPOLLERR)) {
                cout << "fd: " << curr_fd << " 异常断开，已清理" << endl;
                epoll_ctl(epoll_fd, EPOLL_CTL_DEL, curr_fd, NULL);  //从事件表中删除
                close(curr_fd); //直接关闭异常文件描述符                                
                continue; //跳过后续逻辑                                       
            }

            if(curr_fd == listenfd){
                while(true){ //优化2：为了解决单次accept有大量连接积压导致超时和重复wait的问题，accept进入死循环直到后面没有连接
                    sockaddr_in client_addr;
                    socklen_t client_addr_len = sizeof(client_addr);
                    int client_fd = accept(listenfd,(sockaddr*) &client_addr,&client_addr_len);

                    if(client_fd < 0){
                        if(errno == EAGAIN||errno == EWOULDBLOCK) break; //client_fd < 0且errno == EAGAIN代表连接读完，跳出去等下一个epoll_wait
                        perror("Accept Error");//errno != EAGAIN，这是错误了
                        break;
                    }
                    setnonblocking(client_fd);//优化1：将client_fd设为非阻塞
                    users[client_fd].fd = client_fd; //优化：每接收一个新连接，把它丢进全局大数组
                    users[client_fd].buffer = ""; //接收到的数据初始化为空
                    std::cout << "新连接：" << client_fd << std::endl;
                    std::cout << "新连接IP:" << inet_ntoa(client_addr.sin_addr) << std::endl;
                    struct epoll_event client_ev;
                    //增加EPOLLONESHOT，保证一个fd同时只被一个线程处理
                    client_ev.events = EPOLLIN | EPOLLET | EPOLLRDHUP | EPOLLONESHOT; //优化3：LT->ET:规定电平触发 + 监听客户端断开
                    client_ev.data.fd = client_fd;
                    if(epoll_ctl(epoll_fd, EPOLL_CTL_ADD, client_fd, &client_ev) < 0){
                        perror("epoll_ctl: client_fd");
                        close(client_fd);
                    }
                }
            }else{
                //recv将网卡中请求从内核读到用户态，需要由主线程来完成
                while(true){ //优化2：给recv加上死循环，优化单次recv和数据分开发导致读取不全，以及重复wait的问题
                    char buf[1024] = {0};
                    int byte_read = recv(curr_fd,buf,sizeof(buf)-1,0);
                    if(byte_read > 0){
                        users[curr_fd].buffer += buf; //不马上响应，先把这段丢进接收到的数据再说
                    }else if(byte_read == 0){
                        cout << "fd: " << curr_fd << "断开连接" << endl;
                        epoll_ctl(epoll_fd, EPOLL_CTL_DEL, curr_fd, NULL);
                        close(curr_fd);
                        users[curr_fd].buffer.clear(); //清空数据
                        break;
                    }else if(byte_read < 0){
                        if(errno == EAGAIN||errno == EWOULDBLOCK){
                            if(!users[curr_fd].buffer.empty()){
                            pool.submit(handle_client,curr_fd,epoll_fd); //send部分不再交给主线程，而是丢到线程池的任务队列                              
                            }
                            break;
                        }
                        perror("recv");
                        epoll_ctl(epoll_fd, EPOLL_CTL_DEL, curr_fd, NULL);
                        close(curr_fd); //真出错了，关闭连接
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

