#pragma once
#include "vietlint/lexer.hpp"
#include <string>
#include <string_view>
#include <vector>
#include <array>
#include <cstdint>
#include <expected>
#include <regex>
#include <unordered_map>

namespace vietlint {

/// Classification result for an identifier
enum class IdentifierClass : uint8_t {
    PureASCII,           ///< No Vietnamese characters
    PureVietnamese,      ///< All non-ASCII chars are Vietnamese
    MixedVietnamese,     ///< Mix of ASCII + Vietnamese
    TransliteratedViet,  ///< ASCII but matches Vietnamese word patterns (e.g. "ten", "tuoi")
    Abbreviation,        ///< Looks like a Vietnamese abbreviation (e.g. "QLKH")
    Unknown,
};

/// Naming convention style
enum class NamingStyle : uint8_t {
    SnakeCase,           ///< viet_namese_var
    CamelCase,           ///< vietNamese
    PascalCase,          ///< VietNamese
    ScreamingSnake,      ///< VIET_NAM_CONST
    KebabCase,           ///< viet-nam (CSS/HTML)
    MixedCase,           ///< Inconsistent
    Single,              ///< Single token
    Unknown,
};

/// Convention violation severity
enum class Severity : uint8_t { Info, Warning, Error, Fatal };

/// Detailed classification of a single identifier
struct IdentifierAnalysis {
    std::string        identifier;
    IdentifierClass    cls;
    NamingStyle        style;
    float              vietnamese_ratio;  ///< 0.0-1.0 fraction of codepoints that are Vietnamese
    float              confidence;        ///< ML/heuristic confidence 0.0-1.0
    bool               is_keyword;
    bool               has_diacritics;   ///< Contains Vietnamese diacritical marks
    std::vector<std::string> suggestions; ///< Suggested ASCII alternatives or fixes
};

/// Convention rule result
struct ConventionViolation {
    std::string      rule_id;
    std::string      message;
    Severity         severity;
    SourceSpan       span;
    std::string      identifier;
    std::vector<std::string> fixes;
};

/// The Vietnamese identifier classifier
/// Uses regex patterns, Unicode property lookups, and frequency tables
/// to classify identifiers and detect convention violations.
class VietnameseClassifier {
public:
    explicit VietnameseClassifier();
    ~VietnameseClassifier() noexcept = default;

    VietnameseClassifier(const VietnameseClassifier&) = delete;
    VietnameseClassifier& operator=(const VietnameseClassifier&) = delete;

    /// Classify a single identifier token
    [[nodiscard]] IdentifierAnalysis classify(const Token& tok) const noexcept;

    /// Classify raw string (no Token wrapper)
    [[nodiscard]] IdentifierAnalysis classify_string(std::string_view id) const noexcept;

    /// Batch classify a list of tokens efficiently
    [[nodiscard]] std::vector<IdentifierAnalysis>
    classify_batch(std::span<const Token> tokens) const noexcept;

    /// Check for naming convention violations
    [[nodiscard]] std::vector<ConventionViolation>
    check_conventions(std::span<const Token> tokens, Language lang) const noexcept;

    /// Detect naming style of an identifier
    [[nodiscard]] static NamingStyle detect_style(std::string_view id) noexcept;

    /// Transliteration check: does ASCII identifier look like a Vietnamese word?
    [[nodiscard]] bool is_vietnamese_transliteration(std::string_view id) const noexcept;

    /// Suggest camelCase or snake_case equivalent
    [[nodiscard]] std::string suggest_ascii_equivalent(std::string_view viet_id) const noexcept;

private:
    UTF8Scanner scanner_;

    // Vietnamese common word list (subset of most common 2000 words used in identifiers)
    // Stored as sorted array for binary search
    std::vector<std::string> viet_word_list_;
    std::vector<std::string> viet_abbrev_list_;

    // Compiled regexes for Vietnamese patterns
    std::regex viet_char_pattern_;      // Matches Vietnamese Unicode chars
    std::regex viet_word_boundary_;     // Word boundaries in Vietnamese text
    std::regex transliteration_pattern_; // Common Vietnamese words without diacritics

    // Character frequency tables for heuristic classification
    std::unordered_map<uint32_t, float> viet_char_frequency_;

    void init_word_lists() noexcept;
    void init_char_frequencies() noexcept;
    void init_patterns();

    [[nodiscard]] float compute_vietnamese_ratio(std::string_view id) const noexcept;
    [[nodiscard]] std::vector<std::string> compute_style_suggestions(
        std::string_view id, NamingStyle current_style, Language lang) const noexcept;
};

} // namespace vietlint
