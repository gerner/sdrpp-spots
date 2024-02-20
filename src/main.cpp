#include <utils/networking.h>
#include <imgui.h>
#include <module.h>
#include <gui/gui.h>

#define MAX_COMMAND_LENGTH 8192

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

        strcpy(hostname, "localhost");

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

        if (client) { client->close(); }
        if (listener) { listener->close(); }
    }

    void postInit() {
        startServer();
    }

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

    static void dataHandler(int count, uint8_t* data, void* ctx) {
        SpotsModule* _this = (SpotsModule*)ctx;

        for (int i = 0; i < count; i++) {
            if (data[i] == '\n') {
                _this->commandHandler(_this->command);
                _this->command.clear();
                continue;
            }
            if (_this->command.size() < MAX_COMMAND_LENGTH) { _this->command += (char)data[i]; }
        }

        _this->client->readAsync(1024, _this->dataBuf, dataHandler, _this, false);
    }

    static void clientHandler(net::Conn _client, void* ctx) {
        SpotsModule* _this = (SpotsModule*)ctx;
        flog::info("New spot client!");

        _this->client = std::move(_client);
        _this->client->readAsync(1024, _this->dataBuf, dataHandler, _this, false);
        _this->client->waitForEnd();
        _this->client->close();

        flog::info("Spot client disconnected!");

        _this->listener->acceptAsync(clientHandler, _this);
    }

    void commandHandler(std::string cmd) {
        flog::info("spot command: {0}", cmd);
    }

    void startServer() {
        try {
            listener = net::listen(hostname, port);
            listener->acceptAsync(clientHandler, this);
        }
        catch (const std::exception& e) {
            flog::error("Could not start spot server: {}", e.what());
        }
    }

    void stopServer() {
        if (client) { client->close(); }
        listener->close();
    }

    std::string name;
    bool enabled = true;

    char hostname[1024];
    int port = 6214;
    uint8_t dataBuf[1024];
    net::Listener listener;
    net::Conn client;

    std::string command = "";

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
