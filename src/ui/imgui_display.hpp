#pragma once

#include <string>
#include <SDL2/SDL.h>
#include <imgui.h>
#include <imgui_impl_sdl2.h>
#include <imgui_impl_sdlrenderer2.h>

#include "../dsp/frame.hpp"
#include "sdl_display.hpp"

namespace famidec {

class ImGuiDisplay : public SdlDisplay {
public:
    bool init(const std::string& title) {
        if (!SdlDisplay::init(title)) return false;

        IMGUI_CHECKVERSION();
        ImGui::CreateContext();
        ImGuiIO& io = ImGui::GetIO();
        io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
        io.ConfigFlags |= ImGuiConfigFlags_DockingEnable;

        ImGui::StyleColorsDark();

        if (!ImGui_ImplSDL2_InitForSDLRenderer(win_, ren_)) return false;
        if (!ImGui_ImplSDLRenderer2_Init(ren_)) return false;

        set_hotkeys_enabled(false);
        return true;
    }

    ~ImGuiDisplay() {
        ImGui_ImplSDLRenderer2_Shutdown();
        ImGui_ImplSDL2_Shutdown();
        ImGui::DestroyContext();
    }

    void render_gui(Config& cfg, HackRfSource* hackrf, const OsdStats& stats) {
        ImGui_ImplSDLRenderer2_NewFrame();
        ImGui_ImplSDL2_NewFrame();
        ImGui::NewFrame();

        ImGui::Begin("FPV Decoder Controls");

        if (ImGui::CollapsingHeader("RF Settings", ImGuiTreeNodeFlags_DefaultOpen)) {
            bool auto_gain = cfg.gain_auto;
            if (ImGui::Checkbox("Auto Gain", &auto_gain)) {
                cfg.gain_auto = auto_gain;
            }
            
            if (!cfg.gain_auto && hackrf) {
                int lna = hackrf->lna();
                if (ImGui::SliderInt("LNA Gain", &lna, 0, 40)) {
                    hackrf->set_gains(lna, hackrf->vga());
                }
                int vga = hackrf->vga();
                if (ImGui::SliderInt("VGA Gain", &vga, 0, 62)) {
                    hackrf->set_gains(hackrf->lna(), vga);
                }
            }

            bool amp = cfg.amp;
            if (ImGui::Checkbox("RF Amp (+14dB)", &amp)) {
                cfg.amp = amp;
                if (hackrf) hackrf->set_amp(amp);
            }
        }

        if (ImGui::CollapsingHeader("Tuning", ImGuiTreeNodeFlags_DefaultOpen)) {
            float freq = cfg.video_carrier_hz / 1e6f;
            if (ImGui::InputFloat("Frequency (MHz)", &freq, 0.01f, 0.1f, "%.3f")) {
                cfg.video_carrier_hz = freq * 1e6;
                if (hackrf) hackrf->set_center_freq(cfg.center_hz());
            }
            ImGui::Text("Current Channel: %s", stats.channel.c_str());
        }

        if (ImGui::CollapsingHeader("Video Settings")) {
            int mode = (int)cfg.mode;
            if (ImGui::Combo("Mode", &mode, "Color\0Gray\0")) {
                cfg.mode = (mode == 0) ? Config::Mode::Color : Config::Mode::Gray;
            }
            bool crt = stats.crt;
            if (ImGui::Checkbox("CRT Emulation", &crt)) {
                // This would normally update a state in main, 
                // but for now we just show the toggle.
            }
        }

        ImGui::Separator();
        ImGui::Text("FPS: %.2f", stats.fps);
        ImGui::Text("Latency: %.1f ms", stats.video_latency_ms);
        if (stats.clipping) ImGui::TextColored(ImVec4(1,0,0,1), "ADC CLIPPING!");

        if (ImGui::Button("Quit")) {
            // We'll handle quit via the return of poll() or a global flag
        }

        ImGui::End();

        ImGui::Render();
        ImGui_ImplSDLRenderer2_RenderDrawData(ImGui::GetDrawData());
    }

private:
    // We use the existing win_/ren_ from SdlDisplay
};

} // namespace famidec
