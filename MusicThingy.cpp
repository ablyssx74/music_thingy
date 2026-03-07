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
#ifdef __HAIKU__
#include <image.h>
#include <OS.h>
#endif
#include <limits.h>
#include <sstream>
#include <fcntl.h>

// --- Global UI Colors ---
const std::string BLUE   = "\033[94m";
const std::string RED    = "\033[91m";
const std::string ORANGE = "\033[93m";
const std::string WHITE  = "\033[97m";
const std::string YELLOW = "\033[33m";
const std::string GREEN  = "\033[38;5;46m";
const std::string RESET  = "\033[0m";

std::string get_ui_header(int rows) {
    std::stringstream header;
    header << "\033[H\033[2J\033[3J" << BLUE; // <--- FULL CLEAR then BLUE
    header << "\033[" << (rows - 21) << ";10H" << "             Music Thingy\n";
    header << "\033[" << (rows - 20) << ";10H" << "[S]huffle | Vol [+/-] | [H]elp | [Q]uit\n";
    return header.str();
}

std::string get_ui_footer(int rows) {
    std::stringstream footer;
    struct winsize w; ioctl(STDOUT_FILENO, TIOCGWINSZ, &w);
    footer << "\033[" << w.ws_row << ";0H" << RED << "Music Thingy~ $: ";
    return footer.str();
}





std::string statusMsg = "";
std::time_t statusExpiry = 0;
const std::string BASE_URL = "https://somafm.com/";
using json = nlohmann::json;
int selectedFav = 0;
int scrollOffset = 0;
bool showMenu = false;
bool showHelp = false;
bool showNotifications = false;
bool showConfig = false;

// Path for the config file
#ifdef __HAIKU__
std::string configPath = getenv("HOME") + std::string("/config/settings/MusicThingy/config.txt");
#else
std::string configPath = getenv("HOME") + std::string("/.config/MusicThingy/config.txt");
#endif


struct Config {
    bool showNotifications = true;
    bool autoShuffle = false;
    bool showAlbumArt = true;
    bool startMuted = false;
    int defaultVolume = 100;
    std::string quality = "high"; // Options: "highest", "high", "low"
} cfg;

int selectedConfig = 0; // Current menu selection


void save_config() {
    json j;
    j["quality"] = cfg.quality;
    j["showNotifications"] = cfg.showNotifications;
    j["autoShuffle"] = cfg.autoShuffle;
    j["showAlbumArt"] = cfg.showAlbumArt;
    j["startMuted"] = cfg.startMuted;
    j["defaultVolume"] = cfg.defaultVolume;

    std::ofstream outfile(configPath);
    outfile << j.dump(4);
}

void load_config() {
    std::ifstream infile(configPath);
    if (infile.is_open()) {
        try {
            json j = json::parse(infile);
            cfg.quality = j.value("quality", "highest");
            cfg.showNotifications = j.value("showNotifications", true);
            cfg.autoShuffle = j.value("autoShuffle", false);
            cfg.showAlbumArt = j.value("showAlbumArt", true);
            cfg.startMuted = j.value("startMuted", false);
            cfg.defaultVolume = j.value("defaultVolume", 100);
        } catch(...) {}
    }
}



//----EndConfig

// --- For reading arguments from keyboard shortcuts ---
const char* fifoPath = "/tmp/musicthingy_fifo";
const char* respPath = "/tmp/musicthingy_resp";
int fifoFd = -1;

// Delete fifo on exit
void cleanup_fifo() {
    unlink(fifoPath);
    unlink(respPath);
}

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

std::string pendingSong = "";
std::time_t notifyTimer = 0;

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
    channels.clear();
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

void fade_volume(mpv_handle *mpv, double target_vol, double duration_ms) {
    double current_vol;
    mpv_get_property(mpv, "volume", MPV_FORMAT_DOUBLE, &current_vol);

    int steps = 20; // Number of small volume jumps
    double step_size = (target_vol - current_vol) / steps;
    int step_duration = (int)(duration_ms * 1000 / steps); // in microseconds

    for (int i = 0; i < steps; ++i) {
        current_vol += step_size;
        mpv_set_property(mpv, "volume", MPV_FORMAT_DOUBLE, &current_vol);
        usleep(step_duration);
    }
    // Ensure we hit the exact target
    mpv_set_property(mpv, "volume", MPV_FORMAT_DOUBLE, &target_vol);
}

std::string get_quality_url(const std::string& id) {
    if (cfg.quality == "highest") return BASE_URL + id + "256.pls"; // 256k MP3
    if (cfg.quality == "low")     return BASE_URL + id + "64.pls";  // 64k AAC-HE
    return BASE_URL + id + "130.pls"; // Default High (128k AAC)
}

void play_random() {
    if (channels.empty()) return;

    // 1. Fade Out
    double original_vol;
    mpv_get_property(mpv, "volume", MPV_FORMAT_DOUBLE, &original_vol);
    fade_volume(mpv, 0, 300);

    // 2. Pick Station & Load URL
    int idx = rand() % channels.size();
    currentStation = channels[idx].title;
    currentDesc = channels[idx].desc;
    currentListeners = channels[idx].listeners;
    currentSong = "Buffering...";

    // USE THE HELPER HERE
    std::string url = get_quality_url(channels[idx].id);

    const char *cmd[] = {"loadfile", url.c_str(), NULL};
    mpv_command(mpv, cmd);

    // 3. Fade In
    fade_volume(mpv, original_vol, 500);
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
    std::ifstream infile(home + "/config/settings/MusicThingy/favorites.txt");
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


bool draw_help_menu() {
    struct winsize w; ioctl(STDOUT_FILENO, TIOCGWINSZ, &w);

    std::stringstream buffer;

    buffer << get_ui_header(w.ws_row);

    buffer << "\033[6;10H               [b] Back";

    int r = 10; // Start row for shortcuts
    buffer << "\033[" << r++ << ";10H [s] Shuffle      : Play a random station";
    buffer << "\033[" << r++ << ";10H [f] Play Fav     : Play a random favorite";
    buffer << "\033[" << r++ << ";10H [l] List Favs    : Open scrollable favorite menu";
    buffer << "\033[" << r++ << ";10H [a] Add Fav      : Save current station to list";
    buffer << "\033[" << r++ << ";10H [d] Delete Fav   : Remove current station from list";
    buffer << "\033[" << r++ << ";10H [+/-] Volume     : Increase/Decrease volume";
    buffer << "\033[" << r++ << ";10H [m] Mute         : Toggle audio mute";
    buffer << "\033[" << r++ << ";10H [h] Help         : Show this menu";
    buffer << "\033[" << r++ << ";10H [c] Config       : Config Manager";
    buffer << "\033[" << r++ << ";10H [q] Quit         : Exit Music Thingy";

    buffer << get_ui_footer(w.ws_row);
    buffer << RESET;

    std::cout << buffer.str() << std::flush;

    if (kbhit()) {
        char c = getchar();
        if (c == 'b' || c == 27 || c == 'h') {
            return false; // Tell main loop to CLOSE the menu
        }
    }
    return true; // Keep the menu OPEN
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

bool draw_favorites_menu() {
    struct winsize w; ioctl(STDOUT_FILENO, TIOCGWINSZ, &w);

    std::stringstream buffer;

    // 1. Load favorites
    std::string home = getenv("HOME") ? getenv("HOME") : ".";
    #ifdef __HAIKU__
    std::ifstream infile(home + "/config/settings/MusicThingy/favorites.txt");
    #else
    std::ifstream infile(home + "/.config/MusicThingy/favorites.txt");
    #endif

    std::vector<std::string> favUrls;
    std::string line;
    while (std::getline(infile, line)) if (!line.empty()) favUrls.push_back(line);

    // 2. Build UI

    buffer << get_ui_header(w.ws_row);


    buffer << "\033[6;10H [j/k] Scroll | [Enter] Play | [b] Back";

    int maxVisible = 10;
    if (favUrls.empty()) {
        buffer << "\033[7;14H  (No favorites saved yet)";
    } else {
        if (selectedFav < scrollOffset) scrollOffset = selectedFav;
        if (selectedFav >= scrollOffset + maxVisible) scrollOffset = selectedFav - maxVisible + 1;

        for (int i = 0; i < maxVisible && (i + scrollOffset) < (int)favUrls.size(); ++i) {
            int idx = i + scrollOffset;
            buffer << "\033[" << (10 + i) << ";10H";
            if (idx == selectedFav) buffer << BLUE << " > " <<  ORANGE << favUrls[idx] << BLUE;
            else buffer << "   " << favUrls[idx];
        }
    }

    buffer << get_ui_footer(w.ws_row);
    buffer << RESET;
    std::cout << buffer.str() << std::flush;

    // 3. Handle Input
    if (kbhit()) {
        char c = getchar();
        if (c == 'b' || c == 27) return false; // Exit menu
        if (c == 'k' && selectedFav > 0) selectedFav--;
        if (c == 'j' && selectedFav < (int)favUrls.size() - 1) selectedFav++;

        if ((c == '\n' || c == '\r') && !favUrls.empty()) {
            const char *cmd[] = {"loadfile", favUrls[selectedFav].c_str(), NULL};
            mpv_command(mpv, cmd);

            // Manually update metadata here or call your helper
            for (const auto& ch : channels) {
                if (favUrls[selectedFav].find(ch.id) != std::string::npos) {
                    currentStation = ch.title;
                    currentDesc = ch.desc;
                    break;
                }
            }
            return false; // Exit menu after playing
        }
    }

    return true; // Keep menu open if no exit key was pressed
}


//[\033[31mF\033[33ma\033[32mv\033[36mo\033[34m\033[35mr\033[31mi\033[33mt\033[32me\033[94m]

std::string get_bitrate_text() {
    if (cfg.quality == "highest") return "256k";
    if (cfg.quality == "low")     return "64k";
    return "128k"; // Default for "high"
}


void draw_ui() {
    struct winsize w; ioctl(STDOUT_FILENO, TIOCGWINSZ, &w);
    std::stringstream buffer;

    int mute;
    mpv_get_property(mpv, "mute", MPV_FORMAT_FLAG, &mute);
    std::string volColor = mute ? "\033[91m" : "\033[92m"; // Red if muted

    // Build the frame in memory
    buffer << "\033[H\033[2J\033[3J"; // Full Clear

    buffer << get_ui_header(w.ws_row);

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

    buffer << "\033[" << (w.ws_row - 11) << ";10H"  << " * Total Channels: " << (int)channels.size();

    buffer << "\033[" << (w.ws_row - 10) << ";10H" << " * Favorites: " << count_favorites() << "\n";

    buffer << "\033[" << (w.ws_row - 9) << ";10H" << " * Bitrate: " << get_bitrate_text() << "\n";

    buffer << "\033[" << (w.ws_row - 8) << ";10H" << " * Vol: " << volColor << get_vol_bar() << "\n";

    buffer << get_ui_footer(w.ws_row);

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

void send_notification(const std::string& station, const std::string& song) {

    // Filter out common URL patterns to prevent "URL notifications"
    if (song.empty() || song.find("http://") == 0 || song.find("https://") == 0 || song.find("-aac") != std::string::npos || song.find("-mp3") != std::string::npos) {
        return;
    }

    std::string cmd;
    #ifdef __HAIKU__
    cmd = "notify --title \"Music Thingy\" \"" + station + ": " + song + "\" &";
    #else
    cmd = "notify-send \"Music Thingy\" \"" + station + "\n" + song + "\" &";
    #endif
    system(cmd.c_str());
}

bool draw_config_menu() {
    struct winsize w; ioctl(STDOUT_FILENO, TIOCGWINSZ, &w);
    std::stringstream buffer;



    buffer << get_ui_header(w.ws_row);

    // Define the list of options to display
    struct MenuItem { std::string label; bool* val; };
    std::vector<MenuItem> items = {
        {"Desktop Notifications", &cfg.showNotifications},
        {"Auto-Shuffle on Start", &cfg.autoShuffle},
        {"Start Muted",           &cfg.startMuted}
    };

    int totalItems = items.size() + 1; // Toggles + 1 for Quality

    // 1. Draw standard toggles
    for (int i = 0; i < items.size(); ++i) {
        buffer << "\033[" << (12 + i) << ";10H";
        if (i == selectedConfig) buffer << WHITE << " > " << BLUE;
        else buffer << "   ";

        buffer << items[i].label << ": ";

        // COLOR LOGIC FOR ON/OFF
        if (*(items[i].val)) {
            buffer << GREEN << "[ON]" << BLUE; // Green if true
        } else {
            buffer << RED << "[OFF]" << BLUE;  // Red if false
        }
    }

    // 2. Draw the Quality Selector row (High is usually Green, others Yellow/Red)
    int qIdx = items.size();
    buffer << "\033[" << (12 + qIdx) << ";10H";
    if (selectedConfig == qIdx) buffer << WHITE << " > " << BLUE;
    else buffer << "   ";

    buffer << "Audio Quality: [" << GREEN << cfg.quality << BLUE << "]";

    // 3. Add the "Note" if Highest is selected
    if (cfg.quality == "highest") {
        buffer << "\033[" << (4 + qIdx + 2) << ";10H"
        << "\033[93m" << "! Note: 'Highest' may delay notifications" << BLUE;
    }

    buffer << "\033[" << (w.ws_row - 2) << ";10H" << "Settings saved to: " << configPath << RESET;
    std::cout << buffer.str() << std::flush;

    if (kbhit()) {
        char c = std::tolower(getchar());
        if (c == 'b' || c == 27) return false;
        if (c == 'k' && selectedConfig > 0) selectedConfig--;
        if (c == 'j' && selectedConfig < totalItems - 1) selectedConfig++;

        // Handling the Enter Key to Toggle/Cycle
        if (c == '\n' || c == '\r') {
            if (selectedConfig < items.size()) {
                // Toggle the boolean items
                *(items[selectedConfig].val) = !(*(items[selectedConfig].val));
            } else {
                // Cycle the Quality string: highest -> high -> low -> highest
                if (cfg.quality == "highest") cfg.quality = "high";
                else if (cfg.quality == "high") cfg.quality = "low";
                else cfg.quality = "highest";
            }
            save_config(); // Save immediately to disk
        }
    }
    return true;
}




// --- Main Engine ---

int main(int argc, char* argv[]) {
    // --- PART A: SENDER LOGIC (Shortcuts/Terminal Commands) ---
    if (argc > 1) {
        std::string cmd = argv[1]; // Get the command (e.g., "shuffle")
        int fd = open(fifoPath, O_WRONLY | O_NONBLOCK);

        if (fd == -1) {
            std::cerr << "MusicThingy is not running." << std::endl;
            return 1;
        }

        if (cmd == "status") {
            mkfifo(respPath, 0666);
            int respFd = open(respPath, O_RDONLY | O_NONBLOCK);
            write(fd, "status", 6);
            close(fd);

            // Wait for response
            for(int i = 0; i < 50; ++i) {
                char buf[512] = {0};
                if (read(respFd, buf, sizeof(buf)-1) > 0) {
                    std::cout << buf << std::endl;
                    close(respFd); unlink(respPath);
                    return 0;
                }
                usleep(10000);
            }
            close(respFd); unlink(respPath);
            return 1;
        }

        // For all other commands (shuffle, quit, etc.)
        write(fd, cmd.c_str(), cmd.length());
        close(fd);
        return 0; // EXIT SENDER IMMEDIATELY
    }


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

        if (!cmd.empty()) { system(cmd.c_str()); return 0; }
        return 1;
    }


    // 3. PLAYER INITIALIZATION
    srand(time(0));
    signal(SIGWINCH, handle_resize); // Listen for window resize
    atexit(cleanup_fifo);

    mkfifo(fifoPath, 0666);
    int fifoFd = open(fifoPath, O_RDWR | O_NONBLOCK); // Open for Player

    init_mpv();
    fetch_channels();
    system("stty raw -echo");
    draw_ui();

    // --- PLAYER SETUP ---
    load_config();      // Load your new JSON config
    fetch_channels();   // Load SomaFM channels
    init_mpv();

    // --- AUTO-SHUFFLE LOGIC ---
    if (cfg.autoShuffle) {
        play_random(); // Triggers your existing shuffle function
    }

    system("stty raw -echo");
    draw_ui();


    // 4. Main Loop
    while (true) {
        bool needsRedraw = false;

        // --- Inside while(true) loop ---

        // ONLY fire the notification here when the fuse burns down
        if (notifyTimer > 0 && std::time(nullptr) >= notifyTimer) {
            currentSong = pendingSong;

            // IMPORTANT: This is the ONLY place send_notification should be called
            if (cfg.showNotifications) {
                send_notification(currentStation, currentSong);
            }

            notifyTimer = 0; // Reset the fuse
            needsRedraw = true;
        }


        // A. FIFO LISTENER
        char cmdBuf[64]; // Buffer for incoming commands
        ssize_t bytes = read(fifoFd, cmdBuf, sizeof(cmdBuf) - 1);
        if (bytes > 0) {
            cmdBuf[bytes] = '\0';
            std::string cmd(cmdBuf);

            // --- Inside the FIFO Listener (Player side) ---
            if (cmd == "status") {
                int respFd = open(respPath, O_WRONLY | O_NONBLOCK);
                if (respFd != -1) {
                    std::stringstream ss;
                    ss << "Station:   " << currentStation << "\n"
                    << "Now Play:  " << currentSong << "\n"
                    << "Quality: " << get_bitrate_text() << "\n"
                    << "Listeners: " << currentListeners << "\n"
                    << "Stats:     " << count_favorites() << " Favorites | " << channels.size() << " Total Channels";

                    std::string reply = ss.str();
                    write(respFd, reply.c_str(), reply.length());
                    close(respFd);
                }
            }


            else if (cmd == "favorites") {
                play_favorite(); // Reuse existing play_favorite() random logic
                needsRedraw = true;
            }
            else if (cmd == "add_fav") {
                save_favorite();
            }
            else if (cmd == "del_fav") {
                delete_favorite();
            }
            else if (cmd == "quit") {
                goto end; // Jumps to your cleanup and exit logic
            }
            else if (cmd == "shuffle") {
                play_random();
                needsRedraw = true;

            }
            else if (cmd == "vol_up") {
                set_volume('+');
                needsRedraw = true;
            }
            else if (cmd == "vol_down") {
                set_volume('-');
                needsRedraw = true;
            }

        }

        // B. MENU SCREENS

        if (showConfig) {
            showConfig = draw_config_menu(); // Update state directly from the function
            if (!showConfig) {
                draw_ui();
            }
            usleep(10000);
            continue;
        }

        if (showHelp) {
            showHelp = draw_help_menu(); // Update state directly from the function
            if (!showHelp) {
                draw_ui();
            }
            usleep(10000);
            continue;
        }

        if (showMenu) {
            // Apply the same logic to your favorites menu
            showMenu = draw_favorites_menu();
            if (!showMenu) {
                draw_ui();
            }
            usleep(10000);
            continue;
        }

        // C. STATUS EXPIRY

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
        // D. MPV EVENTS
        while (true) {
            mpv_event *event = mpv_wait_event(mpv, 0);
            if (event->event_id == MPV_EVENT_NONE) break;

            if (event->event_id == MPV_EVENT_PROPERTY_CHANGE) {
                mpv_event_property *prop = (mpv_event_property *)event->data;
                if (prop->data && std::string(prop->name) == "media-title") {
                    std::string newTitle = *(char **)prop->data;

                    // 1. Filter out URLs and duplicates
                    if (newTitle != currentSong && newTitle.find("http") != 0) {
                        pendingSong = newTitle;            // Save the "upcoming" song
                        notifyTimer = std::time(nullptr) + 2; // Set the fuse for 2 seconds
                        // Note: Don't update currentSong here; let the timer do it!
                    }
                }

            }
            if (event->event_id == MPV_EVENT_SHUTDOWN) goto end;
        }



        // E. KEYBOARD INPUT
        if (kbhit()) {
            char input = getchar();
            if (input == 'q') break;
            switch (input) {
                case 'l': showMenu = true; selectedFav = 0; break;
                case 's': play_random(); currentSong = "Buffering..."; break;
                case 'a': save_favorite(); break;
                case 'c': showConfig = true; break; // Open Help
                case 'f': play_favorite(); break;
                case 'd': delete_favorite(); break;
                case 'h': showHelp = true; break; // Open Help
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

    struct winsize w;
    ioctl(STDOUT_FILENO, TIOCGWINSZ, &w);
    std::stringstream buffer;
    buffer << get_ui_footer(w.ws_row) << BLUE << "Good bye! " << RESET << std::endl;
    std::cout << buffer.str();

    if (mpv) mpv_terminate_destroy(mpv);
    return 0;
}
