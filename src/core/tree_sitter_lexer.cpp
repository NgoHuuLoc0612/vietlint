#include "vietlint/tree_sitter_lexer.hpp"
#include "vietlint/utf8_scanner.hpp"

#include <algorithm>
#include <cassert>
#include <cstring>
#include <unordered_set>
#include <filesystem>
#include <dlfcn.h>
#include <fstream>

// tree-sitter C API — header-only, no linking needed for type declarations
#if __has_include(<tree_sitter/api.h>)
#  include <tree_sitter/api.h>
#  define HAVE_TREE_SITTER 1
#else
#  define HAVE_TREE_SITTER 0
// Minimal stubs so the file compiles without tree-sitter installed
struct TSParser {};
struct TSTree   {};
struct TSNode   { uint32_t context[4]; const void* id; const void* tree; };
struct TSLanguage {};
using TSPoint = struct { uint32_t row; uint32_t column; };
#endif

namespace vietlint {

// ---------------------------------------------------------------------------
// Grammar registry — static table of all supported grammars
// ---------------------------------------------------------------------------
static const std::vector<GrammarInfo> ALL_GRAMMARS = {
    { TSLanguageID::C,          "C",          "c.so",          {".c", ".h"} },
    { TSLanguageID::Cpp,        "C++",        "cpp.so",        {".cpp",".cxx",".cc",".hpp",".hxx",".h++"} },
    { TSLanguageID::CSharp,     "C#",         "c_sharp.so",    {".cs"} },
    { TSLanguageID::CSS,        "CSS",        "css.so",        {".css"} },
    { TSLanguageID::Go,         "Go",         "go.so",         {".go"} },
    { TSLanguageID::Java,       "Java",       "java.so",       {".java"} },
    { TSLanguageID::JavaScript, "JavaScript", "javascript.so", {".js", ".mjs", ".cjs"} },
    { TSLanguageID::JSDoc,      "JSDoc",      "jsdoc.so",      {} },  // embedded
    { TSLanguageID::JSON,       "JSON",       "json.so",       {".json", ".jsonc"} },
    { TSLanguageID::Julia,      "Julia",      "julia.so",      {".jl"} },
    { TSLanguageID::OCaml,      "OCaml",      "ocaml.so",      {".ml", ".mli"} },
    { TSLanguageID::PHP,        "PHP",        "php.so",        {".php", ".php5", ".phtml"} },
    { TSLanguageID::Python,     "Python",     "python.so",     {".py", ".pyw", ".pyi"} },
    { TSLanguageID::QL,         "QL",         "ql.so",         {".ql", ".qll"} },
    { TSLanguageID::Rust,       "Rust",       "rust.so",       {".rs"} },
    { TSLanguageID::Scala,      "Scala",      "scala.so",      {".scala", ".sc"} },
    { TSLanguageID::TSX,        "TSX",        "tsx.so",        {".tsx"} },
    { TSLanguageID::TypeScript, "TypeScript", "typescript.so", {".ts", ".mts", ".cts"} },
};

// ---------------------------------------------------------------------------
// Node-type maps per language
// Maps tree-sitter grammar node type strings → TokenKind
// ---------------------------------------------------------------------------
static const std::unordered_map<TSLanguageID, NodeTypeMap> NODE_TYPE_MAPS = {
    { TSLanguageID::Python, {
        .identifier_types = {"identifier", "type_comment"},
        .comment_types    = {"comment"},
        .string_types     = {"string", "concatenated_string", "string_content"},
        .keyword_types    = {"def", "class", "import", "from", "return",
                             "if", "elif", "else", "for", "while", "with",
                             "try", "except", "finally", "pass", "break",
                             "continue", "lambda", "yield", "async", "await"},
    }},
    { TSLanguageID::JavaScript, {
        .identifier_types = {"identifier", "property_identifier",
                             "shorthand_property_identifier",
                             "shorthand_property_identifier_pattern"},
        .comment_types    = {"comment", "hash_bang_line"},
        .string_types     = {"string", "template_string", "string_fragment"},
        .keyword_types    = {"function", "class", "var", "let", "const",
                             "return", "if", "else", "for", "while",
                             "import", "export", "async", "await"},
    }},
    { TSLanguageID::TypeScript, {
        .identifier_types = {"identifier", "property_identifier",
                             "type_identifier", "shorthand_property_identifier"},
        .comment_types    = {"comment"},
        .string_types     = {"string", "template_string", "string_fragment"},
        .keyword_types    = {"function", "class", "interface", "type",
                             "enum", "namespace", "import", "export",
                             "const", "let", "var", "return", "async"},
    }},
    { TSLanguageID::TSX, {
        .identifier_types = {"identifier", "property_identifier",
                             "type_identifier"},
        .comment_types    = {"comment"},
        .string_types     = {"string", "template_string", "jsx_text"},
        .keyword_types    = {"function", "class", "return", "import", "export"},
    }},
    { TSLanguageID::C, {
        .identifier_types = {"identifier", "field_identifier",
                             "type_identifier", "statement_identifier"},
        .comment_types    = {"comment"},
        .string_types     = {"string_literal", "char_literal",
                             "string_content"},
        .keyword_types    = {"if", "else", "for", "while", "do", "return",
                             "struct", "enum", "typedef", "sizeof"},
    }},
    { TSLanguageID::Cpp, {
        .identifier_types = {"identifier", "field_identifier",
                             "type_identifier", "namespace_identifier",
                             "statement_identifier"},
        .comment_types    = {"comment"},
        .string_types     = {"string_literal", "char_literal",
                             "raw_string_literal", "string_content"},
        .keyword_types    = {"class", "namespace", "template", "typename",
                             "public", "private", "protected", "virtual",
                             "override", "const", "constexpr", "auto"},
    }},
    { TSLanguageID::Go, {
        .identifier_types = {"identifier", "field_identifier",
                             "type_identifier", "package_identifier",
                             "label_name"},
        .comment_types    = {"comment"},
        .string_types     = {"interpreted_string_literal",
                             "raw_string_literal", "rune_literal"},
        .keyword_types    = {"func", "type", "struct", "interface",
                             "package", "import", "var", "const",
                             "return", "go", "defer", "chan", "map"},
    }},
    { TSLanguageID::Rust, {
        .identifier_types = {"identifier", "field_identifier",
                             "type_identifier", "lifetime"},
        .comment_types    = {"line_comment", "block_comment",
                             "doc_comment"},
        .string_types     = {"string_literal", "char_literal",
                             "raw_string_literal"},
        .keyword_types    = {"fn", "struct", "enum", "trait", "impl",
                             "use", "mod", "pub", "let", "mut",
                             "return", "async", "await"},
    }},
    { TSLanguageID::Java, {
        .identifier_types = {"identifier", "type_identifier"},
        .comment_types    = {"line_comment", "block_comment"},
        .string_types     = {"string_literal", "character_literal",
                             "text_block"},
        .keyword_types    = {"class", "interface", "enum", "extends",
                             "implements", "import", "package",
                             "public", "private", "protected",
                             "static", "final", "return"},
    }},
    { TSLanguageID::CSharp, {
        .identifier_types = {"identifier"},
        .comment_types    = {"comment", "multiline_comment",
                             "documentation_comment"},
        .string_types     = {"string_literal", "verbatim_string_literal",
                             "interpolated_string_expression"},
        .keyword_types    = {"class", "namespace", "interface", "enum",
                             "using", "public", "private", "protected",
                             "static", "void", "return", "async", "await"},
    }},
    { TSLanguageID::Scala, {
        .identifier_types = {"identifier", "operator_identifier"},
        .comment_types    = {"comment", "multiline_comment",
                             "block_comment"},
        .string_types     = {"string", "multiline_string_literal"},
        .keyword_types    = {"class", "object", "trait", "def", "val",
                             "var", "case", "match", "import", "package",
                             "extends", "with", "override"},
    }},
    { TSLanguageID::Julia, {
        .identifier_types = {"identifier"},
        .comment_types    = {"line_comment", "block_comment"},
        .string_types     = {"string_literal", "command_literal",
                             "prefixed_string_literal"},
        .keyword_types    = {"function", "struct", "module", "import",
                             "using", "export", "return", "if", "else",
                             "for", "while", "macro"},
    }},
    { TSLanguageID::OCaml, {
        .identifier_types = {"value_name", "type_constructor_path",
                             "module_name", "label_name"},
        .comment_types    = {"comment", "line_number_directive"},
        .string_types     = {"string", "character"},
        .keyword_types    = {"let", "in", "fun", "function", "match",
                             "with", "type", "module", "open",
                             "struct", "sig", "end"},
    }},
    { TSLanguageID::PHP, {
        .identifier_types = {"name", "variable_name", "member_name"},
        .comment_types    = {"comment", "shell_command_expression"},
        .string_types     = {"string", "encapsed_string", "heredoc"},
        .keyword_types    = {"function", "class", "interface", "trait",
                             "namespace", "use", "return", "if", "else",
                             "foreach", "while", "echo"},
    }},
    { TSLanguageID::CSS, {
        .identifier_types = {"tag_name", "class_name", "id_name",
                             "property_name", "plain_value"},
        .comment_types    = {"comment"},
        .string_types     = {"string_value"},
        .keyword_types    = {},
    }},
    { TSLanguageID::JSON, {
        .identifier_types = {},  // JSON has no identifiers
        .comment_types    = {"comment"},
        .string_types     = {"string_content"},
        .keyword_types    = {"true", "false", "null"},
    }},
    { TSLanguageID::QL, {
        .identifier_types = {"simpleId", "qualId"},
        .comment_types    = {"line_comment", "block_comment"},
        .string_types     = {"string"},
        .keyword_types    = {"class", "predicate", "from", "where",
                             "select", "import", "module"},
    }},
};

// ---------------------------------------------------------------------------
// Impl struct — holds TSParser* and loaded grammar state
// ---------------------------------------------------------------------------
struct TreeSitterLexer::Impl {
    std::filesystem::path wasm_dir;
    LexerOptions          opts;
    UTF8Scanner           scanner;

    // Grammar availability cache: lang_id → wasm path (empty = not found)
    mutable std::unordered_map<uint8_t, std::filesystem::path> grammar_paths;

#if HAVE_TREE_SITTER
    mutable TSParser* parser = nullptr;
#endif

    explicit Impl(std::filesystem::path dir, LexerOptions o) noexcept
        : wasm_dir(std::move(dir)), opts(std::move(o))
    {
#if HAVE_TREE_SITTER
        parser = ts_parser_new();
#endif
        // Scan wasm_dir for available grammars
        probe_grammars();
    }

    ~Impl() noexcept {
#if HAVE_TREE_SITTER
        if (parser) ts_parser_delete(parser);
#endif
    }

    void probe_grammars() noexcept {
        if (wasm_dir.empty()) return;
        std::error_code ec;
        for (auto& g : ALL_GRAMMARS) {
            auto path = wasm_dir / g.wasm_file;
            if (std::filesystem::exists(path, ec)) {
                grammar_paths[static_cast<uint8_t>(g.id)] = path;
            }
        }
    }

    [[nodiscard]] bool has_grammar(TSLanguageID id) const noexcept {
        return grammar_paths.count(static_cast<uint8_t>(id)) > 0;
    }
};

// ---------------------------------------------------------------------------
// Static helpers
// ---------------------------------------------------------------------------
const std::vector<GrammarInfo>& TreeSitterLexer::all_grammars() noexcept {
    return ALL_GRAMMARS;
}

TSLanguageID TreeSitterLexer::detect_language(std::string_view filename) noexcept {
    auto dot = filename.rfind('.');
    if (dot == std::string_view::npos) return TSLanguageID::Unknown;
    auto ext = filename.substr(dot);  // includes the dot

    for (auto& g : ALL_GRAMMARS) {
        for (auto& e : g.extensions) {
            if (ext == e) return g.id;
        }
    }
    return TSLanguageID::Unknown;
}

Language TreeSitterLexer::to_base_language(TSLanguageID id) noexcept {
    switch (id) {
        case TSLanguageID::Python:     return Language::Python;
        case TSLanguageID::JavaScript: return Language::JavaScript;
        case TSLanguageID::TypeScript: return Language::TypeScript;
        case TSLanguageID::TSX:        return Language::TypeScript;
        case TSLanguageID::C:          return Language::C;
        case TSLanguageID::Cpp:        return Language::Cpp;
        default:                       return Language::Unknown;
    }
}

const NodeTypeMap& TreeSitterLexer::get_node_type_map(TSLanguageID id) noexcept {
    static const NodeTypeMap EMPTY{};
    auto it = NODE_TYPE_MAPS.find(id);
    return (it != NODE_TYPE_MAPS.end()) ? it->second : EMPTY;
}

TokenKind TreeSitterLexer::map_node_type(std::string_view node_type,
                                          TSLanguageID lang_id) noexcept {
    auto& map = get_node_type_map(lang_id);

    auto in_list = [&](const std::vector<std::string>& lst) {
        return std::find(lst.begin(), lst.end(), node_type) != lst.end();
    };

    if (in_list(map.identifier_types)) return TokenKind::Identifier;
    if (in_list(map.comment_types))    return TokenKind::Comment;
    if (in_list(map.string_types))     return TokenKind::StringLiteral;
    if (in_list(map.keyword_types))    return TokenKind::Keyword;

    return TokenKind::Invalid;  // skip this node
}

// ---------------------------------------------------------------------------
// Constructor / Destructor
// ---------------------------------------------------------------------------
TreeSitterLexer::TreeSitterLexer(std::filesystem::path wasm_dir,
                                   LexerOptions opts) noexcept
    : impl_(std::make_unique<Impl>(std::move(wasm_dir), std::move(opts)))
{}

TreeSitterLexer::~TreeSitterLexer() noexcept = default;
TreeSitterLexer::TreeSitterLexer(TreeSitterLexer&&) noexcept = default;
TreeSitterLexer& TreeSitterLexer::operator=(TreeSitterLexer&&) noexcept = default;

bool TreeSitterLexer::has_grammar(TSLanguageID lang) const noexcept {
    return impl_->has_grammar(lang);
}

std::vector<GrammarInfo> TreeSitterLexer::available_grammars() const noexcept {
    std::vector<GrammarInfo> result;
    for (auto& g : ALL_GRAMMARS) {
        if (impl_->has_grammar(g.id)) result.push_back(g);
    }
    return result;
}

// ---------------------------------------------------------------------------
// Token builder
// ---------------------------------------------------------------------------
Token TreeSitterLexer::make_token(std::string_view source,
                                   uint32_t start_byte, uint32_t end_byte,
                                   uint32_t start_row,  uint32_t start_col,
                                   TokenKind kind) const noexcept {
    Token tok;
    tok.kind = kind;
    tok.span = { start_byte, end_byte, start_row + 1, start_col + 1 };

    if (end_byte <= source.size()) {
        tok.raw  = std::string(source.substr(start_byte, end_byte - start_byte));
        tok.text = tok.raw;
    }

    if (impl_->opts.track_vietnamese && !tok.raw.empty()) {
        auto bytes = std::span<const uint8_t>(
            reinterpret_cast<const uint8_t*>(tok.raw.data()), tok.raw.size());
        tok.has_vietnamese = impl_->scanner.has_vietnamese_fast(bytes);

        if (tok.has_vietnamese) {
            if (kind == TokenKind::Identifier) {
                // Check if mixed (has both ASCII letters and Vietnamese)
                bool has_ascii = false;
                for (unsigned char c : tok.raw) {
                    if (c < 0x80 && std::isalpha(c)) { has_ascii = true; break; }
                }
                tok.kind = has_ascii ? TokenKind::MixedIdentifier
                                     : TokenKind::VietnameseIdentifier;
            } else if (kind == TokenKind::Comment) {
                tok.kind = TokenKind::VietnameseComment;
            }
        }
    }

    return tok;
}

// ---------------------------------------------------------------------------
// Tree walking
// ---------------------------------------------------------------------------
void TreeSitterLexer::walk_tree(const TSTree* tree,
                                 std::string_view source,
                                 TSLanguageID lang_id,
                                 std::vector<Token>& out) const noexcept {
#if HAVE_TREE_SITTER
    TSNode root = ts_tree_root_node(const_cast<TSTree*>(tree));

    // Iterative DFS using a stack (avoids recursion depth limits)
    std::vector<TSNode> stack;
    stack.reserve(256);
    stack.push_back(root);

    while (!stack.empty()) {
        TSNode node = stack.back();
        stack.pop_back();

        const char* type_cstr = ts_node_type(node);
        std::string_view node_type(type_cstr);

        TokenKind kind = map_node_type(node_type, lang_id);

        if (kind != TokenKind::Invalid) {
            uint32_t start_byte = ts_node_start_byte(node);
            uint32_t end_byte   = ts_node_end_byte(node);
            TSPoint  start_pt   = ts_node_start_point(node);

            // Only emit leaf nodes (no children) or comment/string nodes
            bool is_leaf = (ts_node_child_count(node) == 0);
            bool is_terminal = (kind == TokenKind::Comment ||
                                kind == TokenKind::StringLiteral ||
                                kind == TokenKind::VietnameseComment);

            if (is_leaf || is_terminal) {
                if (end_byte > start_byte) {
                    auto tok = make_token(source, start_byte, end_byte,
                                          start_pt.row, start_pt.column, kind);
                    out.push_back(std::move(tok));
                    // Don't recurse into terminal nodes
                    continue;
                }
            }
        }

        // Push children in reverse order (so leftmost is processed first)
        uint32_t child_count = ts_node_child_count(node);
        for (uint32_t i = child_count; i > 0; --i) {
            stack.push_back(ts_node_child(node, i - 1));
        }
    }
#else
    (void)tree; (void)source; (void)lang_id; (void)out;
#endif
}

// ---------------------------------------------------------------------------
// Core parse function
// ---------------------------------------------------------------------------
std::expected<std::vector<Token>, LexerError>
TreeSitterLexer::parse_with_tree_sitter(std::string_view source,
                                         TSLanguageID lang_id) const noexcept {
#if HAVE_TREE_SITTER
    // Load grammar — for native C API grammars (non-WASM)
    // For WASM grammars, tree-sitter-wasm binding is needed (handled externally).
    // Here we support the native C API path where grammar .so files are linked.
    //
    // WASM grammars are used in the VSCode extension (Node.js/browser).
    // For the CLI, grammars are loaded via dlopen or linked statically.
    //
    // If no native grammar found, return error to trigger fallback.
    auto it = impl_->grammar_paths.find(static_cast<uint8_t>(lang_id));
    if (it == impl_->grammar_paths.end()) {
        return std::unexpected(LexerError{0, "No grammar available for language", TokenKind::Invalid});
    }

    // Load grammar via dlopen and set language on parser
    {
        void* handle = dlopen(it->second.c_str(), RTLD_LAZY | RTLD_LOCAL);
        if (!handle) {
            return std::unexpected(LexerError{0, std::string("dlopen failed: ") + dlerror(), TokenKind::Invalid});
        }
        // Grammar entry point: tree_sitter_<lang>()
        // Find the function name from the .so filename stem
        std::string stem = it->second.stem().string(); // e.g. "python"
        std::string fn_name = "tree_sitter_" + stem;
        // Replace hyphens/dots with underscore
        for (auto& c : fn_name) if (c == '-' || c == '.') c = '_';
        using LangFn = const TSLanguage*(*)();
        auto lang_fn = reinterpret_cast<LangFn>(dlsym(handle, fn_name.c_str()));
        if (!lang_fn) {
            dlclose(handle);
            return std::unexpected(LexerError{0, "dlsym failed: " + fn_name, TokenKind::Invalid});
        }
        const TSLanguage* language = lang_fn();
        if (!ts_parser_set_language(impl_->parser, language)) {
            dlclose(handle);
            return std::unexpected(LexerError{0, "ts_parser_set_language failed", TokenKind::Invalid});
        }
        // Note: dlclose here is safe; libtree-sitter holds a reference
        dlclose(handle);
    }

    // Parse source
    TSTree* tree = ts_parser_parse_string(
        impl_->parser,
        nullptr,
        source.data(),
        static_cast<uint32_t>(source.size())
    );

    if (!tree) {
        return std::unexpected(LexerError{0, "tree-sitter parse failed", TokenKind::Invalid});
    }

    std::vector<Token> tokens;
    tokens.reserve(source.size() / 8);  // rough estimate
    walk_tree(tree, source, lang_id, tokens);
    ts_tree_delete(tree);

    return tokens;
#else
    (void)source; (void)lang_id;
    return std::unexpected(LexerError{0, "tree-sitter not compiled in", TokenKind::Invalid});
#endif
}

// ---------------------------------------------------------------------------
// Public tokenize
// ---------------------------------------------------------------------------
std::expected<std::vector<Token>, LexerError>
TreeSitterLexer::tokenize(std::string_view source,
                           std::string_view filename) const noexcept {
    auto lang_id = detect_language(filename);
    if (lang_id == TSLanguageID::Unknown || !impl_->has_grammar(lang_id)) {
        return std::unexpected(LexerError{0, "No tree-sitter grammar for file", TokenKind::Invalid});
    }
    return parse_with_tree_sitter(source, lang_id);
}

std::vector<Token>
TreeSitterLexer::extract_identifiers(std::string_view source,
                                      std::string_view filename) const noexcept {
    auto result = tokenize(source, filename);
    if (!result) return {};

    std::vector<Token> ids;
    for (auto& tok : *result) {
        if (tok.is_identifier()) ids.push_back(std::move(tok));
    }
    return ids;
}

// ---------------------------------------------------------------------------
// UnifiedLexer — tries tree-sitter, falls back to built-in
// ---------------------------------------------------------------------------
UnifiedLexer::UnifiedLexer(std::filesystem::path wasm_dir,
                             LexerOptions opts) noexcept
    : ts_lexer_(std::move(wasm_dir), opts)
    , fallback_lexer_(opts)
    , opts_(opts)
{}

std::expected<std::vector<Token>, LexerError>
UnifiedLexer::tokenize(std::string_view source,
                        std::string_view filename) const noexcept {
    auto lang_id = TreeSitterLexer::detect_language(filename);

    // Try tree-sitter first
    if (lang_id != TSLanguageID::Unknown && ts_lexer_.has_grammar(lang_id)) {
        auto result = ts_lexer_.tokenize(source, filename);
        if (result) return result;
        // Fall through on error
    }

    // Fallback to built-in lexer
    return fallback_lexer_.tokenize(source, filename);
}

std::vector<Token>
UnifiedLexer::extract_identifiers(std::string_view source,
                                   std::string_view filename) const noexcept {
    auto result = tokenize(source, filename);
    if (!result) return {};

    std::vector<Token> ids;
    for (auto& tok : *result) {
        if (tok.is_identifier()) ids.push_back(std::move(tok));
    }
    return ids;
}

bool UnifiedLexer::has_tree_sitter_support(std::string_view filename) const noexcept {
    auto lang_id = TreeSitterLexer::detect_language(filename);
    return lang_id != TSLanguageID::Unknown && ts_lexer_.has_grammar(lang_id);
}

std::vector<GrammarInfo> UnifiedLexer::available_grammars() const noexcept {
    return ts_lexer_.available_grammars();
}

} // namespace vietlint
