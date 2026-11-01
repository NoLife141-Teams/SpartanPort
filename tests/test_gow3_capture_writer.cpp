// SPDX-License-Identifier: GPL-2.0-or-later
#include <atomic>
#include <cassert>
#include <cstdio>
#include <future>
#include <memory>
#include "gpu/shadps4/video_core/renderer_vulkan/gow3_capture_writer.h"

int main() {
    std::promise<void> entered,release;
    auto released=release.get_future().share();
    std::atomic<unsigned> written{};
    auto retained=std::make_shared<int>(42);
    std::weak_ptr<int> lifetime=retained;
    {
        Vulkan::Gow3::CaptureWriter writer;
        assert(writer.Submit([&,retained] {
            entered.set_value();released.wait();
            assert(*retained==42);++written;
        }));
        retained.reset();
        entered.get_future().wait();
        // A deliberately blocked disk job must not block the GPU's enqueue path.
        for(unsigned i=0;i<7;++i) assert(writer.Submit([&] {++written;}));
        assert(!writer.Submit([] {})); // Includes the in-flight buffer in the bound.
        assert(written==0 && !lifetime.expired());
        release.set_value();
    }
    assert(written==8 && lifetime.expired()); // Shutdown drains and releases all jobs.
    std::puts("GoW3 capture writer: nonblocking enqueue, bounded lifetime and drain passed");
}
