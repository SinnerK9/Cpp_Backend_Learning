// Add this to your thread_pool.h
#include <future>

template<typename F, typename... Args>
auto submit(F&& f, Args&&... args) 
    -> std::future<decltype(f(args...))> {
    
    // 创建一个packaged_task来包装函数
    using return_type = decltype(f(args...));
    auto task = std::make_shared<std::packaged_task<return_type()>>(
        std::bind(std::forward<F>(f), std::forward<Args>(args)...)
    );
    
    // 获取future
    std::future<return_type> result = task->get_future();
    
    // 提交任务
    {
        std::lock_guard<std::mutex> lock(mtx);
        if(stop) {
            throw std::runtime_error("线程池已停止");
        }
        
        // 将任务包装成无参数的function
        tasks.push([task]() {
            (*task)();
        });
    }
    
    condition.notify_one();
    return result;
}