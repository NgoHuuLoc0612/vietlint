#pragma once
#include "vietlint/lexer.hpp"
#include <string>
#include <string_view>
#include <vector>
#include <filesystem>
#include <memory>
#include <expected>
#include <unordered_map>
#include <functional>

// Forward-declare tree-sitter C types to keep header lean
struct TSParser;
struct TSTree;
struct TSLanguage;

namespace vietlint {

// ---------------------------------------------------------------------------
// Extended Language enum — add all tree-sitter grammars
// Keep in sync with tree_sitter_lexer.cpp grammar_map
// ---------------------------------------------------------------------------
// NOTE: The base Language enum in lexer.hpp stays as-is for ABI compatibility.
// TreeSitterLexer maps file extensions → TSLanguageID internally.

enum class TSLanguageID : uint8_t {
    C,
    Cpp,
    CSharp,
    CSS,
    Go,
    Java,
    JavaScript,
    JSDoc,
    JSON,
    Julia,
    OCaml,
    PHP,
    Python,
    QL,
    Rust,
    Scala,
    TSX,
    TypeScript,
    Unknown,
};

/// Info about each grammar
struct GrammarInfo {
    TSLanguageID  id;
    std::string   name;          ///< Display name
    std::string   wasm_file;     ///< e.g. "tree-sitter-python.wasm"
    std::vector<std::string> extensions;  ///< e.g. {".py", ".pyw"}
};

// ---------------------------------------------------------------------------
// Node-type → TokenKind mapping
// tree-sitter node types differ per grammar; this provides a unified view.
// ---------------------------------------------------------------------------
struct NodeTypeMap {
    // Identifier node type names for this language
    std::vector<std::string> identifier_types;
    // Comment node type names
    std::vector<std::string> comment_types;
    // String literal node type names
    std::vector<std::string> string_types;
    // Keyword node type names (optional, for fast path)
    std::vector<std::string> keyword_types;
};

// ---------------------------------------------------------------------------
// TreeSitterLexer
//
// Drop-in replacement for vietlint::Lexer that uses tree-sitter for parsing.
// Falls back to the original Lexer when no grammar is available.
//
// Usage:
//   TreeSitterLexer ts_lexer("/path/to/wasm/dir");
//   auto tokens = ts_lexer.tokenize(source, "main.go");
//
// Thread safety: Each instance owns its TSParser (not thread-safe).
// Use one instance per thread or protect with a mutex.
// ---------------------------------------------------------------------------
class TreeSitterLexer {
public:
    /// wasm_dir: directory containing tree-sitter-*.wasm files
    /// If empty, uses executable directory / "grammars"
    explicit TreeSitterLexer(std::filesystem::path wasm_dir = {},
                              LexerOptions opts = {}) noexcept;
    ~TreeSitterLexer() noexcept;

    TreeSitterLexer(const TreeSitterLexer&) = delete;
    TreeSitterLexer& operator=(const TreeSitterLexer&) = delete;

    // Move is OK
    TreeSitterLexer(TreeSitterLexer&&) noexcept;
    TreeSitterLexer& operator=(TreeSitterLexer&&) noexcept;

    /// Tokenize source. Detects language from filename extension.
    /// Falls back to built-in Lexer if no tree-sitter grammar available.
    [[nodiscard]] std::expected<std::vector<Token>, LexerError>
    tokenize(std::string_view source, std::string_view filename = "") const noexcept;

    /// Extract only identifiers (fast path)
    [[nodiscard]] std::vector<Token>
    extract_identifiers(std::string_view source,
                        std::string_view filename) const noexcept;

    /// Detect language from filename
    [[nodiscard]] static TSLanguageID detect_language(std::string_view filename) noexcept;

    /// Check if a grammar is available for the given language
    [[nodiscard]] bool has_grammar(TSLanguageID lang) const noexcept;

    /// List all available grammars (found wasm files)
    [[nodiscard]] std::vector<GrammarInfo> available_grammars() const noexcept;

    /// All supported grammars (whether wasm present or not)
    [[nodiscard]] static const std::vector<GrammarInfo>& all_grammars() noexcept;

    /// Convert TSLanguageID → base Language (for fallback lexer)
    [[nodiscard]] static Language to_base_language(TSLanguageID id) noexcept;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;

    // Parse source with tree-sitter and extract tokens
    [[nodiscard]] std::expected<std::vector<Token>, LexerError>
    parse_with_tree_sitter(std::string_view source,
                            TSLanguageID lang_id) const noexcept;

    // Walk tree and collect tokens
    void walk_tree(const TSTree* tree,
                   std::string_view source,
                   TSLanguageID lang_id,
                   std::vector<Token>& out) const noexcept;

    // Map a tree-sitter node type string → TokenKind
    [[nodiscard]] static TokenKind map_node_type(std::string_view node_type,
                                                   TSLanguageID lang_id) noexcept;

    // Get node type map for a language
    [[nodiscard]] static const NodeTypeMap& get_node_type_map(TSLanguageID id) noexcept;

    // Build a Token from a tree-sitter node
    [[nodiscard]] Token make_token(std::string_view source,
                                    uint32_t start_byte, uint32_t end_byte,
                                    uint32_t start_row,  uint32_t start_col,
                                    TokenKind kind) const noexcept;
};

// ---------------------------------------------------------------------------
// UnifiedLexer
//
// Wraps both TreeSitterLexer and the original Lexer.
// Always tries tree-sitter first; falls back to built-in for unknown langs.
// ---------------------------------------------------------------------------
class UnifiedLexer {
public:
    explicit UnifiedLexer(std::filesystem::path wasm_dir = {},
                           LexerOptions opts = {}) noexcept;

    [[nodiscard]] std::expected<std::vector<Token>, LexerError>
    tokenize(std::string_view source, std::string_view filename = "") const noexcept;

    [[nodiscard]] std::vector<Token>
    extract_identifiers(std::string_view source,
                        std::string_view filename) const noexcept;

    [[nodiscard]] bool has_tree_sitter_support(std::string_view filename) const noexcept;

    [[nodiscard]] std::vector<GrammarInfo> available_grammars() const noexcept;

private:
    TreeSitterLexer ts_lexer_;
    Lexer           fallback_lexer_;
    LexerOptions    opts_;
};

} // namespace vietlint
