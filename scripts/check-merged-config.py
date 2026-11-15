#!/usr/bin/env python3
"""Assert the merged kernel configuration has the expected values.

`./unixnu defconfig` merges the ordered profile fragments and the device
defconfig into one .config. This checks the merged result rather than the
device defconfig alone, so a value that a profile fragment overrides is caught.

The merged file may carry either the CONFIG_XNU_ prefix from the defconfig
syntax or the bare XNU_ name that the kernel build consumes, and a false value
may be written as n, 0 or "# NAME is not set". All forms are accepted.

Usage:
  python3 scripts/check-merged-config.py CONFIG KEY=VALUE [KEY=VALUE ...]
  python3 scripts/check-merged-config.py CONFIG --expect-file FILE

An expected file has one KEY=VALUE per line; blank lines and # comments are
ignored.
"""
from __future__ import annotations

import re
import sys

SET = re.compile(r'^(?:CONFIG_)?(XNU_[A-Z0-9_]+)=(.+)$')
UNSET = re.compile(r'^#\s*(?:CONFIG_)?(XNU_[A-Z0-9_]+) is not set$')


def parse(path: str) -> dict[str, str]:
    values: dict[str, str] = {}
    for raw in open(path, encoding='utf-8'):
        line = raw.strip()
        unset = UNSET.match(line)
        if unset:
            values[unset.group(1)] = 'n'
            continue
        match = SET.match(line)
        if match:
            values[match.group(1)] = match.group(2).strip().strip('"')
    return values


def expected(args: list[str]) -> dict[str, str]:
    wants: dict[str, str] = {}
    for arg in args:
        if arg == '--expect-file':
            continue
        if '=' not in arg:
            raise SystemExit(f'not a KEY=VALUE expectation: {arg}')
        key, value = arg.split('=', 1)
        wants[key.removeprefix('CONFIG_')] = value
    return wants


def same(actual: str, want: str) -> bool:
    truthy = {'y', '1', 'true'}
    falsy = {'n', '0', 'false', ''}
    if want in truthy and actual in truthy:
        return True
    if want in falsy and actual in falsy:
        return True
    return actual == want


def main() -> int:
    if len(sys.argv) < 3:
        raise SystemExit(__doc__)
    path, args = sys.argv[1], sys.argv[2:]
    if args[0] == '--expect-file':
        wants = expected(
            [line.strip() for line in open(args[1], encoding='utf-8')
             if line.strip() and not line.strip().startswith('#')])
    else:
        wants = expected(args)

    values = parse(path)
    errors = []
    for key, want in wants.items():
        actual = values.get(key)
        if actual is None:
            errors.append(f'{key}: not present in {path} (expected {want})')
        elif not same(actual, want):
            errors.append(f'{key}: is {actual!r}, expected {want!r}')

    for key in sorted(wants):
        print(f'  {key} = {values.get(key, "<absent>")}')
    if errors:
        for error in errors:
            print(f'error: {error}')
        return 1
    print(f'{path}: {len(wants)} value(s) match')
    return 0


if __name__ == '__main__':
    sys.exit(main())
