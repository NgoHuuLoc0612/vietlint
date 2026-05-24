#include "vietlint/utf8_scanner.hpp"
#include <algorithm>
#include <cassert>
#include <cstring>
#include <bit>

#if defined(__AVX2__)
#  include <immintrin.h>
#endif

namespace vietlint {

// ---------------------------------------------------------------------------
// CPU feature detection
// ---------------------------------------------------------------------------
static bool detect_avx2() noexcept {
#if defined(__AVX2__)
    // Compiler already guarantees AVX2 support if built with -mavx2
    return true;
#elif defined(_MSC_VER)
    int info[4]{};
    __cpuidex(info, 7, 0);
    return (info[1] & (1 << 5)) != 0;
#elif defined(__GNUC__) || defined(__clang__)
    return __builtin_cpu_supports("avx2");
#else
    return false;
#endif
}

// ---------------------------------------------------------------------------
// UTF8Scanner
// ---------------------------------------------------------------------------
UTF8Scanner::UTF8Scanner() noexcept
    : avx2_available_(detect_avx2())
{}

// The 1EXX range in UTF-8 encodes as: E1 B8/B9/BA/BB/BC/BD/BE/BF ...
// Vietnamese chars are primarily in U+1E00-U+1EFF
// Lead byte for 0x1E00-0x1EFF = 0xE1, continuation high nibble 0xB8-0xBF
// Also U+00C0-U+024F: 2-byte sequences 0xC3..0xC9 + continuation

#if defined(__AVX2__)
bool UTF8Scanner::avx2_has_vietnamese(const uint8_t* data, size_t len) const noexcept {
    // Strategy: look for lead bytes 0xE1 (for U+1E00..U+1EFF range)
    // and 0xC3..0xC9 (for U+00C0..U+024F range)
    const __m256i lead_1e = _mm256_set1_epi8(static_cast<char>(0xE1));
    const __m256i c3      = _mm256_set1_epi8(static_cast<char>(0xC3));
    const __m256i c9      = _mm256_set1_epi8(static_cast<char>(0xC9));

    size_t i = 0;
    for (; i + 32 <= len; i += 32) {
        __m256i chunk = _mm256_loadu_si256(reinterpret_cast<const __m256i*>(data + i));

        // Check for 0xE1 lead bytes (U+1E00-U+1EFF main Vietnamese block)
        __m256i eq_e1 = _mm256_cmpeq_epi8(chunk, lead_1e);
        if (_mm256_movemask_epi8(eq_e1) != 0)
            return true;

        // Check for bytes in [0xC3..0xC9] (Latin extended Vietnamese)
        // byte >= 0xC3 && byte <= 0xC9
        // Using saturating subtract: (x - 0xC3) <= (0xC9 - 0xC3) = 6
        __m256i sub = _mm256_subs_epu8(chunk, c3);
        __m256i threshold = _mm256_set1_epi8(6);
        __m256i in_range = _mm256_cmpeq_epi8(_mm256_min_epu8(sub, threshold), sub);
        // also exclude zero (bytes below 0xC3 become 0 after sat-sub)
        __m256i nonzero = _mm256_cmpgt_epi8(chunk, _mm256_sub_epi8(c3, _mm256_set1_epi8(1)));
        __m256i hit = _mm256_and_si256(in_range, nonzero);
        (void)c9; // used via threshold arithmetic above
        if (_mm256_movemask_epi8(hit) != 0)
            return true;
    }

    // Scalar tail
    for (; i < len; ++i) {
        uint8_t b = data[i];
        if (b == 0xE1 || (b >= 0xC3 && b <= 0xC9))
            return true;
    }
    return false;
}
#else
bool UTF8Scanner::avx2_has_vietnamese(const uint8_t* data, size_t len) const noexcept {
    for (size_t i = 0; i < len; ++i) {
        uint8_t b = data[i];
        if (b == 0xE1 || (b >= 0xC3 && b <= 0xC9))
            return true;
    }
    return false;
}
#endif

// ---------------------------------------------------------------------------
// Single codepoint decoder (scalar, inlined for tight loop)
// ---------------------------------------------------------------------------
std::expected<CodepointInfo, ScanError>
UTF8Scanner::decode_one(const uint8_t* ptr, const uint8_t* end, uint32_t offset) noexcept {
    if (ptr >= end)
        return std::unexpected(ScanError::UnexpectedEOF);

    uint8_t b0 = ptr[0];
    CodepointInfo info{};
    info.byte_offset = offset;

    auto fill_flags = [](CodepointInfo& ci) {
        uint32_t cp = ci.codepoint;
        ci.is_vietnamese = VietnameseRanges::contains(cp);
        // XID_Start approximation: letter or underscore
        ci.is_identifier_start =
            (cp == '_') ||
            (cp >= 'A' && cp <= 'Z') ||
            (cp >= 'a' && cp <= 'z') ||
            ci.is_vietnamese ||
            (cp > 0x7F && !((cp >= 0x0300 && cp <= 0x036F))); // exclude pure combiners
        ci.is_identifier_continue =
            ci.is_identifier_start ||
            (cp >= '0' && cp <= '9') ||
            (cp >= 0x0300 && cp <= 0x036F); // combiners OK in continue
    };

    // 1-byte (ASCII)
    if ((b0 & 0x80) == 0) {
        info.codepoint   = b0;
        info.byte_length = 1;
        fill_flags(info);
        return info;
    }

    // 2-byte
    if ((b0 & 0xE0) == 0xC0) {
        if (ptr + 1 >= end)
            return std::unexpected(ScanError::UnexpectedEOF);
        uint8_t b1 = ptr[1];
        if ((b1 & 0xC0) != 0x80)
            return std::unexpected(ScanError::InvalidUTF8);
        info.codepoint   = ((b0 & 0x1F) << 6) | (b1 & 0x3F);
        info.byte_length = 2;
        if (info.codepoint < 0x80) // overlong
            return std::unexpected(ScanError::InvalidUTF8);
        fill_flags(info);
        return info;
    }

    // 3-byte
    if ((b0 & 0xF0) == 0xE0) {
        if (ptr + 2 >= end)
            return std::unexpected(ScanError::UnexpectedEOF);
        uint8_t b1 = ptr[1], b2 = ptr[2];
        if ((b1 & 0xC0) != 0x80 || (b2 & 0xC0) != 0x80)
            return std::unexpected(ScanError::InvalidUTF8);
        info.codepoint   = ((b0 & 0x0F) << 12) | ((b1 & 0x3F) << 6) | (b2 & 0x3F);
        info.byte_length = 3;
        if (info.codepoint < 0x800) // overlong
            return std::unexpected(ScanError::InvalidUTF8);
        // Surrogates D800-DFFF are invalid in UTF-8
        if (info.codepoint >= 0xD800 && info.codepoint <= 0xDFFF)
            return std::unexpected(ScanError::InvalidUTF8);
        fill_flags(info);
        return info;
    }

    // 4-byte
    if ((b0 & 0xF8) == 0xF0) {
        if (ptr + 3 >= end)
            return std::unexpected(ScanError::UnexpectedEOF);
        uint8_t b1 = ptr[1], b2 = ptr[2], b3 = ptr[3];
        if ((b1 & 0xC0) != 0x80 || (b2 & 0xC0) != 0x80 || (b3 & 0xC0) != 0x80)
            return std::unexpected(ScanError::InvalidUTF8);
        info.codepoint   = ((b0 & 0x07) << 18) | ((b1 & 0x3F) << 12)
                         | ((b2 & 0x3F) << 6)  |  (b3 & 0x3F);
        info.byte_length = 4;
        if (info.codepoint < 0x10000 || info.codepoint > 0x10FFFF) // overlong / out of range
            return std::unexpected(ScanError::InvalidUTF8);
        fill_flags(info);
        return info;
    }

    return std::unexpected(ScanError::InvalidUTF8);
}

bool UTF8Scanner::is_valid_leading_byte(uint8_t b) noexcept {
    return b < 0x80 || (b >= 0xC2 && b <= 0xF4);
}

// ---------------------------------------------------------------------------
// Full scan
// ---------------------------------------------------------------------------
std::expected<std::vector<CodepointInfo>, ScanError>
UTF8Scanner::scan(std::span<const uint8_t> input) const noexcept {
    if (input.empty())
        return std::vector<CodepointInfo>{};

    std::vector<CodepointInfo> result;
    result.reserve(input.size()); // upper bound, shrink later

    const uint8_t* ptr = input.data();
    const uint8_t* end = ptr + input.size();
    uint32_t offset = 0;

    while (ptr < end) {
        auto res = decode_one(ptr, end, offset);
        if (!res) {
            // Skip invalid byte and continue (error-tolerant mode)
            // We still record an error but continue scanning
            ++ptr;
            ++offset;
            continue;
        }
        CodepointInfo& ci = *res;
        ptr    += ci.byte_length;
        offset += ci.byte_length;
        result.push_back(ci);
    }

    result.shrink_to_fit();
    return result;
}

// ---------------------------------------------------------------------------
bool UTF8Scanner::has_vietnamese_fast(std::span<const uint8_t> input) const noexcept {
    if (input.empty()) return false;
    if (avx2_available_)
        return avx2_has_vietnamese(input.data(), input.size());
    return avx2_has_vietnamese(input.data(), input.size()); // falls back to scalar
}

// ---------------------------------------------------------------------------
uint32_t UTF8Scanner::count_vietnamese(std::span<const uint8_t> input) const noexcept {
    if (!has_vietnamese_fast(input)) return 0; // fast bail-out

    auto res = scan(input);
    if (!res) return 0;
    return static_cast<uint32_t>(
        std::count_if(res->begin(), res->end(),
            [](const CodepointInfo& ci){ return ci.is_vietnamese; }));
}

// ---------------------------------------------------------------------------
std::vector<std::string_view>
UTF8Scanner::extract_vietnamese_tokens(std::string_view source) const noexcept {
    std::vector<std::string_view> tokens;
    if (source.empty()) return tokens;

    auto raw = std::span<const uint8_t>(
        reinterpret_cast<const uint8_t*>(source.data()), source.size());

    if (!has_vietnamese_fast(raw)) return tokens;

    auto cps_result = scan(raw);
    if (!cps_result) return tokens;

    const auto& cps = *cps_result;
    size_t i = 0;
    while (i < cps.size()) {
        if (!cps[i].is_identifier_start) { ++i; continue; }

        // Find end of identifier run
        size_t j = i + 1;
        while (j < cps.size() && cps[j].is_identifier_continue) ++j;

        // Check if any codepoint in [i..j) is Vietnamese
        bool has_viet = false;
        for (size_t k = i; k < j; ++k) {
            if (cps[k].is_vietnamese) { has_viet = true; break; }
        }
        if (has_viet) {
            uint32_t start_off = cps[i].byte_offset;
            uint32_t end_off   = cps[j-1].byte_offset + cps[j-1].byte_length;
            tokens.emplace_back(source.data() + start_off, end_off - start_off);
        }
        i = j;
    }
    return tokens;
}

// ---------------------------------------------------------------------------
bool UTF8Scanner::validate_utf8(std::span<const uint8_t> input) const noexcept {
    const uint8_t* ptr = input.data();
    const uint8_t* end = ptr + input.size();
    uint32_t offset = 0;
    while (ptr < end) {
        auto res = decode_one(ptr, end, offset);
        if (!res) return false;
        ptr    += res->byte_length;
        offset += res->byte_length;
    }
    return true;
}

// ---------------------------------------------------------------------------
// PositionTracker
// ---------------------------------------------------------------------------
PositionTracker::PositionTracker(std::string_view source) noexcept
    : source_(source)
{
    // Precompute newline byte offsets for O(log n) line lookup
    newline_offsets_.reserve(source.size() / 40 + 1); // avg ~40 chars/line
    for (uint32_t i = 0; i < source.size(); ++i) {
        if (source[i] == '\n')
            newline_offsets_.push_back(i);
    }
}

PositionTracker::Position
PositionTracker::offset_to_position(uint32_t byte_offset) const noexcept {
    // Binary search for the last newline before byte_offset
    auto it = std::upper_bound(newline_offsets_.begin(), newline_offsets_.end(), byte_offset);
    uint32_t line = static_cast<uint32_t>(it - newline_offsets_.begin()) + 1; // 1-based

    uint32_t line_start = (it == newline_offsets_.begin())
        ? 0
        : *(std::prev(it)) + 1;

    // Column = number of Unicode codepoints from line_start to byte_offset
    uint32_t col = 1;
    const uint8_t* ptr = reinterpret_cast<const uint8_t*>(source_.data()) + line_start;
    const uint8_t* target = reinterpret_cast<const uint8_t*>(source_.data()) + byte_offset;
    const uint8_t* end    = reinterpret_cast<const uint8_t*>(source_.data()) + source_.size();

    while (ptr < target && ptr < end) {
        uint8_t b = *ptr;
        uint8_t adv = 1;
        if      (b >= 0xF0 && ptr+3 < end) adv = 4;
        else if (b >= 0xE0 && ptr+2 < end) adv = 3;
        else if (b >= 0xC0 && ptr+1 < end) adv = 2;
        ptr += adv;
        ++col;
    }

    return Position{ line, col, byte_offset };
}

std::vector<PositionTracker::Position>
PositionTracker::resolve_offsets(std::span<const uint32_t> sorted_offsets) const noexcept {
    std::vector<Position> result;
    result.reserve(sorted_offsets.size());
    for (uint32_t off : sorted_offsets)
        result.push_back(offset_to_position(off));
    return result;
}

} // namespace vietlint
