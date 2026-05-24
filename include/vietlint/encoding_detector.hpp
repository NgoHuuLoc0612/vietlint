#pragma once
#include <cstdint>
#include <cstddef>
#include <string>
#include <string_view>
#include <span>
#include <expected>
#include <array>
#include <vector>

namespace vietlint {

/// Supported Vietnamese encodings
enum class Encoding : uint8_t {
    UTF8,
    UTF8_BOM,      ///< UTF-8 with BOM
    TCVN3,         ///< TCVN 5773:1993 (old Vietnamese standard)
    VNI,           ///< VNI encoding (used in older Vietnamese software)
    VPS,           ///< VPS encoding
    VISCII,        ///< VISCII 1.1
    Windows1258,   ///< Windows-1258 (Vietnamese)
    Latin1,        ///< ISO-8859-1 (might be misidentified Vietnamese)
    ASCII,         ///< Pure 7-bit ASCII
    Unknown,
};

struct EncodingDetectionResult {
    Encoding    detected;
    float       confidence;    ///< 0.0-1.0
    std::string charset_name;  ///< MIME charset name
    bool        has_bom;
    bool        needs_conversion; ///< True if not UTF-8
    std::string message;
};

/// Encoding detector for Vietnamese source files
/// Uses statistical analysis of byte patterns to identify encoding.
/// TCVN3 has a characteristic high-byte distribution in 0xB0-0xCF range.
/// VNI uses 0x80-0x9F for Vietnamese chars. UTF-8 uses specific lead patterns.
class EncodingDetector {
public:
    EncodingDetector() noexcept = default;
    ~EncodingDetector() noexcept = default;

    EncodingDetector(const EncodingDetector&) = delete;
    EncodingDetector& operator=(const EncodingDetector&) = delete;

    /// Detect encoding from raw bytes (examines first 8KB for speed)
    [[nodiscard]] EncodingDetectionResult
    detect(std::span<const uint8_t> data) const noexcept;

    /// Convert TCVN3 bytes to UTF-8
    [[nodiscard]] std::expected<std::string, std::string>
    tcvn3_to_utf8(std::span<const uint8_t> data) const noexcept;

    /// Convert VNI bytes to UTF-8
    [[nodiscard]] std::expected<std::string, std::string>
    vni_to_utf8(std::span<const uint8_t> data) const noexcept;

    /// Convert Windows-1258 to UTF-8
    [[nodiscard]] std::expected<std::string, std::string>
    win1258_to_utf8(std::span<const uint8_t> data) const noexcept;

    /// Normalize any detected encoding to UTF-8
    [[nodiscard]] std::expected<std::string, std::string>
    normalize_to_utf8(std::span<const uint8_t> data,
                      const EncodingDetectionResult& enc) const noexcept;

private:
    // Statistical thresholds
    static constexpr float TCVN3_HIGH_BYTE_RATIO = 0.08f;
    static constexpr float VNI_HIGH_BYTE_RATIO    = 0.05f;
    static constexpr float UTF8_MIN_VALIDITY      = 0.99f;

    // TCVN3 character mapping table (256 entries -> UTF-32 codepoints)
    static const uint32_t TCVN3_TO_UNICODE[256];

    // VNI character mapping table
    static const uint32_t VNI_TO_UNICODE[256];

    // Windows-1258 mapping
    static const uint32_t WIN1258_TO_UNICODE[256];

    [[nodiscard]] float compute_utf8_validity_score(std::span<const uint8_t> data) const noexcept;
    [[nodiscard]] float compute_tcvn3_score(std::span<const uint8_t> data) const noexcept;
    [[nodiscard]] float compute_vni_score(std::span<const uint8_t> data) const noexcept;
    [[nodiscard]] float compute_win1258_score(std::span<const uint8_t> data) const noexcept;

    [[nodiscard]] static std::string uint32_to_utf8(uint32_t cp) noexcept;

    [[nodiscard]] std::expected<std::string, std::string>
    remap_to_utf8(std::span<const uint8_t> data,
                  const uint32_t* table) const noexcept;
};

} // namespace vietlint
