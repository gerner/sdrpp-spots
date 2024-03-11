#ifndef __SDRPP_SPOTS_HTTP_POLLER_H
#define __SDRPP_SPOTS_HTTP_POLLER_H

#include <thread>
#include <mutex>
#include <condition_variable>
#include <chrono>
#include <curl/curl.h>
#include "../main.h"

class HTTPPoller : public SpotProvider {
public:
    HTTPPoller() {
        curl_global_init(CURL_GLOBAL_DEFAULT);
    }

    virtual ~HTTPPoller() {
        curl_global_cleanup();
    }

    void start() {
        std::unique_lock cv_lk(mtx);
        if (running) { return; }

        // join old thread if it killed itself
        if (workerThread.joinable()) { workerThread.join(); }
        running = true;
        flog::info("starting worker thread");
        workerThread = std::thread(&HTTPPoller::worker, this);
    }

    void stop() {
        std::unique_lock cv_lk(mtx);
        if (!running) { return; }

        // let worker know we're shutting down
        cv_lk.unlock();
        cv.notify_all();
        running = false;
        if (workerThread.joinable()) { workerThread.join(); }
    }

protected:
    virtual void processResponse(std::string response) = 0;
    char url[1024];

private:

    void worker() {
        flog::info("worker starting...");
        std::unique_lock cv_lk(mtx);
        while (running) {
            std::string responseBody = "";
            long responseCode;

            CURL* curl = curl_easy_init();
            if (!curl) {
                flog::error("could not get a curl handle");
                break;
            }
            curl_easy_setopt(curl, CURLOPT_URL, url);
            curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, readResponse);
            curl_easy_setopt(curl, CURLOPT_WRITEDATA, &responseBody);
            CURLcode res = curl_easy_perform(curl);

            if (res != CURLE_OK) {
                curl_easy_cleanup(curl);
                flog::error("error making request {0}", url);
                break;
            }
            curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &responseCode);
            if (responseCode != 200) {
                curl_easy_cleanup(curl);
                flog::error("got error: {0}", responseCode);
                break;
            }

            curl_easy_cleanup(curl);

            processResponse(responseBody);

            cv.wait_for(cv_lk, std::chrono::milliseconds(pollPeriod));
        }
        flog::info("worker stopping.");
    }

    static size_t readResponse(void *contents, size_t size, size_t nmemb, void* ctx) {
        std::string* responseBody = (std::string*) ctx;
        responseBody->append((char*) contents, (char*)contents + size*nmemb);
        return size*nmemb;
    }

    // Threading
    int pollPeriod = 15000;
    bool running = false;
    std::thread workerThread;
    std::condition_variable cv;
    std::mutex mtx;
};

#endif //__SDRPP_SPOTS_HTTP_POLLER_H

