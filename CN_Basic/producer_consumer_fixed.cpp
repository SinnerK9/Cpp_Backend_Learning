#include <iostream>
#include <thread>
#include <mutex>
#include <condition_variable>
#include <queue>
#include <vector>
#include <chrono>
#include <random>

template<typename T>//模板编程
class SafeQueue{
private:
    std::queue<T> queue_;
    mutable std::mutex mtx;//锁,将mtx声明为mutable，这样即使在const成员函数(后面的get_size)中也可以修改它
    std::condition_variable not_full;//用来通知生产者需要继续产出任务
    std::condition_variable not_empty; //用来通知消费者线程有任务
    const size_t max_size;
    bool stop_ = false;

public:
    //构造函数：要用explicit禁止隐式类型转换，多线程智能指针和容器类都应该加explicit
    explicit SafeQueue(size_t max_size = 10) : max_size(max_size) {}
    
    bool push(T value){ //生产者线程通过push方法输入数据
        std::unique_lock<std::mutex> lock(mtx);
        not_full.wait(lock,[this](){
            return queue_.size() < max_size || stop_;
        });
        if(stop_) return false;
        queue_.push(std::move(value)); //此处选择用move而非直接push，可以减少一次拷贝，对于处理复杂数据类型有提升
        not_empty.notify_one();//此时队列必然有数据，唤醒沉睡消费者线程

        return true;
    }

    bool pop(T &value){  //pop方法从队列中取出数据
        std::unique_lock<std::mutex> lock(mtx);
        not_empty.wait(lock,[this](){
            return !queue_.empty() || stop_;
        });//此处的语法：lambda表达式（事实上，只能访问大括号内的变量和全局变量）默认无法访问类内的成员变量，必须捕获this来把它的变量传进去
        if (stop_ && queue_.empty()) return false; //注意判定条件的不同，需要保证队列为空
        value = std::move(queue_.front()); //注意不用pop,用move把对应数据取出。
        queue_.pop();//记得把front弹出
        not_full.notify_one(); //此时队列必然不满，通知一个生产者线程

        return true;
    }

    void stop(){
        std::lock_guard<std::mutex> lock(mtx);//lock_guard够用
        stop_ = true;
        not_empty.notify_all(); // 唤醒所有等待的消费者
        not_full.notify_all();  // 唤醒所有等待的生产者
    }

    size_t get_size() const{ //用size_t这种无符号整数
        //此处必须上锁，因为size()方法需要访问队列，如果不加锁其可能在其他方法修改队列时读，导致竞争
        std::lock_guard<std::mutex> lock(mtx); //此处只需要简单上锁，lock_guard完全够用
        return queue_.size();
    }
    
    //判断队列是否已停止，便于主线程判断
    bool is_stopped() const {
        std::lock_guard<std::mutex> lock(mtx);
        return stop_;
    }
};

SafeQueue<int> task_queue(5); //至多有五个任务

//生产者函数，产出数据 传入生产者编号id，及其要生产的任务数
void producer(int id,int num_items){
    std::random_device rd;
    std::mt19937 gen(rd());
    std::uniform_int_distribution<> sleep_time(100, 500); //这一段的作用是让每次工作等100-500毫秒，模仿实际生产
    for(int i = 0; i < num_items; i++){
         std::this_thread::sleep_for(std::chrono::milliseconds(sleep_time(gen)));//模拟生成数据耗时
         int item = id * 1000 + i;//生成一个独一无二的任务编号，防止重复
         bool success = task_queue.push(item);
         if (success) {
            std::cout << "[生产者" << id << "] 生产: " << item 
                      << " (队列大小: " << task_queue.get_size() << ")" << std::endl;
        } else {
            std::cout << "[生产者" << id << "] 已停止生产" << std::endl;
            break;
        }
    }
    std::cout << "[生产者" << id << "] 完成工作" << std::endl;
}

//消费者函数无限循环，直到队列停止且空
void consumer(int id){
    std::random_device rd;
    std::mt19937 gen(rd());
    std::uniform_int_distribution<> process_time(200, 800); 
    int item;
    int count = 0;
    
    while(true){ 
        bool success = task_queue.pop(item);
        
        if (!success) {
            std::cout << "[消费者" << id << "] 队列已停止" << std::endl;
            break;
        }
        
        std::this_thread::sleep_for(std::chrono::milliseconds(process_time(gen))); //依旧假装工作

        std::cout << "[消费者" << id << "] 处理任务: " << item 
                  << " (已处理: " << ++count << "个)" << std::endl;
    }
    
    std::cout << "[消费者" << id << "] 完成工作，共处理 " << count << " 个任务" << std::endl;
}

int main() {
    std::cout << "===== 生产者-消费者系统启动 =====" << std::endl;
    
    // 创建生产者线程,规定创造的数据条目数
    std::vector<std::thread> producers;
    producers.emplace_back(producer, 1, 6); //注意这种语法，因为producers中的对象全都是线程对象，用emplace_back可以直接创造一个线程对象，直接把传入参数交给thread构造函数
    producers.emplace_back(producer, 2, 8);  
    producers.emplace_back(producer, 3, 5);  
    
    // 创建消费者线程
    std::vector<std::thread> consumers;
    consumers.emplace_back(consumer, 1); 
    consumers.emplace_back(consumer, 2);  
    consumers.emplace_back(consumer, 3);  
    
    //此前所有生产者消费者已经开始工作，现在需要等生产者完成工作
    for (auto& p : producers) {
        p.join();
    }
    std::cout << "\n所有生产者已完成\n" << std::endl;
    
    //使用轮询等待队列为空，放弃硬编码
    std::cout << "等待消费者处理剩余任务..." << std::endl;
    while (task_queue.get_size() > 0) {
        std::cout << "[主线程] 等待队列清空，当前大小: " 
                  << task_queue.get_size() << std::endl;
        std::this_thread::sleep_for(std::chrono::milliseconds(500)); //每500ms检查一次
    }
    
    std::cout << "\n队列已空，准备停止系统...\n" << std::endl;
    
    // 停止队列（通知所有消费者）
    task_queue.stop();
    
    // 等待所有消费者完成
    for (auto& c : consumers) {
        c.join();
    }
    
    std::cout << "\n===== 系统运行结束 =====" << std::endl;
    std::cout << "最终队列大小: " << task_queue.get_size() << std::endl;
    
    return 0;
}