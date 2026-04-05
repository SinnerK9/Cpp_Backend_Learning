#include<iostream>
#include<thread>
#include<mutex>
#include<condition_variable>
#include<vector>
#include<queue>
#include<atomic>
#include<functional>
#include<future>

class threadpool {
private:
    std::mutex mtx;
    std::condition_variable condition;
    std::atomic<bool> stop;
    std::vector<std::thread> workers;  // 用vector容器来管理工作线程
    std::queue<std::function<void()>> tasks;  // 任务队列：存储待执行的函数
    std::atomic<size_t> pending_tasks{0};//采用atomic原子操作，多线程安全加减

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
                    pending_tasks--; //任务执行完毕后，提交但未执行完毕的数量减少
                }
            });
        }
    }
    //submit方法：生产者提交任务到任务序列
    template<typename F, typename ...Args> //模板编程：F表示任何函数，...Args表示传入的参数集
    auto submit(F &&f , Args && ...args) // &&表示万能引用 auto + -> 是后置返回参数的写法！这里表示返回的是以该函数的返回类型为返回类型的future
    -> std::future<decltype(f(args...))>{   //decltype方法得到类型
        using return_type = decltype(f(args...)); //相当于typedef
        //这一段语法最为关键：先把原来的函数和参数bind在一起，参数通过forward传入
        //然后，将其打包为packaged_task，使得其能够传出返回值到future中，其类型为我们得到的return_type()
        //然后，将这个packaged_task转化为共享指针，使其能够拷贝。
        auto task = std::make_shared<std::packaged_task<return_type()>>(std::bind(std::forward<F>(f),std::forward<Args>(args)...));
        //注意：task是一个共享指针，内部的packaged_task才是真正的任务本身，get_future是packaged_task的方法
        std::future<return_type> result = task -> get_future();

        {
            std::lock_guard<std::mutex> lock(mtx);
            if(stop){
                std::cout << "线程池已停止，不再提交" << std:: endl;
                throw std::runtime_error("线程池已停止");
            }
            //注意：threadpool执行函数只会直接task()，执行一个function<void()>，无法辨析task是不是共享指针！
            //在此我们把共享指针解引用并且执行packaged_task中的真正任务的操作封装成了一个function<void()>，这样才可以传进去直接task()执行。
            tasks.push([task](){
                (*task)();
            });
            pending_tasks++; //提交之后，未解决的任务数增加
        }
        condition.notify_one();
        return result;//debug:return必须在唤醒线程之后
    }

    //析构方法，设定线程池停止，唤醒所有线程并且等待他们最终执行完
    ~threadpool(){
        {
            std::lock_guard<std::mutex> lock(mtx);
            stop = true;
        }
        condition.notify_all();
        //增加判断逻辑，等待所有任务完成
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

