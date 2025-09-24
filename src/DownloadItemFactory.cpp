#include "DownloadItemFactory.h"
#include "DownloadItem.h"
#include <memory>

std::shared_ptr<DownloadItem> DownloadItemFactory::create(
    const std::string& url,
    const std::string& format_id,
    const std::string& type,
    const std::string& output_path,
    const std::string& title)
{
    return std::make_shared<DownloadItem>(url, format_id, type, output_path, title);
}