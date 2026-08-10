// WebDisplay — Web GUI mode for fpvdec
// Serves browser UI with real-time video frames (as JPEG) and decoder stats.

#include "web_display.hpp"

#include <cstdio>
#include <cstring>
#include <fstream>
#include <sstream>
#include <thread>
#include <algorithm>
#include <cstdlib>

#ifdef HAVE_WEBGUI
#include "httplib.h"
#include <jpeglib.h>
#include <jerror.h>
#endif

namespace famidec {

#ifdef HAVE_WEBGUI

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
    std::printf("Web config: %s = %s\n", key.c_str(), value.c_str());
    std::fflush(stdout);
}

} // namespace famidec
