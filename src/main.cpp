#include <imgui.h>
#include <module.h>
#include <gui/gui.h>

SDRPP_MOD_INFO{
    /* Name:            */ "sdrpp-spots",
    /* Description:     */ "Display spots on the band chart",
    /* Author:          */ "gerner",
    /* Version:         */ 0, 1, 0,
    /* Max instances    */ -1
};

/**********************************************
 * Two main functionalities:
 * 1. get/manage spots: frequency, label, spot time
 *    cop-out: just have a socket we listen for other stuff to shove in spots
 *    de-dup on label and update spot time
 *    "periodically" clean up "expired" spots
 * 2. draw spots: place on waterfall, similar to frequency_manager
 **********************************************/

struct WaterfallSpot {
    std::string label;
    double frequency;
    //TODO: spot time so we can expire spots
};

class SpotsModule : public ModuleManager::Instance {
public:
    SpotsModule(std::string name) {
        this->name = name;

        fftRedrawHandler.ctx = this;
        fftRedrawHandler.handler = fftRedraw;

        gui::menu.registerEntry(name, menuHandler, this, NULL);
        gui::waterfall.onFFTRedraw.bindHandler(&fftRedrawHandler);

        waterfallSpots.push_back({"test1", 28250000});
        waterfallSpots.push_back({"test2", 28500000});
        waterfallSpots.push_back({"test3", 28750000});
    }

    ~SpotsModule() {
        gui::menu.removeEntry(name);
        gui::waterfall.onFFTRedraw.unbindHandler(&fftRedrawHandler);
    }

    void postInit() {}

    void enable() {
        enabled = true;
    }

    void disable() {
        enabled = false;
    }

    bool isEnabled() {
        return enabled;
    }

private:
    static void menuHandler(void* ctx) {
        SpotsModule* _this = (SpotsModule*)ctx;
        ImGui::Text("Hello SDR++, my name is %s", _this->name.c_str());
    }

    static void fftRedraw(ImGui::WaterFall::FFTRedrawArgs args, void* ctx) {
        SpotsModule* _this = (SpotsModule*)ctx;

        for (auto const spot : _this->waterfallSpots) {
            double centerXpos = args.min.x + std::round((spot.frequency - args.lowFreq) * args.freqToPixelRatio);

            if (spot.frequency >= args.lowFreq && spot.frequency <= args.highFreq) {
                args.window->DrawList->AddLine(ImVec2(centerXpos, args.min.y), ImVec2(centerXpos, args.max.y), IM_COL32(255, 255, 0, 255));
            }

            ImVec2 nameSize = ImGui::CalcTextSize(spot.label.c_str());
            ImVec2 rectMin = ImVec2(centerXpos - (nameSize.x / 2) - 5, args.min.y);
            ImVec2 rectMax = ImVec2(centerXpos + (nameSize.x / 2) + 5, args.min.y + nameSize.y);
            ImVec2 clampedRectMin = ImVec2(std::clamp<double>(rectMin.x, args.min.x, args.max.x), rectMin.y);
            ImVec2 clampedRectMax = ImVec2(std::clamp<double>(rectMax.x, args.min.x, args.max.x), rectMax.y);

            if (clampedRectMax.x - clampedRectMin.x > 0) {
                args.window->DrawList->AddRectFilled(clampedRectMin, clampedRectMax, IM_COL32(255, 255, 0, 255));
            }
            if (rectMin.x >= args.min.x && rectMax.x <= args.max.x) {
                args.window->DrawList->AddText(ImVec2(centerXpos - (nameSize.x / 2), args.min.y), IM_COL32(0, 0, 0, 255), spot.label.c_str());
            }
        }
    }

    std::string name;
    bool enabled = true;

    EventHandler<ImGui::WaterFall::FFTRedrawArgs> fftRedrawHandler;

    std::vector<WaterfallSpot> waterfallSpots;
};

MOD_EXPORT void _INIT_() {
    // Nothing here
}

MOD_EXPORT ModuleManager::Instance* _CREATE_INSTANCE_(std::string name) {
    return new SpotsModule(name);
}

MOD_EXPORT void _DELETE_INSTANCE_(void* instance) {
    delete (SpotsModule*)instance;
}

MOD_EXPORT void _END_() {
    // Nothing here
}
