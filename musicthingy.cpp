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
#include <csignal> // Added for signal handling
#include <fstream>
#include <sys/stat.h>
#include <sys/types.h>
#include <ctime>

std::string statusMsg = "";
std::time_t statusExpiry = 0;

using json = nlohmann::json;

// --- Global State ---
struct Channel { 
    std::string title; 
    std::string id; 
    std::string desc;     
    std::string listeners; 
};

mpv_handle *mpv = nullptr;
std::vector<Channel> channels;
volatile sig_atomic_t resized = 0; // Flag for resize signal

// Initial State (wrapped in Blue codes)
std::string currentStation = "None";
std::string currentSong = "None";
std::string currentDesc = "None";
std::string currentListeners = "0";

// --- Helper Functions ---
void handle_resize(int sig) { resized = 1; }

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
                        ch.value("description", ""), 
                        ch.value("listeners", "0")    
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

int count_favorites() {
    std::string home = getenv("HOME") ? getenv("HOME") : ".";
    std::ifstream infile(home + "/.config/MusicThingy/favorites.txt");
    int lines = 0;
    std::string line;
    while (std::getline(infile, line)) if (!line.empty()) lines++;
    return lines;
}

void draw_ui() {
    struct winsize w; ioctl(STDOUT_FILENO, TIOCGWINSZ, &w);
    std::string BLUE = "\033[94m", RED = "\033[91m", RESET = "\033[0m";
    
    std::cout << "\033[H\033[2J\033[3J"; // Full Clear
    
    // Top Info
    std::cout << "\033[" << (w.ws_row - 12) << ";10H" << BLUE << "                             Music Thingy" << RESET;
    std::cout << "\033[" << (w.ws_row - 11) << ";10H" << BLUE << "[S]huffle S[a]ve Add [F]avorite [D]elete Favorite [+/-] Vol [M]ute [Q]uit" << RESET;
  
    if (std::time(nullptr) < statusExpiry) {
        std::cout << "\033[" << (w.ws_row - 9) << ";10H" << BLUE << ">> " << statusMsg << RESET;
    }
  
    // Content Block (relative to bottom)
    std::cout << "\033[" << (w.ws_row - 7) << ";10H" << BLUE << " * " << currentSong << RESET;
    std::cout << "\033[" << (w.ws_row - 6) << ";10H" << BLUE << " * " << currentDesc << RESET;
    std::cout << "\033[" << (w.ws_row - 5) << ";10H" << BLUE << " * " << currentStation;
    if(!currentListeners.empty()) 
        std::cout << " (" << currentListeners << " listeners)" << RESET;
    
  // Stats Line: Shows Favs / Total
    std::cout << "\033[" << (w.ws_row - 4) << ";10H" << BLUE  << " * Favorites: " << count_favorites() << RESET;
    std::cout << "\033[" << (w.ws_row - 3) << ";10H" << BLUE  << " * Total Channels: " << channels.size() << RESET;
     
    // Prompt at the very bottom
    std::cout << "\033[" << w.ws_row << ";0H" << RED << "MusicThingy> " << RESET << std::flush;
}

void save_favorite() {
    std::string home = getenv("HOME") ? getenv("HOME") : ".";
    std::string dir = home + "/.config/MusicThingy";
    std::string path = dir + "/favorites.txt";
    mkdir(dir.c_str(), 0755);

    // 1. Determine the URL for the current station
    std::string currentUrl = "";
    for(const auto& ch : channels) {
        if(ch.title == currentStation) { 
            currentUrl = "https://somafm.com/" + ch.id + ".pls"; 
            break; 
        }
    }

    if (currentUrl.empty()) {
        statusMsg = "Cannot save: No station selected.";
        statusExpiry = std::time(nullptr) + 2;
        return;
    }

    // 2. Check if URL already exists in the file
    std::ifstream infile(path);
    std::string line;
    bool isDuplicate = false;
    while (std::getline(infile, line)) {
        if (line == currentUrl) {
            isDuplicate = true;
            break;
        }
    }
    infile.close();

    // 3. Save only if it's NOT a duplicate
    if (isDuplicate) {
        statusMsg = "Already in favorites!";
    } else {
        std::ofstream outfile(path, std::ios_base::app);
        if (outfile.is_open()) {
            outfile << currentUrl << std::endl;
            statusMsg = "URL saved to favorites!";
            outfile.close();
        } else {
            statusMsg = "Error opening file!";
        }
    }
    statusExpiry = std::time(nullptr) + 2;
}


void play_favorite() {
    std::string home = getenv("HOME") ? getenv("HOME") : ".";
    std::string path = home + "/.config/MusicThingy/favorites.txt";
    
    std::ifstream infile(path);
    std::vector<std::string> favs;
    std::string line;
    while (std::getline(infile, line)) if (!line.empty()) favs.push_back(line);
    
    if (favs.empty()) {
        statusMsg = "No favorites saved!";
        statusExpiry = std::time(nullptr) + 2;
        return;
    }

    std::string url = favs[rand() % favs.size()];

    // Extract ID from URL to update global state correctly
    // URL format: https://somafm.com
    size_t lastSlash = url.find_last_of('/');
    size_t lastDot = url.find_last_of('.');
    if (lastSlash != std::string::npos && lastDot != std::string::npos) {
        std::string id = url.substr(lastSlash + 1, lastDot - lastSlash - 1);
        for (const auto& ch : channels) {
            if (ch.id == id) {
                currentStation = ch.title;
                currentDesc = ch.desc;
                currentListeners = ch.listeners;
                break;
            }
        }
    }

    currentSong = "Loading Favorite...";
    const char *cmd[] = {"loadfile", url.c_str(), NULL};
    mpv_command(mpv, cmd);
}


void delete_favorite() {
    std::string home = getenv("HOME") ? getenv("HOME") : ".";
    std::string path = home + "/.config/MusicThingy/favorites.txt";
    
    std::string currentUrl = "";
    for(const auto& ch : channels) {
        if(ch.title == currentStation) { 
            currentUrl = "https://somafm.com/" + ch.id + ".pls"; 
            break; 
        }
    }

    if (currentUrl.empty()) return;

    std::ifstream infile(path);
    std::vector<std::string> remaining;
    std::string line;
    bool removed = false;

    while (std::getline(infile, line)) {
        if (line != currentUrl && !line.empty()) remaining.push_back(line);
        else removed = true;
    }
    infile.close();

    if (removed) {
        std::ofstream outfile(path, std::ios::trunc);
        for (const auto& f : remaining) outfile << f << "\n";
        statusMsg = "Deleted from favorites.";
    } else {
        statusMsg = "Not in favorites.";
    }
    statusExpiry = std::time(nullptr) + 2;
}


// --- Main Engine ---
int main() {


    srand(time(0));
    signal(SIGWINCH, handle_resize); // Listen for window resize
    
    init_mpv();
    fetch_channels();
    
    system("stty raw -echo");

    draw_ui();

    while (true) {
        bool needsRedraw = false;
        
   		// Auto-clear status message after timeout
    if (!statusMsg.empty() && std::time(nullptr) >= statusExpiry) {
        statusMsg = "";
        needsRedraw = true;
    }
        // Check if terminal was resized
        if (resized) {
            resized = 0;
            needsRedraw = true;
        }

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
                case 'a': save_favorite(); break; 
                case 'f': play_favorite(); break; 
                case 'd': delete_favorite(); break; 
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
