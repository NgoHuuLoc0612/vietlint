# 🇻🇳 VietLint

**Vietnamese coding convention linter** — Military-grade, enterprise-quality static analysis for Vietnamese source code.

## Architecture

```
vietlint/
├── include/vietlint/         C++20 headers (public API)
│   ├── utf8_scanner.hpp      AVX2 SIMD UTF-8 scanner
│   ├── lexer.hpp             Multi-language tokenizer (Python/JS/C++)
│   ├── classifier.hpp        Vietnamese identifier classifier
│   ├── encoding_detector.hpp UTF-8 / TCVN3 / VNI encoding detector
│   ├── rule_engine.hpp       Convention rule engine + TOML config
│   ├── emitter.hpp           Diagnostic formatter (text/JSON/SARIF/LSP)
│   ├── ml_classifier.hpp     ONNX Runtime ML layer
│   ├── lsp_server.hpp        Full LSP server (JSON-RPC over stdio)
│   └── clang_visitor.hpp     Clang LibTooling AST visitor
│
├── src/
│   ├── core/                 Phase 1: Core engine implementation
│   ├── llvm/                 Phase 2: Clang AST integration
│   ├── ml/                   Phase 3: ML layer + pybind11 bridge
│   └── lsp/                  Phase 5: LSP server implementation
│
├── python/
│   └── backend.py            Phase 4: FastAPI backend + WebSocket
│
├── scripts/
│   └── collect_corpus.py     Phase 3: Training data pipeline
│
├── vscode-extension/         Phase 6: VSCode extension
│   ├── src/extension.ts      LSP client + D3 dashboard
│   └── package.json
│
├── tests/                    Google Test unit tests
├── cmake/
│   └── example_plugin.cpp    Plugin API example
└── CMakeLists.txt            Phase 7: Full build system
```

## Features

### Phase 1 — Core Engine (C++20)
- **AVX2 SIMD UTF-8 scanner** — processes 32 bytes/cycle for Vietnamese Unicode detection
- **Multi-language lexer** — Python, JavaScript/TypeScript, C/C++ tokenization
- **Vietnamese identifier classifier** — regex + heuristic, 6 classes
- **Mixed-language comment detector** — flags EN/VN code-switching
- **Encoding detector** — UTF-8, TCVN3, VNI, Windows-1258 with conversion
- **Rule engine** — 7 built-in rules (VL001–VL007), TOML-configurable
- **Diagnostic emitter** — text/JSON/SARIF/LSP/GCC/GitHub Actions formats
- **Plugin loader** — `dlopen`/`LoadLibrary` shared library rules

### Phase 2 — Clang Integration
- Full C/C++ AST traversal via `RecursiveASTVisitor`
- Checks functions, variables, classes, fields, enums, typedefs, namespaces
- Optional clang-tidy module registration
- Graceful no-op when LLVM not present

### Phase 3 — ML Layer
- **64-dimension feature extractor** (Unicode ratios, n-gram hashes, style flags)
- **ONNX Runtime inference** with fallback to heuristic classifier
- **Training pipeline** — GitHub corpus scraper + sklearn → ONNX export
- **pybind11 bridge** — full Python API with NumPy arrays

### Phase 4 — FastAPI Backend
- REST API: `POST /api/v1/lint`, `/lint/file`, `/lint/format`
- **Server-Sent Events** streaming: `GET /api/v1/lint/stream`
- **WebSocket** real-time as-you-type: `ws://localhost:8765/ws/lint`
- **SQLite history**: projects, scan_runs, violations, rule_stats
- Project stats: per-rule counts, per-file heatmap, severity breakdown

### Phase 5 — LSP Server
- Full JSON-RPC 2.0 over stdio
- `initialize` / `shutdown` / `exit`
- `textDocument/didOpen` + `didChange` (incremental)
- `textDocument/publishDiagnostics` (background thread)
- `textDocument/hover` — rule documentation on hover
- `textDocument/codeAction` — quick-fix suggestions with `WorkspaceEdit`

### Phase 6 — VSCode Extension
- TypeScript LSP client (`vscode-languageclient`)
- Status bar: real-time violation count with color coding
- D3.js heatmap dashboard (`WebviewPanel`)
- Commands: lint file, lint workspace, fix all, generate config, restart server

### Phase 7 — Build System
- Full CMake 3.22+ with optional components
- FetchContent for GoogleTest and pybind11
- Conditional LLVM/Clang, ONNX Runtime, Python bindings
- `install()` targets for binary, libraries, headers

## Build

```bash
# Minimal build (core + LSP + CLI)
cmake -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j$(nproc)

# Full build with all features
cmake -B build \
  -DCMAKE_BUILD_TYPE=Release \
  -DVIETLINT_ENABLE_AVX2=ON \
  -DVIETLINT_ENABLE_CLANG=ON \
  -DVIETLINT_LLVM_DIR=/usr/lib/llvm-17 \
  -DVIETLINT_ENABLE_ONNX=ON \
  -DVIETLINT_ONNX_DIR=/opt/onnxruntime \
  -DVIETLINT_ENABLE_PYTHON=ON \
  -DVIETLINT_ENABLE_TESTS=ON
cmake --build build -j$(nproc)
ctest --test-dir build --output-on-failure
```

## Usage

```bash
# Lint Python files
vietlint lint src/*.py --format=text

# Lint with JSON output
vietlint lint src/ --format=json > violations.json

# SARIF for GitHub Code Scanning
vietlint lint src/ --format=sarif > results.sarif

# Auto-fix violations
vietlint lint src/ --fix

# Run as LSP server (for editor integration)
vietlint lsp

# Classify identifiers from stdin
echo -e "tênKhách\ngetUser\nso_luong" | vietlint classify

# Detect file encoding
vietlint detect legacy_code.c

# Generate project config
vietlint init
```

## Python API

```python
import vietlint_core as vl

# Lint source string
engine = vl.LintEngine({})
diags = engine.lint_source("def tênKhách(): pass", "test.py")
for d in diags:
    print(f"{d['line']}:{d['col']} [{d['rule_id']}] {d['message']}")

# Classify identifiers
clf = vl.IdentifierClassifier()
result = clf.classify("quanLyKhachHang")
print(result['class_name'], result['confidence'])

# Detect encoding
det = vl.EncodingDetector()
enc = det.detect(open("file.c","rb").read())
print(enc['charset'], enc['confidence'])
```

## Rules

| ID      | Name                         | Default  | Description |
|---------|------------------------------|----------|-------------|
| VL001   | Vietnamese Identifier        | warning  | Identifier contains Vietnamese Unicode chars |
| VL002   | Mixed-Language Comment       | info     | Comment mixes Vietnamese and English |
| VL003   | Encoding Declaration         | info     | Missing/invalid encoding declaration (Python) |
| VL004   | Transliteration              | info     | ASCII identifier matches Vietnamese word |
| VL005   | Unicode Normalization        | error    | NFD (decomposed) Vietnamese chars — use NFC |
| VL006   | Consistent Naming Style      | info     | Inconsistent naming convention in file |
| VL007   | Vietnamese String Literal    | info     | Vietnamese text in string literals (i18n) |

## Plugin API

```cpp
// mycheck.cpp
#include <vietlint/emitter.hpp>
#include <vietlint/rule_engine.hpp>

extern "C" void vietlint_register(vietlint::PluginContext* ctx) {
    ctx->registry->register_rule({
        "MY-001", "My Custom Rule", "Description", "category",
        vietlint::Severity::Warning,
        [](const vietlint::RuleContext& ctx) -> vietlint::RuleResult {
            vietlint::RuleResult violations;
            // ... check ctx.tokens ...
            return violations;
        }
    });
}
```

```toml
# .vietlint.toml
[lint]
plugin_paths = ["plugins/"]
```

## License

MIT © VietLint Contributors
