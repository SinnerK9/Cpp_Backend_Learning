#include<iostream>
#include<thread>
#include<mutex>
#include<condition_variable>
#include<vector>
#include<queue>
#include<atomic>
#include<functional>

class threadpool {
private:
    std::mutex mtx;
    std::condition_variable condition;
    std::atomic<bool> stop;
    std::vector<std::thread> workers;  // 用vector容器来管理工作线程
    std::queue<std::function<void()>> tasks;  // 任务队列：存储待执行的函数

    // 新知识：这里的function<void()>相当于将所有可以调用的东西打包成一个统一类型
    // 这里面所有的任务都叫做task，用task()即可执行之

public:
    threadpool(size_t num_threads) : stop(false) {
        // 创建指定数量的工作线程
        for (size_t i = 0; i < num_threads; i++) {
            workers.emplace_back([this] {
                // 每个线程无限循环，执行大括号中的代码
                while (true) {
                    std::function<void()> task;  // 用于存放取出来的任务
                    //注意：此处的大括号位置很重要，从下方左括号开始进入unique_lock的作用域，自动上锁不让别的线程同时操作任务队列
                    {
                        std::unique_lock<std::mutex> lock(mtx);

                        // 线程保持睡眠，直到线程池将要关闭 或者 任务池有任务可取
                        condition.wait(lock, [this] {
                            return stop || !tasks.empty();
                        });

                        // 如果线程池关闭且无任务，线程退出
                        if (stop && tasks.empty()) {
                            return;
                        }

                        // 取出队首任务，使用移动语义提高效率
                        task = std::move(this->tasks.front());
                        tasks.pop();
                    }  // 离开作用域，自动解锁，在执行任务时已经解锁！这一点很重要，因为执行任务本身并不需要操作队列，所以可以让别的线程读取任务防止阻塞
                       //总而言之：锁只保护队列，无需保护任务执行  
                    task();
                }
            });
        }
    }
    //submit方法：生产者提交任务到任务序列
    void submit(std::function<void()> task){
        {
            std::lock_guard<std::mutex> lock(mtx);
            if(stop){
                throw std::runtime_error("线程池已停止");
            }
            tasks.push(move(task)); //注意：此处要使用移动语义
        }
        condition.notify_one();//唤醒一个等待的线程
    }
    //析构方法，设定线程池停止，唤醒所有线程并且等待他们最终执行完
    ~threadpool(){
        {
            std::lock_guard<std::mutex> lock(mtx);
            stop = true;
        }
        condition.notify_all();

        for(auto& worker : workers){
            if(worker.joinable()){
                worker.join();
            }
        }

        std:: cout << "线程池工作结束" << std::endl;
    }
};

