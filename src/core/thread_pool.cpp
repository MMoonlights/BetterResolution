#include "core/thread_pool.hpp"

#include <algorithm>
#include <exception>

namespace br {

uint32_t ThreadPool::hardware_threads() noexcept {
    const unsigned n = std::thread::hardware_concurrency();
    return std::clamp<uint32_t>(n ? n : 1u, 1u, 32u);
}

ThreadPool::ThreadPool(uint32_t threads) { set_threads(threads); }

ThreadPool::~ThreadPool() { stop(); }

void ThreadPool::set_threads(uint32_t threads) {
    std::lock_guard<std::mutex> busy(busy_);
    if (!threads) threads = hardware_threads();
    threads = std::clamp<uint32_t>(threads, 1u, 64u);
    if (threads == threads_.load(std::memory_order_relaxed) && workers_.size() + 1 == threads) return;
    stop();
    threads_.store(threads, std::memory_order_relaxed);
    start(threads - 1);
}

void ThreadPool::start(uint32_t workers) {
    quit_ = false;
    try {
        workers_.reserve(workers);
        for (uint32_t i = 0; i < workers; ++i) workers_.emplace_back([this] { worker_loop(); });
    } catch (...) {
        // При ошибке конструктора дожидается каждого созданного рабочего потока до уничтожения вектора:
        // уничтожение присоединяемого std::thread завершает процесс.
        stop();
        threads_.store(1, std::memory_order_relaxed);
        throw;
    }
}

void ThreadPool::stop() {
    {
        std::lock_guard<std::mutex> lock(m_);
        quit_ = true;
    }
    cv_work_.notify_all();
    for (auto& t : workers_) t.join();
    workers_.clear();
}

void ThreadPool::run_items() {
    for (;;) {
        const uint32_t i = next_.fetch_add(1, std::memory_order_relaxed);
        if (i >= job_count_) break;
        try {
            (*job_)(i);
        } catch (...) {
            std::lock_guard<std::mutex> lock(m_);
            if (!error_) error_ = std::current_exception();
            next_.store(job_count_, std::memory_order_relaxed);
        }
    }
}

void ThreadPool::worker_loop() {
    uint64_t seen = 0;
    for (;;) {
        {
            std::unique_lock<std::mutex> lock(m_);
            cv_work_.wait(lock, [&] { return quit_ || (generation_ != seen && job_slots_ > 0); });
            if (quit_) return;
            seen = generation_;
            --job_slots_;
            ++active_;
        }
        run_items();
        {
            std::lock_guard<std::mutex> lock(m_);
            --active_;
        }
        cv_done_.notify_all();
    }
}

void ThreadPool::parallel_for(uint32_t count, uint32_t max_threads, const std::function<void(uint32_t)>& fn) {
    if (!count) return;
    std::unique_lock<std::mutex> busy(busy_, std::try_to_lock);
    if (!busy.owns_lock()) {
        // Перенастройка или другой parallel_for уже управляет состоянием пула.
        // При выполнении задачи в вызывающем потоке нельзя читать workers_ и состояние задачи.
        for (uint32_t i = 0; i < count; ++i) fn(i);
        return;
    }
    const uint32_t configured = threads_.load(std::memory_order_relaxed);
    const uint32_t want = std::min<uint32_t>({count, max_threads ? max_threads : configured, configured});
    if (want <= 1 || workers_.empty()) {
        for (uint32_t i = 0; i < count; ++i) fn(i);
        return;
    }
    {
        std::lock_guard<std::mutex> lock(m_);
        job_ = &fn;
        job_count_ = count;
        job_slots_ = std::min<uint32_t>(want - 1, static_cast<uint32_t>(workers_.size()));
        next_.store(0, std::memory_order_relaxed);
        error_ = nullptr;
        ++generation_;
    }
    cv_work_.notify_all();
    run_items();
    std::exception_ptr err;
    {
        std::unique_lock<std::mutex> lock(m_);
        // Потоки, которые ещё не проснулись, не должны поздно начинать уже завершённую задачу.
        job_slots_ = 0;
        cv_done_.wait(lock, [&] { return active_ == 0; });
        job_ = nullptr;
        err = error_;
        error_ = nullptr;
    }
    if (err) std::rethrow_exception(err);
}

}
