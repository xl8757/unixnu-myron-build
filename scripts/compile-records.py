#!/usr/bin/env python3
"""Record every XNU compile as <object>.json and then run the real compiler.

`scripts/build-arm-driver.py` and `scripts/check-native-arch-timer-checkpoints.py`
both read a per-object compile recipe, `{directory, file, arguments}`, to
rebuild one translation unit with the flags the kernel build actually used.
Nothing in the tree produces those records on Linux: the objects and their .cpd
dependency files appear, but no .json does, and makedefs has no rule for one.
`BUILD_JSON_COMPILATION_DATABASE` is a different mode that replaces compilation
outright instead of recording it.

MakeInc.def compiles every kernel object through `$(CCACHE) $(CC)`, and nothing
defines CCACHE. Pointing it at this script fills the gap without touching the
build:

    CCACHE="python3 /path/to/compile-records.py" ./unixnu build kernel -D ../device

The script is a transparent wrapper: it writes the record and then execs the
command it was given, so a failing compile still fails and the record matches
the invocation that produced the object.

XNU's C rule compiles without -o and lets clang derive the object name from the
source. Replay consumers do not all handle that: build-mockfs-fixture.py and
build-libpthread-kernel.py both substitute -o on the assumption that the record
carries one, and without it clang writes the object into the record's working
directory and the consumer then fails to find its own destination path. The
record therefore states the output explicitly when the invocation leaves it
implicit. -o <stem>.o in the record's directory is what clang does anyway, so
replaying the recorded arguments still produces the same object; only the
omitted flag is made explicit.
"""
from __future__ import annotations

import json
import os
import sys


def output_path(arguments: list[str]) -> str | None:
    for index, value in enumerate(arguments):
        if value == '-o' and index + 1 < len(arguments):
            return arguments[index + 1]
        if value.startswith('-o') and len(value) > 2:
            return value[2:]
    return None


def input_file(arguments: list[str]) -> str | None:
    for value in arguments:
        if value == '-c' or value == '-x':
            continue
        if value.endswith(('.c', '.cc', '.cpp', '.S', '.s', '.sx')):
            return value
    return None


def inferred_output(source: str) -> str:
    stem = os.path.basename(source)
    return os.path.splitext(stem)[0] + '.o'


def main() -> int:
    arguments = sys.argv[1:]
    if not arguments:
        return os.execvp('true', ['true'])

    source = input_file(arguments)
    output = output_path(arguments)
    recorded = arguments
    if source and not output:
        output = inferred_output(source)
        recorded = [*arguments, '-o', output]
    if output and source:
        record = {
            'directory': os.getcwd(),
            'file': source,
            'arguments': recorded,
        }
        target = f'{output}.json'
        try:
            # The build reads these with .rstrip(',\\n'), so a trailing
            # separator is accepted and a plain object is too.
            with open(target, 'w', encoding='utf-8') as handle:
                json.dump(record, handle)
        except OSError as error:
            print(f'compile-records: cannot write {target}: {error}', file=sys.stderr)

    os.execvp(arguments[0], arguments)


if __name__ == '__main__':
    raise SystemExit(main())
