#include "config_store.hpp"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <fstream>
#include <optional>
#include <sstream>
#include <string>
#include <vector>

namespace famidec {
namespace {
struct Field {
    std::string key;
    bool is_bool = false;
    bool bool_value = false;
    double number = 0.0;
};

bool parse_object(const std::string& text, std::vector<Field>& fields) {
    size_t p = 0;
    auto ws = [&] { while (p < text.size() && std::isspace(static_cast<unsigned char>(text[p]))) ++p; };
    auto string = [&](std::string& out) {
        ws(); if (p >= text.size() || text[p++] != '"') return false;
        out.clear();
        while (p < text.size()) {
            char c = text[p++];
            if (c == '"') return true;
            if (c == '\\' && p < text.size()) c = text[p++];
            out += c;
        }
        return false;
    };
    auto number = [&](double& out) {
        ws(); char* end = nullptr; out = std::strtod(text.c_str() + p, &end);
        if (end == text.c_str() + p) return false;
        p = static_cast<size_t>(end - text.c_str()); return true;
    };
    ws(); if (p >= text.size() || text[p++] != '{') return false;
    ws();
    while (p < text.size() && text[p] != '}') {
        Field f;
        if (!string(f.key)) return false;
        ws(); if (p >= text.size() || text[p++] != ':') return false;
        ws();
        if (text.compare(p, 4, "true") == 0) { p += 4; f.is_bool = true; f.bool_value = true; }
        else if (text.compare(p, 5, "false") == 0) { p += 5; f.is_bool = true; f.bool_value = false; }
        else if (!number(f.number)) return false;
        fields.push_back(f);
        ws();
        if (p < text.size() && text[p] == ',') { ++p; ws(); }
        else if (p < text.size() && text[p] != '}') return false;
    }
    return p < text.size() && text[p] == '}';
}

void apply_fields(const std::vector<Field>& fields, Config& c) {
    for (const auto& f : fields) {
        auto n = [&](double lo, double hi) { return std::clamp(f.number, lo, hi); };
        if (f.key == "video_carrier_hz" && !f.is_bool) c.video_carrier_hz = n(5.6e9, 6.0e9);
        else if (f.key == "sample_rate" && !f.is_bool) c.sample_rate = n(6e6, 20e6);
        else if (f.key == "sample_bits" && !f.is_bool) { int v = static_cast<int>(f.number); if (v == 8 || v == 16) c.sample_bits = v; }
        else if (f.key == "offset_hz" && !f.is_bool) c.offset_hz = n(-2e6, 2e6);
        else if (f.key == "lna_gain" && !f.is_bool) c.lna_gain = std::clamp(static_cast<int>(f.number) / 8 * 8, 0, 40);
        else if (f.key == "vga_gain" && !f.is_bool) c.vga_gain = std::clamp(static_cast<int>(f.number) / 2 * 2, 0, 62);
        else if (f.key == "amp" && f.is_bool) c.amp = f.bool_value;
        else if (f.key == "gain_auto" && f.is_bool) c.gain_auto = f.bool_value;
        else if (f.key == "fm_dev_hz" && !f.is_bool) c.fm_dev_hz = n(1e6, 10e6);
        else if (f.key == "invert" && f.is_bool) c.invert = f.bool_value;
        else if (f.key == "afc" && f.is_bool) c.afc = f.bool_value;
        else if (f.key == "video_lpf_hz" && !f.is_bool) c.video_lpf_hz = n(0, 6e6);
        else if (f.key == "saturation" && !f.is_bool) c.saturation = std::clamp(static_cast<float>(f.number), 0.0f, 2.0f);
        else if (f.key == "hue_deg" && !f.is_bool) c.hue_deg = std::clamp(static_cast<float>(f.number), -180.0f, 180.0f);
        else if (f.key == "denoise" && !f.is_bool) c.denoise = std::clamp(static_cast<float>(f.number), 0.0f, 1.0f);
        else if (f.key == "denoise_temporal" && !f.is_bool) c.denoise_temporal = std::clamp(static_cast<float>(f.number), 0.0f, 1.0f);
        else if (f.key == "denoise_temporal_median" && !f.is_bool) { int v = static_cast<int>(f.number); if (v > 0 && !(v & 1)) ++v; c.denoise_temporal_median = v <= 0 ? 0 : std::clamp(v, 3, 9); }
        else if (f.key == "denoise_temporal_median_strength" && !f.is_bool) c.denoise_temporal_median_strength = std::clamp(static_cast<float>(f.number), 0.0f, 1.0f);
        else if (f.key == "overscan" && !f.is_bool) c.overscan = std::clamp(static_cast<float>(f.number), 0.0f, 0.15f);
        else if (f.key == "frame_width" && !f.is_bool) c.frame_width = std::clamp(static_cast<int>(f.number), 320, 1920);
        else if (f.key == "frame_height" && !f.is_bool) c.frame_height = std::clamp(static_cast<int>(f.number), 240, 1080);
        else if (f.key == "auto_detect" && f.is_bool) c.auto_detect = f.bool_value;
        else if (f.key == "clkout" && f.is_bool) c.clkout = f.bool_value;
        else if (f.key == "enforce_clkin" && f.is_bool) c.enforce_clkin = f.bool_value;
    }
}
}

bool load_config_file(Config& cfg, const std::string& path) {
    std::ifstream in(path);
    if (!in) return false;
    std::stringstream ss; ss << in.rdbuf();
    std::vector<Field> fields;
    if (!parse_object(ss.str(), fields)) return false;
    apply_fields(fields, cfg);
    return true;
}

bool save_config_file(const Config& c, const std::string& path) {
    const std::string tmp = path + ".tmp";
    std::ofstream out(tmp, std::ios::trunc);
    if (!out) return false;
    out << "{\n"
        << "  \"video_carrier_hz\": " << c.video_carrier_hz << ",\n"
        << "  \"sample_rate\": " << c.sample_rate << ",\n"
        << "  \"sample_bits\": " << c.sample_bits << ",\n"
        << "  \"offset_hz\": " << c.offset_hz << ",\n"
        << "  \"lna_gain\": " << c.lna_gain << ",\n"
        << "  \"vga_gain\": " << c.vga_gain << ",\n"
        << "  \"amp\": " << (c.amp ? "true" : "false") << ",\n"
        << "  \"gain_auto\": " << (c.gain_auto ? "true" : "false") << ",\n"
        << "  \"fm_dev_hz\": " << c.fm_dev_hz << ",\n"
        << "  \"invert\": " << (c.invert ? "true" : "false") << ",\n"
        << "  \"afc\": " << (c.afc ? "true" : "false") << ",\n"
        << "  \"video_lpf_hz\": " << c.video_lpf_hz << ",\n"
        << "  \"saturation\": " << c.saturation << ",\n"
        << "  \"hue_deg\": " << c.hue_deg << ",\n"
        << "  \"denoise\": " << c.denoise << ",\n"
        << "  \"denoise_temporal\": " << c.denoise_temporal << ",\n"
        << "  \"denoise_temporal_median\": " << c.denoise_temporal_median << ",\n"
        << "  \"denoise_temporal_median_strength\": " << c.denoise_temporal_median_strength << ",\n"
        << "  \"overscan\": " << c.overscan << ",\n"
        << "  \"frame_width\": " << c.frame_width << ",\n"
        << "  \"frame_height\": " << c.frame_height << ",\n"
        << "  \"auto_detect\": " << (c.auto_detect ? "true" : "false") << ",\n"
        << "  \"clkout\": " << (c.clkout ? "true" : "false") << ",\n"
        << "  \"enforce_clkin\": " << (c.enforce_clkin ? "true" : "false") << "\n}\n";
    out.close();
    return std::rename(tmp.c_str(), path.c_str()) == 0;
}

}  // namespace famidec
