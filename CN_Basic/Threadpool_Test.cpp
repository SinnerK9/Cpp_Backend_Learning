#include "Threadpool.h"
#include <chrono>
#include <iostream>
int main() {
    std::cout << "=== 测试线程池 ===" << std::endl;
    
    // 创建4个工作线程
    threadpool pool(4);
    
    // 提交10个任务
   for(int i = 0; i < 10; i++){
    pool.submit([i]{
        std::this_thread::sleep_for(std::chrono::milliseconds(200));//假装工作
        std::cout << "任务 " << i << " 执行完成，线程ID: " << std::this_thread::get_id() << std::endl;
    });
   }
    std::cout << "所有任务已提交，等待完成..." << std::endl;
    //注意这些操作中的时间关系：200ms是submit的task的执行时间，submit动作与提交的任务内容实际上完全没有关系！

    // 等待所有任务执行完成
    std::this_thread::sleep_for(std::chrono::seconds(3));
    
    std::cout << "测试完成！" << std::endl;
    return 0;
}