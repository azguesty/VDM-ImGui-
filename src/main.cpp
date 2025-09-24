#include <imgui.h>
#include <imgui_impl_glfw.h>
#include <imgui_impl_opengl3.h>
#include <GLFW/glfw3.h>
#include <iostream>
#include <vector>
#include <string>
#include <thread>
#include <atomic>
#include <sstream>
#include <filesystem>
#include <cstdio>      // For FILE, popen, pclose
#include <mutex>
#include <fstream>
#include <regex>

#include "DownloadManager.h"
#include "DownloadItemFactory.h"
#include "paths.h"
#include "Theme.h"

#ifdef _WIN32
#define NOMINMAX

#include <Shobjidl.h>  // Only for folder browser dialog
#include <comdef.h>    // Only for COM error handling
#include <shlobj.h>    // Only for getting Downloads folder

#define popen _popen
#define pclose _pclose

#else
#include <unistd.h>
#endif

// Paths
const std::filesystem::path YTDLP_PATH = std::filesystem::absolute("src/thirdparty/bin/yt-dlp.exe");
const std::filesystem::path FFMPEG_PATH = std::filesystem::absolute("src/thirdparty/bin/ffmpeg.exe");
const std::filesystem::path HISTORY_FILE = "Saves/download_history.json";

// Enhanced format structure to match Python
struct FormatInfo {
    std::string format_id;
    std::string ext;
    std::string resolution;
    std::string vcodec;
    std::string acodec;
    int fps = 0;
    std::string filesize;
    std::string note;
    std::string type;
    int width = 0;
    int height = 0;
    float tbr = 0.0f;
    long long filesize_bytes = 0;
    bool is_best_mp4 = false;
};

struct SearchResult {
    std::string id;
    std::string title;
    std::string uploader;
    std::string url;
    std::string webpage_url;
    long long duration = 0;
    long long view_count = 0;
};

struct HistoryEntry {
    std::string title;
    std::string url;
    std::string format_id;
    std::string format_type;
    std::string output_path;
    std::string status;
    std::string file_size;
    std::string added_time;
    std::string start_time;
    std::string end_time;
};

struct VideoInfo {
    std::string title;
    std::string uploader;
    int duration = 0;
    long long view_count = 0;
    std::string description;
};

// Global data
std::vector<FormatInfo> g_formats;
std::vector<SearchResult> g_search_results;
std::vector<HistoryEntry> g_history;
VideoInfo g_video_info;
DownloadManager g_manager;

// UI State
char g_url_input[1024] = "";
char g_filter_input[256] = "";
std::string g_outputFolder = "downloads";
std::string g_customOutputPath = "";
bool g_useDownloadsFolder = true; // true = downloads folder, false = custom path
int g_currentTab = 0;
int g_inputMode = 0; // 0=URL, 1=Search YouTube, 2=Search All Sites
int g_searchLimit = 10;
int g_selectedFormat = -1;
int g_selectedSearch = -1;
bool g_showBestHighlight = true;
bool g_showConsole = true;
bool g_fetchingFormats = false;
bool g_fetchingSearch = false;
bool g_showSearchTable = false;
int g_maxConcurrent = 3;
bool g_useDarkTheme = true;

// Console and processing
std::vector<std::string> g_consoleMessages;
std::mutex g_consoleMutex;
std::mutex g_queueMutex;
std::string g_info_buffer;
std::atomic<bool> g_processing_info{ false };

// Utility functions
void logToConsole(const std::string& message) {
    std::lock_guard<std::mutex> lock(g_consoleMutex);
    g_consoleMessages.push_back(message);
    if (g_consoleMessages.size() > 1000) {
        g_consoleMessages.erase(g_consoleMessages.begin());
    }
}

std::string formatDuration(int seconds) {
    if (seconds <= 0) return "Unknown";
    int hours = seconds / 3600;
    int minutes = (seconds % 3600) / 60;
    int secs = seconds % 60;
    if (hours > 0) {
        return std::to_string(hours) + ":" +
            (minutes < 10 ? "0" : "") + std::to_string(minutes) + ":" +
            (secs < 10 ? "0" : "") + std::to_string(secs);
    }
    return (minutes < 10 ? "0" : "") + std::to_string(minutes) + ":" +
        (secs < 10 ? "0" : "") + std::to_string(secs);
}

std::string formatViewCount(long long count) {
    if (count <= 0) return "Unknown";
    if (count >= 1000000000) {
        return std::to_string(count / 1000000000) + "." +
            std::to_string((count % 1000000000) / 100000000) + "B views";
    }
	else if (count >= 1000000) {
        return std::to_string(count / 1000000) + "." +
            std::to_string((count % 1000000) / 100000) + "M views";
    }
    else if (count >= 1000) {
        return std::to_string(count / 1000) + "." +
            std::to_string((count % 1000) / 100) + "K views";
    }
    return std::to_string(count) + " views";
}

std::string formatFileSize(long long bytes) {
    if (bytes <= 0) return "";
    float mb = bytes / (1024.0f * 1024.0f);
    return std::to_string((int)(mb * 100) / 100.0f) + " MiB";
}

std::string sanitizeFilename(const std::string& filename) {
    std::string result = filename;
    std::regex invalid_chars(R"([<>:"/\\|?*\s])");
    result = std::regex_replace(result, invalid_chars, "_");
    result = result.erase(result.length() - 1);
    return result;
}

std::string getCurrentOutputPath() {
    return g_useDownloadsFolder ? g_outputFolder : g_customOutputPath;
}

// JSON parsing helpers
std::string extractJsonString(const std::string& json, const std::string& key) {
    std::string search = "\"" + key + "\"";
    size_t pos = json.find(search);
    if (pos == std::string::npos) return "";

    pos = json.find("\"", pos + search.length());
    if (pos == std::string::npos) return "";
    pos++;

    size_t end = json.find("\"", pos);
    if (end == std::string::npos) return "";

    return json.substr(pos, end - pos);
}

long long extractJsonInt(const std::string& json, const std::string& key) {
    std::string search = "\"" + key + "\":";
    size_t pos = json.find(search);
    if (pos == std::string::npos) return 0;
    pos += search.length();
    while (pos < json.length() && (json[pos] == ' ' || json[pos] == '\t')) pos++;
    std::string number;
    while (pos < json.length() && (isdigit(json[pos]) || json[pos] == '.')) {
        number += json[pos];
        pos++;
    }
    if (number.empty()) return 0;

    try {
        return std::stoll(number);  // Changed from std::stoi to std::stoll
    }
    catch (const std::invalid_argument&) {
        return 0;
    }
    catch (const std::out_of_range&) {
        return 0;  // or LLONG_MAX if you prefer
    }
}

// History management
void saveHistory() {
    std::filesystem::create_directories(HISTORY_FILE.parent_path());
    std::ofstream file(HISTORY_FILE);
    if (!file.is_open()) return;

    file << "[\n";
    for (size_t i = 0; i < g_history.size(); ++i) {
        const auto& entry = g_history[i];
        file << "  {\n";
        file << "    \"title\": \"" << entry.title << "\",\n";
        file << "    \"url\": \"" << entry.url << "\",\n";
        file << "    \"format_id\": \"" << entry.format_id << "\",\n";
        file << "    \"format_type\": \"" << entry.format_type << "\",\n";
        file << "    \"output_path\": \"" << entry.output_path << "\",\n";
        file << "    \"status\": \"" << entry.status << "\",\n";
        file << "    \"file_size\": \"" << entry.file_size << "\",\n";
        file << "    \"added_time\": \"" << entry.added_time << "\",\n";
        file << "    \"start_time\": \"" << entry.start_time << "\",\n";
        file << "    \"end_time\": \"" << entry.end_time << "\"\n";
        file << "  }";
        if (i < g_history.size() - 1) file << ",";
        file << "\n";
    }
    file << "]\n";
}

void loadHistory() {
    if (!std::filesystem::exists(HISTORY_FILE)) return;

    std::ifstream file(HISTORY_FILE);
    if (!file.is_open()) return;

    std::string content((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
    file.close();

    // Simple JSON parsing for history entries
    size_t pos = 0;
    while ((pos = content.find("{", pos)) != std::string::npos) {
        size_t end = content.find("}", pos);
        if (end == std::string::npos) break;

        std::string entry_json = content.substr(pos, end - pos + 1);

        HistoryEntry entry;
        entry.title = extractJsonString(entry_json, "title");
        entry.url = extractJsonString(entry_json, "url");
        entry.format_id = extractJsonString(entry_json, "format_id");
        entry.format_type = extractJsonString(entry_json, "format_type");
        entry.output_path = extractJsonString(entry_json, "output_path");
        entry.status = extractJsonString(entry_json, "status");
        entry.file_size = extractJsonString(entry_json, "file_size");
        entry.added_time = extractJsonString(entry_json, "added_time");
        entry.start_time = extractJsonString(entry_json, "start_time");
        entry.end_time = extractJsonString(entry_json, "end_time");

        g_history.push_back(entry);
        pos = end + 1;
    }

    logToConsole("[HISTORY] Loaded " + std::to_string(g_history.size()) + " history entries");
}

void addToHistory(std::shared_ptr<DownloadItem> item) {
    HistoryEntry entry;
    entry.title = item->title;
    entry.url = item->url;
    entry.format_id = item->format_id;
    entry.format_type = item->type;
    entry.output_path = item->output_path;
    entry.status = item->status;

    // Get current time as string - using localtime_s for safety
    auto now = std::chrono::system_clock::now();
    auto time_t = std::chrono::system_clock::to_time_t(now);
    char time_str[100];
    struct tm timeinfo;
    localtime_s(&timeinfo, &time_t);
    strftime(time_str, sizeof(time_str), "%Y-%m-%d %H:%M:%S", &timeinfo);
    entry.added_time = std::string(time_str);

    g_history.insert(g_history.begin(), entry);
    saveHistory();
    logToConsole("[HISTORY] Added entry: " + entry.title);
}

void updateToHistory(std::shared_ptr<DownloadItem> item) {
    for (auto& entry : g_history) {
        if (entry.title == item->title && entry.url == item->url) {
            entry.status = item->status;
            // Update end time if completed or failed
            if (item->status == "Completed" || item->status == "Failed") {
                auto now = std::chrono::system_clock::now();
                auto time_t = std::chrono::system_clock::to_time_t(now);
                char time_str[100];
                struct tm timeinfo;
                localtime_s(&timeinfo, &time_t);
                strftime(time_str, sizeof(time_str), "%Y-%m-%d %H:%M:%S", &timeinfo);
                entry.end_time = std::string(time_str);
            }
            saveHistory();
            logToConsole("[HISTORY] Updated entry: " + entry.title);
            break;
        }
    }
}

// Format fetching with real JSON parsing
void fetchFormats(const std::string& url) {
    g_formats.clear();
    g_video_info = {};
    g_fetchingFormats = true;
    g_selectedFormat = -1;
    g_info_buffer.clear();
    g_processing_info = true;

    logToConsole("[INFO] Fetching formats for: " + url);

    std::thread([url]() {
        std::string cmd = YTDLP_PATH.string() + " -J --no-warnings " + url;

        FILE* pipe = _popen(cmd.c_str(), "r");
        if (!pipe) {
            logToConsole("[ERROR] Failed to start yt-dlp process");
            g_fetchingFormats = false;
            g_processing_info = false;
            return;
        }

        char buffer[4096];
        std::string output;
        while (fgets(buffer, sizeof(buffer), pipe)) {
            output += buffer;
            logToConsole("[INFO] " + std::string(buffer));
        }

        int exit_code = _pclose(pipe);
        g_fetchingFormats = false;

        if (exit_code != 0) {
            logToConsole("[ERROR] yt-dlp failed with exit code: " + std::to_string(exit_code));
            g_processing_info = false;
            return;
        }

        if (output.empty()) {
            logToConsole("[ERROR] No output from yt-dlp");
            g_processing_info = false;
            return;
        }

        // Parse video info
        g_video_info.title = extractJsonString(output, "title");
        g_video_info.uploader = extractJsonString(output, "uploader");
        g_video_info.duration = extractJsonInt(output, "duration");
        g_video_info.view_count = extractJsonInt(output, "view_count");

        logToConsole("[INFO] Video title: " + g_video_info.title);

        // Parse formats
        size_t formats_pos = output.find("\"formats\":");
        if (formats_pos == std::string::npos) {
            logToConsole("[ERROR] No formats found in JSON");
            g_processing_info = false;
            return;
        }

        // Find the formats array
        size_t array_start = output.find("[", formats_pos);
        if (array_start == std::string::npos) {
            logToConsole("[ERROR] Invalid formats array");
            g_processing_info = false;
            return;
        }

        // Parse each format object
        size_t pos = array_start + 1;
        int brace_count = 0;
        std::string format_json;

        while (pos < output.length()) {
            char c = output[pos];

            if (c == '{') {
                if (brace_count == 0) {
                    format_json.clear();
                }
                format_json += c;
                brace_count++;
            }
            else if (c == '}') {
                format_json += c;
                brace_count--;

                if (brace_count == 0) {
                    // Parse this format
                    FormatInfo fmt;
                    fmt.format_id = extractJsonString(format_json, "format_id");
                    fmt.ext = extractJsonString(format_json, "ext");
                    fmt.resolution = extractJsonString(format_json, "resolution");
                    fmt.vcodec = extractJsonString(format_json, "vcodec");
                    fmt.acodec = extractJsonString(format_json, "acodec");
                    fmt.fps = extractJsonInt(format_json, "fps");
                    fmt.note = extractJsonString(format_json, "format_note");
                    fmt.width = extractJsonInt(format_json, "width");
                    fmt.height = extractJsonInt(format_json, "height");
                    fmt.tbr = extractJsonInt(format_json, "tbr");
                    fmt.filesize_bytes = extractJsonInt(format_json, "filesize");

                    // Determine type
                    if (fmt.vcodec == "none" || fmt.vcodec.empty()) {
                        fmt.type = "Audio";
                    }
                    else {
                        fmt.type = "Video";
                    }

                    // Format resolution
                    if (fmt.width > 0 && fmt.height > 0) {
                        fmt.resolution = std::to_string(fmt.width) + "x" + std::to_string(fmt.height);
                    }

                    // Format file size
                    if (fmt.filesize_bytes > 0) {
                        fmt.filesize = formatFileSize(fmt.filesize_bytes);
                    }

                    if (!fmt.format_id.empty()) {
                        g_formats.push_back(fmt);
                    }
                }
            }
            else if (brace_count > 0) {
                format_json += c;
            }
            else if (c == ']') {
                break;
            }

            pos++;
        }

        // Find best MP4 format
        FormatInfo* best_mp4 = nullptr;
        int best_quality = 0;

        for (auto& fmt : g_formats) {
            if (fmt.ext == "mp4" && fmt.type == "Video") {
                int quality = fmt.height * fmt.width;
                if (quality > best_quality) {
                    if (best_mp4) best_mp4->is_best_mp4 = false;
                    fmt.is_best_mp4 = true;
                    best_mp4 = &fmt;
                    best_quality = quality;
                }
            }
        }

        logToConsole("[INFO] Successfully parsed " + std::to_string(g_formats.size()) + " formats");
        g_processing_info = false;
        }).detach();
}

// Search functionality with real yt-dlp integration
void performSearch(const std::string& query) {
    g_search_results.clear();
    g_fetchingSearch = true;
    g_selectedSearch = -1;
    logToConsole("[SEARCH] Searching for: " + query);
    std::thread([query]() {
        std::string search_type = (g_inputMode == 1) ? "ytsearch" : "ytsearch";
        std::string search_query = search_type + std::to_string(g_searchLimit) + ":" + query;
        std::string cmd = YTDLP_PATH.string() + " --flat-playlist --dump-json " + search_query;

        FILE* pipe = popen(cmd.c_str(), "r");
        if (!pipe) {
            logToConsole("[SEARCH ERROR] Failed to start search process");
            g_fetchingSearch = false;
            return;
        }

        char buffer[4096];
        std::string output;
        while (fgets(buffer, sizeof(buffer), pipe)) {
            output += buffer;
            logToConsole("[SEARCH] " + std::string(buffer));
        }

        int exit_code = pclose(pipe);
        g_fetchingSearch = false;

        if (exit_code != 0) {
            logToConsole("[SEARCH ERROR] Search failed with exit code: " + std::to_string(exit_code));
            return;
        }

        // Parse search results - each line is a JSON object
        std::istringstream ss(output);
        std::string line;
        while (std::getline(ss, line) && g_search_results.size() < g_searchLimit) {
            if (line.empty() || line[0] != '{') continue;

            SearchResult result;
            result.id = extractJsonString(line, "id");
            result.title = extractJsonString(line, "title");
            result.uploader = extractJsonString(line, "uploader");
            result.url = extractJsonString(line, "url");
            result.webpage_url = extractJsonString(line, "webpage_url");
            result.duration = extractJsonInt(line, "duration");
            result.view_count = extractJsonInt(line, "view_count");

            if (result.webpage_url.empty()) {
                result.webpage_url = result.url;
            }

            if (!result.title.empty()) {
                g_search_results.push_back(result);
            }
        }

        g_showSearchTable = !g_search_results.empty();
        logToConsole("[SEARCH] Found " + std::to_string(g_search_results.size()) + " results");
        }).detach();
}

// Enhanced download functions
void startDownload(const FormatInfo& fmt) {
    if (g_video_info.title.empty()) {
        logToConsole("[ERROR] No video info available");
        return;
    }

    std::string current_output_path = getCurrentOutputPath();
    std::filesystem::create_directories(current_output_path);

    std::string title = sanitizeFilename(g_video_info.title);
    std::string ext = (fmt.type == "Audio") ? fmt.ext : "mkv";
    std::string filename = title + "." + ext;
    std::string output_path = current_output_path + "\\" + filename;

    auto item = DownloadItemFactory::create(g_url_input, fmt.format_id, fmt.type, output_path, title);
    item->ytDlpPath = YTDLP_PATH;

    g_manager.addToQueue(item);
    addToHistory(item);

    logToConsole("[DOWNLOAD] Added to queue: " + title + " (Format: " + fmt.format_id + ")");
}

void startDownloadBest() {
    if (g_formats.empty()) {
        logToConsole("[ERROR] No formats available");
        return;
    }

    // Find best format
    FormatInfo* best_fmt = nullptr;
    for (auto& fmt : g_formats) {
        if (fmt.is_best_mp4) {
            best_fmt = &fmt;
            break;
        }
    }

    if (!best_fmt) {
        // Fallback to first video format
        for (auto& fmt : g_formats) {
            if (fmt.type == "Video") {
                best_fmt = &fmt;
                break;
            }
        }
    }

    if (!best_fmt) {
        logToConsole("[ERROR] No suitable format found");
        return;
    }

    startDownload(*best_fmt);
}

// Render functions
void renderConsole() {
    if (!g_showConsole) return;
    ImGui::SetNextWindowSize(ImVec2(800, 200), ImGuiCond_FirstUseEver);
    ImGui::Begin("Console Output", &g_showConsole);
    if (ImGui::Button("Clear Console")) {
        std::lock_guard<std::mutex> lock(g_consoleMutex);
        g_consoleMessages.clear();
    }
    ImGui::Separator();

    // Track message count to detect new messages
    static size_t prevMessageCount = 0;

    ImGui::BeginChild("ConsoleScrollArea", ImVec2(0, 0), true);

    bool hasNewMessages = false;
    {
        std::lock_guard<std::mutex> lock(g_consoleMutex);

        // Check if new messages were added
        if (g_consoleMessages.size() > prevMessageCount) {
            hasNewMessages = true;
            prevMessageCount = g_consoleMessages.size();
        }

        for (const auto& message : g_consoleMessages) {
            // Color code messages
            if (message.find("[ERROR]") != std::string::npos) {
                ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.0f, 0.4f, 0.4f, 1.0f));
            }
            else if (message.find("[SEARCH]") != std::string::npos) {
                ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.4f, 0.8f, 1.0f, 1.0f));
            }
            else if (message.find("[DOWNLOAD]") != std::string::npos) {
                ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.4f, 1.0f, 0.4f, 1.0f));
            }
            else {
                ImGui::PushStyleColor(ImGuiCol_Text, Theme::getTextColor());
            }
            ImGui::TextWrapped("%s", message.c_str());
            ImGui::PopStyleColor();
        }
    }

    // Only auto-scroll when new messages arrive
    if (hasNewMessages) {
        ImGui::SetScrollHereY(1.0f);
    }

    ImGui::EndChild();
    ImGui::End();
}

void renderDownloaderTab() {
    // Input mode selection
    const char* modes[] = { "URL", "Search YouTube", "Search All Sites" };
    ImGui::PushItemWidth(150);
    if (ImGui::Combo("##InputMode", &g_inputMode, modes, 3)) {
        g_showSearchTable = false;
        g_search_results.clear();
    }
    ImGui::PopItemWidth();

    ImGui::SameLine();

    // URL/Search input
    ImGui::PushItemWidth(-220);
    if (g_inputMode == 0) {
        ImGui::InputTextWithHint("##URL", "Enter video URL (YouTube, TikTok, Instagram, Facebook, etc.)", g_url_input, sizeof(g_url_input));
    }
    else {
        ImGui::InputTextWithHint("##Search", "Search videos...", g_url_input, sizeof(g_url_input));
    }
    ImGui::PopItemWidth();

    ImGui::SameLine();

    if (g_inputMode == 0) {
        if (ImGui::Button("Fetch Formats")) {
            std::string url = std::string(g_url_input);
            if (!url.empty()) {
                fetchFormats(url);
            }
        }
    }
    else {
        if (ImGui::Button("Search")) {
            std::string query = std::string(g_url_input);
            if (!query.empty()) {
                performSearch(query);
            }
        }

        // Search options
        ImGui::SameLine();
        ImGui::Text("Results:");
        ImGui::SameLine();
        ImGui::PushItemWidth(70);
        ImGui::InputInt("##SearchLimit", &g_searchLimit, 1, 5);
        g_searchLimit = std::max(1, std::min(50, g_searchLimit));
        ImGui::PopItemWidth();
    }

    // Search results table
    if (g_showSearchTable && g_inputMode != 0) {
        ImGui::Separator();
        ImGui::Text("Search Results:");

        if (g_fetchingSearch) {
            ImGui::Text("Fetching results...");
        }
        else if (!g_search_results.empty()) {
            if (ImGui::BeginTable("SearchResults", 4, ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY, ImVec2(0, 200))) {
                ImGui::TableSetupColumn("Title", ImGuiTableColumnFlags_WidthStretch);
                ImGui::TableSetupColumn("Uploader", ImGuiTableColumnFlags_WidthFixed, 150);
                ImGui::TableSetupColumn("Duration", ImGuiTableColumnFlags_WidthFixed, 80);
                ImGui::TableSetupColumn("View Count", ImGuiTableColumnFlags_WidthFixed, 100);
                ImGui::TableHeadersRow();

                for (int i = 0; i < g_search_results.size(); ++i) {
                    const auto& result = g_search_results[i];
                    ImGui::TableNextRow();

                    if (ImGui::TableSetColumnIndex(0)) {
                        if (ImGui::Selectable(result.title.c_str(), g_selectedSearch == i, ImGuiSelectableFlags_SpanAllColumns)) {
                            g_selectedSearch = i;
                        }
                        if (ImGui::IsItemHovered() && ImGui::IsMouseDoubleClicked(0)) {
                            // Double click to select
                            const auto& selected = g_search_results[i];
                            strcpy_s(g_url_input, selected.webpage_url.c_str());
                            g_inputMode = 0;
                            g_showSearchTable = false;
                            fetchFormats(selected.webpage_url);
                        }
                    }
                    if (ImGui::TableSetColumnIndex(1)) ImGui::Text("%s", result.uploader.c_str());
                    if (ImGui::TableSetColumnIndex(2)) ImGui::Text("%s", formatDuration(result.duration).c_str());
                    if (ImGui::TableSetColumnIndex(3)) ImGui::Text("%s", formatViewCount(result.view_count).c_str());
                }
                ImGui::EndTable();
            }

            if (g_selectedSearch >= 0 && ImGui::Button("Select from Search Results")) {
                const auto& selected = g_search_results[g_selectedSearch];
                strcpy_s(g_url_input, selected.webpage_url.c_str());
                g_inputMode = 0;
                g_showSearchTable = false;
                fetchFormats(selected.webpage_url);
            }
        }
    }

    ImGui::Separator();

    // Filter
    ImGui::PushItemWidth(200);
    ImGui::InputTextWithHint("##Filter", "Filter by resolution, type, codec, itag...", g_filter_input, sizeof(g_filter_input));
    ImGui::PopItemWidth();
    ImGui::SameLine();
    ImGui::Text("Filter:");

    // Format table
    if (g_fetchingFormats) {
        ImGui::Text("Fetching formats...");
        ImGui::ProgressBar(-1.0f * ImGui::GetTime(), ImVec2(-1, 0), "");
    }
    else if (!g_formats.empty()) {
        // Video info display
        if (!g_video_info.title.empty()) {
            ImGui::Text("Title: %s", g_video_info.title.c_str());
            if (!g_video_info.uploader.empty()) {
                ImGui::SameLine();
                ImGui::Text("| Uploader: %s", g_video_info.uploader.c_str());
            }
            if (g_video_info.duration > 0) {
                ImGui::SameLine();
                ImGui::Text("| Duration: %s", formatDuration(g_video_info.duration).c_str());
            }
        }

        // Make format table scrollable with fixed height
        if (ImGui::BeginTable("Formats", 10, ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY | ImGuiTableFlags_Sortable, ImVec2(0, 300))) {
            ImGui::TableSetupColumn("Itag", ImGuiTableColumnFlags_WidthFixed, 60);
            ImGui::TableSetupColumn("Ext", ImGuiTableColumnFlags_WidthFixed, 50);
            ImGui::TableSetupColumn("Resolution", ImGuiTableColumnFlags_WidthFixed, 100);
            ImGui::TableSetupColumn("Type", ImGuiTableColumnFlags_WidthFixed, 60);
            ImGui::TableSetupColumn("VCodec", ImGuiTableColumnFlags_WidthFixed, 100);
            ImGui::TableSetupColumn("ACodec", ImGuiTableColumnFlags_WidthFixed, 80);
            ImGui::TableSetupColumn("FPS", ImGuiTableColumnFlags_WidthFixed, 50);
            ImGui::TableSetupColumn("Bitrate", ImGuiTableColumnFlags_WidthFixed, 80);
            ImGui::TableSetupColumn("Size", ImGuiTableColumnFlags_WidthFixed, 80);
            ImGui::TableSetupColumn("Note", ImGuiTableColumnFlags_WidthStretch);
            ImGui::TableHeadersRow();

            std::string filter_text = std::string(g_filter_input);
            std::transform(filter_text.begin(), filter_text.end(), filter_text.begin(), ::tolower);

            for (int i = 0; i < g_formats.size(); ++i) {
                const auto& fmt = g_formats[i];

                // Apply filter
                if (!filter_text.empty()) {
                    std::string search_text = fmt.format_id + " " + fmt.ext + " " + fmt.resolution +
                        " " + fmt.type + " " + fmt.note + " " + fmt.vcodec + " " + fmt.acodec;
                    std::transform(search_text.begin(), search_text.end(), search_text.begin(), ::tolower);
                    if (search_text.find(filter_text) == std::string::npos) continue;
                }

                ImGui::TableNextRow();

                // Color coding
                if (g_showBestHighlight && fmt.is_best_mp4) {
                    ImGui::TableSetBgColor(ImGuiTableBgTarget_RowBg0, IM_COL32(0, 150, 0, 100));
                }
                else if (fmt.type == "Audio") {
                    ImGui::TableSetBgColor(ImGuiTableBgTarget_RowBg0, IM_COL32(128, 128, 128, 80));
                }

                if (ImGui::TableSetColumnIndex(0)) {
                    if (ImGui::Selectable(fmt.format_id.c_str(), g_selectedFormat == i, ImGuiSelectableFlags_SpanAllColumns)) {
                        g_selectedFormat = i;
                    }
                    if (ImGui::IsItemHovered() && ImGui::IsMouseDoubleClicked(0)) {
                        startDownload(fmt);
                    }
                }
                if (ImGui::TableSetColumnIndex(1)) ImGui::Text("%s", (fmt.type == "Audio") ? fmt.ext.c_str() : "mkv");
                if (ImGui::TableSetColumnIndex(2)) ImGui::Text("%s", fmt.resolution.c_str());
                if (ImGui::TableSetColumnIndex(3)) ImGui::Text("%s", fmt.type.c_str());
                if (ImGui::TableSetColumnIndex(4)) ImGui::Text("%s", fmt.vcodec.c_str());
                if (ImGui::TableSetColumnIndex(5)) ImGui::Text("%s", fmt.acodec.c_str());
                if (ImGui::TableSetColumnIndex(6)) ImGui::Text("%d", fmt.fps);
                if (ImGui::TableSetColumnIndex(7)) ImGui::Text("%.1f", fmt.tbr);
                if (ImGui::TableSetColumnIndex(8)) ImGui::Text("%s", fmt.filesize.c_str());
                if (ImGui::TableSetColumnIndex(9)) ImGui::Text("%s", fmt.note.c_str());
            }
            ImGui::EndTable();
        }

        // Action buttons
        ImGui::Separator();
        if (ImGui::Button("Add Selected to Queue")) {
            if (g_selectedFormat >= 0 && g_selectedFormat < g_formats.size()) {
                startDownload(g_formats[g_selectedFormat]);
            }
            else {
                logToConsole("[ERROR] No format selected");
            }
        }
        ImGui::SameLine();
        if (ImGui::Button("Add Best Quality to Queue")) {
            startDownloadBest();
        }
    }
}

void renderQueueTab() {
    auto active = g_manager.getActiveItems();
    auto queue = g_manager.getQueueItems();

    // Queue status
    ImGui::Text("Queue Status: %d queued, %d downloading", (int)queue.size(), (int)active.size());

    ImGui::SameLine();
    if (ImGui::Button("Pause All")) {
        for (auto& item : active) {
            g_manager.cancelDownload(item->key);
        }
        logToConsole("[QUEUE] Paused all downloads");
    }

    ImGui::SameLine();
    if (ImGui::Button("Cancel All")) {
        for (auto& item : active) {
            g_manager.cancelDownload(item->key);
        }
        // Clear queue
        queue.clear();
        logToConsole("[QUEUE] Cancelled all downloads and cleared queue");
    }

    ImGui::SameLine();
    if (ImGui::Button("Clear Completed")) {
        logToConsole("[QUEUE] Cleared completed downloads from view");
    }

    ImGui::Separator();

    // Combine active and queued items
    std::vector<std::shared_ptr<DownloadItem>> all_items;
    all_items.insert(all_items.end(), active.begin(), active.end());
    all_items.insert(all_items.end(), queue.begin(), queue.end());

    if (all_items.empty()) {
        ImGui::Text("Queue is empty");
        return;
    }

    if (ImGui::BeginTable("DownloadQueue", 8, ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY)) {
        ImGui::TableSetupColumn("Title", ImGuiTableColumnFlags_WidthStretch);
        ImGui::TableSetupColumn("Format", ImGuiTableColumnFlags_WidthFixed, 80);
        ImGui::TableSetupColumn("Status", ImGuiTableColumnFlags_WidthFixed, 80);
        ImGui::TableSetupColumn("Progress", ImGuiTableColumnFlags_WidthFixed, 120);
        ImGui::TableSetupColumn("Speed", ImGuiTableColumnFlags_WidthFixed, 80);
        ImGui::TableSetupColumn("Size", ImGuiTableColumnFlags_WidthFixed, 80);
        ImGui::TableSetupColumn("Added", ImGuiTableColumnFlags_WidthFixed, 80);
        ImGui::TableSetupColumn("Actions", ImGuiTableColumnFlags_WidthFixed, 80);
        ImGui::TableHeadersRow();

        for (int i = 0; i < all_items.size(); ++i) {
            auto& item = all_items[i];
            ImGui::TableNextRow();

            // Status color coding
            if (item->status == "Completed") {
                ImGui::TableSetBgColor(ImGuiTableBgTarget_RowBg0, IM_COL32(0, 150, 0, 60));
            }
            else if (item->status == "Failed" || item->status == "Canceled") {
                ImGui::TableSetBgColor(ImGuiTableBgTarget_RowBg0, IM_COL32(150, 0, 0, 60));
            }
            else if (item->status == "Downloading") {
                ImGui::TableSetBgColor(ImGuiTableBgTarget_RowBg0, IM_COL32(0, 100, 200, 60));
            }

            if (ImGui::TableSetColumnIndex(0)) {
                std::string display_title = item->title;
                if (display_title.length() > 50) {
                    display_title = display_title.substr(0, 47) + "...";
                }
                ImGui::Text("%s", display_title.c_str());
            }

            if (ImGui::TableSetColumnIndex(1)) {
                std::string format_text = item->type;
                if (item->format_id != "best") {
                    format_text += " (" + item->format_id + ")";
                }
                ImGui::Text("%s", format_text.c_str());
            }

            if (ImGui::TableSetColumnIndex(2)) ImGui::Text("%s", item->status.c_str());

            if (ImGui::TableSetColumnIndex(3)) {
                float progress = item->progress.load() / 100.0f;
                char progress_text[32];
                sprintf_s(progress_text, "%d%%", item->progress.load());
                ImGui::ProgressBar(progress, ImVec2(-1, 0), progress_text);
            }

            if (ImGui::TableSetColumnIndex(4)) ImGui::Text(""); // Speed placeholder
            if (ImGui::TableSetColumnIndex(5)) ImGui::Text(""); // Size placeholder
            if (ImGui::TableSetColumnIndex(6)) ImGui::Text(""); // Time placeholder

            if (ImGui::TableSetColumnIndex(7)) {
                if (item->status == "Queued" || item->status == "Downloading") {
                    if (ImGui::Button(("Cancel##" + std::to_string(i)).c_str())) {
                        g_manager.cancelDownload(item->key);
                        logToConsole("[QUEUE] Cancelled download: " + item->title);
                    }
                }
            }
        }
        ImGui::EndTable();
    }
}

void renderHistoryTab() {
    ImGui::Text("Total downloads: %d", (int)g_history.size());

    ImGui::SameLine();
    if (ImGui::Button("Export History")) {
        saveHistory();
        logToConsole("[HISTORY] History exported to " + HISTORY_FILE.string());
    }

    ImGui::SameLine();
    if (ImGui::Button("Clear History")) {
        g_history.clear();
        saveHistory();
        logToConsole("[HISTORY] History cleared");
    }

    ImGui::Separator();

    if (g_history.empty()) {
        ImGui::Text("No download history");
        return;
    }

    if (ImGui::BeginTable("DownloadHistory", 7, ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY)) {
        ImGui::TableSetupColumn("Title", ImGuiTableColumnFlags_WidthStretch);
        ImGui::TableSetupColumn("Format", ImGuiTableColumnFlags_WidthFixed, 80);
        ImGui::TableSetupColumn("Status", ImGuiTableColumnFlags_WidthFixed, 80);
        ImGui::TableSetupColumn("File Size", ImGuiTableColumnFlags_WidthFixed, 80);
        ImGui::TableSetupColumn("Date Added", ImGuiTableColumnFlags_WidthFixed, 120);
        ImGui::TableSetupColumn("Duration", ImGuiTableColumnFlags_WidthFixed, 80);
        ImGui::TableSetupColumn("Path", ImGuiTableColumnFlags_WidthStretch);
        ImGui::TableHeadersRow();

        for (int i = 0; i < g_history.size(); ++i) {
            const auto& entry = g_history[i];
            ImGui::TableNextRow();

            // Status color coding
            if (entry.status == "Completed") {
                ImGui::TableSetBgColor(ImGuiTableBgTarget_RowBg0, IM_COL32(0, 100, 0, 40));
            }
            else if (entry.status == "Failed") {
                ImGui::TableSetBgColor(ImGuiTableBgTarget_RowBg0, IM_COL32(100, 0, 0, 40));
            }

            if (ImGui::TableSetColumnIndex(0)) {
                std::string display_title = entry.title;
                if (display_title.length() > 50) {
                    display_title = display_title.substr(0, 47) + "...";
                }
                ImGui::Text("%s", display_title.c_str());
            }

            if (ImGui::TableSetColumnIndex(1)) {
                std::string format_text = entry.format_type;
                if (!entry.format_id.empty() && entry.format_id != "best") {
                    format_text += " (" + entry.format_id + ")";
                }
                ImGui::Text("%s", format_text.c_str());
            }

            if (ImGui::TableSetColumnIndex(2)) ImGui::Text("%s", entry.status.c_str());
            if (ImGui::TableSetColumnIndex(3)) ImGui::Text("%s", entry.file_size.c_str());
            if (ImGui::TableSetColumnIndex(4)) ImGui::Text("%s", entry.added_time.c_str());
            if (ImGui::TableSetColumnIndex(5)) ImGui::Text(""); // Duration calculation placeholder
            if (ImGui::TableSetColumnIndex(6)) {
                std::string display_path = entry.output_path;
                if (display_path.length() > 60) {
                    display_path = "..." + display_path.substr(display_path.length() - 57);
                }
                ImGui::Text("%s", display_path.c_str());
            }
        }
        ImGui::EndTable();
    }
}

#include <Shobjidl.h> // For IFileDialog
#include <comdef.h>   // For _com_error

void browseForCustomPath(char* buffer, size_t bufferSize, std::string& pathOut) {
    if (bufferSize == 0) return;

    HRESULT hr = CoInitializeEx(NULL, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
    if (SUCCEEDED(hr)) {
        IFileDialog* pFileDialog = nullptr;
        hr = CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER,
            IID_PPV_ARGS(&pFileDialog));
        if (SUCCEEDED(hr)) {
            DWORD options;
            pFileDialog->GetOptions(&options);
            pFileDialog->SetOptions(options | FOS_PICKFOLDERS);

            hr = pFileDialog->Show(NULL);
            if (SUCCEEDED(hr)) {
                IShellItem* pItem = nullptr;
                hr = pFileDialog->GetResult(&pItem);
                if (SUCCEEDED(hr)) {
                    PWSTR folderPath = nullptr;
                    pItem->GetDisplayName(SIGDN_FILESYSPATH, &folderPath);
                    if (folderPath) {
                        // Convert wide string to UTF-8 safely
                        int bytesNeeded = WideCharToMultiByte(CP_UTF8, 0, folderPath, -1, nullptr, 0, nullptr, nullptr);
                        if (bytesNeeded > 0) {
                            // Copy only up to bufferSize - 1 to leave space for null terminator
                            size_t copySize = (size_t)bytesNeeded - 1;
                            if (copySize >= bufferSize) copySize = bufferSize - 1;

                            WideCharToMultiByte(CP_UTF8, 0, folderPath, -1, buffer, (int)copySize + 1, nullptr, nullptr);
                            buffer[copySize] = '\0'; // ensure null termination
                            pathOut = std::string(buffer);
                            logToConsole("[SETTINGS] Selected custom path: " + pathOut);
                        }
                        CoTaskMemFree(folderPath);
                    }
                    pItem->Release();
                }
            }
            pFileDialog->Release();
        }
        CoUninitialize();
    }
}

void renderSettingsTab() {
    ImGui::Text("Appearance Settings");
    ImGui::Separator();

    // Theme switcher
    if (ImGui::RadioButton("Dark Theme", Theme::isDarkTheme())) {
        if (!Theme::isDarkTheme()) {
            Theme::setDarkTheme(true);
            logToConsole("[SETTINGS] Theme changed to: Dark");
        }
    }
    ImGui::SameLine();
    if (ImGui::RadioButton("Light Theme", !Theme::isDarkTheme())) {
        if (Theme::isDarkTheme()) {
            Theme::setDarkTheme(false);
            logToConsole("[SETTINGS] Theme changed to: Light");
        }
    }

    ImGui::Separator();
    ImGui::Text("Download Settings");
    ImGui::Separator();

    // Max concurrent downloads
    if (ImGui::SliderInt("Max Concurrent Downloads", &g_maxConcurrent, 1, 10)) {
        g_manager.setMaxConcurrent(g_maxConcurrent);
    }

    // Best MP4 highlight
    if (ImGui::Checkbox("Show Best MP4 Highlight", &g_showBestHighlight)) {
        logToConsole("[SETTINGS] Best MP4 highlight: " + std::string(g_showBestHighlight ? "enabled" : "disabled"));
    }

    // Console visibility
    if (ImGui::Checkbox("Show Console", &g_showConsole)) {
        logToConsole("[SETTINGS] Console visibility: " + std::string(g_showConsole ? "enabled" : "disabled"));
    }

    ImGui::Separator();
    ImGui::Text("Download Path");
    ImGui::Separator();

    // Download path options
    if (ImGui::RadioButton("Use Downloads Folder", g_useDownloadsFolder)) {
        g_useDownloadsFolder = true;
        logToConsole("[SETTINGS] Using downloads folder: " + g_outputFolder);
    }
    ImGui::SameLine();
    if (ImGui::RadioButton("Custom Path", !g_useDownloadsFolder)) {
        if (!g_useDownloadsFolder) return; // already in custom, nothing to do
        g_useDownloadsFolder = false;
        // Pre-fill custom path if empty
        if (g_customOutputPath.empty()) {
            g_customOutputPath = g_outputFolder;
        }
        logToConsole("[SETTINGS] Using custom path: " + g_customOutputPath);
    }

    // Show current path
    std::string current_path = getCurrentOutputPath();
    ImGui::Text("Current path: %s", current_path.c_str());
    if (!std::filesystem::exists(current_path)) {
        ImGui::SameLine();
        ImGui::TextColored(ImVec4(1, 0, 0, 1), "[WILL BE CREATED]");
    }

    // Download path buffers
    static char downloads_buffer[512] = "";
    static char custom_buffer[512] = "";
    static bool last_useDownloads = g_useDownloadsFolder;

    // Initialize buffers once
    if (downloads_buffer[0] == '\0')
        strcpy_s(downloads_buffer, g_outputFolder.c_str());
    if (custom_buffer[0] == '\0')
        strcpy_s(custom_buffer, g_customOutputPath.c_str());

    // Keep downloads buffer updated every frame
    strcpy_s(downloads_buffer, g_outputFolder.c_str());

    // Detect mode switch to pre-fill custom buffer
    if (last_useDownloads != g_useDownloadsFolder) {
        if (!g_useDownloadsFolder) {
            if (custom_buffer[0] == '\0') {
                strcpy_s(custom_buffer, downloads_buffer);
                g_customOutputPath = std::string(custom_buffer); // update immediately
            }
        }
        last_useDownloads = g_useDownloadsFolder;
    }

    // Show the correct input box
    if (g_useDownloadsFolder) {
        ImGui::Text("Downloads Folder:");
        ImGui::BeginDisabled();
        ImGui::InputText("##DownloadsPath", downloads_buffer, sizeof(downloads_buffer));
        ImGui::EndDisabled();

        g_outputFolder = std::string(downloads_buffer); // sync
    }
    else {
        ImGui::Text("Custom Path:");
        if (ImGui::InputText("##CustomPath", custom_buffer, sizeof(custom_buffer))) {
            g_customOutputPath = std::string(custom_buffer);
        }
    }

    ImGui::BeginDisabled(g_useDownloadsFolder); // gray out if using downloads folder
    if (ImGui::Button("Browse...")) {
        browseForCustomPath(custom_buffer, sizeof(custom_buffer), g_customOutputPath);
    }
    ImGui::EndDisabled();

    ImGui::Separator();
    ImGui::Text("Tool Paths");

    ImGui::Text("yt-dlp: %s", YTDLP_PATH.string().c_str());
    if (!std::filesystem::exists(YTDLP_PATH)) {
        ImGui::SameLine();
        ImGui::TextColored(ImVec4(1, 0, 0, 1), "[NOT FOUND]");
    }

    ImGui::Text("ffmpeg: %s", FFMPEG_PATH.string().c_str());
    if (!std::filesystem::exists(FFMPEG_PATH)) {
        ImGui::SameLine();
        ImGui::TextColored(ImVec4(1, 0, 0, 1), "[NOT FOUND]");
    }
}

std::string getUserDownloadsFolder() {
    #ifdef _WIN32
        // First try SHGetKnownFolderPath (Vista+)
        PWSTR downloadsPath = nullptr;
        HRESULT hr = SHGetKnownFolderPath(FOLDERID_Downloads, 0, NULL, &downloadsPath);
        if (SUCCEEDED(hr) && downloadsPath) {
            // Convert wide string to UTF-8
            int len = WideCharToMultiByte(CP_UTF8, 0, downloadsPath, -1, NULL, 0, NULL, NULL);
            std::string result;
            if (len > 0) {
                result.resize(len - 1); // exclude null terminator
                WideCharToMultiByte(CP_UTF8, 0, downloadsPath, -1, &result[0], len, NULL, NULL);
            }
            CoTaskMemFree(downloadsPath);
            return result;
        }
    
        // Fallback: USERPROFILE\Downloads
        char* userProfile = nullptr;
        size_t len = 0;
        errno_t err = _dupenv_s(&userProfile, &len, "USERPROFILE");
        std::string result;
        if (err == 0 && userProfile) {
            result = std::string(userProfile) + "\\Downloads";
            free(userProfile);
        }
        else {
            result = "C:\\Users\\Default\\Downloads"; // last resort
        }
        return result;
    #else
        // for Linux and macOS, assume ~/Downloads
    
        const char* home = std::getenv("HOME");
        if (home) {
            return std::string(home) + "/Downloads";
        }
        return "downloads";
    #endif
}

int WINAPI WinMain(HINSTANCE hInstance, HINSTANCE hPrevInstance, LPSTR lpCmdLine, int nCmdShow) {
    // Initialize the user's Downloads folder
    g_outputFolder = getUserDownloadsFolder();

    // Initialize output folder
    std::filesystem::create_directories(getCurrentOutputPath());

    // Load history
    loadHistory();

    // Initialize GLFW
    if (!glfwInit()) return 1;

    // Create window
    GLFWwindow* window = glfwCreateWindow(1280, 800, "VDM - Video Download Manager", nullptr, nullptr);
    if (!window) {
        glfwTerminate();
        return 1;
    }
    glfwMakeContextCurrent(window);
    glfwSwapInterval(0);

    // Initialize ImGui
    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO(); (void)io;
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
    io.ConfigDebugHighlightIdConflicts = false;

    // Removed ImGuiConfigFlags_DockingEnable for compatibility

    // Setup style - clean white theme
    /*ImGui::StyleColorsLight();
    ImGuiStyle& style = ImGui::GetStyle();
    style.WindowRounding = 0.0f;
    style.ChildRounding = 0.0f;
    style.FrameRounding = 0.0f;
    style.GrabRounding = 0.0f;
    style.PopupRounding = 0.0f;
    style.ScrollbarRounding = 0.0f;
    style.TabRounding = 0.0f; */

    Theme::initialize();

    // Initialize ImGui backends
    ImGui_ImplGlfw_InitForOpenGL(window, true);
    ImGui_ImplOpenGL3_Init("#version 130");

    logToConsole("[VDM] Video Download Manager started");
    logToConsole("[VDM] yt-dlp path: " + YTDLP_PATH.string());
    logToConsole("[VDM] ffmpeg path: " + FFMPEG_PATH.string());

    // Main loop
    while (!glfwWindowShouldClose(window)) {
        glfwPollEvents();

        // Start ImGui frame
        ImGui_ImplOpenGL3_NewFrame();
        ImGui_ImplGlfw_NewFrame();
        ImGui::NewFrame();

        // Create main window that fills the entire viewport
        ImGuiViewport* viewport = ImGui::GetMainViewport();
        ImGui::SetNextWindowPos(viewport->WorkPos);
        ImGui::SetNextWindowSize(viewport->WorkSize);

        ImGuiWindowFlags window_flags = ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove |
            ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoSavedSettings |
            ImGuiWindowFlags_NoBringToFrontOnFocus | ImGuiWindowFlags_NoNavFocus;

        ImGui::Begin("VDM - Video Download Manager", nullptr, window_flags);

        // Tab bar
        if (ImGui::BeginTabBar("MainTabs", ImGuiTabBarFlags_None)) {
            if (ImGui::BeginTabItem("Downloader")) {
                renderDownloaderTab();
                ImGui::EndTabItem();
            }

            if (ImGui::BeginTabItem("Download Queue")) {
                renderQueueTab();
                ImGui::EndTabItem();
            }

            if (ImGui::BeginTabItem("Download History")) {
                renderHistoryTab();
                ImGui::EndTabItem();
            }

            if (ImGui::BeginTabItem("Settings")) {
                renderSettingsTab();
                ImGui::EndTabItem();
            }

            ImGui::EndTabBar();
        }

        ImGui::End();

        // Render console window
        renderConsole();

        // Rendering
        ImGui::Render();
        int display_w, display_h;
        glfwGetFramebufferSize(window, &display_w, &display_h);
        glViewport(0, 0, display_w, display_h);
        ImVec4 bgColor = Theme::getBackgroundColor();
        glClearColor(bgColor.x, bgColor.y, bgColor.z, bgColor.w);
        glClear(GL_COLOR_BUFFER_BIT);
        ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());

        glfwSwapBuffers(window);
    }

    // Cleanup
    ImGui_ImplOpenGL3_Shutdown();
    ImGui_ImplGlfw_Shutdown();
    ImGui::DestroyContext();

    glfwDestroyWindow(window);
    glfwTerminate();

    return 0;
}