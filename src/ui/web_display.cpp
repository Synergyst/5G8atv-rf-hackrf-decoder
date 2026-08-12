// WebDisplay — Web GUI mode for fpvdec
// Serves browser UI with real-time video frames (as JPEG) and decoder stats.

#include "web_display.hpp"
#include "../config_store.hpp"

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
static std::string json_bool(const char* k, bool v) {
    return std::string("  \"") + k + "\": " + (v ? "true" : "false") + ",\n";
}
static std::string json_int(const char* k, int v) {
    return std::string("  \"") + k + "\": " + std::to_string(v) + ",\n";
}
static std::string json_double(const char* k, double v) {
    // %g without limiting precision — avoids scientific notation on large
    // values (e.g. 5865000000 would become 5.87e+09 with %.3g).
    char buf[128];
    std::snprintf(buf, sizeof(buf), "%.0f", v);
    // Only append .0 for integer-valued doubles (carrier freq, sample rate),
    // use %g for everything else.
    char tmp[64];
    std::snprintf(tmp, sizeof(tmp), "%.17g", v);
    // Check if the %g form contains 'e'/'E' (scientific notation)
    if (strchr(tmp, 'e') || strchr(tmp, 'E')) {
        // It was large — use %.0f to avoid scientific notation
        std::snprintf(buf, sizeof(buf), "%.0f", v);
    } else {
        // Small number — %g is fine
        std::snprintf(buf, sizeof(buf), "%g", v);
    }
    return std::string("  \"") + k + "\": " + buf + ",\n";
}
static std::string json_float(const char* k, float v) {
    char buf[64];
    std::snprintf(buf, sizeof(buf), "%g", v);
    return std::string("  \"") + k + "\": " + buf + ",\n";
}

static std::string config_to_json(const Config& cfg) {
    std::vector<std::string> fields;
    auto push = [&](std::string s) {
        // Strip trailing comma+newline, add to vector
        while (!s.empty() && (s.back() == '\n' || s.back() == ',' || s.back() == ' '))
            s.pop_back();
        if (!s.empty()) fields.push_back(s);
    };
    push(json_double("video_carrier_hz", cfg.video_carrier_hz));
    push(json_double("sample_rate", cfg.sample_rate));
    push(json_int("sample_bits", cfg.sample_bits));
    push(json_double("offset_hz", cfg.offset_hz));
    push(json_int("lna_gain", cfg.lna_gain));
    push(json_int("vga_gain", cfg.vga_gain));
    push(json_bool("amp", cfg.amp));
    push(json_bool("gain_auto", cfg.gain_auto));
    push(json_double("fm_dev_hz", cfg.fm_dev_hz));
    push(json_bool("invert", cfg.invert));
    push(json_bool("afc", cfg.afc));
    push(json_double("video_lpf_hz", cfg.video_lpf_hz));
    push(json_float("saturation", cfg.saturation));
    push(json_float("hue_deg", cfg.hue_deg));
    push(json_float("denoise", cfg.denoise));
    push(json_float("denoise_temporal", cfg.denoise_temporal));
    push(json_int("denoise_temporal_median", cfg.denoise_temporal_median));
    push(json_float("denoise_temporal_median_strength", cfg.denoise_temporal_median_strength));
    push(json_float("overscan", cfg.overscan));
    push(json_int("frame_width", cfg.frame_width));
    push(json_int("frame_height", cfg.frame_height));
    push(json_bool("auto_detect", cfg.auto_detect));
    push(json_bool("clkout", cfg.clkout));
    push(json_bool("enforce_clkin", cfg.enforce_clkin));

    std::string j = "{\n";
    for (size_t i = 0; i < fields.size(); ++i) {
        j += "  " + fields[i] + (i + 1 < fields.size() ? "," : "") + "\n";
    }
    j += "}";
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
        if (std::fread(out.data(), 1, out.size(), tmp) != out.size()) {
            out.clear();
        }
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
    request_quit();
    if (server_thread_.joinable()) server_thread_.join();
}

#ifdef HAVE_WEBGUI
bool WebDisplay::init(int port, const std::string& /* title */) {
    port_ = port;
    quit_requested_.store(false);
    restart_requested_.store(false);
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

void WebDisplay::request_quit() {
    quit_requested_.store(true);
#ifdef HAVE_WEBGUI
    std::lock_guard<std::mutex> lock(server_mutex_);
    if (server_) server_->stop();
#endif
}
void WebDisplay::set_target_fps(int fps) { target_fps_ = fps; }

std::string WebDisplay::get_url() const {
    return "http://localhost:" + std::to_string(port_) + "/";
}

#ifdef HAVE_WEBGUI
void WebDisplay::server_thread_func() {
    auto server = std::make_unique<httplib::Server>();
    {
        std::lock_guard<std::mutex> lock(server_mutex_);
        server_ = std::move(server);
    }
    httplib::Server& svr = *server_;

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
        std::string channel = s.channel;
        json_escape(channel);
        ss << "\"channel\":\"" << channel << "\",";
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
        const Config snapshot = runtime_ ? runtime_->snapshot() : *cfg_;
        res.set_content(config_to_json(snapshot), "application/json");
    });

    // ── Minimal JSON parser ────────────────────────────────────────────────
    // Recursive-descent parser for JSON objects. Returns {ok, errors, fields}.
    // Only supports: object, string, number, boolean, null.
    struct JsonField {
        std::string key;
        bool is_bool = false;
        bool bool_val = false;
        double num_val = 0.0;
        std::string str_val;
    };

    auto parse_json = [&](const std::string& b) -> std::pair<bool, std::vector<JsonField>> {
        std::vector<JsonField> fields;
        std::string error;
        size_t p = 0;
        const char* src = b.c_str();
        size_t len = b.size();

        auto skip_ws = [&]() {
            while (p < len && (src[p] == ' ' || src[p] == '\t' || src[p] == '\n' || src[p] == '\r')) ++p;
        };

        auto parse_string = [&](std::string& out) -> bool {
            if (p >= len || src[p] != '"') { error = "Expected string"; return false; }
            ++p; out.clear();
            while (p < len) {
                char c = src[p];
                if (c == '"') { ++p; return true; }
                if (c == '\\') {
                    ++p;
                    if (p < len) out += src[p];
                    else { error = "Unterminated escape"; return false; }
                } else out += c;
                ++p;
            }
            error = "Unterminated string"; return false;
        };

        auto parse_number = [&](double& out) -> bool {
            const char* start = src + p;
            char* end;
            out = std::strtod(start, &end);
            if (end == start) { error = "Bad number"; return false; }
            p = end - src;
            return true;
        };

        auto parse_value = [&]() -> std::optional<JsonField> {
            skip_ws();
            if (p >= len) { error = "Unexpected end"; return std::nullopt; }
            JsonField f;
            char c = src[p];
            if (c == '"') {
                if (!parse_string(f.str_val)) return std::nullopt;
            } else if (c == 't') {
                if (p + 4 > len || b.substr(p, 4) != "true") { error = "Expected 'true'"; return std::nullopt; }
                p += 4; f.is_bool = true; f.bool_val = true;
            } else if (c == 'f') {
                if (p + 5 > len || b.substr(p, 5) != "false") { error = "Expected 'false'"; return std::nullopt; }
                p += 5; f.is_bool = true; f.bool_val = false;
            } else if (c == 'n') {
                if (p + 4 > len || b.substr(p, 4) != "null") { error = "Expected 'null'"; return std::nullopt; }
                p += 4;
            } else if (c == '-' || (c >= '0' && c <= '9')) {
                if (!parse_number(f.num_val)) return std::nullopt;
            } else { error = std::string("Unexpected '") + c + "'"; return std::nullopt; }
            return f;
        };

        // Parse object: { key: value, ... }
        skip_ws();
        if (p >= len || src[p] != '{') { error = "Expected '{' at start"; return {false, {}}; }
        ++p;
        skip_ws();
        while (p < len) {
            if (src[p] == '}') {
                ++p;
                skip_ws();
                if (p != len) { error = "Trailing data"; return {false, {}}; }
                return {true, fields};
            }
            if (src[p] == ',') { ++p; skip_ws(); continue; }
            // Parse key
            skip_ws();
            if (p >= len) { error = "Unexpected end in object"; return {false, {}}; }
            JsonField key;
            if (!parse_string(key.str_val)) return {false, {}};
            // Expect colon
            skip_ws();
            if (p >= len || src[p] != ':') { error = "Expected ':' after key '" + key.str_val + "'"; return {false, {}}; }
            ++p;
            // Parse value
            auto val = parse_value();
            if (!val) return {false, {}};
            key.key = key.str_val;
            key.is_bool = val->is_bool;
            if (val->is_bool) {
                key.bool_val = val->bool_val;
            } else {
                key.num_val = val->num_val;
            }
            fields.push_back(key);
            skip_ws();
        }
        error = "Unexpected end in object";
        return {false, {}};
    };

    // ── /api/config/reset → restore startup baseline (including CLI overrides) ──
    svr.Post("/api/config/reset", [this](const httplib::Request&, httplib::Response& res) {
        if (!cfg_ || !reset_cfg_) {
            res.set_content("{\"ok\":false,\"error\":\"no reset baseline\"}", "application/json");
            res.status = 503;
            return;
        }
        if (runtime_) runtime_->with_config([this](Config& c) { c = *reset_cfg_; });
        else *cfg_ = *reset_cfg_;
        auto push_bool = [&](ConfigChangeType type, bool value) {
            if (!config_queue_) return;
            ConfigChangeEvent event{}; event.type = type; event.val.bool_val = value; config_queue_->push(event);
        };
        auto push_int = [&](ConfigChangeType type, int value) {
            if (!config_queue_) return;
            ConfigChangeEvent event{}; event.type = type; event.val.int_val = value; config_queue_->push(event);
        };
        auto push_double = [&](ConfigChangeType type, double value) {
            if (!config_queue_) return;
            ConfigChangeEvent event{}; event.type = type; event.val.dbl_val = value; config_queue_->push(event);
        };
        auto push_float = [&](ConfigChangeType type, float value) {
            if (!config_queue_) return;
            ConfigChangeEvent event{}; event.type = type; event.val.flt_val = value; config_queue_->push(event);
        };
        push_double(CFG_FM_DEV, reset_cfg_->fm_dev_hz);
        push_bool(CFG_INVERT, reset_cfg_->invert);
        push_double(CFG_VIDEO_LPF, reset_cfg_->video_lpf_hz);
        push_bool(CFG_AFC, reset_cfg_->afc);
        push_float(CFG_SATURATION, reset_cfg_->saturation);
        push_float(CFG_HUE_DEG, reset_cfg_->hue_deg);
        push_float(CFG_OVERSCAN, reset_cfg_->overscan);
        push_float(CFG_DENOISE, reset_cfg_->denoise);
        push_float(CFG_DENOISE_TEMPORAL, reset_cfg_->denoise_temporal);
        push_int(CFG_DENOISE_MEDIAN, reset_cfg_->denoise_temporal_median);
        push_float(CFG_DENOINE_MEDIAN_STRENGTH, reset_cfg_->denoise_temporal_median_strength);
        push_double(CFG_SAMPLE_RATE, reset_cfg_->sample_rate);
        push_int(CFG_SAMPLE_BITS, reset_cfg_->sample_bits);
        push_double(CFG_VIDEO_CARRIER, reset_cfg_->video_carrier_hz);
        push_double(CFG_OFFSET_HZ, reset_cfg_->offset_hz);
        push_bool(CFG_GAIN_AUTO, reset_cfg_->gain_auto);
        push_int(CFG_LNA_GAIN, reset_cfg_->lna_gain);
        push_int(CFG_VGA_GAIN, reset_cfg_->vga_gain);
        push_bool(CFG_AMP, reset_cfg_->amp);
        push_int(CFG_FRAME_WIDTH, reset_cfg_->frame_width);
        push_int(CFG_FRAME_HEIGHT, reset_cfg_->frame_height);
        push_bool(CFG_AUTO_DETECT, reset_cfg_->auto_detect);
        push_bool(CFG_CLkout, reset_cfg_->clkout);
        push_bool(CFG_ENFORCE_CLKIN, reset_cfg_->enforce_clkin);
        bool persisted = save_config_file(*reset_cfg_, reset_cfg_->config_path);
        request_restart();
        std::printf("WebGUI: reset to startup baseline; full restart requested (persisted=%d)\n", persisted);
        std::fflush(stdout);
        res.set_content(std::string("{\"ok\":true,\"restart_queued\":true,\"persisted\":") +
                        (persisted ? "true" : "false") + "}", "application/json");
    });

    // ── /api/config/set → apply full or partial Config JSON ──
    svr.Post("/api/config/set", [this, parse_json = std::move(parse_json)](const httplib::Request& req, httplib::Response& res) {
        if (!cfg_) {
            res.set_content("{\"ok\":false,\"error\":\"no config\"}", "application/json");
            res.status = 503;
            return;
        }
        Config pending = runtime_ ? runtime_->snapshot() : *cfg_;
        const std::string& body = req.body;
        if (body.empty()) {
            res.set_content("{\"ok\":false,\"error\":\"empty body\"}", "application/json");
            res.status = 400;
            return;
        }

        auto [ok, fields] = parse_json(body);
        if (!ok) {
            std::string resp = "{\"ok\":false,\"error\":\"Parse error\"}";
            res.set_content(resp, "application/json");
            res.status = 400;
            return;
        }

        // Find field by key name (exact match, no substring ambiguity).
        auto find_field = [&](const std::string& key) -> const JsonField* {
            for (auto& f : fields) if (f.key == key) return &f;
            return nullptr;
        };

        auto get_bool_field = [&](const std::string& key) -> std::optional<bool> {
            auto* f = find_field(key);
            if (f && f->is_bool) return f->bool_val;
            return std::nullopt;
        };
        auto get_num_field = [&](const std::string& key) -> std::optional<double> {
            auto* f = find_field(key);
            if (f && !f->is_bool) return f->num_val;
            return std::nullopt;
        };

        auto push_bool = [&](ConfigChangeType type, bool value) {
            if (!config_queue_) return;
            ConfigChangeEvent event{};
            event.type = type;
            event.val.bool_val = value;
            if (runtime_) runtime_->submit(event); else config_queue_->push(event);
        };
        auto push_int = [&](ConfigChangeType type, int value) {
            if (!config_queue_) return;
            ConfigChangeEvent event{};
            event.type = type;
            event.val.int_val = value;
            if (runtime_) runtime_->submit(event); else config_queue_->push(event);
        };
        auto push_double = [&](ConfigChangeType type, double value) {
            if (!config_queue_) return;
            ConfigChangeEvent event{};
            event.type = type;
            event.val.dbl_val = value;
            if (runtime_) runtime_->submit(event); else config_queue_->push(event);
        };
        auto push_float = [&](ConfigChangeType type, float value) {
            if (!config_queue_) return;
            ConfigChangeEvent event{};
            event.type = type;
            event.val.flt_val = value;
            if (runtime_) runtime_->submit(event); else config_queue_->push(event);
        };

        bool any_change = false;

        if (auto v = get_bool_field("afc")) {
            pending.afc = *v; any_change = true; push_bool(CFG_AFC, pending.afc);
            std::printf("WebGUI: afc=%s\n", *v ? "true" : "false");
        }
        if (auto v = get_bool_field("invert")) {
            pending.invert = *v; any_change = true; push_bool(CFG_INVERT, pending.invert);
            std::printf("WebGUI: invert=%s\n", *v ? "true" : "false");
        }
        if (auto v = get_bool_field("gain_auto")) {
            pending.gain_auto = *v; any_change = true; push_bool(CFG_GAIN_AUTO, pending.gain_auto);
            std::printf("WebGUI: gain_auto=%s\n", *v ? "true" : "false");
        }
        if (auto v = get_bool_field("amp")) {
            pending.amp = *v; any_change = true; push_bool(CFG_AMP, pending.amp);
            std::printf("WebGUI: amp=%s\n", *v ? "true" : "false");
        }
        if (auto v = get_bool_field("auto_detect")) {
            pending.auto_detect = *v; any_change = true; push_bool(CFG_AUTO_DETECT, pending.auto_detect);
            std::printf("WebGUI: auto_detect=%s\n", *v ? "true" : "false");
        }
        if (auto v = get_bool_field("clkout")) {
            pending.clkout = *v; any_change = true; push_bool(CFG_CLkout, pending.clkout);
            std::printf("WebGUI: clkout=%s\n", *v ? "true" : "false");
        }
        if (auto v = get_bool_field("enforce_clkin")) {
            pending.enforce_clkin = *v; any_change = true; push_bool(CFG_ENFORCE_CLKIN, pending.enforce_clkin);
            std::printf("WebGUI: enforce_clkin=%s\n", *v ? "true" : "false");
        }

        if (auto v = get_num_field("lna_gain")) {
            pending.lna_gain = std::clamp((static_cast<int>(*v) / 8) * 8, 0, 40);
            any_change = true; push_int(CFG_LNA_GAIN, pending.lna_gain);
            std::printf("WebGUI: lna_gain=%d (raw=%.0f)\n", pending.lna_gain, *v);
        }
        if (auto v = get_num_field("vga_gain")) {
            pending.vga_gain = std::clamp((static_cast<int>(*v) / 2) * 2, 0, 62);
            any_change = true; push_int(CFG_VGA_GAIN, pending.vga_gain);
            std::printf("WebGUI: vga_gain=%d (raw=%.0f)\n", pending.vga_gain, *v);
        }
        if (auto v = get_num_field("denoise_temporal_median")) {
            int n = static_cast<int>(*v);
            if (n <= 0) n = 0;
            else { if (n % 2 == 0) ++n; n = std::clamp(n, 3, 9); }
            pending.denoise_temporal_median = n;
            any_change = true; push_int(CFG_DENOISE_MEDIAN, n);
            std::printf("WebGUI: denoise_temporal_median=%d (raw=%.0f)\n", n, *v);
        }
        if (auto v = get_num_field("frame_width")) {
            pending.frame_width = std::clamp(static_cast<int>(*v), 320, 1920);
            any_change = true; push_int(CFG_FRAME_WIDTH, pending.frame_width);
            std::printf("WebGUI: frame_width=%d (raw=%.0f)\n", pending.frame_width, *v);
        }
        if (auto v = get_num_field("frame_height")) {
            pending.frame_height = std::clamp(static_cast<int>(*v), 240, 1080);
            any_change = true; push_int(CFG_FRAME_HEIGHT, pending.frame_height);
            std::printf("WebGUI: frame_height=%d (raw=%.0f)\n", pending.frame_height, *v);
        }

        if (auto v = get_num_field("video_carrier_hz")) {
            pending.video_carrier_hz = std::clamp(*v, 5.6e9, 6.0e9);
            any_change = true; push_double(CFG_VIDEO_CARRIER, pending.video_carrier_hz);
            std::printf("WebGUI: video_carrier_hz=%.3f MHz (raw=%.0f)\n", pending.video_carrier_hz / 1e6, *v);
        }
        if (auto v = get_num_field("offset_hz")) {
            pending.offset_hz = std::clamp(*v, -2e6, 2e6);
            any_change = true; push_double(CFG_OFFSET_HZ, pending.offset_hz);
            std::printf("WebGUI: offset_hz=%.0f Hz (raw=%.0f)\n", pending.offset_hz, *v);
        }
        if (auto v = get_num_field("sample_rate")) {
            pending.sample_rate = std::clamp(*v, 6e6, 20e6);
            any_change = true; push_double(CFG_SAMPLE_RATE, pending.sample_rate);
            std::printf("WebGUI: sample_rate=%.1f MSPS (raw=%.0f)\n", pending.sample_rate / 1e6, *v);
        }
        if (auto v = get_num_field("sample_bits")) {
            int bits = static_cast<int>(*v);
            if (bits == 8 || bits == 16) {
                pending.sample_bits = bits;
                any_change = true;
                push_int(CFG_SAMPLE_BITS, pending.sample_bits);
                std::printf("WebGUI: sample_bits=%d\n", pending.sample_bits);
            }
        }
        if (auto v = get_num_field("fm_dev_hz")) {
            pending.fm_dev_hz = std::clamp(*v, 1e6, 10e6);
            any_change = true; push_double(CFG_FM_DEV, pending.fm_dev_hz);
            std::printf("WebGUI: fm_dev_hz=%.1f MHz (raw=%.0f)\n", pending.fm_dev_hz / 1e6, *v);
        }
        if (auto v = get_num_field("video_lpf_hz")) {
            pending.video_lpf_hz = std::clamp(*v, 0.0, 6e6);
            any_change = true; push_double(CFG_VIDEO_LPF, pending.video_lpf_hz);
            std::printf("WebGUI: video_lpf_hz=%.1f MHz (raw=%.0f)\n", pending.video_lpf_hz / 1e6, *v);
        }

        if (auto v = get_num_field("saturation")) {
            pending.saturation = std::clamp(static_cast<float>(*v), 0.0f, 2.0f);
            any_change = true; push_float(CFG_SATURATION, pending.saturation);
            std::printf("WebGUI: saturation=%.2f (raw=%.4g)\n", pending.saturation, *v);
        }
        if (auto v = get_num_field("hue_deg")) {
            pending.hue_deg = std::clamp(static_cast<float>(*v), -180.0f, 180.0f);
            any_change = true; push_float(CFG_HUE_DEG, pending.hue_deg);
            std::printf("WebGUI: hue_deg=%.1f (raw=%.4g)\n", pending.hue_deg, *v);
        }
        if (auto v = get_num_field("denoise")) {
            pending.denoise = std::clamp(static_cast<float>(*v), 0.0f, 1.0f);
            any_change = true; push_float(CFG_DENOISE, pending.denoise);
            std::printf("WebGUI: denoise=%.2f (raw=%.4g)\n", pending.denoise, *v);
        }
        if (auto v = get_num_field("denoise_temporal")) {
            pending.denoise_temporal = std::clamp(static_cast<float>(*v), 0.0f, 1.0f);
            any_change = true; push_float(CFG_DENOISE_TEMPORAL, pending.denoise_temporal);
            std::printf("WebGUI: denoise_temporal=%.2f (raw=%.4g)\n", pending.denoise_temporal, *v);
        }
        if (auto v = get_num_field("denoise_temporal_median_strength")) {
            pending.denoise_temporal_median_strength = std::clamp(static_cast<float>(*v), 0.0f, 1.0f);
            any_change = true; push_float(CFG_DENOINE_MEDIAN_STRENGTH, pending.denoise_temporal_median_strength);
            std::printf("WebGUI: denoise_temporal_median_strength=%.2f (raw=%.4g)\n", pending.denoise_temporal_median_strength, *v);
        }
        if (auto v = get_num_field("overscan")) {
            pending.overscan = std::clamp(static_cast<float>(*v), 0.0f, 0.15f);
            any_change = true; push_float(CFG_OVERSCAN, pending.overscan);
            std::printf("WebGUI: overscan=%.2f (raw=%.4g)\n", pending.overscan, *v);
        }

        // Apply hardware changes
        if (any_change && source_) {
            source_->set_center_freq(pending.center_hz());
            source_->set_gains(pending.lna_gain, pending.vga_gain);
            source_->set_amp(pending.amp);
        }

        if (any_change) request_restart();

        bool persisted = !any_change || save_config_file(pending, pending.config_path);
        std::printf("WebGUI: config set (changed=%d, persisted=%d, body=%zu bytes)\n", any_change, persisted, body.size());
        std::fflush(stdout);

        std::string response = std::string("{\"ok\":") + (any_change ? "true" : "false") +
                               ",\"changed\":" + (any_change ? "true" : "false") +
                               ",\"persisted\":" + (persisted ? "true" : "false") + "}";
        res.set_content(response, "application/json");
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
        running_.store(false);
        return;
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
    Config pending = runtime_ ? runtime_->snapshot() : *cfg_;

    auto push_bool = [&](ConfigChangeType type, bool v) {
        if (!config_queue_) return;
        ConfigChangeEvent event{};
        event.type = type;
        event.val.bool_val = v;
        config_queue_->push(event);
    };
    auto push_int = [&](ConfigChangeType type, int v) {
        if (!config_queue_) return;
        ConfigChangeEvent event{};
        event.type = type;
        event.val.int_val = v;
        config_queue_->push(event);
    };
    auto push_double = [&](ConfigChangeType type, double v) {
        if (!config_queue_) return;
        ConfigChangeEvent event{};
        event.type = type;
        event.val.dbl_val = v;
        config_queue_->push(event);
    };
    auto push_float = [&](ConfigChangeType type, float v) {
        if (!config_queue_) return;
        ConfigChangeEvent event{};
        event.type = type;
        event.val.flt_val = v;
        config_queue_->push(event);
    };

    if (key == "saturation") {
        pending.saturation = std::clamp(static_cast<float>(std::atof(value.c_str())), 0.0f, 2.0f);
        push_float(CFG_SATURATION, pending.saturation);
    } else if (key == "hue_deg") {
        pending.hue_deg = std::clamp(static_cast<float>(std::atof(value.c_str())), -180.0f, 180.0f);
        push_float(CFG_HUE_DEG, pending.hue_deg);
    } else if (key == "denoise") {
        pending.denoise = std::clamp(static_cast<float>(std::atof(value.c_str())), 0.0f, 1.0f);
        push_float(CFG_DENOISE, pending.denoise);
    } else if (key == "denoise_temporal") {
        pending.denoise_temporal = std::clamp(static_cast<float>(std::atof(value.c_str())), 0.0f, 1.0f);
        push_float(CFG_DENOISE_TEMPORAL, pending.denoise_temporal);
    } else if (key == "denoise_temporal_median") {
        int n = std::atoi(value.c_str());
        if (n <= 0) n = 0;
        else { if (n % 2 == 0) ++n; n = std::clamp(n, 3, 9); }
        pending.denoise_temporal_median = n;
        push_int(CFG_DENOISE_MEDIAN, n);
    } else if (key == "channel") {
        double hz = 0.0;
        if (fpv_channel_freq(value, &hz)) {
            pending.video_carrier_hz = hz;
            if (source_) source_->set_center_freq(pending.center_hz());
            push_double(CFG_VIDEO_CARRIER, pending.video_carrier_hz);
        }
    } else if (key == "offset") {
        pending.offset_hz += std::atof(value.c_str());
        if (source_) source_->set_center_freq(pending.center_hz());
        push_double(CFG_OFFSET_HZ, pending.offset_hz);
    } else if (key == "gain") {
        if (value == "auto") pending.gain_auto = true;
        else if (value == "manual") pending.gain_auto = false;
        else return;
        push_bool(CFG_GAIN_AUTO, pending.gain_auto);
        if (source_ && pending.gain_auto) source_->set_gains(pending.lna_gain, pending.vga_gain);
    } else if (key == "lna") {
        pending.lna_gain = std::clamp((std::atoi(value.c_str()) / 8) * 8, 0, 40);
        if (source_) source_->set_gains(pending.lna_gain, pending.vga_gain);
        push_int(CFG_LNA_GAIN, pending.lna_gain);
    } else if (key == "vga") {
        pending.vga_gain = std::clamp((std::atoi(value.c_str()) / 2) * 2, 0, 62);
        if (source_) source_->set_gains(pending.lna_gain, pending.vga_gain);
        push_int(CFG_VGA_GAIN, pending.vga_gain);
    } else if (key == "amp") {
        pending.amp = value == "on" || value == "true" || value == "1";
        if (source_) source_->set_amp(pending.amp);
        push_bool(CFG_AMP, pending.amp);
    } else {
        std::fprintf(stderr, "WebGUI: unknown key '%s' ignored\n", key.c_str());
        return;
    }

    save_config_file(pending, pending.config_path);
    request_restart();
    std::printf("Web config: %s = %s\n", key.c_str(), value.c_str());
    std::fflush(stdout);
}

} // namespace famidec
