#include <cassert>
#include <thread>
#include "dsp/frame.hpp"

using namespace famidec;

int main() {
    TripleBuffer tb;
    tb.resize(16, 8);
    std::thread producer([&] {
        for (uint64_t seq = 1; seq <= 10000; ++seq) {
            Frame& f = tb.back();
            f.rgba[0] = static_cast<uint32_t>(seq);
            tb.publish(seq);
        }
    });
    uint64_t last = 0;
    for (int i = 0; i < 20000; ++i) {
        if (const Frame* f = tb.acquire()) {
            assert(f->width == 16 && f->height == 8);
            assert(f->seq >= last);
            last = f->seq;
        }
        if (last == 10000) break;
        std::this_thread::yield();
    }
    producer.join();
    assert(last > 0);
    return 0;
}
