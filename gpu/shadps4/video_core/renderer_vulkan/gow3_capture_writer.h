// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include <condition_variable>
#include <cstdio>
#include <exception>
#include <functional>
#include <mutex>
#include <queue>
#include <thread>
#include <utility>

namespace Vulkan::Gow3 {
// Only explicit raw captures instantiate this worker. Jobs retain their completed
// readback buffers; disk I/O never runs inside the scheduler's pending-op mutex.
// The capture producer submits at most eight jobs, bounding retained buffers.
class CaptureWriter {
public:
    CaptureWriter() : worker([this] { Run(); }) {}
    ~CaptureWriter() {
        { std::scoped_lock lock(mutex); closing=true; }
        ready.notify_one();
        worker.join(); // Drain queued writes before their Vulkan allocator dies.
    }
    bool Submit(std::function<void()> job) {
        {
            std::scoped_lock lock(mutex);
            if(closing || pending==8) return false;
            jobs.push(std::move(job));++pending;
        }
        ready.notify_one();
        return true;
    }
private:
    void Run() {
        for(;;) {
            std::function<void()> job;
            {
                std::unique_lock lock(mutex);
                ready.wait(lock,[this] { return closing || !jobs.empty(); });
                if(jobs.empty()) return;
                job=std::move(jobs.front());jobs.pop();
            }
            try { job(); }
            catch(const std::exception& error) { std::fprintf(stderr,"GoW3 capture writer: %s\n",error.what()); }
            catch(...) { std::fputs("GoW3 capture writer: write failed\n",stderr); }
            job={}; // Release the readback before reporting the slot free.
            { std::scoped_lock lock(mutex);--pending; }
        }
    }
    std::mutex mutex;
    std::condition_variable ready;
    std::queue<std::function<void()>> jobs;
    unsigned pending{};
    bool closing{};
    std::thread worker;
};
}
