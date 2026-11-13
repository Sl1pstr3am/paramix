// Copyright (c) 2026 Hamidreza Sadeghi
// SPDX-License-Identifier: MIT
//
// A small fixed-size pool for the spectrogram work. ONNX Runtime brings its own
// pool for the network; this one only has to parallelise a few hundred FFTs per
// chunk, so it is deliberately plain: no work stealing, no task queue growth,
// one blocking parallel_for at a time.

#pragma once

#include <atomic>
#include <cstdint>
#include <condition_variable>
#include <functional>
#include <mutex>
#include <thread>
#include <vector>

namespace demucs::util {

class ThreadPool {
public:
    explicit ThreadPool(int threads) : threads_(threads < 1 ? 1 : threads) {
        if (threads_ <= 1) return;
        workers_.reserve(threads_ - 1);
        for (int i = 0; i < threads_ - 1; ++i)
            workers_.emplace_back([this] { worker_loop(); });
    }

    ~ThreadPool() {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            stopping_ = true;
        }
        wake_.notify_all();
        for (auto& worker : workers_) worker.join();
    }

    ThreadPool(const ThreadPool&) = delete;
    ThreadPool& operator=(const ThreadPool&) = delete;

    int size() const { return threads_; }

    // Runs body(i) for i in [0, count), blocking until all of them are done.
    // The calling thread takes part, so a pool of one is a plain loop.
    //
    // Callable from more than one thread: the pipeline runs a spectrogram on a
    // worker thread while the main thread is in the network, and those two can
    // reach this from either side. Rounds are serialised rather than nested.
    void parallel_for(int count, const std::function<void(int)>& body) {
        if (count <= 0) return;
        if (threads_ <= 1 || count == 1) {
            for (int i = 0; i < count; ++i) body(i);
            return;
        }
        std::lock_guard<std::mutex> round(round_mutex_);
        {
            std::lock_guard<std::mutex> lock(mutex_);
            body_ = &body;
            count_ = count;
            next_.store(0, std::memory_order_relaxed);
            remaining_.store(threads_, std::memory_order_relaxed);
            ++generation_;
        }
        wake_.notify_all();
        run_claimed_items();
        finish_one();

        std::unique_lock<std::mutex> lock(mutex_);
        done_.wait(lock, [this] { return remaining_.load(std::memory_order_acquire) == 0; });
        body_ = nullptr;
    }

private:
    void worker_loop() {
        std::uint64_t seen = 0;
        for (;;) {
            {
                std::unique_lock<std::mutex> lock(mutex_);
                wake_.wait(lock, [&] { return stopping_ || generation_ != seen; });
                if (stopping_) return;
                seen = generation_;
            }
            run_claimed_items();
            finish_one();
        }
    }

    void run_claimed_items() {
        const auto& body = *body_;
        for (;;) {
            const int index = next_.fetch_add(1, std::memory_order_relaxed);
            if (index >= count_) return;
            body(index);
        }
    }

    void finish_one() {
        if (remaining_.fetch_sub(1, std::memory_order_acq_rel) == 1) {
            std::lock_guard<std::mutex> lock(mutex_);
            done_.notify_all();
        }
    }

    int threads_;
    std::vector<std::thread> workers_;
    std::mutex mutex_;
    std::mutex round_mutex_;
    std::condition_variable wake_, done_;
    bool stopping_ = false;
    std::uint64_t generation_ = 0;
    const std::function<void(int)>* body_ = nullptr;
    int count_ = 0;
    std::atomic<int> next_{0};
    std::atomic<int> remaining_{0};
};

}  // namespace demucs::util
