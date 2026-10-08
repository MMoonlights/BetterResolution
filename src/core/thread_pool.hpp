#pragma once

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <functional>
#include <mutex>
#include <thread>
#include <vector>

namespace br {

// Небольшой постоянный пул потоков. parallel_for вызывает fn(i) для i из [0, count), используя
// не более `max_threads` потоков (в работе участвует и вызывающий поток). Если пул занят
// (например, несколько вызовов используют один контекст), задача выполняется в вызывающем потоке.
class ThreadPool {
public:
    explicit ThreadPool(uint32_t threads = 0);
    ~ThreadPool();
    ThreadPool(const ThreadPool&) = delete;
    ThreadPool& operator=(const ThreadPool&) = delete;

    void set_threads(uint32_t threads);
    uint32_t threads() const noexcept { return threads_.load(std::memory_order_relaxed); }

    void parallel_for(uint32_t count, uint32_t max_threads, const std::function<void(uint32_t)>& fn);

    static uint32_t hardware_threads() noexcept;

private:
    void start(uint32_t workers);
    void stop();
    void worker_loop();
    void run_items();

    // Изменение размера может читать число потоков, пока set_threads удерживает busy_.
    // Вектором рабочих потоков и задачами по-прежнему управляют busy_/m_, а не этот atomic.
    std::atomic<uint32_t> threads_{1};
    std::vector<std::thread> workers_;
    std::mutex busy_;
    std::mutex m_;
    std::condition_variable cv_work_;
    std::condition_variable cv_done_;
    const std::function<void(uint32_t)>* job_{};
    uint32_t job_count_{};
    uint32_t job_slots_{};    // Число рабочих потоков, которые могут присоединиться к текущей задаче.
    std::atomic<uint32_t> next_{0};
    uint32_t active_{};
    uint64_t generation_{};
    bool quit_{false};
    std::exception_ptr error_;
};

}
