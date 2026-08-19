module;

#include <curl/curl.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/stat.h>

export module sage.vendor.curl;

import std;

export namespace sage::vendor::curl {

using std::size_t;

class CurlGlobal {
public:
    static void init() {
        static CurlGlobal instance;
        (void)instance;
    }
private:
    CurlGlobal() { curl_global_init(CURL_GLOBAL_ALL); }
    ~CurlGlobal() { curl_global_cleanup(); }
};

class CurlEasy {
public:
    CurlEasy() : handle_(curl_easy_init()) {
        CurlGlobal::init();
        if (handle_) {
            curl_easy_setopt(handle_, CURLOPT_FOLLOWLOCATION, 1L);
            curl_easy_setopt(handle_, CURLOPT_FAILONERROR, 1L);
            curl_easy_setopt(handle_, CURLOPT_NOSIGNAL, 1L);
            curl_easy_setopt(handle_, CURLOPT_USERAGENT, "sage/0.1.0 (Linux; x86_64)");
        }
    }

    ~CurlEasy() {
        if (handle_) {
            curl_easy_cleanup(handle_);
            handle_ = nullptr;
        }
    }

    CurlEasy(const CurlEasy&) = delete;
    CurlEasy& operator=(const CurlEasy&) = delete;

    CurlEasy(CurlEasy&& other) noexcept : handle_(std::exchange(other.handle_, nullptr)) {}
    CurlEasy& operator=(CurlEasy&& other) noexcept {
        if (this != &other) {
            if (handle_) curl_easy_cleanup(handle_);
            handle_ = std::exchange(other.handle_, nullptr);
        }
        return *this;
    }

    [[nodiscard]] CURL* handle() const noexcept { return handle_; }
    [[nodiscard]] explicit operator bool() const noexcept { return handle_ != nullptr; }

    template <typename T>
    CURLcode setopt(CURLoption opt, T val) {
        return curl_easy_setopt(handle_, opt, val);
    }

    std::expected<void, std::string> perform() {
        if (!handle_) return std::unexpected("Uninitialized CURL handle");
        CURLcode res = curl_easy_perform(handle_);
        if (res != CURLE_OK) {
            return std::unexpected(curl_easy_strerror(res));
        }
        return {};
    }

private:
    CURL* handle_{nullptr};
};

using ProgressCallback = std::function<void(size_t downloaded_bytes, size_t total_bytes)>;

inline std::filesystem::path parse_local_url(std::string_view url) {
    if (url.starts_with("file://")) {
        return std::filesystem::path(url.substr(7));
    }
    if (url.starts_with("/")) {
        return std::filesystem::path(url);
    }
    return {};
}

inline std::expected<std::string, std::string> fetch_string(std::string_view url, long timeout_secs = 30) {
    if (auto local_p = parse_local_url(url); !local_p.empty()) {
        if (std::filesystem::exists(local_p)) {
            std::ifstream f(local_p);
            std::stringstream ss;
            ss << f.rdbuf();
            return ss.str();
        }
        return std::unexpected("Local file does not exist: " + local_p.string());
    }

    CurlEasy curl;
    if (!curl) return std::unexpected("Failed to initialize curl");

    std::string response;
    std::string url_str(url);

    curl.setopt(CURLOPT_URL, url_str.c_str());
    curl.setopt(CURLOPT_TIMEOUT, timeout_secs);
    curl.setopt(CURLOPT_WRITEFUNCTION, +[](char* ptr, size_t size, size_t nmemb, void* userdata) -> size_t {
        auto* str = static_cast<std::string*>(userdata);
        str->append(ptr, size * nmemb);
        return size * nmemb;
    });
    curl.setopt(CURLOPT_WRITEDATA, &response);

    auto res = curl.perform();
    if (!res) return std::unexpected(res.error());
    return response;
}

struct RemoteFileInfo {
    size_t content_length{0};
    bool supports_ranges{false};
};

inline std::expected<RemoteFileInfo, std::string> probe_remote_file(std::string_view url) {
    if (auto local_p = parse_local_url(url); !local_p.empty()) {
        if (std::filesystem::exists(local_p)) {
            RemoteFileInfo info;
            std::error_code ec;
            info.content_length = std::filesystem::file_size(local_p, ec);
            info.supports_ranges = false;
            return info;
        }
        return std::unexpected("Local file does not exist: " + local_p.string());
    }

    CurlEasy curl;
    if (!curl) return std::unexpected("Failed to initialize curl");

    std::string url_str(url);
    curl.setopt(CURLOPT_URL, url_str.c_str());
    curl.setopt(CURLOPT_NOBODY, 1L); // HEAD request
    curl.setopt(CURLOPT_HEADER, 1L);
    curl.setopt(CURLOPT_TIMEOUT, 15L);

    bool supports_ranges = false;
    curl.setopt(CURLOPT_HEADERFUNCTION, +[](char* buffer, size_t size, size_t nitems, void* userdata) -> size_t {
        size_t total = size * nitems;
        std::string_view line(buffer, total);
        auto* range_flag = static_cast<bool*>(userdata);
        if (line.starts_with("Accept-Ranges:") || line.starts_with("accept-ranges:")) {
            if (line.find("bytes") != std::string_view::npos) {
                *range_flag = true;
            }
        }
        return total;
    });
    curl.setopt(CURLOPT_HEADERDATA, &supports_ranges);

    auto res = curl.perform();
    if (!res) return std::unexpected(res.error());

    curl_off_t cl = 0;
    curl_easy_getinfo(curl.handle(), CURLINFO_CONTENT_LENGTH_DOWNLOAD_T, &cl);

    RemoteFileInfo info;
    info.content_length = (cl > 0) ? static_cast<size_t>(cl) : 0;
    info.supports_ranges = supports_ranges;
    return info;
}

// Download file with multi-threaded range chunks for high performance
inline std::expected<void, std::string> download_file(
    std::string_view url,
    const std::filesystem::path& dest_path,
    ProgressCallback progress_cb = nullptr,
    size_t num_threads = 4) 
{
    if (auto parent = dest_path.parent_path(); !parent.empty()) {
        std::filesystem::create_directories(parent);
    }

    if (auto local_p = parse_local_url(url); !local_p.empty()) {
        if (std::filesystem::exists(local_p)) {
            std::error_code ec;
            std::filesystem::copy_file(local_p, dest_path, std::filesystem::copy_options::overwrite_existing, ec);
            if (ec) return std::unexpected("Failed to copy local package: " + ec.message());
            if (progress_cb) {
                size_t sz = std::filesystem::file_size(dest_path, ec);
                progress_cb(sz, sz);
            }
            return {};
        }
        return std::unexpected("Local source file does not exist: " + local_p.string());
    }

    auto probe = probe_remote_file(url);
    size_t total_size = probe ? probe->content_length : 0;
    bool parallel_eligible = probe && probe->supports_ranges && (total_size >= 4 * 1024 * 1024); // >= 4MB

    std::string url_str(url);

    if (!parallel_eligible || num_threads <= 1) {
        // Single thread stream download
        int fd = ::open(dest_path.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0644);
        if (fd < 0) return std::unexpected("Failed to open destination file for write");

        struct SingleCtx {
            int fd;
            ProgressCallback cb;
            size_t total;
            size_t downloaded{0};
        } ctx{fd, progress_cb, total_size, 0};

        CurlEasy curl;
        curl.setopt(CURLOPT_URL, url_str.c_str());
        curl.setopt(CURLOPT_WRITEFUNCTION, +[](char* ptr, size_t size, size_t nmemb, void* userdata) -> size_t {
            auto* c = static_cast<SingleCtx*>(userdata);
            size_t bytes = size * nmemb;
            ssize_t written = ::write(c->fd, ptr, bytes);
            if (written > 0) {
                c->downloaded += static_cast<size_t>(written);
                if (c->cb) c->cb(c->downloaded, c->total);
            }
            return bytes;
        });
        curl.setopt(CURLOPT_WRITEDATA, &ctx);

        auto res = curl.perform();
        ::close(fd);
        if (!res) {
            std::filesystem::remove(dest_path);
            return std::unexpected(res.error());
        }
        return {};
    }

    // High performance multi-threaded parallel chunk download
    int fd = ::open(dest_path.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd < 0) return std::unexpected("Failed to create destination file");

    if (::posix_fallocate(fd, 0, static_cast<off_t>(total_size)) != 0) {
        if (::ftruncate(fd, static_cast<off_t>(total_size)) != 0) {
            // Ignore if filesystem already handles write offsets
        }
    }

    std::atomic<size_t> total_downloaded{0};
    size_t chunk_size = (total_size + num_threads - 1) / num_threads;

    std::vector<std::future<std::expected<void, std::string>>> futures;
    futures.reserve(num_threads);

    for (size_t i = 0; i < num_threads; ++i) {
        size_t start = i * chunk_size;
        size_t end = std::min(start + chunk_size - 1, total_size - 1);
        if (start > end) break;

        futures.push_back(std::async(std::launch::async, [url_str, fd, start, end, total_size, &total_downloaded, progress_cb]() -> std::expected<void, std::string> {
            CurlEasy curl;
            if (!curl) return std::unexpected("Failed to init worker curl");

            std::string range_header = std::to_string(start) + "-" + std::to_string(end);
            curl.setopt(CURLOPT_URL, url_str.c_str());
            curl.setopt(CURLOPT_RANGE, range_header.c_str());

            struct ChunkCtx {
                int fd;
                off_t offset;
                size_t total_size;
                std::atomic<size_t>& global_downloaded;
                ProgressCallback cb;
            } cctx{fd, static_cast<off_t>(start), total_size, total_downloaded, progress_cb};

            curl.setopt(CURLOPT_WRITEFUNCTION, +[](char* ptr, size_t size, size_t nmemb, void* userdata) -> size_t {
                auto* c = static_cast<ChunkCtx*>(userdata);
                size_t bytes = size * nmemb;
                ssize_t written = ::pwrite(c->fd, ptr, bytes, c->offset);
                if (written > 0) {
                    c->offset += written;
                    size_t cur = c->global_downloaded.fetch_add(static_cast<size_t>(written)) + static_cast<size_t>(written);
                    if (c->cb) c->cb(cur, c->total_size);
                }
                return bytes;
            });
            curl.setopt(CURLOPT_WRITEDATA, &cctx);

            auto res = curl.perform();
            if (!res) return std::unexpected(res.error());
            return {};
        }));
    }

    std::string error_msg;
    for (auto& f : futures) {
        auto res = f.get();
        if (!res && error_msg.empty()) {
            error_msg = res.error();
        }
    }

    ::close(fd);

    if (!error_msg.empty()) {
        std::filesystem::remove(dest_path);
        return std::unexpected(error_msg);
    }

    if (progress_cb) progress_cb(total_size, total_size);
    return {};
}

} // namespace sage::vendor::curl
