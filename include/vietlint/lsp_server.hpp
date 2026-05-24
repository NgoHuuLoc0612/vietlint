#pragma once
#include "vietlint/rule_engine.hpp"
#include "vietlint/emitter.hpp"
#include <string>
#include <string_view>
#include <vector>
#include <unordered_map>
#include <memory>
#include <functional>
#include <expected>
#include <variant>
#include <optional>
#include <thread>
#include <mutex>
#include <condition_variable>
#include <atomic>
#include <queue>
#include <iostream>

namespace vietlint::lsp {

// ---------------------------------------------------------------------------
// Minimal JSON value type (no external dependency)
// ---------------------------------------------------------------------------
struct JsonNull {};
using JsonValue = std::variant<
    JsonNull,
    bool,
    int64_t,
    double,
    std::string,
    std::vector<std::shared_ptr<struct JsonArray>>,
    std::shared_ptr<struct JsonObject>
>;

struct JsonArray  { std::vector<JsonValue> items; };
struct JsonObject { std::unordered_map<std::string, JsonValue> fields; };

/// Full JSON parser/serializer
class Json {
public:
    [[nodiscard]] static std::expected<JsonValue, std::string> parse(std::string_view s) noexcept;
    [[nodiscard]] static std::string serialize(const JsonValue& v, bool pretty = false) noexcept;

    // Accessors
    [[nodiscard]] static const JsonObject* as_object(const JsonValue& v) noexcept;
    [[nodiscard]] static const JsonArray*  as_array (const JsonValue& v) noexcept;
    [[nodiscard]] static const std::string* as_string(const JsonValue& v) noexcept;
    [[nodiscard]] static std::optional<int64_t>  as_int(const JsonValue& v) noexcept;
    [[nodiscard]] static std::optional<double>   as_double(const JsonValue& v) noexcept;
    [[nodiscard]] static std::optional<bool>     as_bool(const JsonValue& v) noexcept;

    [[nodiscard]] static const JsonValue* get(const JsonObject& obj, std::string_view key) noexcept;

    // Builders
    [[nodiscard]] static JsonValue make_object(
        std::initializer_list<std::pair<std::string, JsonValue>> fields) noexcept;
    [[nodiscard]] static JsonValue make_array(std::vector<JsonValue> items) noexcept;
    [[nodiscard]] static JsonValue make_string(std::string s) noexcept;
    [[nodiscard]] static JsonValue make_int(int64_t v) noexcept;
    [[nodiscard]] static JsonValue make_null() noexcept;
    [[nodiscard]] static JsonValue make_bool(bool v) noexcept;
};

// ---------------------------------------------------------------------------
// JSON-RPC types
// ---------------------------------------------------------------------------
struct RpcRequest {
    JsonValue          id;      ///< string | int | null
    std::string        method;
    std::optional<JsonValue> params;
    bool               is_notification; ///< no response expected
};

struct RpcResponse {
    JsonValue          id;
    std::optional<JsonValue> result;
    std::optional<JsonValue> error;
};

struct RpcError {
    int         code;
    std::string message;
    std::optional<JsonValue> data;
};

// Standard JSON-RPC error codes
namespace RpcErrorCodes {
    constexpr int ParseError     = -32700;
    constexpr int InvalidRequest = -32600;
    constexpr int MethodNotFound = -32601;
    constexpr int InvalidParams  = -32602;
    constexpr int InternalError  = -32603;
    // LSP specific
    constexpr int ServerNotInit  = -32002;
    constexpr int RequestFailed  = -32803;
}

// ---------------------------------------------------------------------------
// LSP data types
// ---------------------------------------------------------------------------
struct Position {
    uint32_t line;      ///< 0-based
    uint32_t character; ///< 0-based UTF-16 offset
};

struct Range {
    Position start;
    Position end;
};

struct TextDocumentIdentifier {
    std::string uri;
};

struct TextDocumentItem {
    std::string uri;
    std::string language_id;
    int32_t     version;
    std::string text;
};

struct VersionedTextDocumentIdentifier {
    std::string uri;
    int32_t     version;
};

struct TextDocumentContentChangeEvent {
    std::optional<Range>  range;
    std::string           text;
};

struct LspDiagnostic {
    Range       range;
    int         severity;  // 1=error,2=warn,3=info,4=hint
    std::string code;
    std::string source;
    std::string message;
    std::vector<std::string> tags;
};

struct PublishDiagnosticsParams {
    std::string               uri;
    int32_t                   version;
    std::vector<LspDiagnostic> diagnostics;
};

struct CodeAction {
    std::string title;
    std::string kind;  // "quickfix" | "refactor"
    std::vector<LspDiagnostic> diagnostics;
    bool is_preferred;
    // WorkspaceEdit
    struct TextEdit { Range range; std::string new_text; };
    std::unordered_map<std::string, std::vector<TextEdit>> changes; // uri -> edits
};

// ---------------------------------------------------------------------------
// LSP message handler function
using RequestHandler     = std::function<JsonValue(const JsonValue& params)>;
using NotificationHandler = std::function<void(const JsonValue& params)>;

// ---------------------------------------------------------------------------
// Document store - tracks open files and their content
class DocumentStore {
public:
    void open(const TextDocumentItem& doc) noexcept;
    void change(const VersionedTextDocumentIdentifier& id,
                std::vector<TextDocumentContentChangeEvent> changes) noexcept;
    void close(const TextDocumentIdentifier& id) noexcept;

    [[nodiscard]] std::optional<std::string>     get_text(std::string_view uri) const noexcept;
    [[nodiscard]] std::optional<std::string>     get_language(std::string_view uri) const noexcept;
    [[nodiscard]] std::optional<int32_t>         get_version(std::string_view uri) const noexcept;
    [[nodiscard]] std::vector<std::string>       all_uris() const noexcept;

private:
    mutable std::shared_mutex mutex_;
    struct DocEntry {
        std::string text;
        std::string language_id;
        int32_t     version;
    };
    std::unordered_map<std::string, DocEntry> docs_;

    static std::string apply_change(const std::string& text,
                                     const TextDocumentContentChangeEvent& change) noexcept;
};

// ---------------------------------------------------------------------------
// LSP Server
// ---------------------------------------------------------------------------
class LspServer {
public:
    explicit LspServer(std::unique_ptr<RuleEngine> engine,
                       std::istream& in  = std::cin,
                       std::ostream& out = std::cout) noexcept;
    ~LspServer() noexcept;

    LspServer(const LspServer&) = delete;
    LspServer& operator=(const LspServer&) = delete;

    /// Main event loop - blocks until shutdown
    void run() noexcept;

    /// Request stop (thread-safe)
    void stop() noexcept;

    /// Register additional request handler
    void on_request(std::string method, RequestHandler handler) noexcept;
    void on_notification(std::string method, NotificationHandler handler) noexcept;

private:
    std::unique_ptr<RuleEngine> engine_;
    std::istream&  in_;
    std::ostream&  out_;
    std::mutex     out_mutex_;

    DocumentStore  docs_;
    std::atomic<bool> running_{false};
    std::atomic<bool> initialized_{false};
    std::atomic<bool> shutdown_requested_{false};

    std::unordered_map<std::string, RequestHandler>      req_handlers_;
    std::unordered_map<std::string, NotificationHandler> notif_handlers_;

    // Lint work queue (background thread)
    struct LintWork { std::string uri; int32_t version; };
    std::queue<LintWork>     work_queue_;
    std::mutex               work_mutex_;
    std::condition_variable  work_cv_;
    std::thread              lint_thread_;

    // I/O helpers
    [[nodiscard]] std::expected<std::string, std::string> read_message() noexcept;
    void send_response(const RpcResponse& resp) noexcept;
    void send_notification(std::string_view method, const JsonValue& params) noexcept;
    void send_error(const JsonValue& id, int code, std::string message) noexcept;

    // Message dispatching
    void dispatch(const std::string& raw_msg) noexcept;
    void handle_request(const RpcRequest& req) noexcept;
    void handle_notification(const RpcRequest& req) noexcept;

    // Built-in LSP handlers
    JsonValue handle_initialize(const JsonValue& params) noexcept;
    JsonValue handle_shutdown(const JsonValue& params) noexcept;
    JsonValue handle_text_document_completion(const JsonValue& params) noexcept;
    JsonValue handle_text_document_hover(const JsonValue& params) noexcept;
    JsonValue handle_code_action(const JsonValue& params) noexcept;
    JsonValue handle_formatting(const JsonValue& params) noexcept;

    void handle_initialized(const JsonValue& params) noexcept;
    void handle_did_open(const JsonValue& params) noexcept;
    void handle_did_change(const JsonValue& params) noexcept;
    void handle_did_close(const JsonValue& params) noexcept;
    void handle_exit(const JsonValue& params) noexcept;

    // Lint and publish
    void enqueue_lint(std::string uri, int32_t version) noexcept;
    void lint_worker() noexcept;
    void publish_diagnostics(const std::string& uri,
                              int32_t version,
                              const std::string& text) noexcept;

    // Serialization helpers
    [[nodiscard]] static JsonValue diagnostic_to_json(const LspDiagnostic& d) noexcept;
    [[nodiscard]] static LspDiagnostic vietlint_to_lsp(const Diagnostic& d) noexcept;
    [[nodiscard]] static std::string rpc_message(const std::string& content) noexcept;

    void register_builtin_handlers() noexcept;
};

} // namespace vietlint::lsp
