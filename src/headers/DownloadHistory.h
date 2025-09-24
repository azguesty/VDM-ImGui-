#pragma once
#include <string>
#include <vector>
#include <fstream>
#include <sstream>
#include <filesystem>

struct HistoryEntry {
    std::string url;
    std::string title;
    std::string format_id;
    std::string status;
};

class DownloadHistory {
public:
    DownloadHistory(const std::string& filename = "download_history.txt")
        : filePath(filename) {
        load();
    }

    void add(const HistoryEntry& entry) {
        history.push_back(entry);
        save();
    }

    void clear() {
        history.clear();
        save();
    }

    void save() {
        std::ofstream f(filePath);
        for (auto& e : history) {
            f << e.url << "|" << e.title << "|" << e.format_id << "|" << e.status << "\n";
        }
    }

    void load() {
        history.clear();
        if (!std::filesystem::exists(filePath)) return;
        std::ifstream f(filePath);
        std::string line;
        while (std::getline(f, line)) {
            std::istringstream iss(line);
            HistoryEntry e;
            std::getline(iss, e.url, '|');
            std::getline(iss, e.title, '|');
            std::getline(iss, e.format_id, '|');
            std::getline(iss, e.status, '|');
            history.push_back(e);
        }
    }

    std::vector<HistoryEntry> history;

private:
    std::string filePath;
};
