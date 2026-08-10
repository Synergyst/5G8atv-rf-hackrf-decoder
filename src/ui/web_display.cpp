// WebDisplay — Web GUI mode for fpvdec
// Serves browser UI with real-time video frames (as JPEG) and decoder stats.

#include "web_display.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <optional>
#include <sstream>
#include <thread>
#include <vector>

#include "../util/fpv_channels.hpp"  // fpv_channel_freq

#ifdef HAVE_WEBGUI
#include "httplib.h"
#include <jpeglib.h>
#include <jerror.h>
#endif

namespace famidec {
#ifdef HAVE_WEBGUI

// ── JSON helpers ─────────────────────────────────────────────────────────────

static void json_escape(std::string& s) {
    std::string out;
    out.reserve(s.size() + 8);
    for (char c : s) {
        if (c == '"')  { out += '\\'; out += '"'; }
        else if (c == '\\') { out += '\\'; out += '\\'; }
        else if (c == '\n') { out += '\\'; out += 'n'; }
        else if (c == '\r') { out += '\\'; out += 'r'; }
        else if (c == '\t') { out += '\\'; out += 't'; }
        else out += c;
    }
    s = std::move(out);
}

// Minimal JSON builder — avoids pulling in a full JSON library.
static std::string json_obj_start() { return "{\n"; }
static std::string json_obj_end()   { return "\n}"; }
static std::string json_bool(const char* k, bool v) {
    return std::string("  \"") + k + "\": " + (v ? "true" : "false") + ",\n";
}
static std::string json_int(const char* k, int v) {
    return std::string("  \"") + k + "\": " + std::to_string(v) + ",\n";
}
static std::string json_uint(const char* k, uint64_t v) {
    return std::string("  \"") + k + "\": " + std::to_string(v) + ",\n";
}
static std::string json_double(const char* k, double v) {
    // Use %.3g to avoid trailing zeros while keeping precision.
    char buf[64];
    std::snprintf(buf, sizeof(buf), "%.3g", v);
    return std::string("  \"") + k + "\": " + buf + ",\n";
}
static std::string json_float(const char* k, float v) {
    char buf[64];
    std::snprintf(buf, sizeof(buf), "%.4g", v);
    return std::string("  \"") + k + "\": " + buf + ",\n";
}
static std::string json_str(const char* k, const std::string& v) {
    std::string esc(v);
    json_escape(esc);
    return std::string("  \"") + k + "\": \"" + esc + "\",\n";
}

static std::string config_to_json(const Config& cfg) {
    std::string j;
    j += json_obj_start();
    // RF
    j += json_double("video_carrier_hz", cfg.video_carrier_hz);
    j += json_double("sample_rate", cfg.sample_rate);
    j += json_double("offset_hz", cfg.offset_hz);
    j += json_int("lna_gain", cfg.lna_gain);
    j += json_int("vga_gain", cfg.vga_gain);
    j += json_bool("amp", cfg.amp);
    j += json_bool("gain_auto", cfg.gain_auto);
    // FM/Video
    j += json_double("fm_dev_hz", cfg.fm_dev_hz);
    j += json_bool("invert", cfg.invert);
    j += json_bool("afc", cfg.afc);
    j += json_double("video_lpf_hz", cfg.video_lpf_hz);
    // Color
    j += json_float("saturation", cfg.saturation);
    j += json_float("hue_deg", cfg.hue_deg);
    // Denoise
    j += json_float("denoise", cfg.denoise);
    j += json_float("denoise_temporal", cfg.denoise_temporal);
    j += json_int("denoise_temporal_median", cfg.denoise_temporal_median);
    j += json_float("denoise_temporal_median_strength", cfg.denoise_temporal_median_strength);
    // Crop
    j += json_float("overscan", cfg.overscan);
    // Resolution
    j += json_int("frame_width", cfg.frame_width);
    j += json_int("frame_height", cfg.frame_height);
    // Auto-detect
    j += json_bool("auto_detect", cfg.auto_detect);
    // GPSDO
    j += json_bool("clkout", cfg.clkout);
    j += json_bool("enforce_clkin", cfg.enforce_clkin);
    j += json_obj_end();
    return j;
}

// ── JPEG encoding (libjpeg-turbo) via tmpfile ────────────────────────────────

static bool encode_frame_jpeg(const Frame& frame, int quality,
                              std::vector<uint8_t>& out) {
    out.clear();
    if (frame.width <= 0 || frame.height <= 0 || frame.rgba.empty()) return false;

    FILE* tmp = std::tmpfile();
    if (!tmp) return false;

    struct jpeg_compress_struct cinfo;
    struct jpeg_error_mgr jerr;

    cinfo.err = jpeg_std_error(&jerr);
    jpeg_create_compress(&cinfo);
    jpeg_stdio_dest(&cinfo, tmp);

    cinfo.image_width = static_cast<JDIMENSION>(frame.width);
    cinfo.image_height = static_cast<JDIMENSION>(frame.height);
    cinfo.input_components = 3;
    cinfo.in_color_space = JCS_RGB;

    jpeg_set_defaults(&cinfo);
    jpeg_set_quality(&cinfo, quality, true);

    JSAMPROW row[1];
    row[0] = (JSAMPLE*)std::malloc(static_cast<size_t>(frame.width) * 3);
    if (!row[0]) {
        jpeg_destroy_compress(&cinfo);
        std::fclose(tmp);
        return false;
    }

    jpeg_start_compress(&cinfo, TRUE);

    const uint8_t* src = reinterpret_cast<const uint8_t*>(frame.rgba.data());
    while (cinfo.next_scanline < (JDIMENSION)frame.height) {
        const uint8_t* px = src + static_cast<size_t>(cinfo.next_scanline) * 4 * frame.width;
        for (int x = 0; x < frame.width; ++x) {
            uint32_t p = *(const uint32_t*)(px + static_cast<size_t>(x) * 4);
            row[0][x * 3 + 0] = static_cast<JSAMPLE>((p >> 16) & 0xFF);
            row[0][x * 3 + 1] = static_cast<JSAMPLE>((p >> 8) & 0xFF);
            row[0][x * 3 + 2] = static_cast<JSAMPLE>(p & 0xFF);
        }
        jpeg_write_scanlines(&cinfo, row, 1);
    }

    jpeg_finish_compress(&cinfo);
    jpeg_destroy_compress(&cinfo);
    std::free(row[0]);

    long fsize = std::ftell(tmp);
    if (fsize > 0) {
        std::fseek(tmp, 0, SEEK_SET);
        out.resize(static_cast<size_t>(fsize));
        std::fread(out.data(), 1, out.size(), tmp);
    }
    std::fclose(tmp);
    return !out.empty();
}

// ── Serve static file helper ─────────────────────────────────────────────────

static void serve_static_file(httplib::Response& res, const std::string& path) {
    std::ifstream ifs(path, std::ios::binary);
    if (!ifs.is_open()) {
        res.set_content("Not Found", "text/plain");
        res.status = 404;
        return;
    }
    std::stringstream ss;
    ss << ifs.rdbuf();
    std::string content = ss.str();

    if (path.ends_with(".js")) res.set_content(content, "application/javascript");
    else if (path.ends_with(".wasm")) res.set_content(content, "application/wasm");
    else if (path.ends_with(".html")) res.set_content(content, "text/html; charset=utf-8");
    else if (path.ends_with(".css")) res.set_content(content, "text/css");
    else if (path.ends_with(".ttf")) res.set_content(content, "font/ttf");
    else if (path.ends_with(".data")) res.set_content(content, "application/octet-stream");
    else res.set_content(content, "application/octet-stream");
}

#endif  // HAVE_WEBGUI

// ── WebDisplay implementation ─────────────────────────────────────────────────

WebDisplay::WebDisplay() : port_(8080) {}

WebDisplay::~WebDisplay() {
    if (running_.load()) {
        running_.store(false);
        if (server_thread_.joinable()) {
            server_thread_.join();
        }
    }
}

#ifdef HAVE_WEBGUI
bool WebDisplay::init(int port, const std::string& /* title */) {
    port_ = port;
    quit_requested_.store(false);
    jpeg_quality_ = 75;

    std::printf("Web GUI starting on port %d\n", port_);
    std::printf("Open http://localhost:%d/ in your browser\n", port_);
    std::printf("Press Ctrl+C to stop\n");
    std::fflush(stdout);

    running_.store(true);
    server_thread_ = std::thread(&WebDisplay::server_thread_func, this);
    return true;
}
#endif  // HAVE_WEBGUI

#ifndef HAVE_WEBGUI
bool WebDisplay::init(int, const std::string&) {
    std::fprintf(stderr, "error: Web GUI not compiled in (rebuild with -DWEBGUI=ON)\n");
    return false;
}
#endif  // HAVE_WEBGUI

void WebDisplay::request_quit() { quit_requested_.store(true); }
void WebDisplay::set_target_fps(int fps) { target_fps_ = fps; }

std::string WebDisplay::get_url() const {
    return "http://localhost:" + std::to_string(port_) + "/";
}

#ifdef HAVE_WEBGUI
void WebDisplay::server_thread_func() {
    httplib::Server svr;

    // ── / → index.html ──
    svr.Get("/", [](const httplib::Request&, httplib::Response& res) {
        std::string path = "docs/webgui/index.html";
        std::ifstream ifs(path);
        if (!ifs.is_open()) {
            path = "index.html";
            ifs.open(path);
        }
        if (!ifs.is_open()) {
            res.set_content("<h1>fpvdec Web UI</h1><p>index.html not found.</p>", "text/html");
            res.status = 500;
            return;
        }
        std::stringstream ss;
        ss << ifs.rdbuf();
        res.set_content(ss.str(), "text/html; charset=utf-8");
        res.set_header("Cache-Control", "no-cache");
    });

    // ── /api/frame → JPEG ──
    svr.Get("/api/frame", [this](const httplib::Request&, httplib::Response& res) {
        std::lock_guard<std::mutex> lock(frame_mutex_);
        if (!current_frame_ || current_frame_->rgba.empty()) {
            res.set_content("", "image/jpeg");
            res.status = 503;
            return;
        }
        std::vector<uint8_t> jpeg;
        if (!encode_frame_jpeg(*current_frame_, jpeg_quality_, jpeg)) {
            res.set_content("JPEG failed", "text/plain");
            res.status = 500;
            return;
        }
        res.set_content(reinterpret_cast<const char*>(jpeg.data()), jpeg.size(), "image/jpeg");
        res.set_header("Cache-Control", "no-store, no-cache");
    });

    // ── /api/stats → JSON ──
    svr.Get("/api/stats", [this](const httplib::Request&, httplib::Response& res) {
        std::lock_guard<std::mutex> lock(frame_mutex_);
        if (!current_stats_) {
            res.set_content("{}", "application/json");
            res.status = 503;
            return;
        }
        const OsdStats& s = *current_stats_;
        std::ostringstream ss;
        ss << "{";
        ss << "\"line_locked\":" << (s.line_locked ? "true" : "false") << ",";
        ss << "\"vsync_locked\":" << (s.vsync_locked ? "true" : "false") << ",";
        ss << "\"burst_amp\":" << s.burst_amp << ",";
        ss << "\"ring_fill\":" << s.ring_fill << ",";
        ss << "\"dropped\":" << s.dropped << ",";
        ss << "\"clipped\":" << s.clipped << ",";
        ss << "\"frames\":" << s.frames << ",";
        ss << "\"lines\":" << s.lines << ",";
        ss << "\"lna\":" << s.lna << ",";
        ss << "\"vga\":" << s.vga << ",";
        ss << "\"amp\":" << (s.amp ? "true" : "false") << ",";
        ss << "\"gain_auto\":" << (s.gain_auto ? "true" : "false") << ",";
        ss << "\"clipping\":" << (s.clipping ? "true" : "false") << ",";
        ss << "\"clkin_locked\":" << (s.clkin_locked ? "true" : "false") << ",";
        ss << "\"freq_mhz\":" << s.freq_mhz << ",";
        ss << "\"channel\":\"" << s.channel << "\",";
        ss << "\"fps\":" << s.fps << ",";
        ss << "\"video_latency_ms\":" << s.video_latency_ms;
        ss << "}";
        res.set_content(ss.str(), "application/json");
    });

    // ── /api/config → JSON (full Config snapshot) ──
    svr.Get("/api/config", [this](const httplib::Request&, httplib::Response& res) {
        if (!cfg_) {
            res.set_content("{}", "application/json");
            res.status = 503;
            return;
        }
        res.set_content(config_to_json(*cfg_), "application/json");
    });

    // ── /api/config/set → apply full or partial Config JSON ──
    svr.Post("/api/config/set", [this](const httplib::Request& req, httplib::Response& res) {
        if (!cfg_) {
            res.set_content("{\"ok\":false,\"error\":\"no config\"}", "application/json");
            res.status = 503;
            return;
        }
        const std::string& body = req.body;
        if (body.empty()) {
            res.set_content("{\"ok\":false,\"error\":\"empty body\"}", "application/json");
            res.status = 400;
            return;
        }
        // Minimal JSON field extractor: parse key:value pairs from body.
        // Supports integers, floats, booleans, strings.
        auto extract_json_field = [&](const std::string& name) -> std::optional<std::string> {
            // Look for "name": value patterns
            std::string search = std::string("\"") + name + std::string("\"");
            auto pos = body.find(search);
            if (pos == std::string::npos) return std::nullopt;
            pos += search.size();
            // Skip whitespace and colon
            while (pos < body.size() && (body[pos] == ' ' || body[pos] == ':' || body[pos] == '\t')) ++pos;
            if (pos >= body.size()) return std::nullopt;
            std::string val;
            if (body[pos] == '"') {
                // String value
                ++pos;
                while (pos < body.size() && body[pos] != '"') {
                    if (body[pos] == '\\' && pos + 1 < body.size()) {
                        val += body[pos + 1];
                        ++pos;
                    } else {
                        val += body[pos];
                    }
                    ++pos;
                }
            } else {
                // Numeric or boolean value — read until comma, brace, or newline
                while (pos < body.size() && body[pos] != ',' && body[pos] != '}' && body[pos] != '\n') {
                    val += body[pos];
                    ++pos;
                }
            }
            return val;
        };

        // ── Boolean fields ──
        auto set_bool = [&](const std::string& name, bool& target) {
            auto v = extract_json_field(name);
            if (v && (*v == "true" || *v == "1")) target = true;
            else if (v && (*v == "false" || *v == "0")) target = false;
        };
        set_bool("afc", cfg_->afc);
        set_bool("invert", cfg_->invert);
        set_bool("gain_auto", cfg_->gain_auto);
        set_bool("amp", cfg_->amp);
        set_bool("auto_detect", cfg_->auto_detect);
        set_bool("clkout", cfg_->clkout);
        set_bool("enforce_clkin", cfg_->enforce_clkin);

        // ── Integer fields ──
        auto set_int = [&](const std::string& name, int& target) {
            auto v = extract_json_field(name);
            if (v) {
                int val = std::atoi(v->c_str());
                if (name == "lna_gain") cfg_->lna_gain = std::clamp((val / 8) * 8, 0, 40);
                else if (name == "vga_gain") cfg_->vga_gain = std::clamp((val / 2) * 2, 0, 62);
                else if (name == "denoise_temporal_median") {
                    int n = std::clamp(val, 3, 9);
                    if (n % 2 == 0) ++n;
                    cfg_->denoise_temporal_median = n;
                }
                else if (name == "frame_width") cfg_->frame_width = std::max(1, val);
                else if (name == "frame_height") cfg_->frame_height = std::max(1, val);
                else cfg_->frame_width = val; // fallback
            }
        };
        set_int("lna_gain", cfg_->lna_gain);
        set_int("vga_gain", cfg_->vga_gain);
        set_int("denoise_temporal_median", cfg_->denoise_temporal_median);
        set_int("frame_width", cfg_->frame_width);
        set_int("frame_height", cfg_->frame_height);

        // ── Double fields ──
        auto set_double = [&](const std::string& name, double& target) {
            auto v = extract_json_field(name);
            if (v) target = std::atof(v->c_str());
        };
        set_double("video_carrier_hz", cfg_->video_carrier_hz);
        set_double("offset_hz", cfg_->offset_hz);
        set_double("sample_rate", cfg_->sample_rate);
        set_double("fm_dev_hz", cfg_->fm_dev_hz);
        set_double("video_lpf_hz", cfg_->video_lpf_hz);

        // ── Float fields ──
        auto set_float = [&](const std::string& name, float& target) {
            auto v = extract_json_field(name);
            if (v) target = static_cast<float>(std::atof(v->c_str()));
        };
        set_float("saturation", cfg_->saturation);
        set_float("hue_deg", cfg_->hue_deg);
        set_float("denoise", cfg_->denoise);
        set_float("denoise_temporal", cfg_->denoise_temporal);
        set_float("denoise_temporal_median_strength", cfg_->denoise_temporal_median_strength);
        set_float("overscan", cfg_->overscan);

        // Clamp floats
        cfg_->saturation = std::clamp(cfg_->saturation, 0.0f, 2.0f);
        cfg_->denoise = std::clamp(cfg_->denoise, 0.0f, 1.0f);
        cfg_->denoise_temporal = std::clamp(cfg_->denoise_temporal, 0.0f, 1.0f);
        cfg_->denoise_temporal_median_strength = std::clamp(cfg_->denoise_temporal_median_strength, 0.0f, 1.0f);
        cfg_->overscan = std::clamp(cfg_->overscan, 0.0f, 0.15f);

        // ── Apply hardware changes ──
        if (source_) {
            source_->set_center_freq(cfg_->center_hz());
            source_->set_gains(cfg_->lna_gain, cfg_->vga_gain);
            source_->set_amp(cfg_->amp);
        }

        std::printf("WebGUI: full config set (%zu bytes)\n", body.size());
        std::fflush(stdout);

        res.set_content("{\"ok\":true}", "application/json");
    });

    // ── /api/set POST → config ──
    svr.Post("/api/set", [this](const httplib::Request& req, httplib::Response& res) {
        std::string key, value;

        auto get_param = [&req](const char* name) -> std::string {
            auto it = req.params.find(name);
            return (it != req.params.end()) ? it->second : "";
        };

        key = get_param("key");
        value = get_param("value");

        if (key.empty() && value.empty()) {
            std::string val = get_param("lna");
            if (!val.empty()) { key = "lna"; value = val; }
            else { val = get_param("vga"); if (!val.empty()) { key = "vga"; value = val; } }
            if (key.empty()) { val = get_param("gain"); if (!val.empty()) { key = "gain"; value = val; } }
            if (key.empty()) { val = get_param("amp"); if (!val.empty()) { key = "amp"; value = val; } }
            if (key.empty()) { val = get_param("channel"); if (!val.empty()) { key = "channel"; value = val; } }
            if (key.empty()) { val = get_param("offset"); if (!val.empty()) { key = "offset"; value = val; } }
        }

        if (key.empty() || value.empty()) {
            res.set_content("Need params", "text/plain");
            res.status = 400;
            return;
        }

        apply_config(key, value);
        res.set_content("OK", "text/plain");
    });

    // ── Static files ──
    svr.Get(R"(/imgui\.(js|wasm))", [](const httplib::Request& req, httplib::Response& res) {
        std::string ext = req.matches[1].str();
        serve_static_file(res, "third_party/webgui/imgui." + ext);
    });

    svr.Get("/data/(.*)", [](const httplib::Request& req, httplib::Response& res) {
        std::string fn = req.matches[1].str();
        serve_static_file(res, "third_party/webgui/data/" + fn);
    });

    svr.Get("/imgui.data", [](const httplib::Request&, httplib::Response& res) {
        serve_static_file(res, "third_party/webgui/imgui.data");
    });

    svr.Get("/*", [](const httplib::Request&, httplib::Response& res) {
        res.set_content("404", "text/plain");
        res.status = 404;
    });

    if (!svr.listen("0.0.0.0", port_)) {
        std::fprintf(stderr, "Failed to listen on port %d\n", port_);
        return;
    }
    while (running_.load() && !quit_requested_.load()) {
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
    running_.store(false);
}
#else  // HAVE_WEBGUI
void WebDisplay::server_thread_func() { /* no-op */ }
#endif  // HAVE_WEBGUI

// ── Frame / stats updates ─────────────────────────────────────────────────────

void WebDisplay::update_frame(const Frame* frame) {
    std::lock_guard<std::mutex> lock(frame_mutex_);
    if (frame) {
        current_frame_ = std::make_unique<Frame>();
        current_frame_->rgba = frame->rgba;
        current_frame_->width = frame->width;
        current_frame_->height = frame->height;
        current_frame_->seq = frame->seq;
    }
}

void WebDisplay::update_stats(const OsdStats& stats) {
    std::lock_guard<std::mutex> lock(frame_mutex_);
    auto now = std::chrono::steady_clock::now();
    auto elapsed = std::chrono::duration_cast<std::chrono::seconds>(
        now - last_stats_update_).count();
    if (!current_stats_ || elapsed > 1) {
        current_stats_ = std::make_unique<OsdStats>();
        *current_stats_ = stats;
        last_stats_update_ = now;
    }
}

void WebDisplay::apply_config(const std::string& key, const std::string& value) {
    if (!cfg_) {
        std::fprintf(stderr, "WebGUI: no config wired in\n");
        return;
    }
    auto set_str = [&](const char* field, const std::string& v) {
        // Write directly into cfg_ for fields that the main loop
        // already polls.
        if (std::strcmp(field, "saturation") == 0) {
            float f = std::atof(v.c_str());
            f = std::clamp(f, 0.0f, 2.0f);
            cfg_->saturation = f;
        } else if (std::strcmp(field, "hue_deg") == 0) {
            cfg_->hue_deg = std::atof(v.c_str());
        } else if (std::strcmp(field, "denoise") == 0) {
            float f = std::atof(v.c_str());
            cfg_->denoise = std::clamp(f, 0.0f, 1.0f);
        } else if (std::strcmp(field, "denoise_temporal") == 0) {
            float f = std::atof(v.c_str());
            cfg_->denoise_temporal = std::clamp(f, 0.0f, 1.0f);
        } else if (std::strcmp(field, "denoise_temporal_median") == 0) {
            int n = std::atoi(v.c_str());
            if (n % 2 == 0) ++n;  // ensure odd
            n = std::clamp(n, 3, 9);
            cfg_->denoise_temporal_median = n;
        } else {
            // Unknown field — ignore silently.
        }
    };

    // Try integer field first, then string/float.
    if (key == "saturation" || key == "hue_deg" || key == "denoise" ||
        key == "denoise_temporal" || key == "denoise_temporal_median") {
        set_str(key.c_str(), value);
        std::printf("Web config: %s = %s\n", key.c_str(), value.c_str());
        std::fflush(stdout);
        return;
    }

    // Channel: parse "F4" -> update carrier freq, push to source.
    if (key == "channel") {
        double hz = 0;
        if (fpv_channel_freq(value, &hz)) {
            cfg_->video_carrier_hz = hz;
            if (source_) {
                source_->set_center_freq(cfg_->center_hz());
            }
            std::printf("Web config: channel %s -> %.3f MHz\n",
                        value.c_str(), hz / 1e6);
            std::fflush(stdout);
            return;
        }
    }

    // Frequency tuning (offset in Hz).
    if (key == "offset") {
        double delta = std::atof(value.c_str());
        cfg_->offset_hz += delta;
        if (source_) {
            source_->set_center_freq(cfg_->center_hz());
        }
        std::printf("Web config: offset -> %.3f MHz\n",
                    cfg_->offset_hz / 1e6);
        std::fflush(stdout);
        return;
    }

    // Gain mode: "auto" or "manual".
    if (key == "gain") {
        if (value == "auto") {
            cfg_->gain_auto = true;
            if (source_) source_->set_gains(cfg_->lna_gain, cfg_->vga_gain);
        } else if (value == "manual") {
            cfg_->gain_auto = false;
        }
        std::printf("Web config: gain = %s\n", value.c_str());
        std::fflush(stdout);
        return;
    }

    // LNA (0-40, step 8).
    if (key == "lna") {
        int lna = std::atoi(value.c_str());
        lna = std::clamp((lna / 8) * 8, 0, 40);
        cfg_->lna_gain = lna;
        if (source_) source_->set_gains(lna, cfg_->vga_gain);
        std::printf("Web config: LNA = %d\n", lna);
        std::fflush(stdout);
        return;
    }

    // VGA (0-62, step 2).
    if (key == "vga") {
        int vga = std::atoi(value.c_str());
        vga = std::clamp((vga / 2) * 2, 0, 62);
        cfg_->vga_gain = vga;
        if (source_) source_->set_gains(cfg_->lna_gain, vga);
        std::printf("Web config: VGA = %d\n", vga);
        std::fflush(stdout);
        return;
    }

    // RF Amp toggle: "on" or "off".
    if (key == "amp") {
        cfg_->amp = (value == "on" || value == "true" || value == "1");
        if (source_) source_->set_amp(cfg_->amp);
        std::printf("Web config: amp = %s\n",
                    cfg_->amp ? "on" : "off");
        std::fflush(stdout);
        return;
    }

    std::fprintf(stderr, "WebGUI: unknown key '%s' ignored\n", key.c_str());
}

} // namespace famidec
