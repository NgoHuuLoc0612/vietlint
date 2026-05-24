# VietLint

**The first dedicated linting system for Vietnamese coding conventions.**

[![License: MIT](https://img.shields.io/badge/License-MIT-yellow.svg)](https://opensource.org/licenses/MIT)
[![C++20](https://img.shields.io/badge/C%2B%2B-20-blue.svg)](https://en.cppreference.com/w/cpp/20)
[![Build](https://img.shields.io/badge/build-passing-brightgreen.svg)]()
[![Platform](https://img.shields.io/badge/platform-Linux%20%7C%20macOS%20%7C%20Windows-lightgrey.svg)]()

---

## Overview

VietLint is a static analysis tool designed to detect and report violations of Vietnamese coding conventions in source code. It addresses a problem that has long been overlooked in the Vietnamese software engineering community: the lack of tooling to enforce consistent, professional naming standards when writing code that inevitably intersects with the Vietnamese language.

Unlike general-purpose linters that focus on syntax errors or code style, VietLint specifically targets the unique challenges that arise when Vietnamese-speaking developers write code — whether that means identifiers containing Vietnamese Unicode characters, transliterated Vietnamese words written without diacritics, or mixed-language naming patterns that reduce code clarity and international maintainability.

VietLint provides a command-line interface, a Language Server Protocol (LSP) implementation for editor integration, a REST API backend, and a Python binding — forming a complete, production-grade linting ecosystem.

---

## The Challenge of Vietnamese Code Linting

Building a linter specifically for Vietnamese presents a set of technical challenges that have no precedent in the existing linting ecosystem.

**The Vietnamese writing system is computationally complex.** Vietnamese uses a Latin-based script augmented with diacritical marks, tone markers, and precomposed Unicode characters that span multiple Unicode blocks — including Latin Extended Additional (U+1E00–U+1EFF), Combining Diacritical Marks (U+0300–U+036F), and others. A single Vietnamese character may be represented as a precomposed NFC codepoint or as a decomposed NFD sequence of base character plus combining marks, creating normalization hazards that simple string comparison cannot handle.

**Vietnamese identifiers exist on a spectrum.** The challenge is not simply detecting Unicode — it is classifying identifiers across six distinct categories: pure ASCII English, pure Vietnamese with diacritics, mixed Vietnamese-ASCII, transliterated Vietnamese without diacritics (e.g., `tinhTong` for `tính tổng`), Vietnamese abbreviations, and ambiguous cases. A heuristic approach fails on transliterated identifiers, which appear as ordinary ASCII strings to conventional tools yet carry Vietnamese semantic meaning.

**The transliteration problem is particularly hard.** Vietnamese developers frequently write identifiers like `nguoiDung`, `matKhau`, `soLuong`, or `tenKhach` — these are Vietnamese words stripped of their diacritical marks, a common practice that produces code that is semantically Vietnamese but syntactically indistinguishable from ASCII. Detecting these requires either an exhaustive word list, a trained machine learning model, or both.

**False positives must be rigorously controlled.** A linter that flags common English words like `list`, `token`, `database`, or `manager` as Vietnamese transliterations is worse than no linter at all. Achieving high recall on genuine violations while maintaining high precision against English identifiers requires a calibrated ML classifier backed by real-world corpus data.

VietLint addresses all of these challenges through a combination of AVX2 SIMD Unicode scanning, a 64-dimensional feature extractor, a GradientBoosting ONNX model trained on approximately 40,000 real-world identifiers scraped from Vietnamese GitHub repositories, and a carefully maintained whitelist of common English programming terms.

---

## Features

### Core Engine (C++20)
- **AVX2 SIMD UTF-8 scanner** — processes 32 bytes per cycle for high-throughput Vietnamese Unicode detection
- **Multi-language lexer** — tokenizes Python, JavaScript/TypeScript, and C/C++ source files
- **Vietnamese identifier classifier** — six-class heuristic classifier with regex and word-list matching
- **Mixed-language comment detector** — flags comments that switch between Vietnamese and English
- **Encoding detector** — identifies UTF-8, TCVN3, VNI, and Windows-1258 encodings with conversion support
- **Rule engine** — eight built-in rules (VL000–VL007), fully configurable via `.vietlint.toml`
- **Diagnostic emitter** — outputs in plain text, JSON, SARIF, GCC-compatible, LSP, and GitHub Actions formats
- **Plugin loader** — supports custom rules as shared libraries via `dlopen`/`LoadLibrary`

### Machine Learning Layer
- **64-dimensional feature extractor** — Unicode character ratios, bigram/trigram n-gram hashes, naming style flags
- **ONNX Runtime inference** — GradientBoosting classifier with F1-macro score of 0.98 across five classes
- **Training pipeline** — GitHub corpus scraper, oversampling for minority classes, sklearn-to-ONNX export
- **Automatic fallback** — gracefully degrades to heuristic classifier when no ONNX model is present
- **Python binding via pybind11** — full NumPy-compatible API for programmatic access

### Language Server Protocol
- Full JSON-RPC 2.0 implementation over stdio
- Supports `initialize`, `shutdown`, `exit`, `textDocument/didOpen`, `textDocument/didChange` (incremental)
- Real-time `textDocument/publishDiagnostics` on background thread
- `textDocument/hover` — inline rule documentation
- `textDocument/codeAction` — quick-fix suggestions with `WorkspaceEdit`

### REST API Backend
- `POST /api/v1/lint` — lint source code from request body
- `POST /api/v1/lint/file` — lint a file by path
- `GET /api/v1/lint/stream` — Server-Sent Events for streaming results
- `WebSocket /ws/lint` — real-time as-you-type linting
- `GET /api/v1/projects`, `POST /api/v1/projects` — project management
- `GET /api/v1/rules` — enumerate all registered rules
- SQLite-backed violation history with per-project statistics

---

## Rules

| ID    | Name                    | Default  | Description |
|-------|-------------------------|----------|-------------|
| VL000 | Parse Error             | error    | Source file could not be parsed |
| VL001 | Vietnamese Identifier   | warning  | Identifier contains Vietnamese Unicode characters |
| VL002 | Mixed-Language Comment  | warning  | Comment mixes Vietnamese and English text |
| VL003 | Encoding Declaration    | info     | Missing or invalid encoding declaration in Python files |
| VL004 | Transliteration         | warning  | Identifier is a transliterated Vietnamese word (e.g., `tinhTong`, `soLuong`) |
| VL005 | Unicode Normalization   | error    | Identifier uses NFD decomposed Vietnamese characters; NFC required |
| VL006 | Consistent Naming Style | info     | File uses inconsistent naming conventions (camelCase vs snake_case) |
| VL007 | Vietnamese String Literal | info   | String literal contains Vietnamese text that should be externalized for i18n |

---

## Project Structure

```
vietlint/
├── include/vietlint/           C++20 public headers
│   ├── utf8_scanner.hpp        AVX2 SIMD UTF-8 scanner
│   ├── lexer.hpp               Multi-language tokenizer
│   ├── classifier.hpp          Vietnamese identifier classifier
│   ├── encoding_detector.hpp   Encoding detection and conversion
│   ├── rule_engine.hpp         Rule engine and TOML configuration
│   ├── emitter.hpp             Diagnostic formatter
│   ├── ml_classifier.hpp       ONNX Runtime ML layer
│   ├── lsp_server.hpp          LSP server (JSON-RPC over stdio)
│   └── clang_visitor.hpp       Clang LibTooling AST visitor
│
├── src/
│   ├── core/                   Core engine implementation
│   ├── llvm/                   Clang AST integration
│   ├── ml/                     ML layer and pybind11 bridge
│   └── lsp/                    LSP server implementation
│
├── python/
│   └── backend.py              FastAPI REST API and WebSocket backend
│
├── scripts/
│   └── collect_corpus.py       Training data pipeline and corpus collector
│
├── tests/                      Unit tests
├── cmake/                      CMake modules and plugin example
└── CMakeLists.txt              Build system
```

---

## Building from Source

### Prerequisites

- CMake 3.22 or later
- GCC 12+ or Clang 14+ with C++20 support
- (Optional) LLVM/Clang development headers for C++ AST analysis
- (Optional) ONNX Runtime for ML inference
- (Optional) Python 3.10+ with pybind11 for Python bindings

### Minimal Build

```bash
git clone https://github.com/NgoHuuLoc0612/vietlint.git
cd vietlint

cmake -B build \
  -DCMAKE_BUILD_TYPE=Release \
  -DVIETLINT_ENABLE_AVX2=OFF

cmake --build build -j$(nproc)
```

### Full Build with ONNX and Python

```bash
cmake -B build \
  -DCMAKE_BUILD_TYPE=Release \
  -DVIETLINT_ENABLE_AVX2=ON \
  -DVIETLINT_ENABLE_PYTHON=ON \
  -DVIETLINT_ENABLE_ONNX=ON \
  -DVIETLINT_ONNX_DIR=/path/to/onnxruntime \
  -DVIETLINT_ENABLE_TREE_SITTER=ON

cmake --build build -j$(nproc)
ctest --test-dir build --output-on-failure
```

### Training the ML Classifier

```bash
# Collect corpus from Vietnamese GitHub repositories (requires token)
python3 scripts/collect_corpus.py \
  --token YOUR_GITHUB_TOKEN \
  --max-repos 100 \
  --output corpus

# Generate and run training script
python3 scripts/collect_corpus.py --write-train-script --output corpus

PYTHONPATH=build/python python3 corpus/train_model.py \
  --corpus corpus/corpus_raw.jsonl \
  --output model.onnx
```

---

## Usage

### Command-Line Interface

```bash
# Lint Python files using text output
vietlint lint src/*.py

# Output as JSON
vietlint lint src/ --format=json > violations.json

# Output as SARIF for GitHub Code Scanning
vietlint lint src/ --format=sarif > results.sarif

# Use a specific ONNX model for enhanced transliteration detection
vietlint lint src/ --model=model.onnx

# Classify identifiers interactively
echo -e "tinhTong\ngetUser\nsoLuong" | vietlint classify --model=model.onnx

# Detect file encoding
vietlint detect legacy_file.c

# Generate a default configuration file
vietlint init

# Run as an LSP server
vietlint lsp
```

### Configuration

VietLint reads `.vietlint.toml` from the project root or a path specified with `--config`.

```toml
[lint]
fail_on_warning  = false
max_violations   = 0
target_encoding  = "utf-8"
model            = "model.onnx"

[rules.VL001]
enabled  = true
severity = "warning"

[rules.VL004]
enabled  = true
severity = "warning"

[rules.VL007]
enabled  = false
```

### Python API

```python
import sys
sys.path.insert(0, "build/python")
import vietlint_core as vl

# Lint a source string
engine = vl.LintEngine({})
diagnostics = engine.lint_source(
    "def tinhTong(soA, soB):\n    ketQua = soA + soB\n    return ketQua",
    "example.py"
)
for d in diagnostics:
    print(f"{d['line']}:{d['col']} [{d['rule_id']}] {d['message']}")

# Classify an identifier
clf = vl.IdentifierClassifier("model.onnx")
result = clf.classify("quanLyKhachHang")
print(result["class_name"], result["confidence"])

# Detect encoding
detector = vl.EncodingDetector()
encoding = detector.detect(open("legacy.c", "rb").read())
print(encoding["charset"], encoding["confidence"])
```

### REST API

Start the backend server:

```bash
PYTHONPATH=build/python python3 -m uvicorn vietlint.backend:app \
  --host 0.0.0.0 --port 8765
```

Lint a code snippet:

```bash
curl -X POST http://localhost:8765/api/v1/lint \
  -H "Content-Type: application/json" \
  -d '{"source": "def tinhTong(soA, soB): pass", "filename": "example.py"}'
```

Interactive API documentation is available at `http://localhost:8765/docs`.

### Plugin API

Custom rules can be written in C++ and loaded as shared libraries:

```cpp
#include <vietlint/emitter.hpp>
#include <vietlint/rule_engine.hpp>

extern "C" void vietlint_register(vietlint::PluginContext* ctx) {
    ctx->registry->register_rule({
        "MY-001", "My Custom Rule",
        "Description of the rule", "category",
        vietlint::Severity::Warning,
        [](const vietlint::RuleContext& ctx) -> vietlint::RuleResult {
            vietlint::RuleResult violations;
            for (const auto& tok : ctx.tokens) {
                // implement rule logic here
            }
            return violations;
        }
    });
}
```

Register the plugin in `.vietlint.toml`:

```toml
[lint]
plugin_paths = ["plugins/"]
```

---

---

## Roadmap

The following improvements are planned for future releases:

- Additional rules (VL008–VL015) covering inconsistent transliteration within a file, Vietnamese text in test assertions, and documentation coverage
- Implementation of `--fix` for automatic in-place correction of detected violations
- Editor integration via LSP for Neovim, Helix, and other LSP-compatible editors
- PyPI package for `pip install vietlint`
- GitHub Actions workflow for continuous integration
- Web-based dashboard built on the existing FastAPI backend
- Expanded corpus with 500+ Vietnamese repositories for improved ML accuracy

---

## Limitations

VietLint is the first tool of its kind, and as such, it carries limitations that are openly acknowledged.

**Rule coverage is narrow.** With eight rules (VL000–VL007), VietLint covers the most common Vietnamese convention violations but does not approach the breadth of mature general-purpose linters such as Pylint or ESLint. Many edge cases in real-world Vietnamese codebases are not yet addressed.

**Transliteration detection has inherent ambiguity.** The five-class ML classifier achieves an F1-macro score of 0.98 on the training corpus, but real-world performance depends heavily on naming style, programming domain, and how closely the input resembles the training data. Short identifiers of two to four characters are particularly difficult to classify reliably, as they share feature space with common English abbreviations.

**The training corpus is skewed toward Python.** The approximately 40,000 identifiers in the current corpus were scraped primarily from Python repositories on GitHub. Identifier patterns in JavaScript, C++, Java, or Go may differ in ways that reduce classifier accuracy for those languages.

**False positives on domain-specific English terms are possible.** The English whitelist covers common programming vocabulary, but domain-specific terms — particularly in fields such as medicine, finance, or engineering — may occasionally be misclassified as transliterated Vietnamese if they share phonetic patterns with Vietnamese words.

**The LSP server has not been tested extensively under load.** The LSP implementation is functional for single-file and small workspace use, but its behavior under large monorepos or with many concurrent file changes has not been benchmarked.

**Windows support is untested.** The build system targets Linux and macOS. Windows builds via MSVC or MinGW are theoretically supported by CMake but have not been validated.

**The REST API backend has no authentication.** The FastAPI backend is intended for local development use. It should not be exposed to a public network without adding appropriate authentication and rate-limiting middleware.

---

## Contributing

Contributions of all kinds are welcome, including bug reports, rule proposals, corpus data, documentation improvements, and pull requests.

This project is an active work in progress. I am committed to improving VietLint continuously, and I acknowledge that as the first tool of its kind, there are bound to be areas where accuracy, coverage, or usability falls short of expectations.

**If you encounter a false positive, a missed violation, a build issue, or any unexpected behavior, please open an issue.** I will address reported problems as promptly as possible. Your feedback is genuinely valuable and directly shapes the direction of this project.

When filing a bug report, please include:
- The VietLint version (`vietlint version`)
- The operating system and compiler version
- A minimal reproducible example of the source file and the unexpected output

---

## License

MIT © VietLint Contributors

---

*VietLint is, to the best of the author's knowledge, the first static analysis tool designed specifically for Vietnamese coding conventions. It was built out of a genuine need felt by Vietnamese software engineers who wanted better tooling for their community — and a belief that the Vietnamese developer ecosystem deserves the same quality of tooling available in other languages.*
