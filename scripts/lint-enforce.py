#!/usr/bin/env python3
"""Lint checks for OCAP Enforce Script (.c) files and addon project structure.

Since no standalone linter or CI-compatible compiler exists for Enforce Script,
this script provides structural and pattern-based checks:
  - addon.gproj validation (valid JSON, required fields)
  - Expected source files exist
  - Balanced braces in .c files
  - Trailing whitespace detection
  - JSON string values built without EscapeJson()
"""

import json
import re
import sys
from pathlib import Path

ADDON_DIR = Path(__file__).resolve().parent.parent / "addon"
SCRIPTS_DIR = ADDON_DIR / "Scripts" / "Game" / "OCAP"

EXPECTED_FILES = [
    "OCAP_GameModeComponent.c",
    "OCAP_CaptureManager.c",
    "OCAP_EventManager.c",
    "OCAP_Session.c",
    "OCAP_TransportService.c",
    "OCAP_Types.c",
]

GPROJ_REQUIRED_FIELDS = ["guid", "type", "name", "version", "dependencies"]

errors = []
warnings = []


def error(msg):
    errors.append(msg)


def warn(msg):
    warnings.append(msg)


# ---------------------------------------------------------------------------
# Checks
# ---------------------------------------------------------------------------

def check_gproj():
    """Validate addon.gproj is valid JSON with required fields."""
    gproj = ADDON_DIR / "addon.gproj"
    if not gproj.exists():
        error("addon.gproj: file not found")
        return

    try:
        data = json.loads(gproj.read_text(encoding="utf-8"))
    except (json.JSONDecodeError, IOError) as e:
        error(f"addon.gproj: failed to read or parse — {e}")
        return

    for field in GPROJ_REQUIRED_FIELDS:
        if field not in data:
            error(f"addon.gproj: missing required field '{field}'")


def check_file_structure():
    """Verify all expected source files exist."""
    for f in EXPECTED_FILES:
        if not (SCRIPTS_DIR / f).exists():
            error(f"{f}: expected file not found in Scripts/Game/OCAP/")


def check_brace_balance(filepath):
    """Check that braces are balanced, ignoring strings and comments."""
    text = filepath.read_text(encoding="utf-8")
    depth = 0
    in_string = False
    in_line_comment = False
    in_block_comment = False
    prev = ""

    for i, ch in enumerate(text):
        if in_line_comment:
            if ch == "\n":
                in_line_comment = False
            prev = ch
            continue

        if in_block_comment:
            if prev == "*" and ch == "/":
                in_block_comment = False
            prev = ch
            continue

        if ch == '"' and prev != "\\":
            in_string = not in_string
            prev = ch
            continue

        if in_string:
            prev = ch
            continue

        if ch == "/" and i + 1 < len(text):
            nxt = text[i + 1]
            if nxt == "/":
                in_line_comment = True
                prev = ch
                continue
            if nxt == "*":
                in_block_comment = True
                prev = ch
                continue

        if ch == "{":
            depth += 1
        elif ch == "}":
            depth -= 1

        prev = ch

    if depth != 0:
        error(f"{filepath.name}: unbalanced braces (depth off by {depth})")


def check_trailing_whitespace(filepath):
    """Check for trailing whitespace on non-empty lines."""
    lines = filepath.read_text(encoding="utf-8").splitlines()
    flagged = []
    for i, line in enumerate(lines, 1):
        if line and line != line.rstrip():
            flagged.append(i)

    if flagged:
        locations = ", ".join(str(n) for n in flagged[:5])
        suffix = f" (and {len(flagged) - 5} more)" if len(flagged) > 5 else ""
        warn(f"{filepath.name}: trailing whitespace on line(s) {locations}{suffix}")


def check_escape_json(filepath):
    """Warn when variables are interpolated into JSON strings without EscapeJson().

    Looks for patterns like:  \\"" + variable + "\\""
    where variable does not pass through OCAP_Util.EscapeJson().
    """
    lines = filepath.read_text(encoding="utf-8").splitlines()

    for i, line in enumerate(lines, 1):
        # Only inspect lines that build json/body strings
        if not re.search(r"(json|body)\s*\+?=", line, re.IGNORECASE):
            continue

        # Find  + <expr> +  segments
        for m in re.finditer(r"\+\s*([^+]+?)\s*\+", line):
            expr = m.group(1).strip()

            # Skip safe patterns
            if "EscapeJson" in expr:
                continue
            if "ToString" in expr:
                continue
            if expr.startswith('"'):
                continue
            if expr.isdigit():
                continue

            # Check whether this expression sits between escaped-quote boundaries
            before = line[: m.start()]
            after = line[m.end() :]
            if '\\"' in before[-15:] and '\\"' in after[:15]:
                warn(
                    f'{filepath.name}:{i}: string variable `{expr}` inserted into '
                    f"JSON without EscapeJson()"
                )


# ---------------------------------------------------------------------------
# Main
# ---------------------------------------------------------------------------

def main():
    print("=== OCAP Addon Lint ===\n")

    print("Checking addon.gproj...")
    check_gproj()

    print("Checking file structure...")
    check_file_structure()

    c_files = sorted(SCRIPTS_DIR.glob("*.c"))
    print(f"Checking {len(c_files)} Enforce Script file(s)...\n")

    for f in c_files:
        check_brace_balance(f)
        check_trailing_whitespace(f)
        check_escape_json(f)

    if warnings:
        print(f"Warnings ({len(warnings)}):")
        for w in warnings:
            print(f"  {w}")
        print()

    if errors:
        print(f"Errors ({len(errors)}):")
        for e in errors:
            print(f"  {e}")
        print()
        print("FAILED")
        return 1

    print("PASSED" + (f" ({len(warnings)} warning(s))" if warnings else ""))
    return 0


if __name__ == "__main__":
    sys.exit(main())
