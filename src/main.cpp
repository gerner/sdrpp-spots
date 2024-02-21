#include <iostream>
#include <sstream>
#include <vector>
#include <list>
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

struct WaterfallSpot {
    std::string label;
    double frequency;
    std::chrono::time_point<std::chrono::system_clock> spotTime;
};

class SpotsModule : public ModuleManager::Instance {
public:
    SpotsModule(std::string name) {
        this->name = name;

        //TODO: config initialization
        strcpy(hostname, "localhost");

        fftRedrawHandler.ctx = this;
        fftRedrawHandler.handler = fftRedraw;

        gui::menu.registerEntry(name, menuHandler, this, NULL);
        gui::waterfall.onFFTRedraw.bindHandler(&fftRedrawHandler);
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
        //TODO: config
        //TODO: start/stop server
    }

    static void fftRedraw(ImGui::WaterFall::FFTRedrawArgs args, void* ctx) {
        SpotsModule* _this = (SpotsModule*)ctx;

        std::lock_guard lk(_this->waterfallMutex);
        auto expirationTime = std::chrono::system_clock::now() - _this->spotLifetime;

        std::vector<float> lanePositions;
        float laneHeight = ImGui::CalcTextSize("TEST").y + 2;
        int laneLimit = 8;
        for (auto it = _this->waterfallSpots.begin(); it != _this->waterfallSpots.end();) {

            // handle expiration of spots
            if(it->spotTime < expirationTime) {
                //flog::info("{0} expired {1} < {2}", it->label, std::chrono::system_clock::to_time_t(it->spotTime), std::chrono::system_clock::to_time_t(expirationTime));
                it = _this->waterfallSpots.erase(it);
                continue;
            //} else {
                //flog::info("{0} not expired {1} < {2}", it->label, std::chrono::system_clock::to_time_t(it->spotTime), std::chrono::system_clock::to_time_t(expirationTime));
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
                args.window->DrawList->AddRectFilled(clampedRectMin, clampedRectMax, _this->spotBgColor);
                args.window->DrawList->AddText(ImVec2(centerXpos - (nameSize.x / 2), targetY), _this->spotTextColor, it->label.c_str());
            }

            // make sure to get the next element in the spot list!
            ++it;
        }
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
        // spot command: DX \t SPOTTER \t FREQ \t DX \t COMMENT \t TIME"
        std::string resp;
        std::vector<std::string> commandParts = split(cmd, '\t');
        if(commandParts.size() == 0) {
            resp = "1 ERROR\n";
            client->write(resp.size(), (uint8_t*)resp.c_str());
            return;
        }

        if("DX" == commandParts[0]) {
            if(commandParts.size() < 6) {
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

            if(spotTime < std::chrono::system_clock::now() - spotLifetime) {
                // silently drop already expired spots
                resp = "0 OK\n";
                client->write(resp.size(), (uint8_t*)resp.c_str());
                return;
            }

            std::string label = commandParts[3];

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
                        }
                        break;
                    }
                }
                // if so, update it
                // if not, add it
                if(!found) {
                    WaterfallSpot spot = {commandParts[3], frequency, spotTime};
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

    /*void worker() {
        // a comment in discord_integration claims SDR++ author is working on a
        // timer which we should probably be using
        while (workerRunning) {
            std::unique_lock cv_lk(m);
            cv.wait_for(cv_lk, 10000ms);
            if(!workerRunning) {
                return;
            }

            // expire spots
            {
                std::lock_guard lk(waterfallMutex);
            }
        }
    }*/

    std::string name;
    bool enabled = true;

    std::chrono::duration<int64_t> spotLifetime = std::chrono::minutes(30);
    ImU32 spotBgColor = IM_COL32(0xCF, 0xFD, 0xBC ,255);
    ImU32 spotTextColor = IM_COL32(0, 0, 0, 255);

    char hostname[1024];
    int port = 6214;
    uint8_t dataBuf[1024];
    net::Listener listener;
    net::Conn client;

    std::string command = "";

    EventHandler<ImGui::WaterFall::FFTRedrawArgs> fftRedrawHandler;

    std::list<WaterfallSpot> waterfallSpots;
    std::mutex waterfallMutex;

    /*
    // Threading
    std::thread workerThread;
    bool workerRunning;
    std::condition_variable cv;
    std::mutex m;
    */
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
