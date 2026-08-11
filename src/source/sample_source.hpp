#pragma once

#include <cstddef>
#include <cstdint>
#include <string>

namespace famidec {

// Interleaved complex IQ sample representation delivered by a source.
// CS8 is HackRF-compatible signed int8 I/Q; CS16 is signed int16 I/Q.
enum class SampleFormat {
    CS8,
    CS16,
};

inline const char* sample_format_name(SampleFormat format) {
    return format == SampleFormat::CS16 ? "CS16" : "CS8";
}

// Common pull-based interface for live SDR input and file playback.
class ISampleSource {
public:
    virtual ~ISampleSource() = default;
    virtual bool start() = 0;
    virtual void stop() = 0;
    // Pause/resume the data flow without destroying the device.
    virtual void pause() {}
    virtual void resume() {}
    // Blocking read of interleaved IQ bytes in sample_format(). Returns 0 at
    // end of stream or when the source has stopped.
    virtual size_t read(uint8_t* buf, size_t len) = 0;
    virtual SampleFormat sample_format() const { return SampleFormat::CS8; }

    virtual uint64_t dropped_bytes() const { return 0; }
    virtual uint64_t total_bytes() const { return 0; }
    virtual uint64_t buffered_bytes() const { return 0; }
    virtual uint64_t clipped_samples() const { return 0; }
    virtual float ring_fill() const { return 0.0f; }
    virtual const std::string& error() const { return empty_error_; }

    // Hardware control hooks. Backends may reject controls that do not map to
    // their hardware (for example a HackRF-style amp on a generic UHD radio).
    virtual bool set_center_freq(double /* center_hz */) { return false; }
    virtual bool set_gains(int /* lna */, int /* vga */) { return false; }
    virtual bool set_amp(bool /* on */) { return false; }

private:
    std::string empty_error_;
};

}  // namespace famidec
