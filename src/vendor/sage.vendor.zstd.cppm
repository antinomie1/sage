module;

#include <zstd.h>
#include <string>
#include <string_view>
#include <vector>
#include <span>
#include <cstdint>
#include <expected>
#include <utility>

export module sage.vendor.zstd;

export namespace sage::vendor::zstd {

class ZstdDecompressStream {
public:
    ZstdDecompressStream() noexcept : dctx_(ZSTD_createDCtx()) {}
    ~ZstdDecompressStream() noexcept {
        if (dctx_) {
            ZSTD_freeDCtx(dctx_);
            dctx_ = nullptr;
        }
    }

    ZstdDecompressStream(const ZstdDecompressStream&) = delete;
    ZstdDecompressStream& operator=(const ZstdDecompressStream&) = delete;

    ZstdDecompressStream(ZstdDecompressStream&& other) noexcept 
        : dctx_(std::exchange(other.dctx_, nullptr)) {}

    ZstdDecompressStream& operator=(ZstdDecompressStream&& other) noexcept {
        if (this != &other) {
            if (dctx_) ZSTD_freeDCtx(dctx_);
            dctx_ = std::exchange(other.dctx_, nullptr);
        }
        return *this;
    }

    void reset() noexcept {
        if (dctx_) {
            ZSTD_DCtx_reset(dctx_, ZSTD_reset_session_only);
        }
    }

    // Decompress streaming chunk
    // Returns remaining bytes to read in frame, or 0 when frame finished
    std::expected<size_t, std::string> decompress_stream(
        ZSTD_inBuffer& in, 
        ZSTD_outBuffer& out) noexcept 
    {
        if (!dctx_) return std::unexpected("Uninitialized ZSTD DCtx");
        size_t ret = ZSTD_decompressStream(dctx_, &out, &in);
        if (ZSTD_isError(ret)) {
            return std::unexpected(ZSTD_getErrorName(ret));
        }
        return ret;
    }

    [[nodiscard]] explicit operator bool() const noexcept { return dctx_ != nullptr; }

private:
    ZSTD_DCtx* dctx_{nullptr};
};

class ZstdCompressStream {
public:
    explicit ZstdCompressStream(int level = 3) noexcept 
        : cctx_(ZSTD_createCCtx()), level_(level) 
    {
        if (cctx_) {
            ZSTD_CCtx_setParameter(cctx_, ZSTD_c_compressionLevel, level_);
        }
    }

    ~ZstdCompressStream() noexcept {
        if (cctx_) {
            ZSTD_freeCCtx(cctx_);
            cctx_ = nullptr;
        }
    }

    ZstdCompressStream(const ZstdCompressStream&) = delete;
    ZstdCompressStream& operator=(const ZstdCompressStream&) = delete;

    ZstdCompressStream(ZstdCompressStream&& other) noexcept 
        : cctx_(std::exchange(other.cctx_, nullptr)),
          level_(other.level_) {}

    ZstdCompressStream& operator=(ZstdCompressStream&& other) noexcept {
        if (this != &other) {
            if (cctx_) ZSTD_freeCCtx(cctx_);
            cctx_ = std::exchange(other.cctx_, nullptr);
            level_ = other.level_;
        }
        return *this;
    }

    std::expected<size_t, std::string> compress_stream(
        ZSTD_inBuffer& in, 
        ZSTD_outBuffer& out, 
        ZSTD_EndDirective end_op = ZSTD_e_continue) noexcept 
    {
        if (!cctx_) return std::unexpected("Uninitialized ZSTD CCtx");
        size_t ret = ZSTD_compressStream2(cctx_, &out, &in, end_op);
        if (ZSTD_isError(ret)) {
            return std::unexpected(ZSTD_getErrorName(ret));
        }
        return ret;
    }

    [[nodiscard]] explicit operator bool() const noexcept { return cctx_ != nullptr; }

private:
    ZSTD_CCtx* cctx_{nullptr};
    int level_{3};
};

inline std::expected<std::vector<uint8_t>, std::string> compress_block(
    std::span<const uint8_t> src, 
    int level = 3) 
{
    size_t max_dest = ZSTD_compressBound(src.size());
    std::vector<uint8_t> dest(max_dest);
    size_t written = ZSTD_compress(dest.data(), max_dest, src.data(), src.size(), level);
    if (ZSTD_isError(written)) {
        return std::unexpected(ZSTD_getErrorName(written));
    }
    dest.resize(written);
    return dest;
}

inline std::expected<std::vector<uint8_t>, std::string> decompress_block(
    std::span<const uint8_t> src) 
{
    unsigned long long const r_size = ZSTD_getFrameContentSize(src.data(), src.size());
    if (r_size == ZSTD_CONTENTSIZE_ERROR || r_size == ZSTD_CONTENTSIZE_UNKNOWN) {
        return std::unexpected("Invalid or unknown ZSTD content size");
    }
    std::vector<uint8_t> dest(static_cast<size_t>(r_size));
    size_t actual = ZSTD_decompress(dest.data(), dest.size(), src.data(), src.size());
    if (ZSTD_isError(actual)) {
        return std::unexpected(ZSTD_getErrorName(actual));
    }
    return dest;
}

} // namespace sage::vendor::zstd
