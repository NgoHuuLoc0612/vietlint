#include "vietlint/emitter.hpp"
#include <sstream>
#include <iostream>
#include <algorithm>
#include <cstring>
#include <iomanip>

#if defined(_WIN32)
#  include <windows.h>
#else
#  include <dlfcn.h>
#endif

namespace vietlint {

// ---------------------------------------------------------------------------
// DiagnosticEmitter
// ---------------------------------------------------------------------------
DiagnosticEmitter::DiagnosticEmitter(EmitterOptions opts) noexcept
    : opts_(std::move(opts))
{
    if (opts_.color == ColorMode::Always) {
        use_color_ = true;
    } else if (opts_.color == ColorMode::Never) {
        use_color_ = false;
    } else {
        // Auto: detect if stdout is a TTY
#if defined(_WIN32)
        use_color_ = (GetFileType(GetStdHandle(STD_OUTPUT_HANDLE)) == FILE_TYPE_CHAR);
#else
        use_color_ = isatty(STDOUT_FILENO);
#endif
    }
}

std::string DiagnosticEmitter::severity_to_string(Severity s) noexcept {
    switch (s) {
        case Severity::Error:   return "error";
        case Severity::Warning: return "warning";
        case Severity::Info:    return "info";
        case Severity::Fatal:   return "fatal";
    }
    return "unknown";
}

int DiagnosticEmitter::severity_to_lsp_code(Severity s) noexcept {
    // LSP DiagnosticSeverity: Error=1, Warning=2, Information=3, Hint=4
    switch (s) {
        case Severity::Fatal:
        case Severity::Error:   return 1;
        case Severity::Warning: return 2;
        case Severity::Info:    return 3;
    }
    return 3;
}

std::string DiagnosticEmitter::color_severity(Severity sev) const noexcept {
    if (!use_color_) return severity_to_string(sev);
    switch (sev) {
        case Severity::Fatal:
        case Severity::Error:   return std::string(RED)    + "error"   + RESET;
        case Severity::Warning: return std::string(YELLOW) + "warning" + RESET;
        case Severity::Info:    return std::string(CYAN)   + "info"    + RESET;
    }
    return severity_to_string(sev);
}

std::string DiagnosticEmitter::escape_json(std::string_view s) noexcept {
    std::string out;
    out.reserve(s.size() + 8);
    for (unsigned char c : s) {
        switch (c) {
            case '"':  out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\n': out += "\\n";  break;
            case '\r': out += "\\r";  break;
            case '\t': out += "\\t";  break;
            default:
                if (c < 0x20) {
                    char buf[8]; std::snprintf(buf, sizeof(buf), "\\u%04X", c);
                    out += buf;
                } else {
                    out += static_cast<char>(c);
                }
        }
    }
    return out;
}

// ---------------------------------------------------------------------------
std::string DiagnosticEmitter::format_text(const Diagnostic& d,
                                             std::string_view source) const noexcept {
    std::ostringstream oss;
    const auto& v = d.violation;

    // file:line:col: severity [RULE_ID]: message
    if (use_color_) oss << BOLD;
    oss << d.file << ':' << v.span.line << ':' << v.span.col << ": ";
    if (use_color_) oss << RESET;

    oss << color_severity(v.severity);
    oss << ' ';
    if (use_color_) oss << DIM;
    oss << '[' << v.rule_id << ']';
    if (use_color_) oss << RESET;
    oss << ": " << v.message << '\n';

    // Show source line context
    if (opts_.show_source && !source.empty() && v.span.line > 0) {
        // Extract the offending line
        uint32_t target_line = v.span.line;
        uint32_t cur_line = 1;
        size_t line_start = 0;
        for (size_t i = 0; i < source.size(); ++i) {
            if (cur_line == target_line) { line_start = i; break; }
            if (source[i] == '\n') ++cur_line;
        }
        size_t line_end = source.find('\n', line_start);
        if (line_end == std::string_view::npos) line_end = source.size();
        std::string_view line_text = source.substr(line_start, line_end - line_start);

        if (use_color_) oss << DIM;
        oss << "    | " << line_text << '\n';
        if (use_color_) oss << RESET;

        // Caret indicator
        if (v.span.col > 0) {
            oss << "    | ";
            for (uint32_t i = 1; i < v.span.col; ++i) oss << ' ';
            if (use_color_) oss << GREEN;
            oss << '^';
            if (use_color_) oss << RESET;
            oss << '\n';
        }
    }

    // Show fix suggestions
    if (!v.fixes.empty()) {
        if (use_color_) oss << CYAN;
        oss << "    suggestion: ";
        for (size_t i = 0; i < v.fixes.size(); ++i) {
            if (i) oss << " or ";
            oss << v.fixes[i];
        }
        if (use_color_) oss << RESET;
        oss << '\n';
    }

    // Rule documentation URL
    if (opts_.show_url) {
        if (use_color_) oss << DIM;
        oss << "    see: " << opts_.base_url << v.rule_id << '\n';
        if (use_color_) oss << RESET;
    }

    return oss.str();
}

// ---------------------------------------------------------------------------
void DiagnosticEmitter::emit(std::span<const Diagnostic> diagnostics,
                              std::string_view source,
                              std::ostream& out) const noexcept {
    switch (opts_.format) {
        case DiagnosticFormat::JSON:
            out << emit_json(diagnostics) << '\n';
            return;
        case DiagnosticFormat::SARIF:
            out << emit_sarif(diagnostics) << '\n';
            return;
        case DiagnosticFormat::LSP:
            out << emit_lsp(diagnostics) << '\n';
            return;
        case DiagnosticFormat::GCC:
            for (const auto& d : diagnostics) {
                const auto& v = d.violation;
                out << d.file << ':' << v.span.line << ':' << v.span.col
                    << ": " << severity_to_string(v.severity)
                    << ": " << v.message << '\n';
            }
            return;
        case DiagnosticFormat::GitHubActions:
            for (const auto& d : diagnostics) {
                const auto& v = d.violation;
                std::string lvl = (v.severity == Severity::Error || v.severity == Severity::Fatal)
                                ? "error" : "warning";
                out << "::" << lvl << " file=" << d.file
                    << ",line=" << v.span.line
                    << ",col="  << v.span.col
                    << ",title=" << v.rule_id << "::"
                    << v.message << '\n';
            }
            return;
        default: // Text
            for (const auto& d : diagnostics)
                out << format_text(d, source);
            return;
    }
}

// ---------------------------------------------------------------------------
std::string DiagnosticEmitter::emit_json(std::span<const Diagnostic> diagnostics) const noexcept {
    std::ostringstream oss;
    oss << "[\n";
    for (size_t i = 0; i < diagnostics.size(); ++i) {
        const auto& d  = diagnostics[i];
        const auto& v  = d.violation;
        oss << "  {\n"
            << "    \"file\": \""    << escape_json(d.file)        << "\",\n"
            << "    \"rule\": \""    << escape_json(v.rule_id)     << "\",\n"
            << "    \"severity\": \"" << severity_to_string(v.severity) << "\",\n"
            << "    \"message\": \"" << escape_json(v.message)     << "\",\n"
            << "    \"line\": "      << v.span.line                << ",\n"
            << "    \"col\": "       << v.span.col                 << ",\n"
            << "    \"identifier\": \"" << escape_json(v.identifier) << "\",\n"
            << "    \"fixes\": [";
        for (size_t j = 0; j < v.fixes.size(); ++j) {
            if (j) oss << ", ";
            oss << '"' << escape_json(v.fixes[j]) << '"';
        }
        oss << "]\n  }";
        if (i + 1 < diagnostics.size()) oss << ',';
        oss << '\n';
    }
    oss << ']';
    return oss.str();
}

// ---------------------------------------------------------------------------
std::string DiagnosticEmitter::emit_sarif(std::span<const Diagnostic> diagnostics,
                                            std::string_view tool_version) const noexcept {
    std::ostringstream oss;
    oss << R"({
  "$schema": "https://json.schemastore.org/sarif-2.1.0.json",
  "version": "2.1.0",
  "runs": [
    {
      "tool": {
        "driver": {
          "name": "VietLint",
          "version": ")" << tool_version << R"(",
          "informationUri": "https://vietlint.io",
          "rules": []
        }
      },
      "results": [
)";

    for (size_t i = 0; i < diagnostics.size(); ++i) {
        const auto& d = diagnostics[i];
        const auto& v = d.violation;
        std::string level = (v.severity == Severity::Error || v.severity == Severity::Fatal)
                          ? "error" : (v.severity == Severity::Warning ? "warning" : "note");
        oss << "        {\n"
            << "          \"ruleId\": \""   << escape_json(v.rule_id)  << "\",\n"
            << "          \"level\": \""    << level                   << "\",\n"
            << "          \"message\": { \"text\": \"" << escape_json(v.message) << "\" },\n"
            << "          \"locations\": [{\n"
            << "            \"physicalLocation\": {\n"
            << "              \"artifactLocation\": { \"uri\": \"" << escape_json(d.file) << "\" },\n"
            << "              \"region\": {\n"
            << "                \"startLine\": "   << v.span.line << ",\n"
            << "                \"startColumn\": " << v.span.col  << "\n"
            << "              }\n"
            << "            }\n"
            << "          }]\n"
            << "        }";
        if (i + 1 < diagnostics.size()) oss << ',';
        oss << '\n';
    }

    oss << R"(      ]
    }
  ]
})";
    return oss.str();
}

// ---------------------------------------------------------------------------
std::string DiagnosticEmitter::emit_lsp(std::span<const Diagnostic> diagnostics) const noexcept {
    std::ostringstream oss;
    oss << "[\n";
    for (size_t i = 0; i < diagnostics.size(); ++i) {
        const auto& d = diagnostics[i];
        const auto& v = d.violation;
        // LSP lines/cols are 0-based
        uint32_t line = v.span.line > 0 ? v.span.line - 1 : 0;
        uint32_t col  = v.span.col  > 0 ? v.span.col  - 1 : 0;
        oss << "  {\n"
            << "    \"range\": {\n"
            << "      \"start\": { \"line\": " << line << ", \"character\": " << col << " },\n"
            << "      \"end\":   { \"line\": " << line << ", \"character\": " << (col + (uint32_t)v.identifier.size()) << " }\n"
            << "    },\n"
            << "    \"severity\": " << severity_to_lsp_code(v.severity) << ",\n"
            << "    \"code\": \""    << escape_json(v.rule_id)  << "\",\n"
            << "    \"source\": \"vietlint\",\n"
            << "    \"message\": \"" << escape_json(v.message) << "\"\n"
            << "  }";
        if (i + 1 < diagnostics.size()) oss << ',';
        oss << '\n';
    }
    oss << ']';
    return oss.str();
}

// ---------------------------------------------------------------------------
void DiagnosticEmitter::emit_summary(const RuleEngine::Stats& stats,
                                      std::ostream& out) const noexcept {
    if (use_color_) out << BOLD;
    out << "\n--- VietLint Summary ---\n";
    if (use_color_) out << RESET;

    out << "Files scanned: " << stats.files_scanned << '\n';

    if (stats.errors > 0) {
        if (use_color_) out << RED;
        out << "Errors:   " << stats.errors;
        if (use_color_) out << RESET;
        out << '\n';
    }
    if (stats.warnings > 0) {
        if (use_color_) out << YELLOW;
        out << "Warnings: " << stats.warnings;
        if (use_color_) out << RESET;
        out << '\n';
    }
    if (stats.infos > 0) {
        if (use_color_) out << CYAN;
        out << "Infos:    " << stats.infos;
        if (use_color_) out << RESET;
        out << '\n';
    }

    bool ok = (stats.errors == 0 && stats.warnings == 0);
    if (ok) {
        if (use_color_) out << GREEN;
        out << "✓ No violations found\n";
        if (use_color_) out << RESET;
    }
}

// ---------------------------------------------------------------------------
// PluginLoader
// ---------------------------------------------------------------------------
PluginLoader::~PluginLoader() noexcept {
    for (auto& p : plugins_) {
        if (p.handle) {
#if defined(_WIN32)
            FreeLibrary(static_cast<HMODULE>(p.handle));
#else
            dlclose(p.handle);
#endif
        }
    }
}

std::expected<PluginInfo, std::string>
PluginLoader::load(const std::filesystem::path& so_path, RuleRegistry& registry) noexcept {
    // Check if already loaded
    for (const auto& p : plugins_) {
        if (p.path == so_path.string())
            return std::unexpected("Plugin already loaded: " + so_path.string());
    }

    void* handle = nullptr;
#if defined(_WIN32)
    handle = static_cast<void*>(LoadLibraryW(so_path.wstring().c_str()));
    if (!handle) {
        DWORD err = GetLastError();
        char buf[256];
        FormatMessageA(FORMAT_MESSAGE_FROM_SYSTEM, nullptr, err, 0, buf, sizeof(buf), nullptr);
        return std::unexpected("Failed to load plugin " + so_path.string() + ": " + buf);
    }
    auto reg_fn = reinterpret_cast<PluginRegisterFn>(
        GetProcAddress(static_cast<HMODULE>(handle), "vietlint_register"));
    if (!reg_fn) {
        FreeLibrary(static_cast<HMODULE>(handle));
        return std::unexpected("Plugin missing 'vietlint_register' export: " + so_path.string());
    }
#else
    handle = dlopen(so_path.c_str(), RTLD_LAZY | RTLD_LOCAL);
    if (!handle) {
        return std::unexpected("Failed to load plugin " + so_path.string()
                               + ": " + std::string(dlerror()));
    }
    dlerror(); // clear error
    auto reg_fn = reinterpret_cast<PluginRegisterFn>(
        dlsym(handle, "vietlint_register"));
    const char* err = dlerror();
    if (err) {
        dlclose(handle);
        return std::unexpected("Plugin missing 'vietlint_register': " + std::string(err));
    }
#endif

    // Collect rule IDs before registration to track which ones the plugin adds
    auto before_ids = registry.rule_ids();

    PluginContext ctx{
        .api_version = PLUGIN_API_VERSION,
        .registry    = &registry,
        .user_data   = nullptr,
    };
    reg_fn(&ctx);

    auto after_ids = registry.rule_ids();
    std::vector<std::string> new_rules;
    for (const auto& id : after_ids) {
        bool was_before = std::find(before_ids.begin(), before_ids.end(), id) != before_ids.end();
        if (!was_before) new_rules.push_back(id);
    }

    PluginInfo info{
        .path             = so_path.string(),
        .name             = so_path.stem().string(),
        .version          = "unknown",
        .handle           = handle,
        .registered_rules = std::move(new_rules),
    };

    plugins_.push_back(info);
    return plugins_.back();
}

bool PluginLoader::unload(const std::filesystem::path& so_path) noexcept {
    auto it = std::find_if(plugins_.begin(), plugins_.end(),
        [&](const PluginInfo& p){ return p.path == so_path.string(); });
    if (it == plugins_.end()) return false;

    if (it->handle) {
#if defined(_WIN32)
        FreeLibrary(static_cast<HMODULE>(it->handle));
#else
        dlclose(it->handle);
#endif
    }
    plugins_.erase(it);
    return true;
}

void PluginLoader::load_directory(const std::filesystem::path& dir,
                                   RuleRegistry& registry) noexcept {
    if (!std::filesystem::exists(dir)) return;
    for (const auto& entry : std::filesystem::directory_iterator(dir)) {
        if (!entry.is_regular_file()) continue;
        auto ext = entry.path().extension().string();
#if defined(_WIN32)
        if (ext != ".dll") continue;
#elif defined(__APPLE__)
        if (ext != ".dylib" && ext != ".so") continue;
#else
        if (ext != ".so") continue;
#endif
        load(entry.path(), registry); // ignore individual load errors
    }
}

std::span<const PluginInfo> PluginLoader::loaded_plugins() const noexcept {
    return plugins_;
}

} // namespace vietlint
