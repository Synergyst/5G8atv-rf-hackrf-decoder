#include "sdl_display.hpp"
#include <algorithm>
#include <cctype>
#include <cstdio>
#include <vector>

namespace famidec {

namespace {

const uint8_t* glyph5x7(char c) {
    static const uint8_t kDigits[10][7] = {
        {0x0E,0x11,0x13,0x15,0x19,0x11,0x0E}, {0x04,0x0C,0x04,0x04,0x04,0x04,0x0E},
        {0x0E,0x11,0x01,0x02,0x04,0x08,0x1F}, {0x1F,0x02,0x04,0x02,0x01,0x11,0x0E},
        {0x02,0x06,0x0A,0x12,0x1F,0x02,0x02}, {0x1F,0x10,0x1E,0x01,0x01,0x11,0x0E},
        {0x06,0x08,0x10,0x1E,0x11,0x11,0x0E}, {0x1F,0x01,0x02,0x04,0x08,0x08,0x08},
        {0x0E,0x11,0x11,0x0E,0x11,0x11,0x0E}, {0x0E,0x11,0x11,0x0F,0x01,0x02,0x0C},
    };
    static const uint8_t kColon[7] = {0x00,0x04,0x00,0x00,0x04,0x00,0x00};
    static const uint8_t kDot[7]   = {0x00,0x00,0x00,0x00,0x00,0x0C,0x0C};
    static const uint8_t kDash[7]  = {0x00,0x00,0x00,0x1F,0x00,0x00,0x00};
    static const uint8_t kSpace[7] = {0,0,0,0,0,0,0};
    static const uint8_t kLetters[26][7] = {
        {0x0E,0x11,0x11,0x1F,0x11,0x11,0x11}, {0x1E,0x11,0x11,0x1E,0x11,0x11,0x1E},
        {0x0E,0x11,0x10,0x10,0x10,0x11,0x0E}, {0x1E,0x11,0x11,0x11,0x11,0x11,0x1E},
        {0x1F,0x10,0x10,0x1E,0x10,0x10,0x1F}, {0x1F,0x10,0x10,0x1E,0x10,0x10,0x10},
        {0x0E,0x11,0x10,0x17,0x11,0x11,0x0F}, {0x11,0x11,0x11,0x1F,0x11,0x11,0x11},
        {0x0E,0x04,0x04,0x04,0x04,0x04,0x0E}, {0x07,0x02,0x02,0x02,0x02,0x12,0x0C},
        {0x11,0x12,0x14,0x18,0x14,0x12,0x11}, {0x10,0x10,0x10,0x10,0x10,0x10,0x1F},
        {0x11,0x1B,0x15,0x15,0x11,0x11,0x11}, {0x11,0x19,0x15,0x13,0x11,0x11,0x11},
        {0x0E,0x11,0x11,0x11,0x11,0x11,0x0E}, {0x1E,0x11,0x11,0x1E,0x10,0x10,0x10},
        {0x0E,0x11,0x11,0x11,0x15,0x12,0x0D}, {0x1E,0x11,0x11,0x1E,0x14,0x12,0x11},
        {0x0F,0x10,0x10,0x0E,0x01,0x01,0x1E}, {0x1F,0x04,0x04,0x04,0x04,0x04,0x04},
        {0x11,0x11,0x11,0x11,0x11,0x11,0x0E}, {0x11,0x11,0x11,0x11,0x11,0x0A,0x04},
        {0x11,0x11,0x11,0x15,0x15,0x1B,0x11}, {0x11,0x11,0x0A,0x04,0x0A,0x11,0x11},
        {0x11,0x11,0x0A,0x04,0x04,0x04,0x04}, {0x1F,0x01,0x02,0x04,0x08,0x10,0x1F},
    };
    if (c >= '0' && c <= '9') return kDigits[c - '0'];
    c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
    if (c >= 'A' && c <= 'Z') return kLetters[c - 'A'];
    switch (c) {
        case ':': return kColon;
        case '.': return kDot;
        case '-': return kDash;
        default: return kSpace;
    }
}

struct CrtLut {
    struct Entry {
        int32_t src;
        uint16_t gain;
    };
    std::vector<Entry> map;
    CrtLut() {
        map.resize(static_cast<size_t>(Frame::kWidth) * Frame::kHeight);
        const double cx = Frame::kWidth / 2.0, cy = Frame::kHeight / 2.0;
        const double k1 = 0.055;
        for (int y = 0; y < Frame::kHeight; ++y) {
            for (int x = 0; x < Frame::kWidth; ++x) {
                double nx = (x - cx) / cx, ny = (y - cy) / cy;
                double r2 = nx * nx + ny * ny;
                double f = 1.0 + k1 * r2;
                int sx = static_cast<int>(cx + nx * f * cx + 0.5);
                int sy = static_cast<int>(cy + ny * f * cy + 0.5);
                Entry& e = map[static_cast<size_t>(y) * Frame::kWidth + x];
                if (sx < 0 || sx >= Frame::kWidth || sy < 0 || sy >= Frame::kHeight) {
                    e.src = -1; e.gain = 0;
                } else {
                    e.src = sy * Frame::kWidth + sx;
                    double vig = 1.0 - 0.18 * r2 * r2;
                    double scan = (sy & 1) ? 0.72 : 1.0;
                    e.gain = static_cast<uint16_t>(std::max(0.0, vig * scan) * 256.0 + 0.5);
                }
            }
        }
    }
};

void apply_crt(const Frame& in, Frame& out) {
    static const CrtLut lut;
    const uint32_t* src = in.rgba.data();
    uint32_t* dst = out.rgba.data();
    for (size_t i = 0; i < lut.map.size(); ++i) {
        const CrtLut::Entry& e = lut.map[i];
        if (e.src < 0) { dst[i] = 0xff000000u; continue; }
        uint32_t p = src[e.src];
        uint32_t r = ((p & 0xffu) * e.gain) >> 8;
        uint32_t g = (((p >> 8) & 0xffu) * e.gain) >> 8;
        uint32_t b = (((p >> 16) & 0xffu) * e.gain) >> 8;
        dst[i] = 0xff000000u | (b << 16) | (g << 8) | r;
    }
}

constexpr int kFontScale = 2;
constexpr int kCharW = 6 * kFontScale;

void frame_text(Frame& f, int x, int y, const std::string& text, uint8_t r, uint8_t g, uint8_t b, int scale = kFontScale) {
    uint32_t px32 = 0xff000000u | (static_cast<uint32_t>(b) << 16) | (static_cast<uint32_t>(g) << 8) | r;
    for (size_t ci = 0; ci < text.size(); ++ci) {
        const uint8_t* gl = glyph5x7(text[ci]);
        for (int row = 0; row < 7; ++row)
            for (int col = 0; col < 5; ++col)
                if (gl[row] & (0x10 >> col)) {
                    int bx = x + static_cast<int>(ci) * 6 * scale + col * scale;
                    int by = y + row * scale;
                    for (int dy = 0; dy < scale; ++dy)
                        for (int dx = 0; dx < scale; ++dx) {
                            int xx = bx + dx, yy = by + dy;
                            if (xx >= 0 && xx < Frame::kWidth && yy >= 0 && yy < Frame::kHeight)
                                f.rgba[static_cast<size_t>(yy) * Frame::kWidth + xx] = px32;
                        }
                }
    }
}

void draw_text(SDL_Renderer* ren, int x, int y, const std::string& text, uint8_t r, uint8_t g, uint8_t b, int scale = kFontScale) {
    SDL_SetRenderDrawColor(ren, r, g, b, 255);
    for (size_t ci = 0; ci < text.size(); ++ci) {
        const uint8_t* gl = glyph5x7(text[ci]);
        for (int row = 0; row < 7; ++row)
            for (int col = 0; col < 5; ++col)
                if (gl[row] & (0x10 >> col)) {
                    SDL_Rect px{x + static_cast<int>(ci) * 6 * scale + col * scale, y + row * scale, scale, scale};
                    SDL_RenderFillRect(ren, &px);
                }
    }
}

} // namespace

bool SdlDisplay::init(const std::string& title, bool use_imgui) {
    if (SDL_Init(SDL_INIT_VIDEO) != 0) return false;
    use_imgui_ = use_imgui;
    win_ = SDL_CreateWindow(title.c_str(), SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED, Frame::kWidth, Frame::kHeight, SDL_WINDOW_RESIZABLE | SDL_WINDOW_ALLOW_HIGHDPI);
    if (!win_) return false;
    ren_ = SDL_CreateRenderer(win_, -1, SDL_RENDERER_ACCELERATED | SDL_RENDERER_PRESENTVSYNC);
    if (!ren_) ren_ = SDL_CreateRenderer(win_, -1, 0);
    if (!ren_) return false;
    SDL_RenderSetLogicalSize(ren_, Frame::kWidth, Frame::kHeight);
    tex_ = SDL_CreateTexture(ren_, SDL_PIXELFORMAT_ABGR8888, SDL_TEXTUREACCESS_STREAMING, Frame::kWidth, Frame::kHeight);
    if (!tex_) return false;
    if (use_imgui_) {
        IMGUI_CHECKVERSION();
        ImGui::CreateContext();
        ImGui_ImplSDL2_InitForSDLRenderer(win_, ren_);
        ImGui_ImplSDLRenderer2_Init(ren_);
    }
    return true;
}

SdlDisplay::~SdlDisplay() {
    if (use_imgui_) {
        ImGui_ImplSDLRenderer2_Shutdown();
        ImGui_ImplSDL2_Shutdown();
        ImGui::DestroyContext();
    }
    if (tex_) SDL_DestroyTexture(tex_);
    if (ren_) SDL_DestroyRenderer(ren_);
    if (win_) SDL_DestroyWindow(win_);
    SDL_Quit();
}

KeyAction SdlDisplay::poll() {
    SDL_Event ev;
    while (SDL_PollEvent(&ev)) {
        if (use_imgui_) ImGui_ImplSDL2_ProcessEvent(&ev);
        if (ev.type == SDL_QUIT) return KeyAction::Quit;
        if (ev.type == SDL_KEYDOWN) {
            if (!hotkeys_enabled_) continue;
            bool shift = (ev.key.keysym.mod & KMOD_SHIFT) != 0;
            switch (ev.key.keysym.sym) {
                case SDLK_ESCAPE: case SDLK_q: return KeyAction::Quit;
                case SDLK_l: return shift ? KeyAction::GainLnaDown : KeyAction::GainLnaUp;
                case SDLK_g: return shift ? KeyAction::GainVgaDown : KeyAction::GainVgaUp;
                case SDLK_c: return KeyAction::ToggleColor;
                case SDLK_s: return KeyAction::Screenshot;
                case SDLK_h: return KeyAction::ToggleHelp;
                case SDLK_RIGHT: return KeyAction::FreqUp;
                case SDLK_LEFT: return KeyAction::FreqDown;
                case SDLK_UP: return KeyAction::FreqUpBig;
                case SDLK_DOWN: return KeyAction::FreqDownBig;
                case SDLK_r: return KeyAction::ToggleCrt;
                case SDLK_v: return KeyAction::ToggleRecord;
                case SDLK_a: return KeyAction::ToggleGainMode;
                case SDLK_b: return KeyAction::ToggleAmp;
                case SDLK_o: return KeyAction::ToggleOsd;
                default: break;
            }
        }
    }
    return KeyAction::None;
}

void SdlDisplay::render(const Frame* frame, const OsdStats& stats, void* app_state) {
    if (frame) { last_frame_ = *frame; have_frame_ = true; }
    if (have_frame_) {
        osd_frame_ = last_frame_;
        if (stats.show_osd) {
            const std::string ch = stats.channel.empty() ? "--" : stats.channel;
            frame_text(osd_frame_, 48, 40, ch, 40, 255, 80, 6);
            if (stats.recording) {
                char recl[16];
                int s = static_cast<int>(stats.rec_seconds);
                std::snprintf(recl, sizeof(recl), "REC %d:%02d", s / 60, s % 60);
                frame_text(osd_frame_, 48, 96, recl, 255, 60, 60, 3);
            }
            char line[96];
            int n = std::snprintf(line, sizeof(line), "V:%s H:%s %.2fFPS %.2f",
                                  stats.vsync_locked ? "OK" : "--", stats.line_locked ? "OK" : "--", stats.fps, stats.freq_mhz);
            if (!stats.channel.empty() && n < static_cast<int>(sizeof(line)))
                n += std::snprintf(line + n, sizeof(line) - n, " %s", stats.channel.c_str());
            if (n < static_cast<int>(sizeof(line)))
                n += std::snprintf(line + n, sizeof(line) - n, " %.0fMS %s L%d V%d%s", stats.video_latency_ms, stats.gain_auto ? "AUTO" : "MAN", stats.lna, stats.vga, stats.amp ? " AMP" : "");
            std::string t(line);
            int x = (Frame::kWidth - static_cast<int>(t.size()) * kCharW) / 2;
            if (x < 4) x = 4;
            frame_text(osd_frame_, x, 28, t, 255, 220, 0);
            if (stats.clipping) frame_text(osd_frame_, Frame::kWidth - 28 - static_cast<int>(std::string("CLIP").size()) * kCharW, 28 + 9 * kFontScale, "CLIP", 255, 60, 60);
        }
        if (stats.crt) {
            apply_crt(osd_frame_, crt_frame_);
            SDL_UpdateTexture(tex_, nullptr, crt_frame_.rgba.data(), Frame::kWidth * 4);
        } else {
            SDL_UpdateTexture(tex_, nullptr, osd_frame_.rgba.data(), Frame::kWidth * 4);
        }
    }
    SDL_SetRenderDrawColor(ren_, 0, 0, 0, 255);
    SDL_RenderClear(ren_);
    SDL_RenderCopy(ren_, tex_, nullptr, nullptr);
    if (stats.show_help && !use_imgui_) {
        static const char* kHelp[] = { "KEYS", "Q ESC QUIT", "A GAIN AUTO-MAN", "L LNA UP", "SHIFT L LNA DOWN", "G VGA UP", "SHIFT G VGA DOWN", "B RF AMP", "C COLOR", "O OSD", "S SCREENSHOT", "H HELP", "ARROWS TUNE", "R CRT", "V REC" };
        int bw = 23 * kCharW + 32, bh = 15 * 9 * kFontScale + 32;
        SDL_Rect box{(Frame::kWidth - bw) / 2, (Frame::kHeight - bh) / 2, bw, bh};
        SDL_SetRenderDrawBlendMode(ren_, SDL_BLENDMODE_BLEND);
        SDL_SetRenderDrawColor(ren_, 0, 0, 0, 200);
        SDL_RenderFillRect(ren_, &box);
        SDL_SetRenderDrawBlendMode(ren_, SDL_BLENDMODE_NONE);
        for (int i = 0; i < 15; ++i) draw_text(ren_, box.x + 16, box.y + 16 + i * 9 * kFontScale, kHelp[i], 255, 220, 0);
    }
    if (use_imgui_) render_imgui(app_state);
    SDL_RenderPresent(ren_);
}

void SdlDisplay::render_imgui(void* app_state) {
    ImGui_ImplSDLRenderer2_NewFrame();
    ImGui_ImplSDL2_NewFrame();
    ImGui::NewFrame();
    ImGui::Begin("Control Panel");
    if (ImGui::Button("Screenshot")) { /* Handled via KeyAction in main loop */ }
    ImGui::End();
    ImGui::Render();
    ImGui_ImplSDLRenderer2_RenderDrawData(ImGui::GetDrawData(), ren_);
}

bool SdlDisplay::screenshot(const Frame& frame, const std::string& path) {
    SDL_Surface* surf = SDL_CreateRGBSurfaceWithFormatFrom(const_cast<uint32_t*>(frame.rgba.data()), Frame::kWidth, Frame::kHeight, 32, Frame::kWidth * 4, SDL_PIXELFORMAT_ABGR8888);
    if (!surf) return false;
    bool ok = SDL_SaveBMP(surf, path.c_str()) == 0;
    SDL_FreeSurface(surf);
    return ok;
}

} // namespace famidec
