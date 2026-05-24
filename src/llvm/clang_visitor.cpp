#include "vietlint/clang_visitor.hpp"

// Clang/LLVM headers — conditionally included
#if __has_include(<clang/AST/ASTContext.h>)
#  define HAVE_CLANG 1
#  include <clang/AST/ASTContext.h>
#  include <clang/AST/Decl.h>
#  include <clang/AST/DeclCXX.h>
#  include <clang/AST/DeclTemplate.h>
#  include <clang/AST/RecursiveASTVisitor.h>
#  include <clang/ASTMatchers/ASTMatchFinder.h>
#  include <clang/Basic/SourceManager.h>
#  include <clang/Basic/Diagnostic.h>
#  include <clang/Frontend/CompilerInstance.h>
#  include <clang/Frontend/FrontendAction.h>
#  include <clang/Frontend/FrontendActions.h>
#  include <clang/Tooling/CommonOptionsParser.h>
#  include <clang/Tooling/Tooling.h>
#  include <clang/Tooling/CompilationDatabase.h>
#  include <llvm/Support/CommandLine.h>
#  include <llvm/Support/raw_ostream.h>
#else
#  define HAVE_CLANG 0
#endif

#include <algorithm>
#include <cassert>
#include <cstring>
#include <sstream>

namespace vietlint::clang_integration {

// ---------------------------------------------------------------------------
// Stub implementations when Clang is not available at compile time
// ---------------------------------------------------------------------------
#if !HAVE_CLANG

// Stub: ASTContext reference cannot be null; use a dummy static object
static ::clang::ASTContext* _stub_ctx = nullptr;
VietnameseAstVisitor::VietnameseAstVisitor(::clang::ASTContext& ctx, AstVisitorOptions opts) noexcept
    : ctx_(ctx), opts_(std::move(opts)) {}

void VietnameseAstVisitor::visit_translation_unit(const ::clang::TranslationUnitDecl*) noexcept {}
std::span<const AstViolation> VietnameseAstVisitor::violations() const noexcept { return {}; }
void VietnameseAstVisitor::clear() noexcept {}
void VietnameseAstVisitor::check_named_decl(const ::clang::NamedDecl*, std::string_view) noexcept {}
void VietnameseAstVisitor::check_function(const ::clang::FunctionDecl*) noexcept {}
void VietnameseAstVisitor::check_variable(const ::clang::VarDecl*) noexcept {}
void VietnameseAstVisitor::check_record(const ::clang::RecordDecl*) noexcept {}
void VietnameseAstVisitor::check_field(const ::clang::FieldDecl*) noexcept {}
void VietnameseAstVisitor::check_enum(const ::clang::EnumDecl*) noexcept {}
void VietnameseAstVisitor::check_typedef(const ::clang::TypedefNameDecl*) noexcept {}
void VietnameseAstVisitor::check_namespace(const ::clang::NamespaceDecl*) noexcept {}
bool VietnameseAstVisitor::is_in_system_header(const ::clang::Decl*) const noexcept { return false; }
bool VietnameseAstVisitor::is_generated_file(std::string_view) const noexcept { return false; }
std::pair<uint32_t,uint32_t> VietnameseAstVisitor::get_location(const ::clang::Decl*) const noexcept { return {0,0}; }
std::string VietnameseAstVisitor::get_filename(const ::clang::Decl*) const noexcept { return ""; }
void VietnameseAstVisitor::emit(AstViolation) noexcept {}

VietnameseAstConsumer::VietnameseAstConsumer(::clang::ASTContext&, AstVisitorOptions opts, std::vector<AstViolation>* out) noexcept
    : opts_(std::move(opts)), out_(out) {}
void VietnameseAstConsumer::HandleTranslationUnit(::clang::ASTContext&) noexcept {}

std::unique_ptr<VietnameseAstConsumer>
VietnameseLintAction::CreateASTConsumer(::clang::CompilerInstance& ci, std::string_view) noexcept {
    return nullptr;
}

ClangAnalyzer::ClangAnalyzer(AstVisitorOptions opts) noexcept : opts_(std::move(opts)) {}

std::expected<std::vector<AstViolation>, std::string>
ClangAnalyzer::analyze(std::span<const std::filesystem::path>, const std::filesystem::path&) noexcept {
    return std::unexpected("Clang LibTooling not available. Build with LLVM/Clang support.");
}

std::expected<std::vector<AstViolation>, std::string>
ClangAnalyzer::analyze_string(std::string_view, std::string_view) noexcept {
    return std::unexpected("Clang LibTooling not available.");
}

Diagnostic ClangAnalyzer::to_diagnostic(const AstViolation& v) noexcept {
    Diagnostic d;
    d.file = v.file;
    d.rule_name = v.rule_id;
    d.violation.rule_id   = v.rule_id;
    d.violation.message   = v.message;
    d.violation.severity  = v.severity;
    d.violation.identifier = v.simple_name;
    d.violation.span = { v.line, v.col, 0, 0 };
    for (auto& s : v.suggestions) d.violation.fixes.push_back(s);
    return d;
}

std::vector<Diagnostic> ClangAnalyzer::to_diagnostics(std::span<const AstViolation> vs) noexcept {
    std::vector<Diagnostic> out;
    out.reserve(vs.size());
    for (auto& v : vs) out.push_back(to_diagnostic(v));
    return out;
}

void register_tidy_checks(void*) noexcept {}

#else // HAVE_CLANG

// ===========================================================================
// Full Clang implementation
// ===========================================================================

// ---------------------------------------------------------------------------
// RecursiveASTVisitor subclass — does the actual traversal
// ---------------------------------------------------------------------------
class VietRecursiveVisitor
    : public ::clang::RecursiveASTVisitor<VietRecursiveVisitor> {
public:
    explicit VietRecursiveVisitor(VietnameseAstVisitor& owner) noexcept
        : owner_(owner) {}

    bool shouldVisitTemplateInstantiations() const { return false; }
    bool shouldVisitImplicitCode() const { return false; }

    bool VisitFunctionDecl(::clang::FunctionDecl* decl) {
        owner_.check_function(decl);
        return true;
    }
    bool VisitVarDecl(::clang::VarDecl* decl) {
        owner_.check_variable(decl);
        return true;
    }
    bool VisitRecordDecl(::clang::RecordDecl* decl) {
        owner_.check_record(decl);
        return true;
    }
    bool VisitFieldDecl(::clang::FieldDecl* decl) {
        owner_.check_field(decl);
        return true;
    }
    bool VisitEnumDecl(::clang::EnumDecl* decl) {
        owner_.check_enum(decl);
        return true;
    }
    bool VisitTypedefNameDecl(::clang::TypedefNameDecl* decl) {
        owner_.check_typedef(decl);
        return true;
    }
    bool VisitNamespaceDecl(::clang::NamespaceDecl* decl) {
        owner_.check_namespace(decl);
        return true;
    }

private:
    VietnameseAstVisitor& owner_;
};

// ---------------------------------------------------------------------------
// VietnameseAstVisitor
// ---------------------------------------------------------------------------
VietnameseAstVisitor::VietnameseAstVisitor(::clang::ASTContext& ctx,
                                             AstVisitorOptions opts) noexcept
    : ctx_(ctx), opts_(std::move(opts))
{}

void VietnameseAstVisitor::clear() noexcept {
    violations_.clear();
    decls_checked_ = 0;
}

std::span<const AstViolation> VietnameseAstVisitor::violations() const noexcept {
    return violations_;
}

void VietnameseAstVisitor::visit_translation_unit(
    const ::clang::TranslationUnitDecl* tu) noexcept {
    VietRecursiveVisitor visitor(*this);
    visitor.TraverseDecl(const_cast<::clang::TranslationUnitDecl*>(tu));
}

// ---------------------------------------------------------------------------
// Location helpers
// ---------------------------------------------------------------------------
bool VietnameseAstVisitor::is_in_system_header(const ::clang::Decl* decl) const noexcept {
    if (!opts_.skip_system_headers) return false;
    auto& sm = ctx_.getSourceManager();
    auto loc = decl->getLocation();
    if (loc.isInvalid()) return true;
    return sm.isInSystemHeader(loc) || sm.isInExternCSystemHeader(loc);
}

bool VietnameseAstVisitor::is_generated_file(std::string_view filename) const noexcept {
    if (!opts_.skip_generated) return false;
    return filename.find(".pb.h")     != std::string_view::npos
        || filename.find(".pb.cc")    != std::string_view::npos
        || filename.find("generated") != std::string_view::npos
        || filename.find("_generated") != std::string_view::npos
        || filename.find("moc_")      != std::string_view::npos
        || filename.find("ui_")       != std::string_view::npos
        || filename.find(".g.cpp")    != std::string_view::npos;
}

std::pair<uint32_t,uint32_t>
VietnameseAstVisitor::get_location(const ::clang::Decl* decl) const noexcept {
    auto& sm = ctx_.getSourceManager();
    auto loc = sm.getSpellingLoc(decl->getLocation());
    if (loc.isInvalid()) return {0, 0};
    return {
        sm.getSpellingLineNumber(loc),
        sm.getSpellingColumnNumber(loc)
    };
}

std::string VietnameseAstVisitor::get_filename(const ::clang::Decl* decl) const noexcept {
    auto& sm = ctx_.getSourceManager();
    auto loc = sm.getSpellingLoc(decl->getLocation());
    if (loc.isInvalid()) return "<unknown>";
    auto file_id = sm.getFileID(loc);
    auto file_ref = sm.getFileEntryRefForID(file_id);
    if (!file_ref) return "<unknown>";
    return std::string(file_ref->getName());
}

// ---------------------------------------------------------------------------
void VietnameseAstVisitor::emit(AstViolation v) noexcept {
    violations_.push_back(std::move(v));
}

// ---------------------------------------------------------------------------
// Core name-checking logic
// ---------------------------------------------------------------------------
void VietnameseAstVisitor::check_named_decl(const ::clang::NamedDecl* decl,
                                              std::string_view kind) noexcept {
    if (!decl) return;
    if (is_in_system_header(decl)) return;

    auto name = decl->getNameAsString();
    if (name.empty()) return;

    ++decls_checked_;

    auto file = get_filename(decl);
    if (is_generated_file(file)) return;

    // Strip project root for cleaner output
    if (!opts_.project_root.empty() && file.rfind(opts_.project_root, 0) == 0)
        file = file.substr(opts_.project_root.size() + 1);

    auto raw_span = std::span<const uint8_t>(
        reinterpret_cast<const uint8_t*>(name.data()), name.size());

    if (!scanner_.has_vietnamese_fast(raw_span)) return;

    auto [line, col] = get_location(decl);

    // Use a minimal Token wrapper
    Token tok;
    tok.raw  = name;
    tok.text = name;
    tok.kind = TokenKind::Identifier;
    tok.has_vietnamese = true;
    tok.span = {0, 0, line, col};

    auto analysis = classifier_.classify(tok);
    if (analysis.cls == IdentifierClass::PureASCII) return;

    AstViolation v;
    v.qualified_name = decl->getQualifiedNameAsString();
    v.simple_name    = name;
    v.decl_kind      = std::string(kind);
    v.file           = file;
    v.line           = line;
    v.col            = col;
    v.severity       = Severity::Warning;

    if (analysis.cls == IdentifierClass::MixedVietnamese) {
        v.rule_id = "VL-AST-002";
        v.message = std::string(kind) + " '" + name + "' mixes Vietnamese and ASCII characters. "
                    "Found in " + v.qualified_name;
        v.severity = Severity::Warning;
    } else {
        v.rule_id = "VL-AST-001";
        v.message = std::string(kind) + " '" + name + "' contains Vietnamese characters. "
                    "Use ASCII identifiers for portability.";
        v.severity = Severity::Warning;
    }

    // Suggestion: ASCII equivalent
    v.suggestions.push_back(classifier_.suggest_ascii_equivalent(name));

    emit(std::move(v));
}

// ---------------------------------------------------------------------------
void VietnameseAstVisitor::check_function(const ::clang::FunctionDecl* decl) noexcept {
    if (!opts_.check_function_names) return;
    // Skip operator overloads and conversion functions
    if (decl->isOverloadedOperator()) return;
    if (auto* md = ::clang::dyn_cast<::clang::CXXMethodDecl>(decl)) {
        if (md->isImplicit()) return;
    }
    check_named_decl(decl, "function");

    // Also check parameter names
    if (opts_.check_parameter_names) {
        for (auto* param : decl->parameters()) {
            if (!param->getName().empty())
                check_named_decl(param, "parameter");
        }
    }
}

void VietnameseAstVisitor::check_variable(const ::clang::VarDecl* decl) noexcept {
    if (!opts_.check_variable_names) return;
    if (decl->isImplicit()) return;
    // Skip compiler-generated temporaries
    auto name = decl->getNameAsString();
    if (name.empty() || name[0] == '_' && name.size() > 2 && name[1] == '_') return;
    check_named_decl(decl, "variable");
}

void VietnameseAstVisitor::check_record(const ::clang::RecordDecl* decl) noexcept {
    if (!opts_.check_class_names) return;
    if (decl->isImplicit() || decl->isAnonymousStructOrUnion()) return;
    const char* kind = decl->isClass() ? "class"
                     : decl->isStruct() ? "struct" : "union";
    check_named_decl(decl, kind);
}

void VietnameseAstVisitor::check_field(const ::clang::FieldDecl* decl) noexcept {
    if (!opts_.check_field_names) return;
    if (decl->isAnonymousStructOrUnion()) return;
    check_named_decl(decl, "field");
}

void VietnameseAstVisitor::check_enum(const ::clang::EnumDecl* decl) noexcept {
    if (!opts_.check_enum_names) return;
    check_named_decl(decl, "enum");
    for (auto* ec : decl->enumerators())
        check_named_decl(ec, "enumerator");
}

void VietnameseAstVisitor::check_typedef(const ::clang::TypedefNameDecl* decl) noexcept {
    if (!opts_.check_typedef_names) return;
    if (decl->isImplicit()) return;
    check_named_decl(decl, "typedef");
}

void VietnameseAstVisitor::check_namespace(const ::clang::NamespaceDecl* decl) noexcept {
    if (!opts_.check_namespace_names) return;
    if (decl->isAnonymousNamespace()) return;
    check_named_decl(decl, "namespace");
}

// ---------------------------------------------------------------------------
// VietnameseAstConsumer
// ---------------------------------------------------------------------------
VietnameseAstConsumer::VietnameseAstConsumer(::clang::ASTContext& ctx,
                                               AstVisitorOptions opts,
                                               std::vector<AstViolation>* out) noexcept
    : opts_(std::move(opts)), out_(out)
{}

void VietnameseAstConsumer::HandleTranslationUnit(::clang::ASTContext& ctx) noexcept {
    VietnameseAstVisitor visitor(ctx, opts_);
    visitor.visit_translation_unit(ctx.getTranslationUnitDecl());
    auto vs = visitor.violations();
    out_->insert(out_->end(), vs.begin(), vs.end());
}

// ---------------------------------------------------------------------------
// VietnameseLintAction
// ---------------------------------------------------------------------------
std::unique_ptr<VietnameseAstConsumer>
VietnameseLintAction::CreateASTConsumer(::clang::CompilerInstance& ci,
                                          std::string_view) noexcept {
    return std::make_unique<VietnameseAstConsumer>(
        ci.getASTContext(), opts_, out_);
}

// ---------------------------------------------------------------------------
// ASTFrontendAction wrapper for ClangTool
// ---------------------------------------------------------------------------
class VietLintFrontendAction : public ::clang::ASTFrontendAction {
public:
    VietLintFrontendAction(AstVisitorOptions opts, std::vector<AstViolation>* out)
        : opts_(std::move(opts)), out_(out) {}

    std::unique_ptr<::clang::ASTConsumer>
    CreateASTConsumer(::clang::CompilerInstance& ci, ::llvm::StringRef file) override {
        return std::make_unique<VietnameseAstConsumer>(
            ci.getASTContext(), opts_, out_);
    }

private:
    AstVisitorOptions      opts_;
    std::vector<AstViolation>* out_;
};

// Factory callable for ClangTool
struct VietLintActionFactory : public ::clang::tooling::FrontendActionFactory {
    AstVisitorOptions          opts;
    std::vector<AstViolation>* out;

    VietLintActionFactory(AstVisitorOptions o, std::vector<AstViolation>* out_ptr)
        : opts(std::move(o)), out(out_ptr) {}

    std::unique_ptr<::clang::FrontendAction> create() override {
        return std::make_unique<VietLintFrontendAction>(opts, out);
    }
};

// ---------------------------------------------------------------------------
// ClangAnalyzer
// ---------------------------------------------------------------------------
ClangAnalyzer::ClangAnalyzer(AstVisitorOptions opts) noexcept
    : opts_(std::move(opts)) {}

std::expected<std::vector<AstViolation>, std::string>
ClangAnalyzer::analyze(std::span<const std::filesystem::path> files,
                        const std::filesystem::path& compile_db_dir) noexcept {
    if (files.empty()) return std::vector<AstViolation>{};

    std::string err_msg;
    auto db = ::clang::tooling::CompilationDatabase::autoDetectFromDirectory(
        compile_db_dir.string(), err_msg);
    if (!db) {
        // Fallback: fixed compilation database with basic C++20 flags
        std::vector<std::string> sources;
        sources.reserve(files.size());
        for (auto& f : files) sources.push_back(f.string());

        db = std::make_unique<::clang::tooling::FixedCompilationDatabase>(
            compile_db_dir.string(),
            std::vector<std::string>{
                "-std=c++20", "-Wall", "-x", "c++"
            });
    }

    std::vector<std::string> source_paths;
    source_paths.reserve(files.size());
    for (const auto& f : files) source_paths.push_back(f.string());

    ::clang::tooling::ClangTool tool(*db, source_paths);

    // Suppress compiler diagnostics — we only care about our violations
    tool.setDiagnosticConsumer(new ::clang::IgnoringDiagConsumer());

    std::vector<AstViolation> violations;
    auto factory = std::make_unique<VietLintActionFactory>(opts_, &violations);

    int result = tool.run(factory.get());
    if (result != 0 && violations.empty()) {
        return std::unexpected("ClangTool failed with code " + std::to_string(result));
    }

    return violations;
}

std::expected<std::vector<AstViolation>, std::string>
ClangAnalyzer::analyze_string(std::string_view source,
                               std::string_view filename) noexcept {
    std::vector<AstViolation> violations;

    // Build a virtual filesystem with the source string
    auto action = std::make_unique<VietLintFrontendAction>(opts_, &violations);

    bool ok = ::clang::tooling::runToolOnCodeWithArgs(
        std::move(action),
        ::llvm::StringRef(source.data(), source.size()),
        /* args */ std::vector<std::string>{"-std=c++20", "-Wall"},
        ::llvm::StringRef(filename.data(), filename.size())
    );

    if (!ok) {
        return std::unexpected(
            "Failed to parse " + std::string(filename));
    }
    return violations;
}

Diagnostic ClangAnalyzer::to_diagnostic(const AstViolation& v) noexcept {
    Diagnostic d;
    d.file      = v.file;
    d.rule_name = v.rule_id;
    d.violation.rule_id    = v.rule_id;
    d.violation.message    = v.message;
    d.violation.severity   = v.severity;
    d.violation.identifier = v.simple_name;
    d.violation.span = {
        /*start=*/0, /*end=*/0,
        v.line, v.col
    };
    d.violation.fixes = v.suggestions;
    return d;
}

std::vector<Diagnostic>
ClangAnalyzer::to_diagnostics(std::span<const AstViolation> vs) noexcept {
    std::vector<Diagnostic> out;
    out.reserve(vs.size());
    for (auto& v : vs) out.push_back(to_diagnostic(v));
    return out;
}

} // namespace vietlint::clang_integration

// ---------------------------------------------------------------------------
// Clang-Tidy check registration (outside namespace to avoid symbol conflicts)
// ---------------------------------------------------------------------------
#if __has_include(<clang-tidy/ClangTidyCheck.h>)
#  include <clang-tidy/ClangTidyCheck.h>
#  include <clang-tidy/ClangTidyModule.h>
#  include <clang-tidy/ClangTidyModuleRegistry.h>
#  define HAVE_CLANG_TIDY 1
#endif

namespace vietlint::clang_integration {

#if defined(HAVE_CLANG_TIDY)

class VietNamingCheck : public ::clang::tidy::ClangTidyCheck {
public:
    VietNamingCheck(::llvm::StringRef name,
                    ::clang::tidy::ClangTidyContext* ctx)
        : ClangTidyCheck(name, ctx) {}

    void registerMatchers(::clang::ast_matchers::MatchFinder* finder) override {
        using namespace ::clang::ast_matchers;
        finder->addMatcher(namedDecl(isExpansionInMainFile()).bind("decl"), this);
    }

    void check(const ::clang::ast_matchers::MatchFinder::MatchResult& result) override {
        const auto* decl = result.Nodes.getNodeAs<::clang::NamedDecl>("decl");
        if (!decl) return;

        auto name = decl->getNameAsString();
        if (name.empty()) return;

        UTF8Scanner scanner;
        auto raw = std::span<const uint8_t>(
            reinterpret_cast<const uint8_t*>(name.data()), name.size());
        if (!scanner.has_vietnamese_fast(raw)) return;

        diag(decl->getLocation(),
             "VietLint [VL-AST-001]: identifier '%0' contains Vietnamese characters. "
             "Consider using ASCII equivalents.")
            << name;
    }
};

class VietLintModule : public ::clang::tidy::ClangTidyModule {
public:
    void addCheckFactories(::clang::tidy::ClangTidyCheckFactories& factories) override {
        factories.registerCheck<VietNamingCheck>("vietlint-naming-convention");
    }
};

static ::clang::tidy::ClangTidyModuleRegistry::Add<VietLintModule>
    X("vietlint-module", "VietLint Vietnamese naming convention checks");

#endif // HAVE_CLANG_TIDY

void register_tidy_checks(void* /*check_factories*/) noexcept {
    // Registration happens via static initializer above when linked
}

#endif // HAVE_CLANG

} // namespace vietlint::clang_integration
