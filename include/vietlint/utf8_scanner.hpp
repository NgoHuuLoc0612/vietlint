#pragma once
#include <cstdint>
#include <cstddef>
#include <span>
#include <vector>
#include <string_view>
#include <expected>
#include <immintrin.h>
#include <array>
#include <string>

namespace vietlint {

/// Error codes for scanner operations
enum class ScanError : uint8_t {
    InvalidUTF8,
    UnexpectedEOF,
    BufferTooSmall,
    InternalError,
};

/// A single scanned codepoint with position metadata
struct CodepointInfo {
    uint32_t codepoint;
    uint32_t byte_offset;  ///< Byte offset in source
    uint16_t byte_length;  ///< 1-4 bytes consumed
    bool is_vietnamese;    ///< In Vietnamese Unicode range
    bool is_identifier_start;
    bool is_identifier_continue;
};

/// Vietnamese Unicode block information
/// Primary ranges: U+00C0-U+024F (Latin Extended), U+1E00-U+1EFF (Latin Extended Additional)
/// U+0300-U+036F (Combining Diacritical Marks)
struct VietnameseRanges {
    // Latin Extended-A/B with diacritics used in Vietnamese
    static constexpr uint32_t LATIN_EXT_A_START   = 0x00C0;
    static constexpr uint32_t LATIN_EXT_A_END      = 0x024F;
    // Combining Diacritical Marks
    static constexpr uint32_t COMBINING_START      = 0x0300;
    static constexpr uint32_t COMBINING_END        = 0x036F;
    // Latin Extended Additional (main Vietnamese block)
    static constexpr uint32_t LATIN_EXT_ADD_START  = 0x1E00;
    static constexpr uint32_t LATIN_EXT_ADD_END    = 0x1EFF;

    [[nodiscard]] static constexpr bool contains(uint32_t cp) noexcept {
        return (cp >= LATIN_EXT_A_START && cp <= LATIN_EXT_A_END)
            || (cp >= COMBINING_START    && cp <= COMBINING_END)
            || (cp >= LATIN_EXT_ADD_START && cp <= LATIN_EXT_ADD_END);
    }
};

/// SIMD-accelerated UTF-8 scanner for Vietnamese source files
/// Uses AVX2 to scan 32 bytes at a time for continuation-byte patterns
/// and Vietnamese-range codepoints.
class UTF8Scanner {
public:
    explicit UTF8Scanner() noexcept;
    ~UTF8Scanner() noexcept = default;

    UTF8Scanner(const UTF8Scanner&) = delete;
    UTF8Scanner& operator=(const UTF8Scanner&) = delete;
    UTF8Scanner(UTF8Scanner&&) noexcept = default;
    UTF8Scanner& operator=(UTF8Scanner&&) noexcept = default;

    /// Full scan: returns all codepoints with Vietnamese flags.
    /// SIMD used for fast multi-byte detection; scalar fallback for tail bytes.
    [[nodiscard]] std::expected<std::vector<CodepointInfo>, ScanError>
    scan(std::span<const uint8_t> input) const noexcept;

    /// Fast check: does this buffer contain any Vietnamese codepoints?
    /// Uses AVX2 byte-range comparison for O(n/32) detection.
    [[nodiscard]] bool has_vietnamese_fast(std::span<const uint8_t> input) const noexcept;

    /// Count Vietnamese codepoints in buffer
    [[nodiscard]] uint32_t count_vietnamese(std::span<const uint8_t> input) const noexcept;

    /// Extract only Vietnamese identifier tokens (runs of viet chars + ascii word chars)
    [[nodiscard]] std::vector<std::string_view>
    extract_vietnamese_tokens(std::string_view source) const noexcept;

    /// Validate UTF-8 well-formedness using SIMD
    [[nodiscard]] bool validate_utf8(std::span<const uint8_t> input) const noexcept;

private:
    bool avx2_available_;

    // AVX2 fast path: scan 32 bytes for 0xE1..0xE2 lead bytes (Vietnamese range)
    [[nodiscard]] bool avx2_has_vietnamese(const uint8_t* data, size_t len) const noexcept;

    // Scalar UTF-8 decode of a single codepoint; advances ptr
    [[nodiscard]] static std::expected<CodepointInfo, ScanError>
    decode_one(const uint8_t* ptr, const uint8_t* end, uint32_t offset) noexcept;

    // Check if codepoint is valid UTF-8 sequence starter
    [[nodiscard]] static bool is_valid_leading_byte(uint8_t b) noexcept;
};

/// SIMD-optimized line/column tracker for diagnostics
class PositionTracker {
public:
    struct Position {
        uint32_t line;    ///< 1-based
        uint32_t column;  ///< 1-based, in Unicode codepoints
        uint32_t offset;  ///< Byte offset from start
    };

    explicit PositionTracker(std::string_view source) noexcept;

    /// Convert byte offset to line/column
    [[nodiscard]] Position offset_to_position(uint32_t byte_offset) const noexcept;

    /// Fast bulk: resolve many offsets in sorted order
    [[nodiscard]] std::vector<Position>
    resolve_offsets(std::span<const uint32_t> sorted_offsets) const noexcept;

private:
    // Precomputed newline positions for O(log n) lookup
    std::vector<uint32_t> newline_offsets_;
    std::string_view source_;
};

} // namespace vietlint
