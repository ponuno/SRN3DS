"""Report test files that tools/run_host_tests.ps1 never registers.

Scans ``tests/*.c``, ``tests/*.py`` and ``source/shader_packs/*/test_*.py`` and
checks each path against the runner's code (comments do not count). Python
files get an extra column: whether the path is actually invoked from
``$pyTests``, including conditional ``$pyTests += ...`` entries. The scan is
read-only: it never edits ``run_host_tests.ps1``.

Use ``--strict`` in CI to exit 1 when any test file is unregistered.
"""
import argparse
import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
RUNNER = ROOT / "tools" / "run_host_tests.ps1"

ASSIGN_PYTESTS = re.compile(r"\$pyTests\s*(?:\+=|=)\s*")
QUOTED = re.compile(r'"([^"\r\n]+)"|\'([^\'\r\n]+)\'')
TEST_GLOBS = (
    ("tests/*.c", "C"),
    ("tests/*.py", "PY"),
    ("source/shader_packs/*/test_*.py", "PY"),
)


def rel(path):
    """Repo-relative POSIX path, the form used inside run_host_tests.ps1."""
    return path.relative_to(ROOT).as_posix()


def strip_comments(text):
    """Drop PowerShell ``#`` and ``<# ... #>`` comments, but not quoted ``#``."""
    out = []
    in_block = False
    for line in text.splitlines(keepends=True):
        if in_block:
            if "#>" in line:
                line = line.split("#>", 1)[1]
                in_block = False
            else:
                continue
        quote = None
        kept = []
        for ch in line:
            if quote:
                kept.append(ch)
                if ch == quote:
                    quote = None
            elif ch in "'\"":
                quote = ch
                kept.append(ch)
            elif ch == "#":
                break
            else:
                kept.append(ch)
        line = "".join(kept)
        if "<#" in line:
            before, _, after = line.partition("<#")
            if "#>" in after:
                line = before + after.split("#>", 1)[1]
            else:
                line = before
                in_block = True
        out.append(line)
    return "".join(out)


def quoted_strings(text):
    """String literals in ``text``, normalised to POSIX separators."""
    return {(first or second).replace("\\", "/")
            for first, second in QUOTED.findall(text)}


def pytest_scripts(code):
    """Union of ``.py`` strings assigned/appended to ``$pyTests``.

    A union is deliberate: ``$pyTests = @()`` under ``-SkipPython`` must not
    erase the real list from the report.
    """
    scripts = set()
    for match in ASSIGN_PYTESTS.finditer(code):
        tail = code[match.end():].lstrip()
        if tail.startswith("@("):
            depth = 0
            end = len(tail)
            for i, ch in enumerate(tail):
                if ch == "(":
                    depth += 1
                elif ch == ")":
                    depth -= 1
                    if depth == 0:
                        end = i + 1
                        break
            body = tail[:end]
        else:
            body = tail.split("\n", 1)[0]
        for value in quoted_strings(body):
            if value.endswith(".py"):
                scripts.add(value)
    return scripts


def collect_files():
    files = []
    for pattern, kind in TEST_GLOBS:
        for path in sorted(ROOT.glob(pattern)):
            if path.is_file():
                files.append((rel(path), kind))
    return files


def scan():
    code = strip_comments(RUNNER.read_text(encoding="utf-8", errors="replace"))
    mentions = quoted_strings(code)
    called = pytest_scripts(code)
    rows = []
    for name, kind in collect_files():
        registered = name in mentions or name in code
        rows.append((name, kind, registered,
                     (name in called) if kind == "PY" else None))
    return rows


def main():
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--strict", action="store_true",
                        help="exit 1 if any test file is unregistered")
    args = parser.parse_args()
    if not RUNNER.is_file():
        print(f"runner not found: {RUNNER}", file=sys.stderr)
        return 2

    rows = scan()
    width = max([len("FILE")] + [len(name) for name, _, _, _ in rows])
    print(f"{'FILE':<{width}}  TYPE  REGISTERED  IN $pyTests")
    for name, kind, registered, in_pytests in rows:
        state = "yes" if registered else "NO"
        py = "-" if in_pytests is None else ("yes" if in_pytests else "NO")
        print(f"{name:<{width}}  {kind:<4}  {state:<10}  {py}")

    missing = [name for name, _, registered, _ in rows if not registered]
    n_py = sum(1 for _, kind, _, _ in rows if kind == "PY")
    print(f"\nScanned {len(rows)} test files ({len(rows) - n_py} C, {n_py} Python): "
          f"{len(rows) - len(missing)} registered, {len(missing)} unregistered")
    if missing:
        print("Unregistered:")
        for name in missing:
            print(f"  {name}")
    return 1 if (args.strict and missing) else 0


if __name__ == "__main__":
    sys.exit(main())
