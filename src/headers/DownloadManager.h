#pragma once
#include "DownloadItem.h"
#include <vector>
#include <memory>
#include <functional>
#include <mutex>
#include <thread>
#include <queue>

class DownloadManager {
public:
    DownloadManager() = default;
    ~DownloadManager();

    // Add a download item to the queue
    void addToQueue(std::shared_ptr<DownloadItem> item);

    // Get currently active items
    std::vector<std::shared_ptr<DownloadItem>> getActiveItems();

    // Get queued items
    std::vector<std::shared_ptr<DownloadItem>> getQueueItems();

    // Cancel download by key
    void cancelDownload(const std::string& key);

    // Set max concurrent downloads
    void setMaxConcurrent(int max);

    // Callback when queue changes (progress updates, new items, cancellation)
    std::function<void()> onQueueUpdated;

private:
    void processQueue();
    void startDownload(std::shared_ptr<DownloadItem> item);
    void onDownloadComplete(std::shared_ptr<DownloadItem> item, bool success);

    std::vector<std::shared_ptr<DownloadItem>> m_queue;
    std::vector<std::shared_ptr<DownloadItem>> m_activeDownloads;
    std::mutex m_mutex;
    int m_maxConcurrent = 3;
    std::atomic<int> m_itemCounter{ 0 };
};