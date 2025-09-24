#pragma once
#include <string>
#include <atomic>
#include <filesystem>

class DownloadItem {
public:
    // Constructor - matching the factory usage exactly
    DownloadItem(const std::string& url,
        const std::string& format_id,
        const std::string& type,
        const std::string& output_path,
        const std::string& title)
        : url(url), format_id(format_id), type(type), output_path(output_path),
        title(title), key(title) {
    }

    // No copying (unique per download)
    DownloadItem(const DownloadItem&) = delete;
    DownloadItem& operator=(const DownloadItem&) = delete;

    std::string url;
    std::string format_id;
    std::string type;
    std::string output_path;
    std::string key;
    std::string title;
    std::filesystem::path ytDlpPath;
    std::atomic<int> progress{ 0 };
    std::string status = "Pending";
};