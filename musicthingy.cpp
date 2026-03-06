#include <iostream>
#include <string>
#include <vector>
#include <cstdlib>
#include <sys/ioctl.h>
#include <unistd.h>
#include <curl/curl.h>
#include <mpv/client.h>
#include "nlohmann/json.hpp"
#include <poll.h> 

using json = nlohmann::json;

// --- Global State ---
struct Channel { 
    std::string title; 
    std::string id; 
    std::string desc;     // Added
    std::string listeners; // Added
};

mpv_handle *mpv = nullptr;
std::vector<Channel> channels;

// Add \033[94m (Blue) at the start and \033[0m (Reset) at the end
std::string currentStation = "\033[94mNone\033[0m";
std::string currentSong = "\033[94mNone\033[0m";
std::string currentDesc = "\033[94mNone\033[0m";
std::string currentListeners = "\033[94mNone\033[0m";

// --- Helper Functions ---
bool kbhit() {
    struct pollfd fds; fds.fd = STDIN_FILENO; fds.events = POLLIN;
    return poll(&fds, 1, 0) > 0;
}

static size_t WriteCallback(void* contents, size_t size, size_t nmemb, void* userp) {
    ((std::string*)userp)->append((char*)contents, size * nmemb);
    return size * nmemb;
}

// --- Logic Functions ---

void fetch_channels() {
    CURL* curl = curl_easy_init();
    std::string buffer;
    if(curl) {
        curl_easy_setopt(curl, CURLOPT_URL, "https://somafm.com/channels.json");
        curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, WriteCallback);
        curl_easy_setopt(curl, CURLOPT_WRITEDATA, &buffer);
        curl_easy_setopt(curl, CURLOPT_USERAGENT, "MusicThingy/1.0");
        if(curl_easy_perform(curl) == CURLE_OK) {
            try {
                auto data = json::parse(buffer);
                for (auto& ch : data["channels"]) {
                    channels.push_back({
                        ch.value("title", ""), 
                        ch.value("id", ""),
                        ch.value("description", ""), // Parse description
                        ch.value("listeners", "0")    // Parse listeners
                    });
                }
            } catch(...) {}
        }
        curl_easy_cleanup(curl);
    }
}

void init_mpv() {
    mpv = mpv_create();
    if (!mpv) exit(1);
    mpv_set_option_string(mpv, "input-default-bindings", "yes");
    mpv_set_option_string(mpv, "terminal", "no"); 
    mpv_initialize(mpv);
    mpv_observe_property(mpv, 0, "media-title", MPV_FORMAT_STRING);
}

void play_random() {
    if (channels.empty()) return;
    int idx = rand() % channels.size();
    
    // Update global state with new fields
    currentStation = channels[idx].title;
    currentDesc = channels[idx].desc;
    currentListeners = channels[idx].listeners;
    
    std::string url = "https://somafm.com/" + channels[idx].id + ".pls";
    const char *cmd[] = {"loadfile", url.c_str(), NULL};
    mpv_command(mpv, cmd);
}

void set_volume(char direction) {
    double vol;
    mpv_get_property(mpv, "volume", MPV_FORMAT_DOUBLE, &vol);
    if (direction == '+') vol += 5;
    if (direction == '-') vol -= 5;
    if (vol > 100) vol = 100; if (vol < 0) vol = 0;
    mpv_set_property(mpv, "volume", MPV_FORMAT_DOUBLE, &vol);
}

void toggle_mute() {
    int mute;
    mpv_get_property(mpv, "mute", MPV_FORMAT_FLAG, &mute);
    mute = !mute;
    mpv_set_property(mpv, "mute", MPV_FORMAT_FLAG, &mute);
}

void draw_ui() {
    struct winsize w; ioctl(STDOUT_FILENO, TIOCGWINSZ, &w);
    std::string BLUE = "\033[94m", RED = "\033[91m", RESET = "\033[0m", GREY = "\033[90m";
    
    std::cout << "\033[H\033[2J\033[3J"; // Clear screen
    
    std::cout << "\033[" << (w.ws_row - 15) << ";10H" << BLUE << "Music Thingy" << RESET;
    std::cout << "\033[" << (w.ws_row - 14) << ";10H" << BLUE << "[S]huffle [+/-] Vol [M]ute [Q]uit" << RESET;
  
     // Print Song
    std::cout << "\033[" << (w.ws_row - 10) << ";10H" << BLUE << " * " << currentSong;
   
     // Print Description
    std::cout << "\033[" << (w.ws_row - 9) << ";10H" << BLUE << " * " << currentDesc << RESET;

    
    // Print Station and Listener count
    std::cout << "\033[" << (w.ws_row - 8) << ";10H"  << BLUE << " * " << currentStation;
    if(!currentListeners.empty()) 
        std::cout << BLUE << " (" << currentListeners << " listeners)" << RESET;
     
    std::cout << "\033[" << w.ws_row << ";0H" << RED << "MusicThingy> " << RESET << std::flush;
}

// --- Main Engine ---
int main() {
    srand(time(0));
    init_mpv();
    fetch_channels();
    
    system("stty raw -echo");
    draw_ui();

    while (true) {
        bool needsRedraw = false;
        while (true) {
            mpv_event *event = mpv_wait_event(mpv, 0);
            if (event->event_id == MPV_EVENT_NONE) break;
            if (event->event_id == MPV_EVENT_PROPERTY_CHANGE) {
                mpv_event_property *prop = (mpv_event_property *)event->data;
                if (prop->data && std::string(prop->name) == "media-title") {
                    currentSong = *(char **)prop->data;
                    needsRedraw = true;
                }
            }
            if (event->event_id == MPV_EVENT_SHUTDOWN) goto end;
        }

        if (kbhit()) {
            char input = getchar();
            if (input == 'q') break;
            switch (input) {
                case 's': play_random(); currentSong = "Buffering..."; break;
                case '+': case '-': set_volume(input); break;
                case 'm': toggle_mute(); break;
            }
            needsRedraw = true;
        }
        if (needsRedraw) draw_ui();
        usleep(10000); 
    }

end:
    system("stty cooked echo");
    if (mpv) mpv_terminate_destroy(mpv);
    return 0;
}
