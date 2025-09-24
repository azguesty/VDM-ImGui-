#include "DownloadManager.h"
#include <algorithm>
#include <iostream>
#include <cstdio>
#include <filesystem>
#include <regex>

extern const std::filesystem::path YTDLP_PATH;
extern const std::filesystem::path FFMPEG_PATH;
extern void logToConsole(const std::string& message);
extern void addToHistory(std::shared_ptr<DownloadItem> item);
extern void updateToHistory(std::shared_ptr<DownloadItem> item);

DownloadManager::~DownloadManager() {
    // Cancel all active downloads
    std::lock_guard<std::mutex> lock(m_mutex);
    for (auto& item : m_activeDownloads) {
        item->status = "Canceled";
    }
}

void DownloadManager::addToQueue(std::shared_ptr<DownloadItem> item) {
    std::lock_guard<std::mutex> lock(m_mutex);

    // Assign unique ID
    item->key = "download_" + std::to_string(++m_itemCounter);

    m_queue.push_back(item);
    logToConsole("[QUEUE] Added to queue: " + item->title);

    if (onQueueUpdated) onQueueUpdated();

    // Try to start download if under concurrent limit
    processQueue();
}

void DownloadManager::processQueue() {
    // This function should be called with mutex already locked
    while (m_activeDownloads.size() < m_maxConcurrent && !m_queue.empty()) {
        auto item = m_queue.front();
        m_queue.erase(m_queue.begin());
        startDownload(item);
    }
}

void DownloadManager::startDownload(std::shared_ptr<DownloadItem> item) {
    // This function should be called with mutex already locked
    item->status = "Downloading";
    m_activeDownloads.push_back(item);

    logToConsole("[DOWNLOAD] Starting: " + item->title);

    // Start download in a separate thread
    std::thread([this, item]() {
        // Determine format selector
        std::string format_selector;
        if (item->format_id == "best") {
            format_selector = "bestvideo+bestaudio/best";
        }
        else {
            // Check if we need to combine video and audio
            format_selector = item->format_id;
            // For video-only formats, try to add best audio
            if (item->type == "Video") {
                format_selector += "+bestaudio";
            }
        }

        // Build command
        std::string cmd = YTDLP_PATH.string() + " -f " + format_selector + " " + item->url +
            " -o " + item->output_path +
            " --newline --force-overwrites --no-warnings --embed-metadata" +
            " --ignore-errors --ffmpeg-location " + FFMPEG_PATH.string();
        std::cout << cmd << "\n";

        // Add merge format for non-audio downloads
        if (item->type != "Audio") {
            cmd += " --merge-output-format mkv";
        }

        logToConsole("[DOWNLOAD] Command: " + cmd);

        // Execute download with progress tracking
        FILE* pipe = _popen(cmd.c_str(), "r");
        if (!pipe) {
            logToConsole("[ERROR] Failed to start download process");
            onDownloadComplete(item, false);
            return;
        }

        char buffer[256];
        std::string last_line;

        while (fgets(buffer, sizeof(buffer), pipe)) {
            std::string line(buffer);
            line.erase(line.find_last_not_of(" \n\r\t") + 1); // trim

            if (!line.empty()) {
                last_line = line;
                logToConsole("[" + item->key + "] " + line);

                // Parse progress
                std::regex progress_regex(R"(\[download\]\s+(\d{1,3}(?:\.\d+)?)%)");
                std::smatch match;
                if (std::regex_search(line, match, progress_regex)) {
                    try {
                        float progress_f = std::stof(match[1].str());
                        item->progress = static_cast<int>(progress_f);
                    }
                    catch (...) {}
                }

                // Parse status updates
                if (line.find("Merging formats") != std::string::npos ||
                    line.find("[Merger]") != std::string::npos) {
                    item->status = "Merging...";
                }
                else if (line.find("Deleting original file") != std::string::npos) {
                    item->status = "Cleaning up...";
                }
                else if (line.find("[ffmpeg]") != std::string::npos &&
                    (line.find("Converting") != std::string::npos ||
                        line.find("Merging") != std::string::npos)) {
                    item->status = "Processing...";
                }
                else if (item->progress > 0 && item->progress < 100) {
                    item->status = "Downloading";
                }
            }
        }

        int exit_code = _pclose(pipe);
        bool success = (exit_code == 0);

        if (success) {
            item->progress = 100;
            item->status = "Completed";
            logToConsole("[DOWNLOAD] Completed: " + item->title);
        }
        else {
            item->status = "Failed";
            logToConsole("[DOWNLOAD] Failed: " + item->title + " (exit code: " + std::to_string(exit_code) + ")");
        }

        onDownloadComplete(item, success);
        }).detach();
}

void DownloadManager::onDownloadComplete(std::shared_ptr<DownloadItem> item, bool success) {
    {
        std::lock_guard<std::mutex> lock(m_mutex);

        // Remove from active downloads
        auto it = std::find(m_activeDownloads.begin(), m_activeDownloads.end(), item);
        if (it != m_activeDownloads.end()) {
            m_activeDownloads.erase(it);
        }

        // Process next item in queue
		item->status = success ? "Completed" : "Failed";
        updateToHistory(item);
        processQueue();
    }

    if (onQueueUpdated) onQueueUpdated();
}

std::vector<std::shared_ptr<DownloadItem>> DownloadManager::getActiveItems() {
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_activeDownloads;
}

std::vector<std::shared_ptr<DownloadItem>> DownloadManager::getQueueItems() {
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_queue;
}

void DownloadManager::cancelDownload(const std::string& key) {
    std::lock_guard<std::mutex> lock(m_mutex);

    // Cancel active download
    auto active_it = std::find_if(m_activeDownloads.begin(), m_activeDownloads.end(),
        [&key](const std::shared_ptr<DownloadItem>& item) {
            return item->key == key;
        });

    if (active_it != m_activeDownloads.end()) {
        (*active_it)->status = "Canceled";
        m_activeDownloads.erase(active_it);
        logToConsole("[DOWNLOAD] Canceled active download: " + key);
        processQueue(); // Start next download
        if (onQueueUpdated) onQueueUpdated();
        return;
    }

    // Remove from queue
    auto queue_it = std::find_if(m_queue.begin(), m_queue.end(),
        [&key](const std::shared_ptr<DownloadItem>& item) {
            return item->key == key;
        });

    if (queue_it != m_queue.end()) {
        (*queue_it)->status = "Canceled";
        m_queue.erase(queue_it);
        logToConsole("[QUEUE] Removed from queue: " + key);
        if (onQueueUpdated) onQueueUpdated();
    }
}

void DownloadManager::setMaxConcurrent(int max) {
    std::lock_guard<std::mutex> lock(m_mutex);
    m_maxConcurrent = std::max(1, std::min(10, max));
    processQueue(); // Try to start more downloads if limit increased
}