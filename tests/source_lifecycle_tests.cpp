#include <cassert>
#include <cstdint>
#include <string>
#include "source/sample_source.hpp"

using namespace famidec;

class MockSource final : public ISampleSource {
public:
    bool start() override { ++starts; running = true; return true; }
    void stop() override { ++stops; running = false; }
    size_t read(uint8_t* buf, size_t len) override {
        if (!running) return 0;
        for (size_t i = 0; i < len; ++i) buf[i] = static_cast<uint8_t>(i);
        return len;
    }
    bool restart() override { stop(); return start(); }
    const std::string& error() const override { return error_text; }
    int starts = 0;
    int stops = 0;
    bool running = false;
    std::string error_text;
};

int main() {
    MockSource source;
    uint8_t bytes[8]{};
    assert(source.start());
    assert(source.read(bytes, sizeof(bytes)) == sizeof(bytes));
    assert(source.restart());
    assert(source.starts == 2);
    assert(source.stops == 1);
    source.stop();
    assert(source.read(bytes, sizeof(bytes)) == 0);
    assert(!source.failed());
    return 0;
}
