#pragma once
#include "vietlint/utf8_scanner.hpp"
#include <string>
#include <string_view>
#include <vector>
#include <expected>
#include <variant>
#include <cstdint>
#include <memory>
#include <functional>

namespace vietlint {

/// Source language type
enum class Language : uint8_t {
    Python,
    JavaScript,
    TypeScript,
    C,
    Cpp,
    Unknown,
};

Language detect_language(std::string_view filename) noexcept;

/// Token kinds - unified across all supported languages
enum class TokenKind : uint16_t {
    // Structural
    Identifier,
    Keyword,
    Number,
    StringLiteral,
    ByteString,
    TemplateLiteral,      // JS template `...`
    Comment,              // single-line
    BlockComment,         // multi-line /* */ or """ """
    Shebang,              // #!
    Decorator,            // @decorator
    // Operators / punctuation
    Punctuation,
    Operator,
    // Special
    Whitespace,
    Newline,
    EndOfFile,
    Invalid,
    // Vietnamese-specific augmented tokens
    VietnameseIdentifier,  ///< Identifier containing Vietnamese chars
    MixedIdentifier,       ///< Both ASCII and Vietnamese chars
    VietnameseComment,     ///< Comment body contains Vietnamese text
    EncodingDecl,          ///< -*- coding: utf-8 -*- or similar
};

/// Span within source file
struct SourceSpan {
    uint32_t start;   ///< Byte offset start
    uint32_t end;     ///< Byte offset end (exclusive)
    uint32_t line;    ///< 1-based line
    uint32_t col;     ///< 1-based column
};

/// A lexical token
struct Token {
    TokenKind    kind;
    SourceSpan   span;
    std::string  text;    ///< Normalized text (may differ from raw for strings)
    std::string  raw;     ///< Exact source bytes
    bool         has_vietnamese = false;

    [[nodiscard]] std::string_view view() const noexcept { return raw; }
    [[nodiscard]] bool is_comment()    const noexcept {
        return kind == TokenKind::Comment || kind == TokenKind::BlockComment
            || kind == TokenKind::VietnameseComment;
    }
    [[nodiscard]] bool is_identifier() const noexcept {
        return kind == TokenKind::Identifier
            || kind == TokenKind::VietnameseIdentifier
            || kind == TokenKind::MixedIdentifier;
    }
};

/// Lexer error
struct LexerError {
    uint32_t    offset;
    std::string message;
    TokenKind   context;
};

/// Options controlling lexer behavior
struct LexerOptions {
    bool preserve_whitespace = false;
    bool preserve_comments   = true;   ///< Required for convention checking
    bool track_vietnamese    = true;
    Language language        = Language::Unknown; ///< auto-detect if Unknown
};

/// Multi-language lexer capable of tokenizing Python, JS/TS, C/C++
/// Thread-safe: all state is per-call (no mutable members).
class Lexer {
public:
    explicit Lexer(LexerOptions opts = {}) noexcept;
    ~Lexer() noexcept = default;

    Lexer(const Lexer&) = delete;
    Lexer& operator=(const Lexer&) = delete;

    /// Tokenize source with given language (or auto-detect from filename)
    [[nodiscard]] std::expected<std::vector<Token>, LexerError>
    tokenize(std::string_view source, std::string_view filename = "") const noexcept;

    /// Extract only identifiers (fast path, skips most tokens)
    [[nodiscard]] std::vector<Token>
    extract_identifiers(std::string_view source, Language lang) const noexcept;

private:
    LexerOptions opts_;
    UTF8Scanner  scanner_;

    // Per-language tokenizers
    [[nodiscard]] std::expected<std::vector<Token>, LexerError>
    tokenize_python(std::string_view src) const noexcept;

    [[nodiscard]] std::expected<std::vector<Token>, LexerError>
    tokenize_js(std::string_view src) const noexcept;

    [[nodiscard]] std::expected<std::vector<Token>, LexerError>
    tokenize_cpp(std::string_view src) const noexcept;

    // Shared helpers
    [[nodiscard]] Token make_identifier(std::string_view src,
                                         uint32_t start, uint32_t end,
                                         uint32_t line, uint32_t col) const noexcept;

    [[nodiscard]] static bool is_python_keyword(std::string_view s) noexcept;
    [[nodiscard]] static bool is_js_keyword(std::string_view s) noexcept;
    [[nodiscard]] static bool is_cpp_keyword(std::string_view s) noexcept;

    static const char* const PYTHON_KEYWORDS[];
    static const char* const JS_KEYWORDS[];
    static const char* const CPP_KEYWORDS[];
};

} // namespace vietlint
