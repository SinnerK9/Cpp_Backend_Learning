#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <cstring>
#include <unistd.h>
#include <iostream>
#include <sys/epoll.h>
#include <fcntl.h> //优化1：用于设置非阻塞的头文件
#include <errno.h>

using namespace std; 


//新增函数：setnonblocking用于给描述符fd加上非阻塞属性！
int setnonblocking(int fd){
    int old_flag = fcntl(fd,F_GETFL);
    int new_flag = old_flag | O_NONBLOCK;
    fcntl(fd,F_SETFL,new_flag);
    return old_flag;
}

int main() {
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
    ev.events = EPOLLIN; 
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
                    std::cout << "新连接：" << client_fd << std::endl;
                    std::cout << "新连接IP:" << inet_ntoa(client_addr.sin_addr) << std::endl;
                    struct epoll_event client_ev;
                    client_ev.events = EPOLLIN;
                    client_ev.data.fd = client_fd;
                    if(epoll_ctl(epoll_fd, EPOLL_CTL_ADD, client_fd, &client_ev) < 0){
                        perror("epoll_ctl: client_fd");
                        close(client_fd);
                    }
                }
            }else{
                while(true){ //优化2：给recv加上死循环，优化单次recv和数据分开发导致读取不全，以及重复wait的问题
                    char buf[1024] = {0};
                    int byte_read = recv(curr_fd,buf,sizeof(buf)-1,0);
                    if(byte_read > 0){
                        std::cout << "fd: " << curr_fd << "received: " << buf << std::endl;
                        const char* response = "HTTP/1.1 200 OK\r\nContent-Type: text/html\r\n\r\n<h1>Hello from epoll server!</h1>";
                        send(curr_fd, response, strlen(response), 0);
                    }else if(byte_read == 0){
                        cout << "fd: " << curr_fd << "断开连接" << endl;
                        epoll_ctl(epoll_fd, EPOLL_CTL_DEL, curr_fd, NULL);
                        close(curr_fd);
                    }else if(byte_read < 0){
                        if(errno == EAGAIN||errno == EWOULDBLOCK) break; //数据读完了，跳出循环
                        perror("recv");
                        epoll_ctl(epoll_fd, EPOLL_CTL_DEL, curr_fd, NULL);
                        close(curr_fd); //真出错了，关闭连接
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

