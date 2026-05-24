#include "vietlint/lexer.hpp"
#include <algorithm>
#include <cctype>
#include <cstring>
#include <unordered_set>

namespace vietlint {

// ---------------------------------------------------------------------------
// Keyword tables
// ---------------------------------------------------------------------------
const char* const Lexer::PYTHON_KEYWORDS[] = {
    "False","None","True","and","as","assert","async","await",
    "break","class","continue","def","del","elif","else","except",
    "finally","for","from","global","if","import","in","is",
    "lambda","nonlocal","not","or","pass","raise","return","try",
    "while","with","yield", nullptr
};
const char* const Lexer::JS_KEYWORDS[] = {
    "break","case","catch","class","const","continue","debugger",
    "default","delete","do","else","export","extends","false","finally",
    "for","function","if","import","in","instanceof","let","new","null",
    "return","static","super","switch","this","throw","true","try",
    "typeof","var","void","while","with","yield","async","await",
    "of","from","as","interface","type","enum","implements",
    "declare","abstract","readonly","namespace","module", nullptr
};
const char* const Lexer::CPP_KEYWORDS[] = {
    "alignas","alignof","and","and_eq","asm","auto","bitand","bitor",
    "bool","break","case","catch","char","char8_t","char16_t","char32_t",
    "class","compl","concept","const","consteval","constexpr","constinit",
    "const_cast","continue","co_await","co_return","co_yield","decltype",
    "default","delete","do","double","dynamic_cast","else","enum","explicit",
    "export","extern","false","float","for","friend","goto","if","inline",
    "int","long","mutable","namespace","new","noexcept","not","not_eq",
    "nullptr","operator","or","or_eq","private","protected","public",
    "reinterpret_cast","requires","return","short","signed","sizeof",
    "static","static_assert","static_cast","struct","switch","template",
    "this","thread_local","throw","true","try","typedef","typeid",
    "typename","union","unsigned","using","virtual","void","volatile",
    "wchar_t","while","xor","xor_eq", nullptr
};

// ---------------------------------------------------------------------------
Language detect_language(std::string_view filename) noexcept {
    auto ext_start = filename.rfind('.');
    if (ext_start == std::string_view::npos) return Language::Unknown;
    auto ext = filename.substr(ext_start + 1);

    if (ext == "py")                         return Language::Python;
    if (ext == "js" || ext == "mjs" || ext == "cjs") return Language::JavaScript;
    if (ext == "ts" || ext == "tsx")         return Language::TypeScript;
    if (ext == "c" || ext == "h")            return Language::C;
    if (ext == "cpp" || ext == "cxx" || ext == "cc"
        || ext == "hpp" || ext == "hxx")     return Language::Cpp;
    return Language::Unknown;
}

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------
static bool is_ascii_id_start(char c) noexcept {
    return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || c == '_';
}
static bool is_ascii_id_cont(char c) noexcept {
    return is_ascii_id_start(c) || (c >= '0' && c <= '9');
}

static bool keyword_in_table(std::string_view s, const char* const* table) noexcept {
    for (int i = 0; table[i]; ++i)
        if (s == table[i]) return true;
    return false;
}
bool Lexer::is_python_keyword(std::string_view s) noexcept { return keyword_in_table(s, PYTHON_KEYWORDS); }
bool Lexer::is_js_keyword(std::string_view s)     noexcept { return keyword_in_table(s, JS_KEYWORDS); }
bool Lexer::is_cpp_keyword(std::string_view s)    noexcept { return keyword_in_table(s, CPP_KEYWORDS); }

// ---------------------------------------------------------------------------
Lexer::Lexer(LexerOptions opts) noexcept : opts_(std::move(opts)) {}

// ---------------------------------------------------------------------------
Token Lexer::make_identifier(std::string_view src,
                              uint32_t start, uint32_t end,
                              uint32_t line, uint32_t col) const noexcept {
    Token tok;
    tok.span = { start, end, line, col };
    tok.raw  = std::string(src.substr(start, end - start));
    tok.text = tok.raw;

    // Check for Vietnamese content
    auto raw_span = std::span<const uint8_t>(
        reinterpret_cast<const uint8_t*>(tok.raw.data()), tok.raw.size());
    tok.has_vietnamese = scanner_.has_vietnamese_fast(raw_span);

    if (tok.has_vietnamese) {
        // Determine if purely Vietnamese or mixed
        bool has_ascii_id = false;
        for (char c : tok.raw) {
            if (is_ascii_id_start(c) && static_cast<uint8_t>(c) < 0x80) {
                has_ascii_id = true; break;
            }
        }
        tok.kind = has_ascii_id ? TokenKind::MixedIdentifier
                                : TokenKind::VietnameseIdentifier;
    } else {
        tok.kind = TokenKind::Identifier;
    }
    return tok;
}

// ---------------------------------------------------------------------------
// Python tokenizer
// ---------------------------------------------------------------------------
std::expected<std::vector<Token>, LexerError>
Lexer::tokenize_python(std::string_view src) const noexcept {
    std::vector<Token> tokens;
    tokens.reserve(src.size() / 8);

    const auto* data = reinterpret_cast<const uint8_t*>(src.data());
    size_t n = src.size();
    size_t i = 0;
    uint32_t line = 1, col = 1;

    auto advance = [&](size_t count = 1) {
        for (size_t k = 0; k < count && i < n; ++k) {
            if (src[i] == '\n') { ++line; col = 1; }
            else                { ++col; }
            ++i;
        }
    };

    while (i < n) {
        uint32_t tok_line = line, tok_col = col;
        uint32_t tok_start = static_cast<uint32_t>(i);
        uint8_t  b = data[i];
        char     c = static_cast<char>(b);

        // Encoding declaration: # -*- coding: ... -*-
        if (c == '#' && i == 0) {
            while (i < n && src[i] != '\n') advance();
            std::string raw(src.substr(tok_start, i - tok_start));
            Token t;
            t.kind = (raw.find("coding") != std::string::npos)
                   ? TokenKind::EncodingDecl : TokenKind::Comment;
            t.span = {tok_start, (uint32_t)i, tok_line, tok_col};
            t.raw = raw; t.text = raw;
            tokens.push_back(std::move(t));
            continue;
        }

        // Shebang
        if (c == '#' && i == 0) {
            while (i < n && src[i] != '\n') advance();
            continue;
        }

        // Comment
        if (c == '#') {
            while (i < n && src[i] != '\n') advance();
            std::string raw(src.substr(tok_start, i - tok_start));
            auto raw_span2 = std::span<const uint8_t>(
                reinterpret_cast<const uint8_t*>(raw.data()), raw.size());
            Token t;
            t.kind = scanner_.has_vietnamese_fast(raw_span2)
                   ? TokenKind::VietnameseComment : TokenKind::Comment;
            t.span = {tok_start, (uint32_t)i, tok_line, tok_col};
            t.raw = raw; t.text = raw;
            tokens.push_back(std::move(t));
            continue;
        }

        // Triple-quoted strings """ or '''
        if ((c == '"' || c == '\'') && i + 2 < n && src[i+1] == c && src[i+2] == c) {
            char q = c;
            advance(3);
            while (i + 2 < n) {
                if (src[i] == q && src[i+1] == q && src[i+2] == q) {
                    advance(3); break;
                }
                advance();
            }
            Token t;
            t.kind = TokenKind::BlockComment; // triple strings often docstrings
            t.span = {tok_start, (uint32_t)i, tok_line, tok_col};
            t.raw  = std::string(src.substr(tok_start, i - tok_start));
            t.text = t.raw;
            auto raw_span2 = std::span<const uint8_t>(
                reinterpret_cast<const uint8_t*>(t.raw.data()), t.raw.size());
            t.has_vietnamese = scanner_.has_vietnamese_fast(raw_span2);
            tokens.push_back(std::move(t));
            continue;
        }

        // Single-quoted strings
        if (c == '"' || c == '\'') {
            char q = c;
            advance();
            while (i < n && src[i] != q) {
                if (src[i] == '\\') advance(); // skip escape
                advance();
            }
            if (i < n) advance(); // closing quote
            Token t;
            t.kind = TokenKind::StringLiteral;
            t.span = {tok_start, (uint32_t)i, tok_line, tok_col};
            t.raw  = std::string(src.substr(tok_start, i - tok_start));
            t.text = t.raw;
            tokens.push_back(std::move(t));
            continue;
        }

        // Identifier / keyword (ASCII + Vietnamese multi-byte)
        if (is_ascii_id_start(c) || b >= 0x80) {
            // Consume while identifier-continue bytes (ASCII + Vietnamese multibyte)
            while (i < n) {
                uint8_t nb = data[i];
                if (nb < 0x80) {
                    if (!is_ascii_id_cont(static_cast<char>(nb))) break;
                    advance(); // ASCII id-continue char
                } else if (nb < 0xC0) {
                    advance(); // UTF-8 continuation byte
                } else {
                    advance(); // UTF-8 lead byte
                }
            }
            auto tok = make_identifier(src, tok_start, (uint32_t)i, tok_line, tok_col);
            if (tok.kind == TokenKind::Identifier && is_python_keyword(tok.raw))
                tok.kind = TokenKind::Keyword;
            tokens.push_back(std::move(tok));
            continue;
        }

        // Newline
        if (c == '\n') {
            if (opts_.preserve_whitespace) {
                Token t; t.kind = TokenKind::Newline;
                t.span = {tok_start, tok_start+1, tok_line, tok_col};
                t.raw = "\n"; t.text = "\n";
                tokens.push_back(std::move(t));
            }
            advance();
            continue;
        }

        // Whitespace
        if (std::isspace(static_cast<unsigned char>(c))) {
            while (i < n && std::isspace(static_cast<unsigned char>(src[i]))
                   && src[i] != '\n') advance();
            if (opts_.preserve_whitespace) {
                Token t; t.kind = TokenKind::Whitespace;
                t.span = {tok_start, (uint32_t)i, tok_line, tok_col};
                t.raw  = std::string(src.substr(tok_start, i-tok_start));
                t.text = t.raw;
                tokens.push_back(std::move(t));
            }
            continue;
        }

        // Number
        if (c >= '0' && c <= '9') {
            while (i < n && (std::isalnum(static_cast<unsigned char>(src[i])) || src[i] == '.' || src[i] == '_'))
                advance();
            Token t; t.kind = TokenKind::Number;
            t.span = {tok_start, (uint32_t)i, tok_line, tok_col};
            t.raw = std::string(src.substr(tok_start, i-tok_start));
            t.text = t.raw;
            tokens.push_back(std::move(t));
            continue;
        }

        // Decorator
        if (c == '@') {
            advance();
            uint32_t dec_start = (uint32_t)i;
            while (i < n && (is_ascii_id_cont(src[i]) || src[i] == '.')) advance();
            Token t; t.kind = TokenKind::Decorator;
            t.span = {tok_start, (uint32_t)i, tok_line, tok_col};
            t.raw = std::string(src.substr(tok_start, i-tok_start));
            t.text = t.raw;
            tokens.push_back(std::move(t));
            continue;
        }

        // Punctuation/operator
        Token t; t.kind = TokenKind::Punctuation;
        t.span = {tok_start, tok_start+1, tok_line, tok_col};
        t.raw = std::string(1, c);
        t.text = t.raw;
        tokens.push_back(std::move(t));
        advance();
    }

    Token eof; eof.kind = TokenKind::EndOfFile;
    eof.span = {(uint32_t)n, (uint32_t)n, line, col};
    tokens.push_back(eof);
    return tokens;
}

// ---------------------------------------------------------------------------
// JavaScript/TypeScript tokenizer
// ---------------------------------------------------------------------------
std::expected<std::vector<Token>, LexerError>
Lexer::tokenize_js(std::string_view src) const noexcept {
    std::vector<Token> tokens;
    tokens.reserve(src.size() / 8);
    const auto* data = reinterpret_cast<const uint8_t*>(src.data());
    size_t n = src.size();
    size_t i = 0;
    uint32_t line = 1, col = 1;

    auto advance = [&](size_t count = 1) {
        for (size_t k = 0; k < count && i < n; ++k) {
            if (src[i] == '\n') { ++line; col = 1; } else ++col;
            ++i;
        }
    };

    while (i < n) {
        uint32_t tok_start = (uint32_t)i;
        uint32_t tok_line = line, tok_col = col;
        uint8_t b = data[i];
        char c = static_cast<char>(b);

        // Line comment //
        if (c == '/' && i+1 < n && src[i+1] == '/') {
            while (i < n && src[i] != '\n') advance();
            std::string raw(src.substr(tok_start, i-tok_start));
            auto sp = std::span<const uint8_t>(reinterpret_cast<const uint8_t*>(raw.data()), raw.size());
            Token t; t.kind = scanner_.has_vietnamese_fast(sp) ? TokenKind::VietnameseComment : TokenKind::Comment;
            t.span = {tok_start,(uint32_t)i,tok_line,tok_col}; t.raw=raw; t.text=raw;
            tokens.push_back(std::move(t));
            continue;
        }

        // Block comment /* */
        if (c == '/' && i+1 < n && src[i+1] == '*') {
            advance(2);
            while (i+1 < n && !(src[i] == '*' && src[i+1] == '/')) advance();
            if (i+1 < n) advance(2);
            std::string raw(src.substr(tok_start, i-tok_start));
            auto sp = std::span<const uint8_t>(reinterpret_cast<const uint8_t*>(raw.data()), raw.size());
            Token t; t.kind = scanner_.has_vietnamese_fast(sp) ? TokenKind::VietnameseComment : TokenKind::BlockComment;
            t.span = {tok_start,(uint32_t)i,tok_line,tok_col}; t.raw=raw; t.text=raw;
            tokens.push_back(std::move(t));
            continue;
        }

        // Template literal `...`
        if (c == '`') {
            advance();
            while (i < n && src[i] != '`') {
                if (src[i] == '\\') advance();
                advance();
            }
            if (i < n) advance();
            Token t; t.kind = TokenKind::TemplateLiteral;
            t.span = {tok_start,(uint32_t)i,tok_line,tok_col};
            t.raw = std::string(src.substr(tok_start, i-tok_start)); t.text = t.raw;
            tokens.push_back(std::move(t));
            continue;
        }

        // Strings
        if (c == '"' || c == '\'') {
            char q = c; advance();
            while (i < n && src[i] != q) {
                if (src[i] == '\\') advance();
                advance();
            }
            if (i < n) advance();
            Token t; t.kind = TokenKind::StringLiteral;
            t.span = {tok_start,(uint32_t)i,tok_line,tok_col};
            t.raw = std::string(src.substr(tok_start, i-tok_start)); t.text=t.raw;
            tokens.push_back(std::move(t)); continue;
        }

        // Identifier
        if (is_ascii_id_start(c) || b >= 0x80) {
            while (i < n) {
                uint8_t nb = data[i];
                if (nb < 0x80) {
                    if (!is_ascii_id_cont(static_cast<char>(nb))) break;
                    advance();
                } else {
                    advance(); // multibyte
                }
            }
            auto tok = make_identifier(src, tok_start, (uint32_t)i, tok_line, tok_col);
            if (tok.kind == TokenKind::Identifier && is_js_keyword(tok.raw))
                tok.kind = TokenKind::Keyword;
            tokens.push_back(std::move(tok)); continue;
        }

        // Whitespace/newline
        if (c == '\n') { advance(); continue; }
        if (std::isspace((unsigned char)c)) { while(i<n && std::isspace((unsigned char)src[i]) && src[i]!='\n') advance(); continue; }

        // Number
        if (c >= '0' && c <= '9') {
            while (i<n && (std::isalnum((unsigned char)src[i]) || src[i]=='.' || src[i]=='_' || src[i]=='n')) advance();
            Token t; t.kind=TokenKind::Number; t.span={tok_start,(uint32_t)i,tok_line,tok_col};
            t.raw=std::string(src.substr(tok_start,i-tok_start)); t.text=t.raw;
            tokens.push_back(std::move(t)); continue;
        }

        Token t; t.kind=TokenKind::Punctuation; t.span={tok_start,tok_start+1,tok_line,tok_col};
        t.raw=std::string(1,c); t.text=t.raw; tokens.push_back(std::move(t)); advance();
    }
    Token eof; eof.kind=TokenKind::EndOfFile; eof.span={(uint32_t)n,(uint32_t)n,line,col};
    tokens.push_back(eof);
    return tokens;
}

// ---------------------------------------------------------------------------
// C/C++ tokenizer
// ---------------------------------------------------------------------------
std::expected<std::vector<Token>, LexerError>
Lexer::tokenize_cpp(std::string_view src) const noexcept {
    std::vector<Token> tokens;
    tokens.reserve(src.size() / 8);
    const auto* data = reinterpret_cast<const uint8_t*>(src.data());
    size_t n = src.size();
    size_t i = 0;
    uint32_t line = 1, col = 1;

    auto advance = [&](size_t count = 1) {
        for (size_t k = 0; k < count && i < n; ++k) {
            if (src[i] == '\n') { ++line; col = 1; } else ++col;
            ++i;
        }
    };

    while (i < n) {
        uint32_t tok_start = (uint32_t)i;
        uint32_t tok_line = line, tok_col = col;
        uint8_t b = data[i];
        char c = static_cast<char>(b);

        // // line comment
        if (c == '/' && i+1 < n && src[i+1] == '/') {
            while (i < n && src[i] != '\n') advance();
            std::string raw(src.substr(tok_start, i-tok_start));
            auto sp = std::span<const uint8_t>(reinterpret_cast<const uint8_t*>(raw.data()), raw.size());
            Token t; t.kind = scanner_.has_vietnamese_fast(sp) ? TokenKind::VietnameseComment : TokenKind::Comment;
            t.span={tok_start,(uint32_t)i,tok_line,tok_col}; t.raw=raw; t.text=raw;
            tokens.push_back(std::move(t)); continue;
        }
        // /* block comment */
        if (c == '/' && i+1 < n && src[i+1] == '*') {
            advance(2);
            while (i+1 < n && !(src[i]=='*' && src[i+1]=='/')) advance();
            if (i+1 < n) advance(2);
            std::string raw(src.substr(tok_start, i-tok_start));
            auto sp = std::span<const uint8_t>(reinterpret_cast<const uint8_t*>(raw.data()), raw.size());
            Token t; t.kind = scanner_.has_vietnamese_fast(sp) ? TokenKind::VietnameseComment : TokenKind::BlockComment;
            t.span={tok_start,(uint32_t)i,tok_line,tok_col}; t.raw=raw; t.text=raw;
            tokens.push_back(std::move(t)); continue;
        }

        // Raw string R"..."
        if (c == 'R' && i+1 < n && src[i+1] == '"') {
            advance(2);
            size_t delim_start = i;
            while (i < n && src[i] != '(') advance();
            std::string delim = ")" + std::string(src.substr(delim_start, i - delim_start)) + "\"";
            if (i < n) advance(); // (
            auto found = src.find(delim, i);
            if (found != std::string_view::npos) {
                while (i <= found + delim.size()) advance();
            } else { while (i < n) advance(); }
            Token t; t.kind=TokenKind::StringLiteral; t.span={tok_start,(uint32_t)i,tok_line,tok_col};
            t.raw=std::string(src.substr(tok_start,i-tok_start)); t.text=t.raw;
            tokens.push_back(std::move(t)); continue;
        }

        // Strings
        if (c == '"' || c == '\'') {
            char q=c; advance();
            while (i<n && src[i]!=q) { if (src[i]=='\\') advance(); advance(); }
            if (i<n) advance();
            Token t; t.kind=TokenKind::StringLiteral; t.span={tok_start,(uint32_t)i,tok_line,tok_col};
            t.raw=std::string(src.substr(tok_start,i-tok_start)); t.text=t.raw;
            tokens.push_back(std::move(t)); continue;
        }

        // Identifier (includes u8, L, wchar prefixes)
        if (is_ascii_id_start(c) || b >= 0x80) {
            while (i < n) {
                uint8_t nb = data[i];
                if (nb < 0x80) {
                    if (!is_ascii_id_cont(static_cast<char>(nb))) break;
                    advance();
                } else {
                    advance(); // multibyte
                }
            }
            auto tok = make_identifier(src, tok_start, (uint32_t)i, tok_line, tok_col);
            if (tok.kind == TokenKind::Identifier && is_cpp_keyword(tok.raw))
                tok.kind = TokenKind::Keyword;
            tokens.push_back(std::move(tok)); continue;
        }

        // Preprocessor directives
        if (c == '#' && col == 1) {
            while (i < n && src[i] != '\n') {
                if (src[i] == '\\' && i+1 < n && src[i+1] == '\n') advance();
                advance();
            }
            Token t; t.kind=TokenKind::Punctuation; t.span={tok_start,(uint32_t)i,tok_line,tok_col};
            t.raw=std::string(src.substr(tok_start,i-tok_start)); t.text=t.raw;
            tokens.push_back(std::move(t)); continue;
        }

        if (c=='\n'||std::isspace((unsigned char)c)) { advance(); continue; }

        if (c>='0'&&c<='9') {
            while (i<n&&(std::isalnum((unsigned char)src[i])||src[i]=='.'||src[i]=='\''||src[i]=='x'||src[i]=='b')) advance();
            Token t; t.kind=TokenKind::Number; t.span={tok_start,(uint32_t)i,tok_line,tok_col};
            t.raw=std::string(src.substr(tok_start,i-tok_start)); t.text=t.raw;
            tokens.push_back(std::move(t)); continue;
        }

        Token t; t.kind=TokenKind::Operator; t.span={tok_start,tok_start+1,tok_line,tok_col};
        t.raw=std::string(1,c); t.text=t.raw; tokens.push_back(std::move(t)); advance();
    }
    Token eof; eof.kind=TokenKind::EndOfFile; eof.span={(uint32_t)n,(uint32_t)n,line,col};
    tokens.push_back(eof); return tokens;
}

// ---------------------------------------------------------------------------
std::expected<std::vector<Token>, LexerError>
Lexer::tokenize(std::string_view source, std::string_view filename) const noexcept {
    Language lang = opts_.language;
    if (lang == Language::Unknown)
        lang = detect_language(filename);

    switch (lang) {
        case Language::Python:     return tokenize_python(source);
        case Language::JavaScript:
        case Language::TypeScript: return tokenize_js(source);
        case Language::C:
        case Language::Cpp:        return tokenize_cpp(source);
        default:
            // Heuristic: try Python first (most common in Vietnamese codebases)
            return tokenize_python(source);
    }
}

std::vector<Token>
Lexer::extract_identifiers(std::string_view source, Language lang) const noexcept {
    auto result = tokenize(source, "");
    if (!result) return {};
    std::vector<Token> ids;
    ids.reserve(result->size() / 4);
    for (auto& t : *result) {
        if (t.is_identifier()) ids.push_back(std::move(t));
    }
    return ids;
}

} // namespace vietlint
