#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <cstring>
#include <unistd.h>
#include <iostream>
#include <sys/select.h>

using namespace std; 

int main() {
    int listenfd = socket(PF_INET, SOCK_STREAM, 0);
    if (listenfd < 0) {
        perror("Socket Error!");
        return -1;
    }

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

    fd_set master_set,read_fds; //两个集合，一个存储所有监听fd，一个存储就绪fd
    FD_ZERO(&master_set); //清理监听集合里的垃圾数据
    FD_SET(listenfd,&master_set);//将fd加入监听集合
    int max_fd = listenfd; //此时加入集合的fd就是最大fd号，用于遍历

    while(true){ 
        read_fds = master_set; //每次进入循环前需要重新设置，因为内核会直接对其进行改动
        //select方法最为关键，其作用是把监听fd集合丢给内核让其帮忙轮询
        //传入参数有五个，首先是遍历的fd数目(0-max_fd)，然后是可以用于修改的就绪集合
        int ready = select(max_fd+1,&read_fds,NULL,NULL,NULL); 
        if(ready < 0){
            perror("select");
            continue;
        }

        //开始遍历
        for(int fd = 0; fd <= max_fd; fd++){
            if(FD_ISSET(fd,&read_fds)){
                if(fd == listenfd){
                    //判断任务是新连接，执行accept
                    struct sockaddr_in client_addr;
                    socklen_t client_addr_len = sizeof(client_addr);
                    int client_fd = accept(listenfd, (struct sockaddr*) &client_addr, &client_addr_len);
                    if(client_fd > 0){ //accept成功，成功创建socket的文件描述符
                        FD_SET(client_fd, &master_set);  //加入监控
                        if(client_fd > max_fd){
                            max_fd = client_fd;//更新select方法检查的最大fd，纳入监控
                        }
                        std::cout << "新连接: " <<  client_fd << std::endl;
                        std::cout << "新连接IP: " << inet_ntoa(client_addr.sin_addr) << std::endl;
                    }
                }else{
                    //不是新连接，那就是客户端发数据了，调用recv
                    char buf[1024] = {0};
                    int byte_read = recv(fd , buf , sizeof(buf) - 1, 0);
                    if(byte_read > 0){
                        std::cout << "fd: " << fd << " received: " << buf << std::endl;
                        const char* response = "HTTP/1.1 200 OK\r\nContent-Type: text/html\r\n\r\n<h1>Hello!</h1>";
                        send(fd,response,strlen(response),0);
                    }else{
                        //客户端出错，或者选择断开连接
                        std::cout << "fd " << fd << " 断开连接" << std::endl;
                        close(fd); // 关闭socket
                        FD_CLR(fd,&master_set); //从监听集合删除
                    }
                }
            }
        }
    }
    return 0;
}