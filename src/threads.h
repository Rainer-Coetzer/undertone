// A small fixed pool for splitting a frame across cores. The calling thread works too. Jobs are
// serialised by a mutex, so the preview and an export can never interleave their work.
#pragma once
#include <algorithm>
#include <atomic>
#include <condition_variable>
#include <functional>
#include <mutex>
#include <thread>
#include <vector>

class ThreadPool {
public:
    static ThreadPool& get() { static ThreadPool pool; return pool; }
    int size() const { return (int)workers.size() + 1; }

    void forEach(int count, const std::function<void(int)>& fn) {
        if (count <= 0) return;
        std::lock_guard<std::mutex> job(jobMutex);
        {
            std::lock_guard<std::mutex> lock(m);
            current = &fn; total = count; next = 0; busy = (int)workers.size(); ++generation;
        }
        wake.notify_all();
        drain();
        std::unique_lock<std::mutex> lock(m);
        done.wait(lock, [this] { return busy == 0; });
        current = nullptr;
    }

private:
    ThreadPool() {
        const int n = std::max(1, (int)std::thread::hardware_concurrency() - 1);
        for (int i = 0; i < n; ++i)
            workers.emplace_back([this] { run(); });
    }
    ~ThreadPool() {
        { std::lock_guard<std::mutex> lock(m); quit = true; ++generation; }
        wake.notify_all();
        for (auto& t : workers) t.join();
    }

    void drain() {
        for (int i; (i = next.fetch_add(1)) < total;) (*current)(i);
    }
    void run() {
        unsigned seen = 0;
        for (;;) {
            {
                std::unique_lock<std::mutex> lock(m);
                wake.wait(lock, [&] { return generation != seen; });
                seen = generation;
                if (quit) return;
            }
            drain();
            { std::lock_guard<std::mutex> lock(m); --busy; }
            done.notify_one();
        }
    }

    std::vector<std::thread> workers;
    std::mutex m, jobMutex;
    std::condition_variable wake, done;
    const std::function<void(int)>* current = nullptr;
    int total = 0, busy = 0;
    std::atomic<int> next{0};
    unsigned generation = 0;
    bool quit = false;
};

// Splits [0, count) into contiguous chunks, one call per chunk.
inline void parallelRange(int count, const std::function<void(int, int)>& fn) {
    ThreadPool& pool = ThreadPool::get();
    const int chunks = std::min(count, pool.size() * 2);
    pool.forEach(chunks, [&](int c) {
        fn((int)((long long)count * c / chunks), (int)((long long)count * (c + 1) / chunks));
    });
}
