#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <cstring>
#include <unistd.h> //close
#include <iostream>  

using namespace std; 

int main() {
    //调用socket方法,共有三个传入参数：协议族，流传输协议，默认protocol
    int listenfd = socket(PF_INET, SOCK_STREAM, 0);
    if (listenfd < 0) {
        perror("Socket Error!");
        return -1;
    }

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));//debug：提前将addr内全部清零，防止垃圾值
    addr.sin_family = AF_INET;      
    addr.sin_addr.s_addr = htonl(INADDR_ANY);  
    addr.sin_port = htons(8080);//值得注意：port和IP都需要转化成计算机格式！

    int opt = 1;
    setsockopt(listenfd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt)); //第一个优化：在bind之前加入setsockopt，允许端口重用，避开time_wait

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

    while(true){ //优化：使得socket一直保持工作，不要应答完一次就return
        struct sockaddr_in client_addr;
        socklen_t client_addr_len = sizeof(client_addr);//debug：此处用到的IP长度必须是socklen_t类型，其本质是一个无符号整型，用int会报错
        int client_socket = accept(listenfd, (struct sockaddr*)&client_addr, &client_addr_len);
        if (client_socket < 0) {
            continue;//相应地，此处作出变动，接收失败不应return，而该接收下一个
        }

        cout << "Client Connected! IP: " << inet_ntoa(client_addr.sin_addr) << endl;

        char buf[1000] = {0};
        int byte_read = recv(client_socket, buf, sizeof(buf) - 1, 0);
        cout << "Received: " << buf << endl;

        const char* response = "HTTP/1.1 200 OK\r\nContent-Type: text/html\r\n\r\n<h1>Hello!</h1>";
        send(client_socket, response, strlen(response), 0);

        close(client_socket);
    }
    
    close(listenfd);

    return 0;
}