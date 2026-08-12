#include "runtime_coordinator.hpp"

#include <algorithm>
#include <cmath>
#include <complex>
#include <cstdio>
#include <vector>

#include "dsp/dc_blocker.hpp"
#include "dsp/fir.hpp"
#include "dsp/fm_detector.hpp"
#include "dsp/nco.hpp"
#include "source/file_source.hpp"
#include "source/hackrf_source.hpp"
#ifdef HAVE_SOAPYSDR
#include "source/soapy_source.hpp"
#endif
#ifdef HAVE_UHD
#include "source/uhd_source.hpp"
#endif

namespace famidec {

namespace {
void dsp_loop(ConfigChangeQueue* events, const Config& cfg, ISampleSource* src,
              NtscDecoder* dec, IRawRecorder* recorder,
              std::atomic<float>* mean_raw, std::atomic<bool>& running,
              RuntimeLifecycle& lifecycle) {
    constexpr size_t block_bytes = 1 << 16;
    const size_t bytes_per_sample =
        src->sample_format() == SampleFormat::CS16 ? 4 : 2;
    double sample_rate = cfg.sample_rate;
    std::vector<uint8_t> raw(block_bytes);
    std::vector<std::complex<float>> iq(block_bytes / 2);
    std::vector<float> composite(block_bytes / 2);
    DcBlocker dc;
    double offset = cfg.offset_hz;
    Nco mixer(offset, sample_rate);
    FirFilterC channel_filter(design_lowpass(
        std::min(8.0e6, sample_rate * 0.49), sample_rate, 47));
    double deviation = cfg.fm_dev_hz;
    bool invert = cfg.invert;
    FmDetector detector(sample_rate, deviation, invert);
    double video_lpf = cfg.video_lpf_hz;
    bool use_video_lpf = video_lpf > 0.0;
    FirFilterF video_filter(design_lowpass(
        use_video_lpf ? std::min(video_lpf, sample_rate * 0.45) : 1.0,
        sample_rate, 63));

    while (running.load(std::memory_order_relaxed)) {
        ConfigChangeEvent event;
        while (events && events->pop(event)) {
            switch (event.type) {
                case CFG_FM_DEV: deviation = event.val.dbl_val; lifecycle.request_restart(); break;
                case CFG_INVERT: invert = event.val.bool_val; lifecycle.request_restart(); break;
                case CFG_VIDEO_LPF: video_lpf = event.val.dbl_val; lifecycle.request_restart(); break;
                case CFG_SAMPLE_RATE: sample_rate = event.val.dbl_val; lifecycle.request_restart(); break;
                case CFG_SAMPLE_BITS: lifecycle.request_restart(); break;
                case CFG_OFFSET_HZ: offset = event.val.dbl_val; lifecycle.request_restart(); break;
                case CFG_FRAME_WIDTH:
                case CFG_FRAME_HEIGHT: lifecycle.request_restart(); break;
                case CFG_SATURATION: dec->set_saturation(event.val.flt_val); break;
                case CFG_HUE_DEG: dec->set_hue_deg(event.val.flt_val); break;
                case CFG_OVERSCAN: dec->set_overscan(event.val.flt_val); break;
                default: break;
            }
        }

        const size_t n = src->read(raw.data(), raw.size() - raw.size() % bytes_per_sample);
        if (n == 0) break;
        if (recorder) recorder->write(raw.data(), n);
        const size_t samples = n / bytes_per_sample;
        if (src->sample_format() == SampleFormat::CS16) {
            const auto* input = reinterpret_cast<const int16_t*>(raw.data());
            for (size_t i = 0; i < samples; ++i)
                iq[i] = dc.process({input[2 * i] / 32768.0f,
                                     input[2 * i + 1] / 32768.0f});
        } else {
            for (size_t i = 0; i < samples; ++i)
                iq[i] = dc.process({static_cast<int8_t>(raw[2 * i]) / 128.0f,
                                     static_cast<int8_t>(raw[2 * i + 1]) / 128.0f});
        }
        if (offset != 0.0)
            for (size_t i = 0; i < samples; ++i) iq[i] *= mixer.next();
        channel_filter.process(iq.data(), iq.data(), samples);
        detector.process(iq.data(), composite.data(), samples);
        if (use_video_lpf) video_filter.process(composite.data(), composite.data(), samples);
        float sum = 0.0f;
        for (size_t i = 0; i < samples; ++i) sum += composite[i];
        const float old_mean = mean_raw->load(std::memory_order_relaxed);
        mean_raw->store(0.97f * old_mean + 0.03f * sum / static_cast<float>(samples),
                        std::memory_order_relaxed);
        dec->process(composite.data(), samples);
    }
}
} // namespace

RuntimeCoordinator::RuntimeCoordinator(Config& cfg, RuntimeControl& control,
                                       RuntimeLifecycle& lifecycle,
                                       std::atomic<bool>& running)
    : cfg_(cfg), control_(control), lifecycle_(lifecycle), running_(running) {}

RuntimeCoordinator::~RuntimeCoordinator() {
    stop_dsp();
    stop_source();
}

bool RuntimeCoordinator::create_source(bool pace_file) {
    const Config snapshot = control_.snapshot();
    switch (snapshot.input) {
        case Config::Input::HackRF:
            source_ = std::make_unique<HackRfSource>(cfg_);
            break;
        case Config::Input::File:
            source_ = std::make_unique<FileSource>(cfg_, pace_file);
            break;
#ifdef HAVE_SOAPYSDR
        case Config::Input::SoapySDR:
            source_ = std::make_unique<SoapySource>(cfg_, snapshot.soapysdr_device_args);
            break;
#endif
#ifdef HAVE_UHD
        case Config::Input::UHD:
            source_ = std::make_unique<UhdSource>(cfg_);
            break;
#endif
        default:
            return false;
    }
    return source_ != nullptr;
}

bool RuntimeCoordinator::start_source() {
    return source_ && source_->start();
}

void RuntimeCoordinator::stop_source() {
    if (source_) source_->stop();
}

bool RuntimeCoordinator::start_dsp(NtscDecoder* decoder, IRawRecorder* recorder,
                                   std::atomic<float>* mean_raw,
                                   ConfigChangeQueue* events) {
    if (!source_ || !decoder || !mean_raw || dsp_.joinable()) return false;
    const Config cfg = control_.snapshot();
    dsp_ = std::thread(dsp_loop, events, std::cref(cfg), source_.get(), decoder,
                       recorder, mean_raw, std::ref(running_), std::ref(lifecycle_));
    return true;
}

void RuntimeCoordinator::stop_dsp() {
    running_.store(false, std::memory_order_relaxed);
    if (dsp_.joinable()) dsp_.join();
}

} // namespace famidec
