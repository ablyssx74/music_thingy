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
#include <csignal>
#include <fstream>
#include <sys/stat.h>
#include <sys/types.h>
#include <ctime>
#include <unistd.h>
#ifdef __HAIKU__
#include <image.h>
#include <OS.h>
#endif
#include <limits.h>
#include <sstream>

// --- OS Path Helper ---
std::string get_self_path() {
    char buffer[PATH_MAX];
    #ifdef __HAIKU__
    image_info info;
    int32 cookie = 0;
    while (get_next_image_info(0, &cookie, &info) == B_OK) {
        if (info.type == B_APP_IMAGE) return std::string(info.name);
    }
    #else
    // Linux/Unix
    ssize_t count = readlink("/proc/self/exe", buffer, PATH_MAX);
    if (count > 0) return std::string(buffer, count);
    #endif
    return "";
}



std::string statusMsg = "";
std::time_t statusExpiry = 0;
const std::string BASE_URL = "https://somafm.com/";

using json = nlohmann::json;

int selectedFav = 0;
int scrollOffset = 0;
bool showMenu = false;


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

std::string currentSong = "None";
std::string currentDesc = "None";
std::string currentStation = "Press [s] to shuffle!";
std::string currentListeners = "";

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
        curl_easy_setopt(curl, CURLOPT_URL, (BASE_URL + "channels.json").c_str());
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

    // In play_random and play_favorite
    std::string url = BASE_URL + channels[idx].id + ".pls";
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
    #ifdef __HAIKU__
    std::ifstream infile(home + "/config/settings/MusicThingy/favorites.tx");
    #else
    std::ifstream infile(home + "/.config/MusicThingy/favorites.txt");
    #endif
    int lines = 0;
    std::string line;
    while (std::getline(infile, line)) if (!line.empty()) lines++;
    return lines;
}

bool is_favorite() {
    std::string home = getenv("HOME") ? getenv("HOME") : ".";
    #ifdef __HAIKU__
    std::ifstream infile(home + "/config/settings/MusicThingy/favorites.txt");
    #else
    std::ifstream infile(home + "/.config/MusicThingy/favorites.txt");
    #endif


    std::string currentUrl = "";
    for(const auto& ch : channels) {
        if(ch.title == currentStation) {
            currentUrl = BASE_URL + ch.id + ".pls";
            break;
        }
    }

    if (currentUrl.empty()) return false;

    std::string line;
    while (std::getline(infile, line)) {
        if (line == currentUrl) return true;
    }
    return false;
}

std::string get_vol_bar() {
    double vol;
    mpv_get_property(mpv, "volume", MPV_FORMAT_DOUBLE, &vol);
    int filled = (int)(vol / 10);
    std::string bar = "[";
    for (int i = 0; i < 10; ++i) {
        if (i < filled) bar += "|";
        else bar += ".";
    }
    bar += "]";
    return bar;
}

void update_metadata_from_url(const std::string& url) {
    for (const auto& ch : channels) {
        // Match the channel ID within the URL string
        if (url.find(ch.id) != std::string::npos) {
            currentStation = ch.title;
            currentDesc = ch.desc;
            currentListeners = ch.listeners;
            currentSong = "Loading Favorite...";
            break;

        }
    }
}


void draw_favorites_menu() {
    struct winsize w; ioctl(STDOUT_FILENO, TIOCGWINSZ, &w);
    std::string BLUE = "\033[94m", WHITE = "\033[97m", RESET = "\033[0m";
    std::stringstream buffer;

    // Load favorites from file
    std::string home = getenv("HOME") ? getenv("HOME") : ".";
    #ifdef __HAIKU__
    std::ifstream infile(home + "/config/settings/MusicThingy/favorites.txt");
    #else
    std::ifstream infile(home + "/.config/MusicThingy/favorites.txt");
    #endif

    std::vector<std::string> favUrls;
    std::string line;
    while (std::getline(infile, line)) if (!line.empty()) favUrls.push_back(line);

    buffer << "\033[H\033[2J\033[3J" << BLUE;
    buffer << "\033[5;10H=== FAVORITES (Use [j/k] to scroll, [Enter] to play, [q] to back) ===";

    int maxVisible = 10; // How many favorites to show at once
    if (favUrls.empty()) {
        buffer << "\033[7;10H  (No favorites saved yet)";
    } else {
        // Adjust scroll offset to keep selection in view
        if (selectedFav < scrollOffset) scrollOffset = selectedFav;
        if (selectedFav >= scrollOffset + maxVisible) scrollOffset = selectedFav - maxVisible + 1;

        for (int i = 0; i < maxVisible && (i + scrollOffset) < favUrls.size(); ++i) {
            int idx = i + scrollOffset;
            buffer << "\033[" << (7 + i) << ";10H";

            if (idx == selectedFav) buffer << WHITE << " > " << favUrls[idx] << BLUE;
            else buffer << "   " << favUrls[idx];
        }
    }

    buffer << RESET;
    std::cout << buffer.str() << std::flush;

    // Local Input Handling for the Menu
    if (kbhit()) {
        char c = getchar();
        if (c == 'b' || c == 27) showMenu = false;
        if (c == 'k' && selectedFav > 0) selectedFav--;
        if (c == 'j' && selectedFav < (int)favUrls.size() - 1) selectedFav++;

        // Check for both Newline and Carriage Return
        if ((c == '\n' || c == '\r') && !favUrls.empty()) {
            const char *cmd[] = {"loadfile", favUrls[selectedFav].c_str(), NULL};

            // Execute and capture any immediate error
            if (mpv_command(mpv, cmd) < 0) {
                statusMsg = "Error: Could not load file.";
                statusExpiry = std::time(nullptr) + 2;
            } else {
                // Update local metadata state immediately
                update_metadata_from_url(favUrls[selectedFav]);
                showMenu = false;
            }
        }
    }
}

//[\033[31mF\033[33ma\033[32mv\033[36mo\033[34m\033[35mr\033[31mi\033[33mt\033[32me\033[94m]

void draw_ui() {
    struct winsize w; ioctl(STDOUT_FILENO, TIOCGWINSZ, &w);
    std::string BLUE = "\033[94m", RED = "\033[91m", YELLOW = "\033[33m", GREEN = "\033[38;5;46m", RESET = "\033[0m";

    // This is your 'back buffer'
    std::stringstream buffer;

    // Build the frame in memory
    buffer << "\033[H\033[2J\033[3J"; // Full Clear
    buffer << BLUE; // Set the color to BLUE for everything following

    buffer << "\033[" << (w.ws_row - 21) << ";10H"  "                                Music Thingy" << "\n";
    buffer << "\033[" << (w.ws_row - 20) << ";10H" << "[S]huffle | [F]av Play | [L]ist Favs | [A]dd Fav | [D]el Fav | Vol [+/-] [M]ute | [Q]uit" << "\n";

    if (std::time(nullptr) < statusExpiry) {
        buffer << "\033[" << (w.ws_row - 16) << ";10H" << GREEN << ">> " << statusMsg << "\n" << RESET << BLUE ;
    }

    if (!currentSong.empty() && currentSong != "None")
        buffer << "\033[" << (w.ws_row - 15) << ";10H" << " * " << currentSong << "\n";

    if (!currentDesc.empty() && currentDesc != "None" && currentDesc != "None") {
        buffer << "\033[" << (w.ws_row - 14) << ";10H" << " * " << currentDesc;
    }

    buffer << "\033[" << (w.ws_row - 13) << ";10H" << BLUE << " * "  << currentStation;
    if (is_favorite()) buffer << " " << "[\033[31mF\033[33ma\033[32mv\033[36mo\033[34m\033[35mr\033[31mi\033[33mt\033[32me\033[94m]" << RESET;

    if(!currentListeners.empty()) buffer << "\033[" << (w.ws_row - 12) << ";10H" << BLUE << " * Listeners: " << currentListeners;

    buffer << "\033[" << (w.ws_row - 11) << ";10H" << " * Favorites: " << count_favorites() << "\n";
    buffer << "\033[" << (w.ws_row - 10) << ";10H" << " * Vol: " << get_vol_bar() << "\n";

    buffer << "\033[" << w.ws_row << ";0H" << RED << "MusicThingy> ";

    buffer << RESET;
    // ONE SINGLE WRITE to the physical screen (The 'Swap')
    std::cout << buffer.str() << std::flush;
}

void list_favorites() {
    std::string home = getenv("HOME") ? getenv("HOME") : ".";
    #ifdef __HAIKU__
    std::string path = home + "/config/settings/MusicThingy/favorites.txt";
    #else
    std::string path = home + "/.config/MusicThingy/favorites.txt";
    #endif

    std::ifstream infile(path);
    if (!infile.is_open()) {
        statusMsg = "No favorites file found.";
        statusExpiry = std::time(nullptr) + 3;
        return;
    }

    std::string line;
    std::string listStr = "Favorites: ";
    bool first = true;

    while (std::getline(infile, line)) {
        if (line.empty()) continue;

        // Extract the ID from the URL (e.g., "groovesalad" from ".../groovesalad.pls")
        size_t lastSlash = line.find_last_of('/');
        size_t lastDot = line.find_last_of('.');
        if (lastSlash != std::string::npos && lastDot != std::string::npos) {
            std::string id = line.substr(lastSlash + 1, lastDot - lastSlash - 1);

            // Find the human-readable title from your channels vector
            for (const auto& ch : channels) {
                if (ch.id == id) {
                    if (!first) listStr += ", ";
                    listStr += ch.title;
                    first = false;
                    break;
                }
            }
        }
    }

    if (first) statusMsg = "Your favorites list is empty.";
    else statusMsg = listStr;

    statusExpiry = std::time(nullptr) + 10; // Show for 10 seconds
}


void save_favorite() {
    std::string home = getenv("HOME") ? getenv("HOME") : ".";
    #ifdef __HAIKU__
    std::string dir = home + "/config/settings/MusicThingy";
    std::string path = dir + "/favorites.txt";
    #else
    std::string dir = home + "/.config/MusicThingy";
    std::string path = dir + "/favorites.txt";
    #endif



    mkdir(dir.c_str(), 0755);

    // 1. Determine the URL for the current station
    std::string currentUrl = "";
    for(const auto& ch : channels) {
        if(ch.title == currentStation) {
            currentUrl = BASE_URL + ch.id + ".pls";
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
    #ifdef __HAIKU__
    std::string path = home + "/config/settings/MusicThingy/favorites.txt";
    #else
    std::string path = home + "/.config/MusicThingy/favorites.txt";
    #endif



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
    #ifdef __HAIKU__
    std::string path = home + "/config/settings/MusicThingy/favorites.txt";
    #else
    std::string path = home + "/.config/MusicThingy/favorites.txt";
    #endif


    std::string currentUrl = "";
    for(const auto& ch : channels) {
        if(ch.title == currentStation) {
            currentUrl = BASE_URL + ch.id + ".pls";
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



int main(int argc, char* argv[]) {
    // Check if we are running in a terminal (not piped or clicked from GUI)
    if (!isatty(STDIN_FILENO)) {
        std::string path = get_self_path();
        if (path.empty()) return 1;

        std::string cmd = "";

        #ifdef __HAIKU__
        // Haiku: 'Terminal' is always available.
        cmd = "Terminal -t \"Music Thingy\" " + path + " &";
        #else
        // Linux: Search for available terminals
        struct Term { std::string name; std::string flag; };
        std::vector<Term> terms = {
            {"x-terminal-emulator", "-e"},
            {"konsole", "-e"},
            {"gnome-terminal", "--"}, // Modern GNOME requires '--' for command execution
            {"xfce4-terminal", "-e"},
            {"xterm", "-e"}
        };

        for (const auto& t : terms) {
            // Check if the terminal exists in the user's PATH
            if (system(("command -v " + t.name + " >/dev/null 2>&1").c_str()) == 0) {
                cmd = t.name + " " + t.flag + " \"" + path + "\" &";
                break;
            }
        }
        #endif

        if (!cmd.empty()) {
            system(cmd.c_str());
            return 0; // Exit the background process
        } else {
            // Optional: Fallback error if no terminal is found
            return 1;
        }
    }



    srand(time(0));
    signal(SIGWINCH, handle_resize); // Listen for window resize

    init_mpv();
    fetch_channels();

    system("stty raw -echo");

    draw_ui();

    while (true) {
        if (showMenu) {
            draw_favorites_menu();
            usleep(20000);
            continue; // Skip the regular UI while menu is open
        }

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
                case 'l': showMenu = true; selectedFav = 0; break;
                case 's': play_random(); currentSong = "Buffering..."; break;
                case 'a': save_favorite(); break;
                case 'f': play_favorite(); break;
                case 'd': delete_favorite(); break;
                case '+': case '-': set_volume(input); break;
                case 'm': toggle_mute(); break;
            }
            needsRedraw = true;
        }
        if (needsRedraw || resized) {
            resized = 0;
            draw_ui();
        }
        usleep(20000);
    }

    end:
    system("stty cooked echo");
    if (mpv) mpv_terminate_destroy(mpv);
    return 0;
}
