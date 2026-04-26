#ifndef THREAD_POOL_H
#define THREAD_POOL_H

#include <thread>
#include <mutex>
#include <condition_variable>
#include <vector>
#include <queue>
#include <atomic>
#include <functional>
#include <future>
#include <memory>
#include <iostream>
#include <chrono>


class ThreadPool {
public:
    explicit ThreadPool(size_t num_threads)
        : stop_(false)
        , pending_tasks_(0)
    {
        for (size_t i = 0; i < num_threads; i++) {
            workers_.emplace_back([this] {
                worker_loop_();
            });
        }
    }

    ThreadPool(const ThreadPool&) = delete;
    ThreadPool& operator=(const ThreadPool&) = delete;

    template<typename F, typename... Args>
    auto submit(F&& f, Args&&... args)
        -> std::future<decltype(f(args...))>
    {
        using return_type = decltype(f(args...));
        auto task = std::make_shared<std::packaged_task<return_type()>>(
            std::bind(std::forward<F>(f), std::forward<Args>(args)...)
        );
        std::future<return_type> result = task->get_future();

        {
            std::lock_guard<std::mutex> lock(mtx_);
            if (stop_) {
                throw std::runtime_error(
                    "ThreadPool: cannot submit task to stopped pool"
                );
            }
            tasks_.emplace([task]() {
                (*task)();
            });
            pending_tasks_++;
        }
        condition_.notify_one();
        return result;
    }

    //新接口：查询
    size_t pending_count() const {
        return pending_tasks_;
    }

    size_t thread_count() const {
        return workers_.size();
    }

    ~ThreadPool() {
        {
            std::lock_guard<std::mutex> lock(mtx_);
            stop_ = true;
        }

        //唤醒所有等待的线程
        condition_.notify_all();

        //等待所有任务完成
        while (pending_tasks_ > 0) {
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }

        //等待所有线程退出
        for (auto& worker : workers_) {
            if (worker.joinable()) {
                worker.join();
            }
        }
    }

private:
    void worker_loop_() {
        while (true) {
            std::function<void()> task;

            {
                std::unique_lock<std::mutex> lock(mtx_);
                condition_.wait(lock, [this] {
                    return stop_ || !tasks_.empty();
                });

                if (stop_ && tasks_.empty()) {
                    return;  //线程退出
                }

                task = std::move(tasks_.front());
                tasks_.pop();
            }

            //在锁外面执行，不阻塞其他线程取任务
            task();
            pending_tasks_--;
        }
    }

    std::mutex mtx_;
    std::condition_variable condition_;
    std::atomic<bool> stop_;
    std::atomic<size_t> pending_tasks_;

    std::vector<std::thread> workers_;
    std::queue<std::function<void()>> tasks_;
};


#endif 