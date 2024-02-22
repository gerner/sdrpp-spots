#include <cstring>
#include <iostream>
#include <sstream>
#include <vector>
#include <list>
#include <utils/networking.h>
#include <utils/freq_formatting.h>
#include <imgui.h>
#include <module.h>
#include <gui/gui.h>
#include <gui/style.h>
#include <core.h>
#include <config.h>
#define CONCAT(a, b) ((std::string(a) + b).c_str())

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

std::vector<std::string> split(const std::string &s, char delim) {
    std::vector<std::string> result;
    std::stringstream ss (s);
    std::string item;

    while (getline (ss, item, delim)) {
        result.push_back (item);
    }

    return result;
}

int parseTime(const std::string &s, std::chrono::time_point<std::chrono::system_clock>* t) {
    std::tm tm{};
    // HHMM YYYY-mm-dd
    size_t loc = s.find(" ");
    if(loc == s.npos || loc+1 >= s.size()) {
        return 1;
    }
    std::string timeS = s.substr(0, loc);
    int time = std::stoi(timeS);
    if(time < 0) {
        return 1;
    }
    tm.tm_sec = 0;
    tm.tm_min = time % 100;
    if(tm.tm_min >= 60) {
        return 1;
    }
    tm.tm_hour = time / 100;
    if(tm.tm_hour >= 24) {
        return 1;
    }

    std::string dateS = s.substr(loc+1);
    loc = dateS.find("-");
    if(loc == s.npos || loc+1 >= dateS.size()) {
        return 1;
    }
    std::string yearS = dateS.substr(0, loc);
    int year = std::stoi(yearS);
    if(year < 0) {
        return 1;
    }
    dateS = dateS.substr(loc+1);
    loc = dateS.find("-");
    if(loc == s.npos || loc+1 >= dateS.size()) {
        return 1;
    }
    std::string monthS = dateS.substr(0, loc);
    int month = std::stoi(monthS);
    if(month < 1 || month > 12) {
        return 1;
    }
    std::string dayS = dateS.substr(loc+1);
    int day = std::stoi(dayS);
    if(day < 1 || day > 31) {
        return 1;
    }

    tm.tm_year = year - 1900;
    tm.tm_mon = month-1; // Jan = 0
    tm.tm_mday = day;
    tm.tm_isdst = 0;

    // from https://stackoverflow.com/a/38298359
    std::time_t tLocal = std::mktime(&tm);
    time_t tUTC = tLocal + (std::mktime(std::localtime(&tLocal)) - std::mktime(std::gmtime(&tLocal)));
    *t = std::chrono::system_clock::from_time_t(tUTC);

    return 0;
}

std::string format_duration(std::chrono::system_clock::duration duration) {
    // 1:00
    const size_t bufsize=128;
    char buf[bufsize];
    int64_t sec = std::chrono::duration_cast<std::chrono::seconds>(duration).count();
    if(sec < 0) {
        strcpy(buf, "neg");
    } else if(sec < 60) {
        snprintf(buf, bufsize, "%ld sec", sec);
    } else if(sec < 3600) {
        snprintf(buf, bufsize, "%ld:%ld min", sec/60, sec%60);
    } else if(sec < 24*3600) {
        snprintf(buf, bufsize, "%ld:%ld hrs", sec/3600, sec%3600/60);
    } else {
        strcpy(buf, "days");
    }
    return std::string(buf);
}

// actual spots we're keeping track of
struct WaterfallSpot {
    std::string label;
    double frequency;
    std::chrono::time_point<std::chrono::system_clock> spotTime;
    std::string comment;
    std::string location;
};

// info about how we draw spots on the waterfall so we can figure out clicks
struct WaterfallLabel {
    WaterfallSpot* spot;
    ImVec2 rectMin;
    ImVec2 rectMax;
};

ConfigManager config;

class SpotsModule : public ModuleManager::Instance {
public:
    SpotsModule(std::string name) {
        this->name = name;

        config.acquire();
        if (!config.conf.contains(name)) {
            config.conf[name]["host"] = "localhost";
            config.conf[name]["port"] = 6214;
            config.conf[name]["autoStart"] = false;
            config.conf[name]["spotLifetime"] = 30;
            config.conf[name]["maxSpotLifetime"] = 240;
        }

        //TODO: config initialization
        std::string hostname = config.conf[name]["host"];
        strcpy(host, hostname.c_str());
        port = config.conf[name]["port"];
        autoStart = config.conf[name]["autoStart"];
        spotLifetime = config.conf[name]["spotLifetime"];
        maxSpotLifetime = config.conf[name]["maxSpotLifetime"];
        config.release(true);

        fftRedrawHandler.ctx = this;
        fftRedrawHandler.handler = fftRedraw;
        inputHandler.ctx = this;
        inputHandler.handler = fftInput;

        gui::menu.registerEntry(name, menuHandler, this, NULL);
        gui::waterfall.onFFTRedraw.bindHandler(&fftRedrawHandler);
        gui::waterfall.onInputProcess.bindHandler(&inputHandler);
    }

    ~SpotsModule() {
        gui::menu.removeEntry(name);
        gui::waterfall.onFFTRedraw.unbindHandler(&fftRedrawHandler);
        gui::waterfall.onInputProcess.unbindHandler(&inputHandler);

        if (client) { client->close(); }
        if (listener) { listener->close(); }
    }

    void postInit() {
        // If autostart is enabled, start the server
        if (autoStart) { startServer(); }
    }

    void start() {
        startServer();
    }

    void stop() {
        stopServer();
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
        float menuWidth = ImGui::GetContentRegionAvail().x;

        //TODO: config
        if (_this->running) { style::beginDisabled(); }
        if (ImGui::InputText(CONCAT("##_spots_host_", _this->name), _this->host, 1023)) {
            config.acquire();
            config.conf[_this->name]["host"] = std::string(_this->host);
            config.release(true);
        }
        ImGui::SameLine();
        ImGui::SetNextItemWidth(menuWidth - ImGui::GetCursorPosX());
        if (ImGui::InputInt(CONCAT("##_spots_port_", _this->name), &_this->port, 0, 0)) {
            config.acquire();
            config.conf[_this->name]["port"] = _this->port;
            config.release(true);
        }
        if (_this->running) { style::endDisabled(); }

        if (ImGui::Checkbox(CONCAT("Listen on startup##_spots_auto_lst_", _this->name), &_this->autoStart)) {
            config.acquire();
            config.conf[_this->name]["autoStart"] = _this->autoStart;
            config.release(true);
        }

        ImGui::LeftLabel("Spot Lifetime");
        ImGui::SetNextItemWidth(menuWidth - ImGui::GetCursorPosX());
        if (ImGui::SliderInt(("##_spots_spotlifetime_" + _this->name).c_str(), &_this->spotLifetime, 1, _this->maxSpotLifetime)) {
            config.acquire();
            config.conf[_this->name]["spotLifetime"] = _this->spotLifetime;
            config.release(true);
        }

        ImGui::FillWidth();
        if(ImGui::Button(CONCAT("Clear spots##_spots_clear_", _this->name), ImVec2(menuWidth, 0))) {
            std::lock_guard lk(_this->waterfallMutex);
            _this->waterfallSpots.clear();
        }

        //start/stop server
        ImGui::FillWidth();
        if (_this->running && ImGui::Button(CONCAT("Stop##_spots_stop_", _this->name), ImVec2(menuWidth, 0))) {
            _this->stop();
        }
        else if (!_this->running && ImGui::Button(CONCAT("Start##_spots_stop_", _this->name), ImVec2(menuWidth, 0))) {
            _this->start();
        }

        ImGui::TextUnformatted("Status:");
        ImGui::SameLine();

        if(_this->running) {
            auto sinceLastUpdate = std::chrono::system_clock::now() - _this->lastUpdate;
            if(std::chrono::duration_cast<std::chrono::seconds>(sinceLastUpdate).count() > 12*3600) {
                ImGui::TextColored(ImVec4(0.0, 1.0, 0.0, 1.0), "Waiting...");
            } else {
                std::string lastDataLabel = format_duration(sinceLastUpdate);
                ImGui::TextColored(ImVec4(0.0, 1.0, 0.0, 1.0), "%s ago", lastDataLabel.c_str());
            }
        } else {
            ImGui::TextUnformatted("Idle");
        }
    }

    static void fftRedraw(ImGui::WaterFall::FFTRedrawArgs args, void* ctx) {
        SpotsModule* _this = (SpotsModule*)ctx;

        std::lock_guard lk(_this->waterfallMutex);
        auto expirationTime = std::chrono::system_clock::now() - std::chrono::minutes(_this->maxSpotLifetime);
        auto displayTime = std::chrono::system_clock::now() - std::chrono::minutes(_this->spotLifetime);

        std::vector<float> lanePositions;
        float laneHeight = ImGui::CalcTextSize("TEST").y + 2;
        int laneLimit = 8;
        _this->waterfallLabels.clear();
        for (auto it = _this->waterfallSpots.begin(); it != _this->waterfallSpots.end();) {

            // handle expiration of spots
            if(it->spotTime < displayTime) {
                if(it->spotTime < expirationTime) {
                    it = _this->waterfallSpots.erase(it);
                } else {
                    ++it;
                }
                continue;
            }

            // skip spots outside waterfall frequency range
            if (it->frequency < args.lowFreq && it->frequency < args.highFreq) {
                ++it;
                continue;
            }

            double centerXpos = args.min.x + std::round((it->frequency - args.lowFreq) * args.freqToPixelRatio);

            ImVec2 nameSize = ImGui::CalcTextSize(it->label.c_str());
            float leftEdge = centerXpos - (nameSize.x/2) - 5;
            float rightEdge = centerXpos + (nameSize.x/2) + 5;

            // choose a "lane" for the label to go in
            // highest lane that it'll fit
            // if none, add a lane
            float targetY = -1;
            int i = 0;
            for(auto laneIt = lanePositions.begin(); laneIt != lanePositions.end(); laneIt++) {
                if(leftEdge - 2 >= *laneIt) {
                    *laneIt = rightEdge;
                    targetY = args.min.y + i * laneHeight;
                    break;
                }
                i++;
            }
            if(targetY < 0) {
                if(i < laneLimit) {
                    targetY = args.min.y + i * laneHeight;
                    lanePositions.push_back(rightEdge);
                } else {
                    // sorry, no space
                    ++it;
                    continue;
                }
            }

            if (it->frequency >= args.lowFreq && it->frequency <= args.highFreq) {
                args.window->DrawList->AddLine(ImVec2(centerXpos, targetY), ImVec2(centerXpos, args.max.y), _this->spotBgColor);
            }

            ImVec2 rectMin = ImVec2(centerXpos - (nameSize.x / 2) - 5, targetY);
            ImVec2 rectMax = ImVec2(centerXpos + (nameSize.x / 2) + 5, targetY + nameSize.y);
            ImVec2 clampedRectMin = ImVec2(std::clamp<double>(rectMin.x, args.min.x, args.max.x), rectMin.y);
            ImVec2 clampedRectMax = ImVec2(std::clamp<double>(rectMax.x, args.min.x, args.max.x), rectMax.y);

            if (clampedRectMax.x - clampedRectMin.x > 0) {
                _this->waterfallLabels.push_back({&(*it), rectMin, rectMax});
                args.window->DrawList->AddRectFilled(clampedRectMin, clampedRectMax, _this->spotBgColor);
                args.window->DrawList->AddText(ImVec2(centerXpos - (nameSize.x / 2), targetY), _this->spotTextColor, it->label.c_str());
            }

            // make sure to get the next element in the spot list!
            ++it;
        }
    }

    // stuff to check if we click on a label on the waterfall
    // inspired by freuqency_manager module
    bool mouseAlreadyDown = false;
    bool mouseClickedInLabel = false;
    static void fftInput(ImGui::WaterFall::InputHandlerArgs args, void* ctx) {
        SpotsModule* _this = (SpotsModule*)ctx;
        if (_this->mouseClickedInLabel) {
            if (!ImGui::IsMouseDown(ImGuiMouseButton_Left)) {
                _this->mouseClickedInLabel = false;
            }
            gui::waterfall.inputHandled = true;
            return;
        }

        // First check that the mouse clicked outside of any label. Also get the bookmark that's hovered
        bool inALabel = false;
        WaterfallLabel hoveredLabel;

        for(auto label : _this->waterfallLabels) {
            ImVec2 clampedRectMin = ImVec2(std::clamp<double>(label.rectMin.x, args.fftRectMin.x, args.fftRectMax.x), label.rectMin.y);
            ImVec2 clampedRectMax = ImVec2(std::clamp<double>(label.rectMax.x, args.fftRectMin.x, args.fftRectMax.x), label.rectMax.y);

            if (ImGui::IsMouseHoveringRect(clampedRectMin, clampedRectMax)) {
                inALabel = true;
                hoveredLabel = label;
                break;
            }
        }

        // Check if mouse was already down
        if (ImGui::IsMouseClicked(ImGuiMouseButton_Left) && !inALabel) {
            _this->mouseAlreadyDown = true;
        }
        if (!ImGui::IsMouseDown(ImGuiMouseButton_Left)) {
            _this->mouseAlreadyDown = false;
            _this->mouseClickedInLabel = false;
        }

        // If yes, cancel
        if (_this->mouseAlreadyDown || !inALabel) { return; }

        gui::waterfall.inputHandled = true;

        ImVec2 clampedRectMin = ImVec2(std::clamp<double>(hoveredLabel.rectMin.x, args.fftRectMin.x, args.fftRectMax.x), hoveredLabel.rectMin.y);
        ImVec2 clampedRectMax = ImVec2(std::clamp<double>(hoveredLabel.rectMax.x, args.fftRectMin.x, args.fftRectMax.x), hoveredLabel.rectMax.y);

        if (ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
            _this->mouseClickedInLabel = true;
            tuner::tune(tuner::TUNER_MODE_NORMAL, gui::waterfall.selectedVFO, hoveredLabel.spot->frequency);
        }

        ImGui::BeginTooltip();
        ImGui::TextUnformatted(hoveredLabel.spot->label.c_str());
        ImGui::Separator();
        ImGui::Text("Frequency: %s", utils::formatFreq(hoveredLabel.spot->frequency).c_str());
        std::string lastSpotted = format_duration(std::chrono::system_clock::now() - hoveredLabel.spot->spotTime) + " ago";
        ImGui::Text("Last spotted: %s", lastSpotted.c_str());
        ImGui::Text("Location: %s", hoveredLabel.spot->location.c_str());
        ImGui::Text("Comment: %s", hoveredLabel.spot->comment.c_str());
        ImGui::EndTooltip();
    }

    static void dataHandler(int count, uint8_t* data, void* ctx) {
        SpotsModule* _this = (SpotsModule*)ctx;

        // read up to newline
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
        //flog::info("New spot client!");

        _this->client = std::move(_client);
        _this->client->readAsync(1024, _this->dataBuf, dataHandler, _this, false);
        _this->client->waitForEnd();
        _this->client->close();

        //flog::info("Spot client disconnected!");

        _this->listener->acceptAsync(clientHandler, _this);
    }

    void commandHandler(std::string cmd) {
        //flog::info("spot command: {0}", cmd);

        // command format: COMMAND [\t args...]
        // spot command: DX \t SPOTTER \t FREQ \t DX \t COMMENT \t TIME \t LOCATION"
        std::string resp;
        std::vector<std::string> commandParts = split(cmd, '\t');
        if(commandParts.size() == 0) {
            resp = "1 ERROR\n";
            client->write(resp.size(), (uint8_t*)resp.c_str());
            return;
        }

        if("DX" == commandParts[0]) {
            if(commandParts.size() < 7) {
                resp = "1 ERROR\n";
                client->write(resp.size(), (uint8_t*)resp.c_str());
                return;
            }

            // frequency comes in kHz
            double frequency = std::stod(commandParts[2]);
            if(frequency <= 0) {
                resp = "1 ERROR\n";
                client->write(resp.size(), (uint8_t*)resp.c_str());
                return;
            }
            frequency *= 1000;

            // time is HHMM YYYY-MM-DD
            std::chrono::time_point<std::chrono::system_clock> spotTime;
            if(parseTime(commandParts[5], &spotTime) != 0) {
                resp = "1 ERROR\n";
                client->write(resp.size(), (uint8_t*)resp.c_str());
                return;
            }

            //everything is ok with input, we have a spot
            lastUpdate = std::chrono::system_clock::now();

            if(spotTime < std::chrono::system_clock::now() - std::chrono::minutes(maxSpotLifetime)) {
                // silently drop already expired spots
                resp = "0 OK\n";
                client->write(resp.size(), (uint8_t*)resp.c_str());
                return;
            }

            std::string label = commandParts[3];
            std::string comment = commandParts[4];
            std::string location = commandParts[6];

            {
                bool found = false;
                std::lock_guard lk(waterfallMutex);
                // see if we already have a corresponding spot
                for(auto spot : waterfallSpots) {
                    if(spot.label == label) {
                        found = true;
                        if(spotTime > spot.spotTime) {
                            spot.spotTime = spotTime;
                            spot.frequency = frequency;
                            spot.comment = comment;
                            spot.location = location;
                        }
                        break;
                    }
                }
                // if so, update it
                // if not, add it
                if(!found) {
                    WaterfallSpot spot = {
                        commandParts[3],
                        frequency,
                        spotTime,
                        comment,
                        location
                    };
                    waterfallSpots.insert(
                        std::lower_bound(
                            waterfallSpots.begin(),
                            waterfallSpots.end(),
                            spot.frequency,
                            [](const WaterfallSpot &lhs, double f) { return lhs.frequency < f; }
                        ),
                        spot
                    );
                }
            }

            resp = "0 OK\n";
            client->write(resp.size(), (uint8_t*)resp.c_str());
        }
    }

    void startServer() {
        if (running) { return; }
        try {
            listener = net::listen(host, port);
            listener->acceptAsync(clientHandler, this);
            running = true;
        }
        catch (const std::exception& e) {
            flog::error("Could not start spot server: {}", e.what());
        }
    }

    void stopServer() {
        if (!running) { return; }
        if (client) { client->close(); }
        listener->close();
        running = false;
    }

    std::string name;
    bool enabled = true;
    bool running = false;
    std::chrono::time_point<std::chrono::system_clock> lastUpdate;

    int spotLifetime = 30; // don't display stuff older than this in minutes
    int maxSpotLifetime = 240; // drop spots older than this in minutes
    ImU32 spotBgColor = IM_COL32(0xCF, 0xFD, 0xBC ,255);
    ImU32 spotTextColor = IM_COL32(0, 0, 0, 255);

    char host[1024];
    int port = 6214;
    bool autoStart = false;
    uint8_t dataBuf[1024];
    net::Listener listener;
    net::Conn client;

    std::string command = "";

    EventHandler<ImGui::WaterFall::FFTRedrawArgs> fftRedrawHandler;
    EventHandler<ImGui::WaterFall::InputHandlerArgs> inputHandler;

    std::list<WaterfallSpot> waterfallSpots;
    std::list<WaterfallLabel> waterfallLabels;
    std::mutex waterfallMutex;
};

MOD_EXPORT void _INIT_() {
    config.setPath(core::args["root"].s() + "/spots_config.json");
    config.load(json::object());
    config.enableAutoSave();
}

MOD_EXPORT ModuleManager::Instance* _CREATE_INSTANCE_(std::string name) {
    return new SpotsModule(name);
}

MOD_EXPORT void _DELETE_INSTANCE_(void* instance) {
    delete (SpotsModule*)instance;
}

MOD_EXPORT void _END_() {
    config.disableAutoSave();
    config.save();
}
