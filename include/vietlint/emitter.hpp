#pragma once
#include "vietlint/rule_engine.hpp"
#include <string>
#include <vector>
#include <ostream>
#include <memory>
#include <filesystem>
#include <expected>
#include <functional>
#include <span>

namespace vietlint {

/// Output format for diagnostics
enum class DiagnosticFormat : uint8_t {
    Text,         ///< Human-readable terminal output
    JSON,         ///< JSON array of violation objects
    SARIF,        ///< SARIF 2.1.0 (GitHub Code Scanning compatible)
    LSP,          ///< LSP publishDiagnostics format
    GCC,          ///< file:line:col: severity: message
    GitHubActions,///< ::error file=...,line=...,col=...,title=...:message
};

/// ANSI color support
enum class ColorMode : uint8_t { Auto, Always, Never };

/// Emitter options
struct EmitterOptions {
    DiagnosticFormat format      = DiagnosticFormat::Text;
    ColorMode        color       = ColorMode::Auto;
    bool             show_source = true;    ///< Print source line excerpt
    bool             show_url    = true;    ///< Print rule documentation URL
    int              context_lines = 2;     ///< Lines of context around violation
    std::string      base_url    = "https://vietlint.io/rules/";
};

/// Diagnostic emitter - formats and outputs diagnostics
class DiagnosticEmitter {
public:
    explicit DiagnosticEmitter(EmitterOptions opts = {}) noexcept;
    ~DiagnosticEmitter() noexcept = default;

    DiagnosticEmitter(const DiagnosticEmitter&) = delete;
    DiagnosticEmitter& operator=(const DiagnosticEmitter&) = delete;

    /// Emit a batch of diagnostics to a stream
    void emit(std::span<const Diagnostic> diagnostics,
              std::string_view source,
              std::ostream& out) const noexcept;

    /// Emit as JSON to string
    [[nodiscard]] std::string emit_json(std::span<const Diagnostic> diagnostics) const noexcept;

    /// Emit as SARIF 2.1.0 to string
    [[nodiscard]] std::string emit_sarif(std::span<const Diagnostic> diagnostics,
                                          std::string_view tool_version = "1.0.0") const noexcept;

    /// Emit LSP Diagnostic array as JSON
    [[nodiscard]] std::string emit_lsp(std::span<const Diagnostic> diagnostics) const noexcept;

    /// Format a single diagnostic as text with optional ANSI colors
    [[nodiscard]] std::string format_text(const Diagnostic& d,
                                           std::string_view source = "") const noexcept;

    /// Print summary line: "N errors, M warnings in K files"
    void emit_summary(const RuleEngine::Stats& stats, std::ostream& out) const noexcept;

    // Public static helpers used by LSP server
    [[nodiscard]] static std::string severity_to_string(Severity s) noexcept;
    [[nodiscard]] static int severity_to_lsp_code(Severity s) noexcept;

private:
    EmitterOptions opts_;
    bool           use_color_;

    static constexpr const char* RESET  = "\033[0m";
    static constexpr const char* RED    = "\033[1;31m";
    static constexpr const char* YELLOW = "\033[1;33m";
    static constexpr const char* CYAN   = "\033[0;36m";
    static constexpr const char* GREEN  = "\033[1;32m";
    static constexpr const char* BOLD   = "\033[1m";
    static constexpr const char* DIM    = "\033[2m";

    [[nodiscard]] std::string color_severity(Severity sev) const noexcept;
    [[nodiscard]] static std::string escape_json(std::string_view s) noexcept;
};

// ---------------------------------------------------------------------------
// Plugin Loader
// ---------------------------------------------------------------------------

/// Plugin API version
constexpr uint32_t PLUGIN_API_VERSION = 0x00010000; // 1.0.0

/// Plugin entry point signature
/// Plugins must export: extern "C" void vietlint_register(PluginContext*)
struct PluginContext {
    uint32_t      api_version;
    RuleRegistry* registry;
    void*         user_data;
};

using PluginRegisterFn = void(*)(PluginContext*);

/// Information about a loaded plugin
struct PluginInfo {
    std::string   path;
    std::string   name;
    std::string   version;
    void*         handle;  ///< dlopen/LoadLibrary handle
    std::vector<std::string> registered_rules;
};

/// Plugin loader - manages shared library rule plugins
class PluginLoader {
public:
    explicit PluginLoader() noexcept = default;
    ~PluginLoader() noexcept;

    PluginLoader(const PluginLoader&) = delete;
    PluginLoader& operator=(const PluginLoader&) = delete;

    /// Load a plugin from shared library path
    [[nodiscard]] std::expected<PluginInfo, std::string>
    load(const std::filesystem::path& so_path, RuleRegistry& registry) noexcept;

    /// Unload a plugin by path (removes its rules from registry)
    [[nodiscard]] bool unload(const std::filesystem::path& so_path) noexcept;

    /// Load all plugins from directory
    void load_directory(const std::filesystem::path& dir, RuleRegistry& registry) noexcept;

    /// List all loaded plugins
    [[nodiscard]] std::span<const PluginInfo> loaded_plugins() const noexcept;

private:
    std::vector<PluginInfo> plugins_;
};

} // namespace vietlint
