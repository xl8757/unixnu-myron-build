#!/usr/bin/env python3
"""Inputs of the PMGR integration builder (build-sheng-platform-power.py) and its AMFI audit, from this board's build.

Both read a prelinked reference kernel (build/native-prelink/native-owner-ufs-cpu-desktop-*-candidate.macho with its
.json plan) and the original kernelcache (.cache/firmware/kernelcache.macho/kernelcache.release.ipad7.decompressed).
sheng keeps them from earlier builds; here the reference is the previous prelinked kernel of this board
(out/<board>/kernel/kernel.macho, built from the same original kernelcache, which the audit checks byte by byte) and
the kernelcache is the one the ipsw step extracted. A first build from scratch has no previous kernel: build once
without CONFIG_XNU_SHENG_PMGR.
"""
import hashlib
import shutil
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]          # out/<board>/tree
WORK = ROOT.parent                                  # out/<board>
KERNELCACHE_SHA256 = 'f18b4df865405f2a287b84977d7aca745e056d656d9fc9df1662970ce2969de7'   # board.toml [kernel.prelink]


def sha256(path):
    digest = hashlib.sha256()
    with path.open('rb') as handle:
        for block in iter(lambda: handle.read(1 << 20), b''):
            digest.update(block)
    return digest.hexdigest()


def main():
    kernel, plan = WORK / 'kernel/kernel.macho', WORK / 'kernel/kernel.json'
    reference = ROOT / 'build/native-prelink/native-owner-ufs-cpu-desktop-myron-candidate.macho'
    if kernel.is_file() and plan.is_file():
        reference.parent.mkdir(parents=True, exist_ok=True)
        shutil.copyfile(kernel, reference)
        shutil.copyfile(plan, reference.with_suffix('.json'))
    elif reference.is_file() and reference.with_suffix('.json').is_file():
        kernel = reference          # the prelink step removed the previous kernel and failed before writing the new one
    else:
        raise SystemExit(f'{kernel} and its .json are missing: build the kernel once without CONFIG_XNU_SHENG_PMGR')
    caches = sorted(WORK.parent.glob('ipsw/*/kernelcache.decompressed'))
    cache = next((c for c in caches if sha256(c) == KERNELCACHE_SHA256), None)
    if cache is None:
        raise SystemExit('No extracted kernelcache with the pinned sha256: run "unixnu ipsw fetch"')
    target = ROOT / '.cache/firmware/kernelcache.macho/kernelcache.release.ipad7.decompressed'
    target.parent.mkdir(parents=True, exist_ok=True)
    if not target.is_file() or sha256(target) != KERNELCACHE_SHA256:
        shutil.copyfile(cache, target)
    print(f'reference {reference.name} from {kernel} (sha256 {sha256(reference)[:16]}); kernelcache {cache}')


if __name__ == '__main__':
    main()
