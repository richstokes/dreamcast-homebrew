#pragma once
#include <cstddef>
#include <string>

constexpr size_t DC_CART_LIMIT = 1024 * 1024;
constexpr size_t DC_URL_LIMIT = 1024;
struct DownloadStatus {
    bool busy = false;
    bool success = false;
    size_t received = 0;
    size_t total = 0;
    std::string message;
    std::string path;
};

// One worker at a time. Poll copies its state under a KOS mutex; no VM access
// occurs on the worker. Cancellation is cooperative, including SDK dial/DNS.
bool dc_download_start(const std::string& url);
DownloadStatus dc_download_status();
void dc_download_cancel();
void dc_download_reap();
std::string dc_download_screen();
