#!/usr/bin/env python3
"""
fetch_grammars.py — Download tree-sitter WASM grammar files for VietLint.

Usage:
    python3 scripts/fetch_grammars.py                    # fetch all
    python3 scripts/fetch_grammars.py --langs python go  # fetch specific
    python3 scripts/fetch_grammars.py --list             # list available
    python3 scripts/fetch_grammars.py --dir ./grammars   # custom output dir

The script tries multiple CDN sources in order:
  1. Unpkg (npm CDN) — latest stable
  2. jsDelivr (npm CDN mirror)
  3. GitHub Releases (web-tree-sitter release assets)
"""

import argparse
import hashlib
import json
import sys
import time
import urllib.request
import urllib.error
from pathlib import Path

# ---------------------------------------------------------------------------
# Grammar definitions
# Each entry: (language_id, npm_package, wasm_filename, version)
# ---------------------------------------------------------------------------
GRAMMARS = [
    ("c",          "tree-sitter-c",          "tree-sitter-c.wasm",          "0.21.4"),
    ("cpp",        "tree-sitter-cpp",        "tree-sitter-cpp.wasm",        "0.22.3"),
    ("c_sharp",    "tree-sitter-c-sharp",    "tree-sitter-c_sharp.wasm",    "0.21.3"),
    ("css",        "tree-sitter-css",        "tree-sitter-css.wasm",        "0.21.3"),
    ("go",         "tree-sitter-go",         "tree-sitter-go.wasm",         "0.21.0"),
    ("java",       "tree-sitter-java",       "tree-sitter-java.wasm",       "0.21.0"),
    ("javascript", "tree-sitter-javascript", "tree-sitter-javascript.wasm", "0.21.4"),
    ("jsdoc",      "tree-sitter-jsdoc",      "tree-sitter-jsdoc.wasm",      "0.21.4"),
    ("json",       "tree-sitter-json",       "tree-sitter-json.wasm",       "0.21.0"),
    ("julia",      "tree-sitter-julia",      "tree-sitter-julia.wasm",      "0.21.1"),
    ("ocaml",      "tree-sitter-ocaml",      "tree-sitter-ocaml.wasm",      "0.22.0"),
    ("php",        "tree-sitter-php",        "tree-sitter-php.wasm",        "0.22.8"),
    ("python",     "tree-sitter-python",     "tree-sitter-python.wasm",     "0.21.0"),
    ("ql",         "tree-sitter-ql",         "tree-sitter-ql.wasm",         "0.3.3"),
    ("rust",       "tree-sitter-rust",       "tree-sitter-rust.wasm",       "0.21.2"),
    ("scala",      "tree-sitter-scala",      "tree-sitter-scala.wasm",      "0.21.0"),
    ("tsx",        "tree-sitter-typescript", "tree-sitter-tsx.wasm",        "0.21.2"),
    ("typescript", "tree-sitter-typescript", "tree-sitter-typescript.wasm", "0.21.2"),
]

# Main tree-sitter runtime WASM
RUNTIME = ("web-tree-sitter", "tree-sitter.wasm", "0.22.6")

# ---------------------------------------------------------------------------
# CDN URL builders
# ---------------------------------------------------------------------------
def unpkg_url(package: str, version: str, filename: str) -> str:
    return f"https://unpkg.com/{package}@{version}/{filename}"

def jsdelivr_url(package: str, version: str, filename: str) -> str:
    return f"https://cdn.jsdelivr.net/npm/{package}@{version}/{filename}"

def github_url(package: str, version: str, filename: str) -> str:
    # e.g. https://github.com/tree-sitter/tree-sitter-python/releases/download/v0.21.0/tree-sitter-python.wasm
    repo = package.replace("tree-sitter-", "")
    return f"https://github.com/tree-sitter/tree-sitter-{repo}/releases/download/v{version}/{filename}"

SOURCES = [unpkg_url, jsdelivr_url, github_url]

# ---------------------------------------------------------------------------
def fetch_url(url: str, timeout: int = 30) -> bytes | None:
    """Fetch URL, return bytes or None on error."""
    try:
        req = urllib.request.Request(url, headers={"User-Agent": "vietlint-fetch/1.0"})
        with urllib.request.urlopen(req, timeout=timeout) as resp:
            if resp.status == 200:
                return resp.read()
    except (urllib.error.URLError, urllib.error.HTTPError, OSError):
        pass
    return None

def download_grammar(lang_id: str, package: str, filename: str,
                     version: str, out_dir: Path, verbose: bool) -> bool:
    """Try each CDN source until one succeeds."""
    out_path = out_dir / filename
    if out_path.exists():
        if verbose:
            print(f"  [skip] {filename} already exists")
        return True

    for source_fn in SOURCES:
        url = source_fn(package, version, filename)
        if verbose:
            print(f"  [try]  {url}")
        data = fetch_url(url)
        if data and data[:4] == b'\x00asm':  # WASM magic bytes
            out_path.write_bytes(data)
            size_kb = len(data) / 1024
            print(f"  [ok]   {filename} ({size_kb:.0f} KB) from {url.split('/')[2]}")
            return True
        time.sleep(0.1)  # be polite

    print(f"  [FAIL] {filename} — could not fetch from any source", file=sys.stderr)
    return False

def download_runtime(out_dir: Path, verbose: bool) -> bool:
    """Download the tree-sitter runtime WASM."""
    package, filename, version = RUNTIME
    out_path = out_dir / filename
    if out_path.exists():
        if verbose:
            print(f"  [skip] {filename} already exists")
        return True

    for source_fn in [unpkg_url, jsdelivr_url]:
        url = source_fn(package, version, filename)
        if verbose:
            print(f"  [try]  {url}")
        data = fetch_url(url)
        if data and data[:4] == b'\x00asm':
            out_path.write_bytes(data)
            size_kb = len(data) / 1024
            print(f"  [ok]   {filename} ({size_kb:.0f} KB)")
            return True
        time.sleep(0.1)

    print(f"  [FAIL] {filename}", file=sys.stderr)
    return False

def generate_manifest(out_dir: Path) -> None:
    """Write a manifest.json listing all present grammars."""
    manifest = []
    for lang_id, package, filename, version in GRAMMARS:
        p = out_dir / filename
        if p.exists():
            manifest.append({
                "lang": lang_id,
                "file": filename,
                "version": version,
                "size": p.stat().st_size,
                "sha256": hashlib.sha256(p.read_bytes()).hexdigest(),
            })
    manifest_path = out_dir / "manifest.json"
    manifest_path.write_text(json.dumps(manifest, indent=2))
    print(f"\n[manifest] written to {manifest_path}  ({len(manifest)} grammars)")

# ---------------------------------------------------------------------------
def main():
    parser = argparse.ArgumentParser(description="Fetch tree-sitter WASM grammars")
    parser.add_argument("--dir",    default="grammars", help="Output directory (default: grammars/)")
    parser.add_argument("--langs",  nargs="*",          help="Languages to fetch (default: all)")
    parser.add_argument("--list",   action="store_true",help="List available grammars and exit")
    parser.add_argument("--no-runtime", action="store_true", help="Skip tree-sitter.wasm runtime")
    parser.add_argument("--verbose", "-v", action="store_true")
    args = parser.parse_args()

    if args.list:
        print("Available grammars:")
        for lang_id, pkg, fname, ver in GRAMMARS:
            print(f"  {lang_id:12s}  {pkg}@{ver}  →  {fname}")
        return

    out_dir = Path(args.dir)
    out_dir.mkdir(parents=True, exist_ok=True)
    print(f"Output directory: {out_dir.resolve()}\n")

    # Filter grammars
    selected = GRAMMARS
    if args.langs:
        selected = [g for g in GRAMMARS if g[0] in args.langs]
        if not selected:
            print(f"No grammars matched: {args.langs}", file=sys.stderr)
            sys.exit(1)

    # Download runtime
    if not args.no_runtime:
        print("Fetching tree-sitter runtime...")
        download_runtime(out_dir, args.verbose)
        print()

    # Download grammars
    ok = 0
    fail = 0
    print(f"Fetching {len(selected)} grammar(s)...")
    for lang_id, package, filename, version in selected:
        print(f"\n[{lang_id}]")
        if download_grammar(lang_id, package, filename, version, out_dir, args.verbose):
            ok += 1
        else:
            fail += 1

    generate_manifest(out_dir)
    print(f"\nDone: {ok} ok, {fail} failed")
    if fail:
        sys.exit(1)

if __name__ == "__main__":
    main()
