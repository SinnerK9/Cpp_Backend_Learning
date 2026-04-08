#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <cstring>
#include <unistd.h> //close
#include <iostream>
#include <thread>
#include <vector>
#include "Threadpool.h"
#include <mutex> 

using namespace std; 
std::mutex cout_mtx;
void Handle_Client(int client_socket, struct sockaddr_in client_addr){
    //加上 ()，正确获取线程 ID
    {
        std::lock_guard<std::mutex> lock(cout_mtx);
        cout << "Thread " << this_thread::get_id() << " Client IP: " << inet_ntoa(client_addr.sin_addr) << endl;
    }

    char buf[1000] = {0};
    int byte_read = recv(client_socket, buf, sizeof(buf) -1, 0);
    
    if(byte_read > 0) {
        {
            std::lock_guard<std::mutex> lock(cout_mtx);
            cout << "Thread " << this_thread::get_id() << " received: \n" << buf << endl;
        }

        const char* response = "HTTP/1.1 200 OK\r\n"
            "Content-Type: text/html; charset=utf-8\r\n"
            "Content-Length: 15\r\n"
            "Connection: close\r\n" //关键修复：告诉浏览器发完就关连接
            "\r\n"
            "<h1>Hello!</h1>";

        send(client_socket, response, strlen(response), 0);
    }

    close(client_socket);
    
    {
        std::lock_guard<std::mutex> lock(cout_mtx);
        cout << "Thread " << this_thread::get_id() << " finished handling client" << endl;
    }
}

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

    threadpool pool(8); //优化：创造一个8线程线程池
    while(true){ 
        struct sockaddr_in client_addr;
        socklen_t client_addr_len = sizeof(client_addr);//debug：此处用到的IP长度必须是socklen_t类型，其本质是一个无符号整型，用int会报错
        int client_socket = accept(listenfd, (struct sockaddr*)&client_addr, &client_addr_len);
        if (client_socket < 0) {
            perror("Accept Error!");
            continue;
        }
        cout << "Client Connected! IP: " << inet_ntoa(client_addr.sin_addr) << endl;
        pool.submit(Handle_Client, client_socket, client_addr); //将处理客户端请求和函数和所有参数传入submit令其打包成packagedtask
    }

    close(listenfd);
    return 0;
}