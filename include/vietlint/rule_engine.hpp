#pragma once
#include "vietlint/classifier.hpp"
#include "vietlint/encoding_detector.hpp"
#include <string>
#include <string_view>
#include <vector>
#include <memory>
#include <functional>
#include <unordered_map>
#include <expected>
#include <filesystem>
#include <span>
#include <variant>
#include <atomic>
#include <shared_mutex>

namespace vietlint {

/// Rule configuration from .vietlint.toml
struct RuleConfig {
    std::string             rule_id;
    bool                    enabled        = true;
    Severity                severity       = Severity::Warning;
    std::unordered_map<std::string, std::string> params;

    [[nodiscard]] std::string param(std::string_view key,
                                    std::string_view default_val = "") const noexcept;
    [[nodiscard]] bool param_bool(std::string_view key, bool default_val = false) const noexcept;
    [[nodiscard]] int  param_int(std::string_view key, int default_val = 0) const noexcept;
};

/// Context passed to each rule check
struct LintConfig; // forward declaration
struct RuleContext {
    std::string_view            source;
    std::string_view            filename;
    Language                    language;
    std::span<const Token>      tokens;
    const EncodingDetectionResult* encoding;
    const RuleConfig*           config;
    const LintConfig*           lint_config = nullptr;
};

/// A single rule result
using RuleResult = std::vector<ConventionViolation>;

/// Rule function signature
using RuleFn = std::function<RuleResult(const RuleContext&)>;

/// A registered rule
struct Rule {
    std::string id;
    std::string name;
    std::string description;
    std::string category;  ///< "naming", "comments", "encoding", "style"
    Severity    default_severity;
    RuleFn      check;
};

/// Diagnostic - combined violation with file context
struct Diagnostic {
    std::string          file;
    ConventionViolation  violation;
    std::string          rule_name;

    [[nodiscard]] std::string format() const noexcept;
};

/// Rule registry - thread-safe rule storage
class RuleRegistry {
public:
    RuleRegistry() noexcept;
    ~RuleRegistry() noexcept = default;

    RuleRegistry(const RuleRegistry&) = delete;
    RuleRegistry& operator=(const RuleRegistry&) = delete;

    /// Register a built-in or plugin rule
    void register_rule(Rule rule) noexcept;

    /// Get rule by ID
    [[nodiscard]] const Rule* find(std::string_view id) const noexcept;

    /// Get all registered rules
    [[nodiscard]] std::vector<std::string> rule_ids() const noexcept;

    /// Get rules by category
    [[nodiscard]] std::vector<const Rule*>
    rules_by_category(std::string_view category) const noexcept;

private:
    mutable std::shared_mutex mutex_;
    std::unordered_map<std::string, Rule> rules_;
};

/// Project-wide lint configuration
struct LintConfig {
    std::string                              project_root;
    std::vector<std::string>                 include_patterns;
    std::vector<std::string>                 exclude_patterns;
    std::unordered_map<std::string, RuleConfig> rule_configs;
    std::string                              target_encoding = "utf-8";
    bool                                     fail_on_warning = false;
    int                                      max_violations  = 0; ///< 0 = unlimited
    std::string                              model_path;      ///< ONNX model path
    std::vector<std::string>                 plugin_paths;

    /// Load from .vietlint.toml file
    [[nodiscard]] static std::expected<LintConfig, std::string>
    load_toml(const std::filesystem::path& path) noexcept;

    /// Write default config to path
    [[nodiscard]] static std::expected<void, std::string>
    write_default(const std::filesystem::path& path) noexcept;
};

/// The main rule execution engine
class RuleEngine {
public:
    explicit RuleEngine(LintConfig config) noexcept;
    ~RuleEngine() noexcept = default;

    RuleEngine(const RuleEngine&) = delete;
    RuleEngine& operator=(const RuleEngine&) = delete;

    /// Register all built-in rules
    void register_builtin_rules() noexcept;

    /// Run all enabled rules on a file
    [[nodiscard]] std::expected<std::vector<Diagnostic>, std::string>
    lint_file(const std::filesystem::path& file) const noexcept;

    /// Run all enabled rules on source text (no file I/O)
    [[nodiscard]] std::vector<Diagnostic>
    lint_source(std::string_view source, std::string_view filename) const noexcept;

    /// Run a specific rule
    [[nodiscard]] RuleResult
    run_rule(std::string_view rule_id, const RuleContext& ctx) const noexcept;

    /// Access registry for plugin registration
    [[nodiscard]] RuleRegistry& registry() noexcept { return registry_; }

    /// Get lint stats
    struct Stats {
        uint64_t files_scanned;
        uint64_t errors;
        uint64_t warnings;
        uint64_t infos;
    };
    [[nodiscard]] Stats stats() const noexcept;
    void reset_stats() noexcept;

private:
    LintConfig  config_;
    RuleRegistry registry_;
    Lexer        lexer_;
    VietnameseClassifier classifier_;
    EncodingDetector     encoding_detector_;

    mutable std::atomic<uint64_t> stat_files_{0};
    mutable std::atomic<uint64_t> stat_errors_{0};
    mutable std::atomic<uint64_t> stat_warnings_{0};
    mutable std::atomic<uint64_t> stat_infos_{0};

    // Built-in rule implementations
    static RuleResult rule_vl001_vietnamese_identifier(const RuleContext& ctx);
    static RuleResult rule_vl002_mixed_language_comment(const RuleContext& ctx);
    static RuleResult rule_vl003_encoding_declaration(const RuleContext& ctx);
    static RuleResult rule_vl004_transliteration(const RuleContext& ctx);
    static RuleResult rule_vl005_unicode_normalization(const RuleContext& ctx);
    static RuleResult rule_vl006_consistent_naming(const RuleContext& ctx);
    static RuleResult rule_vl007_viet_in_string_literal(const RuleContext& ctx);
    static RuleResult rule_vl008_inconsistent_transliteration(const RuleContext& ctx);
    static RuleResult rule_vl009_vietnamese_in_exception(const RuleContext& ctx);
    static RuleResult rule_vl010_missing_english_docstring(const RuleContext& ctx);
    static RuleResult rule_vl011_vietnamese_import_alias(const RuleContext& ctx);

    [[nodiscard]] bool is_rule_enabled(std::string_view rule_id) const noexcept;
    [[nodiscard]] Severity effective_severity(std::string_view rule_id,
                                               Severity default_sev) const noexcept;

    void record_diagnostic(const Diagnostic& d) const noexcept;
};

} // namespace vietlint
