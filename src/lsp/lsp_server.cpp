#include "vietlint/lsp_server.hpp"
#include <sstream>
#include <charconv>
#include <algorithm>
#include <stdexcept>
#include <cstring>
#include <cassert>
#include <shared_mutex>

namespace vietlint::lsp {

// ===========================================================================
// Json implementation — recursive descent parser
// ===========================================================================
namespace {

struct Parser {
    std::string_view src;
    size_t pos = 0;

    void skip_ws() noexcept {
        while (pos < src.size() && (src[pos]==' '||src[pos]=='\t'||src[pos]=='\n'||src[pos]=='\r'))
            ++pos;
    }

    std::expected<JsonValue, std::string> parse_value() noexcept;
    std::expected<JsonValue, std::string> parse_string() noexcept;
    std::expected<JsonValue, std::string> parse_number() noexcept;
    std::expected<JsonValue, std::string> parse_array() noexcept;
    std::expected<JsonValue, std::string> parse_object() noexcept;

    bool consume(char c) noexcept { if (pos < src.size() && src[pos]==c) { ++pos; return true; } return false; }
    bool expect(char c) noexcept {
        skip_ws();
        return consume(c);
    }
};

static uint32_t hex4(std::string_view s, size_t pos) noexcept {
    uint32_t v = 0;
    for (int i = 0; i < 4 && pos+i < s.size(); ++i) {
        char c = s[pos+i];
        v <<= 4;
        if (c>='0'&&c<='9') v|=(c-'0');
        else if (c>='a'&&c<='f') v|=(c-'a'+10);
        else if (c>='A'&&c<='F') v|=(c-'A'+10);
    }
    return v;
}

static void cp_to_utf8(uint32_t cp, std::string& out) noexcept {
    if (cp < 0x80)        { out += (char)cp; }
    else if (cp < 0x800)  { out += (char)(0xC0|(cp>>6)); out += (char)(0x80|(cp&0x3F)); }
    else if (cp < 0x10000){ out += (char)(0xE0|(cp>>12)); out += (char)(0x80|((cp>>6)&0x3F)); out += (char)(0x80|(cp&0x3F)); }
    else                  { out += (char)(0xF0|(cp>>18)); out += (char)(0x80|((cp>>12)&0x3F)); out += (char)(0x80|((cp>>6)&0x3F)); out += (char)(0x80|(cp&0x3F)); }
}

std::expected<JsonValue, std::string> Parser::parse_string() noexcept {
    if (!consume('"')) return std::unexpected("Expected '\"'");
    std::string out;
    out.reserve(32);
    while (pos < src.size() && src[pos] != '"') {
        char c = src[pos++];
        if (c != '\\') { out += c; continue; }
        if (pos >= src.size()) return std::unexpected("Unterminated string escape");
        char e = src[pos++];
        switch (e) {
            case '"':  out += '"';  break;
            case '\\': out += '\\'; break;
            case '/':  out += '/';  break;
            case 'n':  out += '\n'; break;
            case 'r':  out += '\r'; break;
            case 't':  out += '\t'; break;
            case 'b':  out += '\b'; break;
            case 'f':  out += '\f'; break;
            case 'u': {
                if (pos + 4 > src.size()) return std::unexpected("Bad \\u escape");
                uint32_t cp = hex4(src, pos); pos += 4;
                // Handle surrogate pairs
                if (cp >= 0xD800 && cp <= 0xDBFF && pos+1 < src.size() && src[pos]=='\\' && src[pos+1]=='u') {
                    pos += 2;
                    uint32_t lo = hex4(src, pos); pos += 4;
                    if (lo >= 0xDC00 && lo <= 0xDFFF)
                        cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
                }
                cp_to_utf8(cp, out);
                break;
            }
            default: out += e;
        }
    }
    consume('"');
    return JsonValue{std::move(out)};
}

std::expected<JsonValue, std::string> Parser::parse_number() noexcept {
    size_t start = pos;
    if (pos < src.size() && src[pos] == '-') ++pos;
    while (pos < src.size() && src[pos] >= '0' && src[pos] <= '9') ++pos;
    bool is_float = false;
    if (pos < src.size() && src[pos] == '.') { is_float = true; ++pos; while (pos<src.size()&&src[pos]>='0'&&src[pos]<='9') ++pos; }
    if (pos < src.size() && (src[pos]=='e'||src[pos]=='E')) { is_float=true; ++pos; if (pos<src.size()&&(src[pos]=='+'||src[pos]=='-')) ++pos; while(pos<src.size()&&src[pos]>='0'&&src[pos]<='9') ++pos; }
    std::string_view num = src.substr(start, pos-start);
    if (is_float) {
        double d = 0; std::from_chars(num.data(), num.data()+num.size(), d);
        return JsonValue{d};
    } else {
        int64_t i = 0; std::from_chars(num.data(), num.data()+num.size(), i);
        return JsonValue{i};
    }
}

std::expected<JsonValue, std::string> Parser::parse_array() noexcept {
    if (!consume('[')) return std::unexpected("Expected '['");
    auto arr = std::make_shared<JsonArray>();
    skip_ws();
    if (consume(']')) return JsonValue{std::vector<std::shared_ptr<JsonArray>>{}};
    while (true) {
        skip_ws();
        auto v = parse_value();
        if (!v) return v;
        arr->items.push_back(std::move(*v));
        skip_ws();
        if (!consume(',')) break;
    }
    skip_ws();
    if (!consume(']')) return std::unexpected("Expected ']'");
    // Wrap in vector<shared_ptr<JsonArray>> for the variant
    std::vector<std::shared_ptr<JsonArray>> wrapper;
    wrapper.push_back(arr);
    return JsonValue{std::move(wrapper)};
}

std::expected<JsonValue, std::string> Parser::parse_object() noexcept {
    if (!consume('{')) return std::unexpected("Expected '{'");
    auto obj = std::make_shared<JsonObject>();
    skip_ws();
    if (consume('}')) return JsonValue{obj};
    while (true) {
        skip_ws();
        auto key = parse_string();
        if (!key) return key;
        skip_ws();
        if (!consume(':')) return std::unexpected("Expected ':'");
        skip_ws();
        auto val = parse_value();
        if (!val) return val;
        obj->fields[std::get<std::string>(*key)] = std::move(*val);
        skip_ws();
        if (!consume(',')) break;
    }
    skip_ws();
    if (!consume('}')) return std::unexpected("Expected '}'");
    return JsonValue{obj};
}

std::expected<JsonValue, std::string> Parser::parse_value() noexcept {
    skip_ws();
    if (pos >= src.size()) return std::unexpected("Unexpected EOF");
    char c = src[pos];
    if (c == '"') return parse_string();
    if (c == '{') return parse_object();
    if (c == '[') return parse_array();
    if (c == '-' || (c >= '0' && c <= '9')) return parse_number();
    if (src.substr(pos, 4) == "true")  { pos+=4; return JsonValue{true}; }
    if (src.substr(pos, 5) == "false") { pos+=5; return JsonValue{false}; }
    if (src.substr(pos, 4) == "null")  { pos+=4; return JsonValue{JsonNull{}}; }
    return std::unexpected(std::string("Unexpected char: ") + c);
}

} // anonymous namespace

std::expected<JsonValue, std::string> Json::parse(std::string_view s) noexcept {
    Parser p{s, 0};
    return p.parse_value();
}

std::string Json::serialize(const JsonValue& v, bool /*pretty*/) noexcept {
    return std::visit([](const auto& val) -> std::string {
        using T = std::decay_t<decltype(val)>;
        if constexpr (std::is_same_v<T, JsonNull>)   return "null";
        if constexpr (std::is_same_v<T, bool>)        return val ? "true" : "false";
        if constexpr (std::is_same_v<T, int64_t>)     return std::to_string(val);
        if constexpr (std::is_same_v<T, double>) {
            char buf[32]; snprintf(buf, sizeof(buf), "%.17g", val); return buf;
        }
        if constexpr (std::is_same_v<T, std::string>) {
            std::string out = "\"";
            for (unsigned char c : val) {
                switch(c) {
                    case '"':  out += "\\\""; break;
                    case '\\': out += "\\\\"; break;
                    case '\n': out += "\\n";  break;
                    case '\r': out += "\\r";  break;
                    case '\t': out += "\\t";  break;
                    default:
                        if (c < 0x20) { char b[8]; snprintf(b,sizeof(b),"\\u%04X",c); out+=b; }
                        else out += (char)c;
                }
            }
            return out + "\"";
        }
        if constexpr (std::is_same_v<T, std::vector<std::shared_ptr<JsonArray>>>) {
            if (val.empty()) return "[]";
            std::string out = "[";
            for (size_t i = 0; i < val[0]->items.size(); ++i) {
                if (i) out += ",";
                out += serialize(val[0]->items[i]);
            }
            return out + "]";
        }
        if constexpr (std::is_same_v<T, std::shared_ptr<JsonObject>>) {
            if (!val) return "{}";
            std::string out = "{";
            bool first = true;
            for (auto& [k, fv] : val->fields) {
                if (!first) out += ",";
                out += "\"" + k + "\":" + serialize(fv);
                first = false;
            }
            return out + "}";
        }
        return "null";
    }, v);
}

const JsonObject* Json::as_object(const JsonValue& v) noexcept {
    auto* p = std::get_if<std::shared_ptr<JsonObject>>(&v);
    return p ? p->get() : nullptr;
}
const JsonArray* Json::as_array(const JsonValue& v) noexcept {
    auto* p = std::get_if<std::vector<std::shared_ptr<JsonArray>>>(&v);
    return (p && !p->empty()) ? (*p)[0].get() : nullptr;
}
const std::string* Json::as_string(const JsonValue& v) noexcept {
    return std::get_if<std::string>(&v);
}
std::optional<int64_t> Json::as_int(const JsonValue& v) noexcept {
    if (auto* i = std::get_if<int64_t>(&v)) return *i;
    if (auto* d = std::get_if<double>(&v))  return static_cast<int64_t>(*d);
    return std::nullopt;
}
std::optional<double> Json::as_double(const JsonValue& v) noexcept {
    if (auto* d = std::get_if<double>(&v))  return *d;
    if (auto* i = std::get_if<int64_t>(&v)) return static_cast<double>(*i);
    return std::nullopt;
}
std::optional<bool> Json::as_bool(const JsonValue& v) noexcept {
    auto* b = std::get_if<bool>(&v); return b ? std::optional<bool>(*b) : std::nullopt;
}
const JsonValue* Json::get(const JsonObject& obj, std::string_view key) noexcept {
    auto it = obj.fields.find(std::string(key));
    return it != obj.fields.end() ? &it->second : nullptr;
}
JsonValue Json::make_object(std::initializer_list<std::pair<std::string, JsonValue>> fields) noexcept {
    auto obj = std::make_shared<JsonObject>();
    for (auto& [k, v] : fields) obj->fields[k] = v;
    return JsonValue{obj};
}
JsonValue Json::make_array(std::vector<JsonValue> items) noexcept {
    auto arr = std::make_shared<JsonArray>();
    arr->items = std::move(items);
    std::vector<std::shared_ptr<JsonArray>> w; w.push_back(arr);
    return JsonValue{std::move(w)};
}
JsonValue Json::make_string(std::string s) noexcept { return JsonValue{std::move(s)}; }
JsonValue Json::make_int(int64_t v) noexcept        { return JsonValue{v}; }
JsonValue Json::make_null() noexcept                { return JsonValue{JsonNull{}}; }
JsonValue Json::make_bool(bool v) noexcept          { return JsonValue{v}; }

// ===========================================================================
// DocumentStore
// ===========================================================================
void DocumentStore::open(const TextDocumentItem& doc) noexcept {
    std::unique_lock lock(mutex_);
    docs_[doc.uri] = DocEntry{doc.text, doc.language_id, doc.version};
}

std::string DocumentStore::apply_change(const std::string& text,
                                          const TextDocumentContentChangeEvent& change) noexcept {
    if (!change.range) return change.text; // full replacement
    const auto& range = *change.range;

    // Convert line/char to byte offset
    auto find_offset = [&](uint32_t line, uint32_t col) -> size_t {
        size_t off = 0; uint32_t cur_line = 0;
        while (off < text.size() && cur_line < line) {
            if (text[off] == '\n') ++cur_line;
            ++off;
        }
        // Advance col characters (UTF-16 units, approximate with UTF-8 codepoints)
        for (uint32_t c = 0; c < col && off < text.size() && text[off] != '\n'; ++c) {
            uint8_t b = static_cast<uint8_t>(text[off]);
            if      (b < 0x80)  off += 1;
            else if (b < 0xE0)  off += 2;
            else if (b < 0xF0)  off += 3;
            else                off += 4;
        }
        return off;
    };

    size_t start_off = find_offset(range.start.line, range.start.character);
    size_t end_off   = find_offset(range.end.line,   range.end.character);
    if (start_off > text.size()) start_off = text.size();
    if (end_off   > text.size()) end_off   = text.size();

    return text.substr(0, start_off) + change.text + text.substr(end_off);
}

void DocumentStore::change(const VersionedTextDocumentIdentifier& id,
                             std::vector<TextDocumentContentChangeEvent> changes) noexcept {
    std::unique_lock lock(mutex_);
    auto it = docs_.find(id.uri);
    if (it == docs_.end()) return;
    for (const auto& c : changes)
        it->second.text = apply_change(it->second.text, c);
    it->second.version = id.version;
}

void DocumentStore::close(const TextDocumentIdentifier& id) noexcept {
    std::unique_lock lock(mutex_);
    docs_.erase(id.uri);
}

std::optional<std::string> DocumentStore::get_text(std::string_view uri) const noexcept {
    std::shared_lock lock(mutex_);
    auto it = docs_.find(std::string(uri));
    if (it == docs_.end()) return std::nullopt;
    return it->second.text;
}
std::optional<std::string> DocumentStore::get_language(std::string_view uri) const noexcept {
    std::shared_lock lock(mutex_);
    auto it = docs_.find(std::string(uri));
    if (it == docs_.end()) return std::nullopt;
    return it->second.language_id;
}
std::optional<int32_t> DocumentStore::get_version(std::string_view uri) const noexcept {
    std::shared_lock lock(mutex_);
    auto it = docs_.find(std::string(uri));
    if (it == docs_.end()) return std::nullopt;
    return it->second.version;
}
std::vector<std::string> DocumentStore::all_uris() const noexcept {
    std::shared_lock lock(mutex_);
    std::vector<std::string> uris;
    uris.reserve(docs_.size());
    for (auto& [uri, _] : docs_) uris.push_back(uri);
    return uris;
}

// ===========================================================================
// LspServer
// ===========================================================================
LspServer::LspServer(std::unique_ptr<RuleEngine> engine,
                     std::istream& in,
                     std::ostream& out) noexcept
    : engine_(std::move(engine)), in_(in), out_(out)
{
    register_builtin_handlers();
}

LspServer::~LspServer() noexcept {
    stop();
    if (lint_thread_.joinable()) lint_thread_.join();
}

void LspServer::stop() noexcept {
    running_.store(false, std::memory_order_release);
    work_cv_.notify_all();
}

void LspServer::on_request(std::string method, RequestHandler handler) noexcept {
    req_handlers_[std::move(method)] = std::move(handler);
}
void LspServer::on_notification(std::string method, NotificationHandler handler) noexcept {
    notif_handlers_[std::move(method)] = std::move(handler);
}

// ---------------------------------------------------------------------------
std::string LspServer::rpc_message(const std::string& content) noexcept {
    return "Content-Length: " + std::to_string(content.size()) + "\r\n\r\n" + content;
}

std::expected<std::string, std::string> LspServer::read_message() noexcept {
    // Read headers
    size_t content_length = 0;
    std::string line;
    while (true) {
        if (!std::getline(in_, line)) return std::unexpected("EOF");
        // Strip \r
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (line.empty()) break; // end of headers
        if (line.rfind("Content-Length: ", 0) == 0) {
            std::from_chars(line.data() + 16, line.data() + line.size(), content_length);
        }
    }
    if (content_length == 0) return std::unexpected("Missing Content-Length");

    std::string body(content_length, '\0');
    if (!in_.read(body.data(), static_cast<std::streamsize>(content_length)))
        return std::unexpected("Failed to read body");
    return body;
}

void LspServer::send_response(const RpcResponse& resp) noexcept {
    auto obj = std::make_shared<JsonObject>();
    obj->fields["jsonrpc"] = JsonValue{std::string("2.0")};
    obj->fields["id"]      = resp.id;
    if (resp.result)  obj->fields["result"] = *resp.result;
    if (resp.error) {
        auto err_obj = std::make_shared<JsonObject>();
        // error is JsonValue holding an object
        obj->fields["error"] = *resp.error;
    }
    std::string body = Json::serialize(JsonValue{obj});
    std::lock_guard lock(out_mutex_);
    out_ << rpc_message(body) << std::flush;
}

void LspServer::send_notification(std::string_view method, const JsonValue& params) noexcept {
    auto obj = std::make_shared<JsonObject>();
    obj->fields["jsonrpc"] = JsonValue{std::string("2.0")};
    obj->fields["method"]  = JsonValue{std::string(method)};
    obj->fields["params"]  = params;
    std::string body = Json::serialize(JsonValue{obj});
    std::lock_guard lock(out_mutex_);
    out_ << rpc_message(body) << std::flush;
}

void LspServer::send_error(const JsonValue& id, int code, std::string message) noexcept {
    auto err = Json::make_object({
        {"code",    Json::make_int(code)},
        {"message", Json::make_string(std::move(message))},
    });
    RpcResponse resp;
    resp.id    = id;
    resp.error = err;
    send_response(resp);
}

// ---------------------------------------------------------------------------
void LspServer::dispatch(const std::string& raw_msg) noexcept {
    auto parsed = Json::parse(raw_msg);
    if (!parsed) {
        send_error(Json::make_null(), RpcErrorCodes::ParseError, "Parse error: " + parsed.error());
        return;
    }
    auto* obj = Json::as_object(*parsed);
    if (!obj) {
        send_error(Json::make_null(), RpcErrorCodes::InvalidRequest, "Not a JSON object");
        return;
    }

    RpcRequest req;
    auto* method_v = Json::get(*obj, "method");
    if (!method_v) {
        send_error(Json::make_null(), RpcErrorCodes::InvalidRequest, "Missing method");
        return;
    }
    auto* method_s = Json::as_string(*method_v);
    if (!method_s) {
        send_error(Json::make_null(), RpcErrorCodes::InvalidRequest, "method must be string");
        return;
    }
    req.method = *method_s;

    auto* id_v = Json::get(*obj, "id");
    req.is_notification = (id_v == nullptr);
    if (id_v) req.id = *id_v;
    else       req.id = Json::make_null();

    auto* params_v = Json::get(*obj, "params");
    if (params_v) req.params = *params_v;

    if (req.is_notification) handle_notification(req);
    else                     handle_request(req);
}

void LspServer::handle_request(const RpcRequest& req) noexcept {
    if (!initialized_.load() && req.method != "initialize") {
        send_error(req.id, RpcErrorCodes::ServerNotInit, "Server not initialized");
        return;
    }
    auto it = req_handlers_.find(req.method);
    if (it == req_handlers_.end()) {
        send_error(req.id, RpcErrorCodes::MethodNotFound,
                   "Method not found: " + req.method);
        return;
    }
    try {
        JsonValue params = req.params.value_or(Json::make_null());
        JsonValue result = it->second(params);
        RpcResponse resp;
        resp.id     = req.id;
        resp.result = result;
        send_response(resp);
    } catch (const std::exception& e) {
        send_error(req.id, RpcErrorCodes::InternalError,
                   std::string("Internal error: ") + e.what());
    }
}

void LspServer::handle_notification(const RpcRequest& req) noexcept {
    auto it = notif_handlers_.find(req.method);
    if (it == notif_handlers_.end()) return;
    JsonValue params = req.params.value_or(Json::make_null());
    it->second(params);
}

// ---------------------------------------------------------------------------
// initialize
// ---------------------------------------------------------------------------
JsonValue LspServer::handle_initialize(const JsonValue& /*params*/) noexcept {
    initialized_.store(true, std::memory_order_release);

    // Start lint worker thread
    lint_thread_ = std::thread([this]{ lint_worker(); });

    return Json::make_object({
        {"capabilities", Json::make_object({
            {"textDocumentSync", Json::make_object({
                {"openClose",         Json::make_bool(true)},
                {"change",            Json::make_int(2)},  // incremental
                {"willSave",          Json::make_bool(false)},
                {"willSaveWaitUntil", Json::make_bool(false)},
                {"save",              Json::make_object({{"includeText", Json::make_bool(false)}})},
            })},
            {"hoverProvider",         Json::make_bool(true)},
            {"codeActionProvider",    Json::make_bool(true)},
            {"documentFormattingProvider", Json::make_bool(false)},
            {"diagnosticProvider",    Json::make_object({
                {"identifier",        Json::make_string("vietlint")},
                {"interFileDependencies", Json::make_bool(false)},
                {"workspaceDiagnostics",  Json::make_bool(false)},
            })},
        })},
        {"serverInfo", Json::make_object({
            {"name",    Json::make_string("vietlint-lsp")},
            {"version", Json::make_string("1.0.0")},
        })},
    });
}

JsonValue LspServer::handle_shutdown(const JsonValue& /*params*/) noexcept {
    shutdown_requested_.store(true, std::memory_order_release);
    return Json::make_null();
}

// ---------------------------------------------------------------------------
// textDocument/codeAction
// ---------------------------------------------------------------------------
JsonValue LspServer::handle_code_action(const JsonValue& params) noexcept {
    auto* obj = Json::as_object(params);
    if (!obj) return Json::make_array({});

    auto* td_v = Json::get(*obj, "textDocument");
    if (!td_v) return Json::make_array({});
    auto* td_obj = Json::as_object(*td_v);
    if (!td_obj) return Json::make_array({});
    auto* uri_v = Json::get(*td_obj, "uri");
    if (!uri_v) return Json::make_array({});
    auto* uri_s = Json::as_string(*uri_v);
    if (!uri_s) return Json::make_array({});

    auto text_opt = docs_.get_text(*uri_s);
    if (!text_opt) return Json::make_array({});

    auto diagnostics = engine_->lint_source(*text_opt, *uri_s);
    std::vector<JsonValue> actions;

    for (const auto& d : diagnostics) {
        if (d.violation.fixes.empty()) continue;
        for (const auto& fix : d.violation.fixes) {
            // Build WorkspaceEdit for each fix
            auto edit_range = Json::make_object({
                {"start", Json::make_object({
                    {"line",      Json::make_int(d.violation.span.line > 0 ? d.violation.span.line - 1 : 0)},
                    {"character", Json::make_int(d.violation.span.col > 0 ? d.violation.span.col - 1 : 0)},
                })},
                {"end", Json::make_object({
                    {"line",      Json::make_int(d.violation.span.line > 0 ? d.violation.span.line - 1 : 0)},
                    {"character", Json::make_int(d.violation.span.col > 0 ? (int64_t)d.violation.span.col - 1 + (int64_t)d.violation.identifier.size() : 0)},
                })},
            });

            auto text_edit = Json::make_object({
                {"range",   edit_range},
                {"newText", Json::make_string(fix)},
            });

            std::vector<JsonValue> edits_arr;
            edits_arr.push_back(text_edit);

            auto action = Json::make_object({
                {"title",       Json::make_string("VietLint: rename to '" + fix + "'")},
                {"kind",        Json::make_string("quickfix")},
                {"isPreferred", Json::make_bool(true)},
                {"edit",        Json::make_object({
                    {"changes", Json::make_object({{*uri_s, Json::make_array(std::move(edits_arr))}})}
                })},
            });
            actions.push_back(std::move(action));
        }
    }
    return Json::make_array(std::move(actions));
}

JsonValue LspServer::handle_text_document_hover(const JsonValue& params) noexcept {
    auto* obj = Json::as_object(params);
    if (!obj) return Json::make_null();
    auto* td  = Json::get(*obj, "textDocument");
    auto* pos = Json::get(*obj, "position");
    if (!td || !pos) return Json::make_null();
    auto* td_obj  = Json::as_object(*td);
    auto* pos_obj = Json::as_object(*pos);
    if (!td_obj || !pos_obj) return Json::make_null();
    auto* uri_v = Json::get(*td_obj, "uri");
    if (!uri_v) return Json::make_null();
    auto* uri_s = Json::as_string(*uri_v);
    if (!uri_s) return Json::make_null();

    auto text = docs_.get_text(*uri_s);
    if (!text) return Json::make_null();

    auto* line_v = Json::get(*pos_obj, "line");
    auto* char_v = Json::get(*pos_obj, "character");
    auto line = Json::as_int(*line_v).value_or(0);
    auto col  = Json::as_int(*char_v).value_or(0);

    // Find word at position
    auto diags = engine_->lint_source(*text, *uri_s);
    for (const auto& d : diags) {
        int64_t d_line = d.violation.span.line > 0 ? d.violation.span.line - 1 : 0;
        int64_t d_col  = d.violation.span.col  > 0 ? d.violation.span.col  - 1 : 0;
        if (d_line == line && d_col <= col &&
            col < d_col + (int64_t)d.violation.identifier.size()) {
            std::string md = "**[" + d.violation.rule_id + "]** " + d.violation.message;
            if (!d.violation.fixes.empty()) {
                md += "\n\n**Suggestions:** ";
                for (size_t i = 0; i < d.violation.fixes.size(); ++i) {
                    if (i) md += ", ";
                    md += "`" + d.violation.fixes[i] + "`";
                }
            }
            return Json::make_object({
                {"contents", Json::make_object({
                    {"kind",  Json::make_string("markdown")},
                    {"value", Json::make_string(md)},
                })},
            });
        }
    }
    return Json::make_null();
}

JsonValue LspServer::handle_text_document_completion(const JsonValue& /*params*/) noexcept {
    return Json::make_array({});
}

JsonValue LspServer::handle_formatting(const JsonValue& /*params*/) noexcept {
    return Json::make_null();
}

// ---------------------------------------------------------------------------
// Notifications
// ---------------------------------------------------------------------------
void LspServer::handle_initialized(const JsonValue& /*params*/) noexcept {}

void LspServer::handle_did_open(const JsonValue& params) noexcept {
    auto* obj = Json::as_object(params);
    if (!obj) return;
    auto* td_v = Json::get(*obj, "textDocument");
    if (!td_v) return;
    auto* td = Json::as_object(*td_v);
    if (!td) return;

    TextDocumentItem item;
    if (auto* v = Json::get(*td, "uri"))        if (auto* s = Json::as_string(*v))   item.uri         = *s;
    if (auto* v = Json::get(*td, "languageId")) if (auto* s = Json::as_string(*v))   item.language_id = *s;
    if (auto* v = Json::get(*td, "version"))    if (auto  i = Json::as_int(*v))       item.version     = (int32_t)*i;
    if (auto* v = Json::get(*td, "text"))       if (auto* s = Json::as_string(*v))   item.text        = *s;

    if (item.uri.empty()) return;
    docs_.open(item);
    enqueue_lint(item.uri, item.version);
}

void LspServer::handle_did_change(const JsonValue& params) noexcept {
    auto* obj = Json::as_object(params);
    if (!obj) return;
    auto* td_v = Json::get(*obj, "textDocument");
    auto* changes_v = Json::get(*obj, "contentChanges");
    if (!td_v || !changes_v) return;

    auto* td = Json::as_object(*td_v);
    if (!td) return;
    VersionedTextDocumentIdentifier id;
    if (auto* v = Json::get(*td, "uri"))     if (auto* s = Json::as_string(*v)) id.uri     = *s;
    if (auto* v = Json::get(*td, "version")) if (auto  i = Json::as_int(*v))    id.version = (int32_t)*i;
    if (id.uri.empty()) return;

    std::vector<TextDocumentContentChangeEvent> changes;
    auto* arr = Json::as_array(*changes_v);
    if (arr) {
        for (const auto& c_v : arr->items) {
            auto* c_obj = Json::as_object(c_v);
            if (!c_obj) continue;
            TextDocumentContentChangeEvent change;
            if (auto* text_v = Json::get(*c_obj, "text"))
                if (auto* s = Json::as_string(*text_v))
                    change.text = *s;
            // range is optional
            if (auto* range_v = Json::get(*c_obj, "range")) {
                auto* robj = Json::as_object(*range_v);
                if (robj) {
                    Range r{};
                    auto read_pos = [&](const char* key) -> Position {
                        Position p{}; auto* pv = Json::get(*robj, key);
                        if (!pv) return p; auto* pobj = Json::as_object(*pv);
                        if (!pobj) return p;
                        if (auto* lv = Json::get(*pobj,"line"))      p.line      = (uint32_t)Json::as_int(*lv).value_or(0);
                        if (auto* cv = Json::get(*pobj,"character"))  p.character = (uint32_t)Json::as_int(*cv).value_or(0);
                        return p;
                    };
                    r.start = read_pos("start");
                    r.end   = read_pos("end");
                    change.range = r;
                }
            }
            changes.push_back(std::move(change));
        }
    }
    docs_.change(id, std::move(changes));
    enqueue_lint(id.uri, id.version);
}

void LspServer::handle_did_close(const JsonValue& params) noexcept {
    auto* obj = Json::as_object(params);
    if (!obj) return;
    auto* td_v = Json::get(*obj, "textDocument");
    if (!td_v) return;
    auto* td = Json::as_object(*td_v);
    if (!td) return;
    if (auto* v = Json::get(*td, "uri")) {
        if (auto* s = Json::as_string(*v)) {
            docs_.close(TextDocumentIdentifier{*s});
            // Clear diagnostics for closed file
            send_notification("textDocument/publishDiagnostics", Json::make_object({
                {"uri",         Json::make_string(*s)},
                {"diagnostics", Json::make_array({})},
            }));
        }
    }
}

void LspServer::handle_exit(const JsonValue& /*params*/) noexcept {
    running_.store(false, std::memory_order_release);
}

// ---------------------------------------------------------------------------
// Lint worker
// ---------------------------------------------------------------------------
void LspServer::enqueue_lint(std::string uri, int32_t version) noexcept {
    std::lock_guard lock(work_mutex_);
    work_queue_.push(LintWork{std::move(uri), version});
    work_cv_.notify_one();
}

void LspServer::lint_worker() noexcept {
    while (running_.load(std::memory_order_acquire)) {
        LintWork work;
        {
            std::unique_lock lock(work_mutex_);
            work_cv_.wait(lock, [this]{
                return !work_queue_.empty() || !running_.load(std::memory_order_relaxed);
            });
            if (!running_.load(std::memory_order_relaxed) && work_queue_.empty()) return;
            work = std::move(work_queue_.front());
            work_queue_.pop();
        }
        auto text = docs_.get_text(work.uri);
        if (!text) continue;
        auto ver  = docs_.get_version(work.uri).value_or(work.version);
        publish_diagnostics(work.uri, ver, *text);
    }
}

LspDiagnostic LspServer::vietlint_to_lsp(const Diagnostic& d) noexcept {
    const auto& v = d.violation;
    LspDiagnostic lsp;
    uint32_t line = v.span.line > 0 ? v.span.line - 1 : 0;
    uint32_t col  = v.span.col  > 0 ? v.span.col  - 1 : 0;
    uint32_t end_col = col + (uint32_t)v.identifier.size();
    lsp.range    = {{line, col}, {line, end_col}};
    lsp.severity = DiagnosticEmitter::severity_to_lsp_code(v.severity);
    lsp.code     = v.rule_id;
    lsp.source   = "vietlint";
    lsp.message  = v.message;
    return lsp;
}

JsonValue LspServer::diagnostic_to_json(const LspDiagnostic& d) noexcept {
    auto make_pos = [](const Position& p) {
        return Json::make_object({
            {"line",      Json::make_int(p.line)},
            {"character", Json::make_int(p.character)},
        });
    };
    return Json::make_object({
        {"range", Json::make_object({
            {"start", make_pos(d.range.start)},
            {"end",   make_pos(d.range.end)},
        })},
        {"severity", Json::make_int(d.severity)},
        {"code",     Json::make_string(d.code)},
        {"source",   Json::make_string(d.source)},
        {"message",  Json::make_string(d.message)},
    });
}

void LspServer::publish_diagnostics(const std::string& uri,
                                     int32_t version,
                                     const std::string& text) noexcept {
    auto raw_diags = engine_->lint_source(text, uri);
    std::vector<JsonValue> lsp_diags;
    lsp_diags.reserve(raw_diags.size());
    for (const auto& d : raw_diags)
        lsp_diags.push_back(diagnostic_to_json(vietlint_to_lsp(d)));

    send_notification("textDocument/publishDiagnostics", Json::make_object({
        {"uri",         Json::make_string(uri)},
        {"version",     Json::make_int(version)},
        {"diagnostics", Json::make_array(std::move(lsp_diags))},
    }));
}

// ---------------------------------------------------------------------------
void LspServer::register_builtin_handlers() noexcept {
    on_request("initialize",    [this](auto& p){ return handle_initialize(p); });
    on_request("shutdown",      [this](auto& p){ return handle_shutdown(p); });
    on_request("textDocument/hover",       [this](auto& p){ return handle_text_document_hover(p); });
    on_request("textDocument/codeAction",  [this](auto& p){ return handle_code_action(p); });
    on_request("textDocument/completion",  [this](auto& p){ return handle_text_document_completion(p); });
    on_request("textDocument/formatting",  [this](auto& p){ return handle_formatting(p); });

    on_notification("initialized",             [this](auto& p){ handle_initialized(p); });
    on_notification("textDocument/didOpen",    [this](auto& p){ handle_did_open(p); });
    on_notification("textDocument/didChange",  [this](auto& p){ handle_did_change(p); });
    on_notification("textDocument/didClose",   [this](auto& p){ handle_did_close(p); });
    on_notification("exit",                    [this](auto& p){ handle_exit(p); });
}

// ---------------------------------------------------------------------------
void LspServer::run() noexcept {
    running_.store(true, std::memory_order_release);

    while (running_.load(std::memory_order_acquire)) {
        auto msg = read_message();
        if (!msg) {
            if (shutdown_requested_.load()) break;
            continue;
        }
        dispatch(*msg);
        if (shutdown_requested_.load() && !running_.load()) break;
    }
    running_.store(false, std::memory_order_release);
    work_cv_.notify_all();
}

} // namespace vietlint::lsp
