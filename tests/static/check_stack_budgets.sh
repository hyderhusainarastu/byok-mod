#!/usr/bin/env bash
# tests/static/check_stack_budgets.sh -- thin wrapper around
# check_stack_budgets.py (the actual check; see that file's own header for
# what it enforces and why). A .sh entry point exists alongside the .py one
# because tests/README.md's "run everything" recipe is shell, and this
# needs no venv/dependency beyond plain python3.
set -euo pipefail
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
exec python3 "$SCRIPT_DIR/check_stack_budgets.py"
