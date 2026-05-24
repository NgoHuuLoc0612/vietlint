#pragma once
#include "vietlint/rule_engine.hpp"
#if __has_include(<clang/AST/ASTConsumer.h>)
#  include <clang/AST/ASTConsumer.h>
#endif
#include "vietlint/utf8_scanner.hpp"
#include <string>
#include <string_view>
#include <vector>
#include <memory>
#include <expected>
#include <functional>
#include <unordered_map>
#include <filesystem>
#include <span>

// Forward-declare Clang types to keep header lean
namespace clang {
    class ASTContext;
    class Decl;
    class NamedDecl;
    class FunctionDecl;
    class VarDecl;
    class RecordDecl;
    class FieldDecl;
    class EnumDecl;
    class EnumConstantDecl;
    class TypedefNameDecl;
    class NamespaceDecl;
    class CXXMethodDecl;
    class ParmVarDecl;
    class TranslationUnitDecl;
    class SourceManager;
    class DiagnosticsEngine;
    class CompilerInstance;
    namespace tooling {
        class ClangTool;
        class CompilationDatabase;
    }
}

#if __has_include(<clang/AST/ASTConsumer.h>)
namespace vietlint::clang_integration {

/// Result from AST analysis of a single declaration
struct AstViolation {
    std::string    qualified_name;   ///< Fully qualified declaration name
    std::string    simple_name;      ///< Unqualified name
    std::string    decl_kind;        ///< "function","variable","class","field","param",...
    std::string    file;
    uint32_t       line;
    uint32_t       col;
    std::string    rule_id;
    std::string    message;
    Severity       severity;
    std::vector<std::string> suggestions;
};

/// Visitor options
struct AstVisitorOptions {
    bool check_function_names  = true;
    bool check_variable_names  = true;
    bool check_parameter_names = true;
    bool check_class_names     = true;
    bool check_field_names     = true;
    bool check_enum_names      = true;
    bool check_namespace_names = true;
    bool check_typedef_names   = true;
    bool check_comments        = true;
    bool skip_system_headers   = true;
    bool skip_generated        = true;   ///< Skip __generated__ / .pb.h files
    std::string project_root;
};

/// The Clang AST visitor that detects Vietnamese naming violations
/// Traverses the full C/C++ AST and checks every NamedDecl.
class VietnameseAstVisitor {
public:
    explicit VietnameseAstVisitor(::clang::ASTContext& ctx,
                                   AstVisitorOptions opts = {}) noexcept;
    ~VietnameseAstVisitor() noexcept = default;

    VietnameseAstVisitor(const VietnameseAstVisitor&) = delete;
    VietnameseAstVisitor& operator=(const VietnameseAstVisitor&) = delete;

    /// Visit the translation unit — call this to start analysis
    void visit_translation_unit(const ::clang::TranslationUnitDecl* tu) noexcept;

    /// Access collected violations after visit
    [[nodiscard]] std::span<const AstViolation> violations() const noexcept;

    /// Clear results for reuse
    void clear() noexcept;

    /// Total declarations checked
    [[nodiscard]] uint64_t decls_checked() const noexcept { return decls_checked_; }

private:
    ::clang::ASTContext& ctx_;
    AstVisitorOptions    opts_;
    UTF8Scanner          scanner_;
    VietnameseClassifier classifier_;
    std::vector<AstViolation> violations_;
    uint64_t decls_checked_ = 0;

    // Per-decl-kind checkers
    void check_named_decl(const ::clang::NamedDecl* decl, std::string_view kind) noexcept;
    void check_function(const ::clang::FunctionDecl* decl) noexcept;
    void check_variable(const ::clang::VarDecl* decl) noexcept;
    void check_record(const ::clang::RecordDecl* decl) noexcept;
    void check_field(const ::clang::FieldDecl* decl) noexcept;
    void check_enum(const ::clang::EnumDecl* decl) noexcept;
    void check_typedef(const ::clang::TypedefNameDecl* decl) noexcept;
    void check_namespace(const ::clang::NamespaceDecl* decl) noexcept;

    // Helpers
    [[nodiscard]] bool is_in_system_header(const ::clang::Decl* decl) const noexcept;
    [[nodiscard]] bool is_generated_file(std::string_view filename) const noexcept;
    [[nodiscard]] std::pair<uint32_t, uint32_t>
    get_location(const ::clang::Decl* decl) const noexcept;
    [[nodiscard]] std::string get_filename(const ::clang::Decl* decl) const noexcept;

    void emit(AstViolation v) noexcept;
    friend class VietRecursiveVisitor;
};

/// AST consumer that drives the visitor
class VietnameseAstConsumer : public ::clang::ASTConsumer {
public:
    explicit VietnameseAstConsumer(::clang::ASTContext& ctx,
                                    AstVisitorOptions opts,
                                    std::vector<AstViolation>* out) noexcept;

    void HandleTranslationUnit(::clang::ASTContext& ctx) noexcept override;

private:
    AstVisitorOptions      opts_;
    std::vector<AstViolation>* out_;
};

/// Clang frontend action that creates the AST consumer
class VietnameseLintAction {
public:
    explicit VietnameseLintAction(AstVisitorOptions opts,
                                   std::vector<AstViolation>* out) noexcept
        : opts_(std::move(opts)), out_(out) {}

    // Called by Clang to create an AST consumer for this TU
    std::unique_ptr<VietnameseAstConsumer>
    CreateASTConsumer(::clang::CompilerInstance& ci, std::string_view file) noexcept;

private:
    AstVisitorOptions      opts_;
    std::vector<AstViolation>* out_;
};

/// High-level ClangTool wrapper
/// Usage:
///   ClangAnalyzer analyzer(opts);
///   auto violations = analyzer.analyze({"src/main.cpp"}, compile_db_path);
class ClangAnalyzer {
public:
    explicit ClangAnalyzer(AstVisitorOptions opts = {}) noexcept;
    ~ClangAnalyzer() noexcept = default;

    ClangAnalyzer(const ClangAnalyzer&) = delete;
    ClangAnalyzer& operator=(const ClangAnalyzer&) = delete;

    /// Analyze a list of source files using a compile_commands.json directory
    [[nodiscard]] std::expected<std::vector<AstViolation>, std::string>
    analyze(std::span<const std::filesystem::path> files,
            const std::filesystem::path& compile_db_dir) noexcept;

    /// Analyze a single source string without a compile DB (uses default flags)
    [[nodiscard]] std::expected<std::vector<AstViolation>, std::string>
    analyze_string(std::string_view source,
                   std::string_view filename = "input.cpp") noexcept;

    /// Convert AstViolation to vietlint::Diagnostic
    [[nodiscard]] static Diagnostic to_diagnostic(const AstViolation& v) noexcept;

    /// Convert batch
    [[nodiscard]] static std::vector<Diagnostic>
    to_diagnostics(std::span<const AstViolation> vs) noexcept;

private:
    AstVisitorOptions opts_;
};

// ---------------------------------------------------------------------------
// Custom Clang-Tidy check registration
// Registers VietLint as a clang-tidy module so it runs alongside other checks
// ---------------------------------------------------------------------------

/// Clang-Tidy check name
constexpr std::string_view TIDY_CHECK_NAME = "vietlint-naming-convention";

/// Register VietLint checks into a clang-tidy CheckFactories map
/// Call this from your clang-tidy plugin registration function:
///   extern "C" void registerChecks(clang::tidy::CheckFactories& factories) {
///       vietlint::clang_integration::register_tidy_checks(factories);
///   }
void register_tidy_checks(void* check_factories) noexcept;

} // namespace vietlint::clang_integration
#endif // __has_include(<clang/AST/ASTConsumer.h>)
