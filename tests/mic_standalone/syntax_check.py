"""Syntax-check one Apollo source file on a machine that cannot link Apollo.

Usage: python3 tests/mic_standalone/syntax_check.py src/mic.cpp
Requires build/compile_commands.json from an earlier CMake configure.
"""
import json
import os
import shlex
import subprocess
import sys

root = os.path.abspath(os.path.join(os.path.dirname(__file__), "..", ".."))
target = os.path.abspath(sys.argv[1])
with open(os.path.join(root, "build", "compile_commands.json")) as handle:
    entries = json.load(handle)
entry = next(e for e in entries if e["file"].endswith("src/confighttp.cpp"))

args = []
skip = False
for arg in shlex.split(entry["command"]):
    if skip:
        skip = False
        continue
    if arg in ("-o", "-MF", "-MT"):
        skip = True
        continue
    if arg in ("-c", "-MD") or arg.endswith("confighttp.cpp"):
        continue
    args.append(arg)

args += [
    "-I/opt/homebrew/opt/openssl@3/include",
    "-I/opt/homebrew/include",
    "-fsyntax-only",
    target,
]
sys.exit(subprocess.call(args, cwd=entry["directory"]))
