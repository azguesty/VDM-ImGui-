#pragma once
#include "DownloadItem.h"
#include <memory>
#include <string>

class DownloadItemFactory {
public:
    static std::shared_ptr<DownloadItem> create(
        const std::string& url,
        const std::string& format_id,
        const std::string& type,
        const std::string& output_path,
        const std::string& title);
};