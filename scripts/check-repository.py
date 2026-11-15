#!/usr/bin/env python3
"""Structural checks for the NX769J device repository.

These are the checks this repository can run on its own. The authoritative
record validation is UniXNU's `scripts/porting-record.py check`, which runs in
the Device documentation workflow and needs the UniXNU checkout for its
component catalog.

Usage: python3 scripts/check-repository.py [repository-root]
"""
from __future__ import annotations

import os
import re
import sys
import tomllib
from pathlib import Path

HEX40 = re.compile(r'^[0-9a-f]{40}$')
DEFCONFIG = re.compile(r'^(CONFIG_[A-Z0-9_]+=(y|n|[0-9]+|".*")|# CONFIG_[A-Z0-9_]+ is not set)$')

ERRORS: list[str] = []


def fail(message: str) -> None:
    ERRORS.append(message)


def load(path: Path) -> dict:
    try:
        return tomllib.loads(path.read_text(encoding='utf-8'))
    except (OSError, tomllib.TOMLDecodeError) as error:
        fail(f'{path.name}: does not parse: {error}')
        return {}


def check_toml(root: Path) -> tuple[dict, dict]:
    for name in ('board.toml', 'porting.toml', 'feeds.toml', 'unixnu.lock', 'package/unixnu.toml'):
        if not (root / name).is_file():
            fail(f'{name}: missing')
    for path in sorted(root.rglob('*.toml')):
        load(path)
    return load(root / 'board.toml'), load(root / 'porting.toml')


def check_board(board: dict) -> None:
    if board.get('schema_version') != 2:
        fail('board.toml: schema_version must be 2')
    section = board.get('board', {})
    for key in ('name', 'soc', 'soc_source', 'os_profile', 'description', 'kit'):
        if not str(section.get(key, '')).strip():
            fail(f'board.toml: board.{key} is required')
    if section.get('example'):
        fail('board.toml: board.example must be removed; build and bootimg refuse examples')
    if not board.get('components', {}).get('include'):
        fail('board.toml: components.include is required')
    if not board.get('kernel', {}).get('defconfig'):
        fail('board.toml: kernel.defconfig is required')
    if not board.get('device_tree', {}).get('source'):
        fail('board.toml: device_tree.source is required')


def check_identity(board: dict, record: dict) -> None:
    if record.get('schema_version') != 1:
        fail('porting.toml: schema_version must be 1')
    info = record.get('device', {})
    for key in ('name', 'soc', 'os_profile', 'summary'):
        if not str(info.get(key, '')).strip():
            fail(f'porting.toml: device.{key} is required')
    for key in ('name', 'soc', 'os_profile'):
        if info.get(key) != board.get('board', {}).get(key):
            fail(f'porting.toml: device.{key} must match board.toml (record={info.get(key)!r})')


def check_coverage(board: dict, record: dict, catalog: Path) -> None:
    selected = set(board.get('components', {}).get('include', []))
    known: dict[str, dict] = {}
    if catalog.is_file():
        known = {c['id']: c for c in tomllib.loads(catalog.read_text())['component']}
        pending = list(selected)
        while pending:
            component = pending.pop()
            if component not in known:
                fail(f'board.toml: unknown component {component}')
                continue
            for dependency in known[component].get('dependencies', []):
                if dependency not in selected:
                    selected.add(dependency)
                    pending.append(dependency)
    covered = {c for area in record.get('area', []) for c in area.get('components', [])}
    for component in sorted(selected - covered):
        fail(f'porting.toml: component {component} has no owning area')


def check_referenced_files(root: Path, board: dict, record: dict) -> None:
    kernel = board.get('kernel', {})
    defconfig = kernel.get('defconfig', '')
    if defconfig and not (root / 'configs' / defconfig).is_file():
        fail(f'board.toml: kernel.defconfig file missing: configs/{defconfig}')

    source = board.get('device_tree', {}).get('source', '')
    if source.startswith('device:'):
        rel = source[len('device:'):]
        if not (root / rel).is_file():
            fail(f'board.toml: device_tree.source file missing: {rel}')

    for series in kernel.get('patch_series', []):
        if series.startswith('device:'):
            rel = series[len('device:'):]
            if not (root / rel).is_file():
                fail(f'board.toml: device patch series missing: {rel}')
            else:
                base = (root / rel).parent
                for line in (root / rel).read_text(encoding='utf-8').splitlines():
                    line = line.strip()
                    if line and not line.startswith('#') and not (base / line).is_file():
                        fail(f'patches/xnu/series: patch file missing: {line}')

    for area in record.get('area', []):
        for item in area.get('evidence', []):
            reference = item.get('reference', '')
            if reference.startswith('https://'):
                continue
            if not (root / reference).is_file():
                fail(f'porting.toml: {area.get("id")}: evidence file missing: {reference}')

    for name in ('.gitignore', 'local.toml.example', 'README.md', 'AGENTS.md', 'feeds.toml',
                 'unixnu.lock', 'proprietary-files.txt', '.github/workflows/porting-record.yml'):
        if not (root / name).is_file():
            fail(f'{name}: missing')


def check_defconfig(root: Path, board: dict) -> None:
    name = board.get('kernel', {}).get('defconfig', '')
    path = root / 'configs' / name
    if not path.is_file():
        return
    seen: set[str] = set()
    for number, raw in enumerate(path.read_text(encoding='utf-8').splitlines(), 1):
        line = raw.strip()
        # Blank lines and ordinary comments are allowed. Only "# CONFIG_X is not
        # set" is a value, so it is validated like any other line.
        if not line or (line.startswith('#') and 'is not set' not in line):
            continue
        if not DEFCONFIG.match(line):
            fail(f'configs/{name}:{number}: not a defconfig line: {line}')
            continue
        key = line.split('=')[0].lstrip('# ').split(' ')[0]
        if key in seen:
            fail(f'configs/{name}:{number}: duplicate key {key}')
        seen.add(key)


def check_pins(root: Path) -> None:
    feeds = load(root / 'feeds.toml')
    sources = feeds.get('source', [])
    for source in sources:
        commit = source.get('commit', '')
        if not HEX40.match(commit):
            fail(f'feeds.toml: source {source.get("name")}: commit must be 40 hex characters')
    lock_path = root / 'unixnu.lock'
    lock = tomllib.loads(lock_path.read_text(encoding='utf-8')) if lock_path.is_file() else {}
    for name, entry in lock.get('source', {}).items():
        if not HEX40.match(entry.get('commit', '')):
            fail(f'unixnu.lock: source {name}: commit must be 40 hex characters')
    for source in sources:
        pinned = lock.get('source', {}).get(source.get('name'), {}).get('commit')
        if pinned and pinned != source.get('commit'):
            fail(f'unixnu.lock: source {source.get("name")} does not match feeds.toml')


def find_catalog(root: Path) -> Path:
    """Locate UniXNU's components.toml.

    Inside a UniXNU checkout it is at the root; in CI the tools checkout is a
    sibling of the device checkout. An explicit UNIXNU_COMPONENTS wins.
    """
    candidates = [
        Path(p) for p in (os.environ.get('UNIXNU_COMPONENTS', ''), str(root / 'components.toml'))
        if p
    ] + [root.parent / d / 'components.toml' for d in ('UniXNU', 'tools', 'unixnu')]
    for candidate in candidates:
        if candidate.is_file():
            return candidate
    return Path()


def main() -> int:
    root = Path(sys.argv[1] if len(sys.argv) > 1 else '.').resolve()
    board, record = check_toml(root)
    if board and record:
        check_board(board)
        check_identity(board, record)
        check_coverage(board, record, find_catalog(root))
        check_referenced_files(root, board, record)
        check_defconfig(root, board)
        check_pins(root)

    if ERRORS:
        for message in ERRORS:
            print(f'error: {message}')
        print(f'{len(ERRORS)} error(s)')
        return 1
    print(f'{root.name}: repository structure is consistent')
    return 0


if __name__ == '__main__':
    sys.exit(main())
