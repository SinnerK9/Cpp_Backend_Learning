#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <cstring>
#include <unistd.h> //close
#include <iostream>
#include <thread>
#include <vector>

using namespace std; 
//优化：专门创建一个处理请求的函数，和主函数分离开来
void Handle_Client(int client_socket,struct sockaddr_in client_addr){
    //输出该处理socket线程的id和socket的IP地址，注意需要转化为字符串格式
    cout << "Thread" << this_thread::get_id << "Client IP address: " << inet_ntoa(client_addr.sin_addr) << endl;
    char buf[1000] = {0};
    int byte_read = recv(client_socket,buf,sizeof(buf) -1, 0);
    if(byte_read > 0) {
        cout << "Thread " << this_thread::get_id() << " received: " << buf << endl;
        const char* response = "HTTP/1.1 200 OK\r\n"
            "Content-Type: text/html\r\n"
            "Content-Length: 19\r\n"
            "\r\n"
            "<h1>Hello!</h1>";

        send(client_socket, response, strlen(response), 0);
    }

    close(client_socket);
    cout << "Thread " << this_thread::get_id() << " finished handling client" << endl;

}

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

    vector<std::thread> Threads;  //多线程优化:增加一个vector容器来管理多线程
    while(true){ //优化：使得socket一直保持工作，不要应答完一次就return
        struct sockaddr_in client_addr;
        socklen_t client_addr_len = sizeof(client_addr);//debug：此处用到的IP长度必须是socklen_t类型，其本质是一个无符号整型，用int会报错
        int client_socket = accept(listenfd, (struct sockaddr*)&client_addr, &client_addr_len);
        if (client_socket < 0) {
            perror("Accept Error!");
            continue;
        }
        cout << "Client Connected! IP: " << inet_ntoa(client_addr.sin_addr) << endl;
        thread client_thread(Handle_Client, client_socket, client_addr);
        client_thread.detach();  // 分离线程，不等待它结束

    }
     for(auto& t : Threads) {
        if(t.joinable()) {
            t.join();
        }
    }

    close(listenfd);
    return 0;
}