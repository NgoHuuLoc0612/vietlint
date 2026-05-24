#!/bin/bash
set -e
TMPDIR=$(mktemp -d)
mkdir -p $TMPDIR/vietlint

cp vietlint/__init__.py $TMPDIR/vietlint/
cp vietlint/backend.py $TMPDIR/vietlint/
cp pyproject.toml $TMPDIR/
cp README.md $TMPDIR/
cp setup.py $TMPDIR/

cd $TMPDIR
python3 -m build --wheel --no-isolation
cp dist/*.whl $(cd - && pwd)/dist/
rm -rf $TMPDIR
echo "Done: dist/vietlint-1.0.0-py3-none-any.whl"
