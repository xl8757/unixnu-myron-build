#!/usr/bin/env python3
"""Prepare the pinned sources and host tools of the Adreno user-space build in this tree (scripts/build-turnip-darwin.py,
scripts/build-native-metal-bridge.py, scripts/build-adreno-metal-plugin.py through build-myron-metal-plugin.py):

- .cache/mesa-work: Mesa at the commit and tree of scripts/data/adreno-compiler-inputs.json, a blobless sparse checkout of its
  sparse_directories, with the recorded patches applied once and verified by mesa_patch_identity.validate_recorded_patches;
- .cache/mesa-venv: Meson and the Python modules of scripts/requirements-adreno-compiler.txt at their pinned versions;
- .cache/mesa-tools: GNU Bison 3.8.2 built from its release tarball (size and SHA-256 checked);
- .cache/gpu-reference/metal2vulkan: the translator at the commit of scripts/data/gpu-metal-vulkan-inputs.json, its recorded
  files checked (build-native-metal-bridge.py applies NativeMetal's patches to it).

These are the steps UniXNU-sheng's build-adreno-compiler-host.py takes before its host IR3 build, which myron does not need. First
the host tools those builds call are checked (Xcode's iPhoneOS SDK, ninja, glslang, shaderc, SPIRV-Tools, LLVM 21 or newer, Rust
1.98.0 with the aarch64-apple-ios target); a missing one stops here with the command that installs it. --check stops after that.
Nothing here runs on or reaches the phone."""
import argparse
import hashlib
import json
from pathlib import Path
import re
import shutil
import subprocess
import sys
import tarfile
import urllib.request
from mesa_patch_identity import validate_recorded_patches

parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('--check', action='store_true', help='only check the host tools')
args = parser.parse_args()
ROOT = Path(__file__).resolve().parents[1]
CACHE = ROOT / '.cache'
pins = json.loads((ROOT / 'scripts/data/adreno-compiler-inputs.json').read_text())
translator = json.loads((ROOT / 'scripts/data/gpu-metal-vulkan-inputs.json').read_text())['translator']
source = CACHE / 'mesa-work'
venv = CACHE / 'mesa-venv'
tools = CACHE / 'mesa-tools'
m2v = CACHE / 'gpu-reference/metal2vulkan'


def run(command, *, cwd=ROOT, log=None, timeout=60):
    command = [str(x) for x in command]
    if log:
        with (CACHE / log).open('w') as stream:
            result = subprocess.run(command, cwd=cwd, stdout=stream, stderr=subprocess.STDOUT, timeout=timeout)
        if result.returncode:
            raise SystemExit(log + ':\n' + '\n'.join((CACHE / log).read_text().splitlines()[-20:]))
        return ''
    return subprocess.check_output(command, cwd=cwd, text=True, timeout=timeout).strip()


def works(command):
    try:
        return subprocess.run([str(x) for x in command], capture_output=True, text=True, timeout=60).returncode == 0
    except OSError:
        return False


# 1. Host tools.
missing = []
if not works(['xcrun', '--sdk', 'iphoneos', '--show-sdk-path']):
    missing.append(('the iPhoneOS SDK (xcrun --sdk iphoneos)', 'install Xcode and run: sudo xcode-select -s /Applications/Xcode.app'))
for tool, formula in (('ninja', 'ninja'), ('glslangValidator', 'glslang'), ('glslc', 'shaderc'), ('spirv-val', 'spirv-tools')):
    if not shutil.which(tool):
        missing.append((tool, 'brew install ' + formula))
llvm_as = Path('/opt/homebrew/opt/llvm/bin/llvm-as')
version = subprocess.run([str(llvm_as), '--version'], capture_output=True, text=True).stdout if llvm_as.exists() else ''
major = re.search(r'LLVM version (\d+)', version)
if not major or int(major[1]) < 21:
    missing.append(('LLVM 21 or newer (llvm-as)', 'brew install llvm   (or: brew upgrade llvm)'))
if not shutil.which('rustup'):
    missing.append(('rustup', 'install it from https://rustup.rs, then open a new terminal'))
else:
    if not works(['cargo', '+1.98.0', '--version']):
        missing.append(('Rust 1.98.0', 'rustup toolchain install 1.98.0 --profile minimal'))
    targets = subprocess.run(['rustup', 'target', 'list', '--installed', '--toolchain', '1.98.0'], capture_output=True, text=True).stdout
    if 'aarch64-apple-ios' not in targets.split():
        missing.append(('the aarch64-apple-ios target of Rust 1.98.0', 'rustup target add aarch64-apple-ios --toolchain 1.98.0'))
if missing:
    print('Missing host tools of the GPU user-space build:')
    for what, how in missing:
        print('  %-48s %s' % (what, how))
    raise SystemExit(1)
print('PASS: host tools (iPhoneOS SDK, ninja, glslang, shaderc, SPIRV-Tools, LLVM %s, Rust 1.98.0 with aarch64-apple-ios)' % major[1])
if args.check:
    raise SystemExit(0)

# 2. Mesa: the pinned commit, sparse, blobless, with the recorded patch sequence applied to a pristine checkout once. A checkout
# with changes must equal exactly the recorded patches (validate_recorded_patches refuses anything else).
CACHE.mkdir(exist_ok=True)
if not (source / '.git').exists():
    run(['git', 'init', source])
    run(['git', '-C', source, 'remote', 'add', 'origin', pins['mesa']['mirror']])
    run(['git', '-C', source, 'fetch', '--depth=1', '--filter=blob:none', 'origin', pins['mesa']['commit']], timeout=600)
    run(['git', '-C', source, 'sparse-checkout', 'init', '--cone'])
    run(['git', '-C', source, 'sparse-checkout', 'set', *pins['mesa']['sparse_directories']])
    run(['git', '-C', source, 'checkout', '--detach', 'FETCH_HEAD'], timeout=900)
if run(['git', '-C', source, 'rev-parse', 'HEAD']) != pins['mesa']['commit'] or \
        run(['git', '-C', source, 'rev-parse', 'HEAD^{tree}']) != pins['mesa']['tree']:
    raise SystemExit('Pinned Mesa commit/tree differs')
clean = subprocess.run(['git', '-C', source, 'diff', '--quiet', 'HEAD'], capture_output=True).returncode == 0
if clean and not run(['git', '-C', source, 'ls-files', '--others', '--exclude-standard']):
    for name in pins['patches']:
        run(['git', '-C', source, 'apply', ROOT / name])
validate_recorded_patches(ROOT, source, pins)
print('PASS: Mesa %s with its %d recorded patches' % (pins['mesa']['commit'][:12], len(pins['patches'])))

# 3. Meson and its Python modules, at the pinned versions.
if not (venv / 'bin/python').exists():
    run([sys.executable, '-m', 'venv', venv])
requirements = ROOT / 'scripts/requirements-adreno-compiler.txt'
expected = {line.split('==')[0].lower(): line.split('==')[1] for line in requirements.read_text().splitlines() if line.strip()}
installed = json.loads(run([venv / 'bin/python', '-c', 'import importlib.metadata,json; print(json.dumps({d.metadata["Name"].lower():'
                                                       'd.version for d in importlib.metadata.distributions()}))']))
if any(installed.get(name) != value for name, value in expected.items()):
    run([venv / 'bin/pip', 'install', '-r', requirements], log='mesa-python-install.log', timeout=600)
print('PASS: Meson venv (%s)' % ', '.join('%s %s' % item for item in sorted(expected.items())))

# 4. GNU Bison 3.8.2 from the release tarball (Mesa's grammars need 3.x; macOS ships 2.3).
bison = tools / 'bin/bison'
if not bison.exists() or run([bison, '--version']).splitlines()[0] != 'bison (GNU Bison) 3.8.2':
    archive = CACHE / 'tool-sources/bison-3.8.2.tar.xz'
    archive.parent.mkdir(exist_ok=True)
    if not archive.exists():
        with urllib.request.urlopen(pins['bison']['url'], timeout=60) as stream:
            archive.write_bytes(stream.read(pins['bison']['bytes'] + 1))
    data = archive.read_bytes()
    if len(data) != pins['bison']['bytes'] or hashlib.sha256(data).hexdigest() != pins['bison']['sha256']:
        raise SystemExit('GNU Bison release input differs')
    tool_source = archive.parent / 'bison-3.8.2'
    if not tool_source.exists():
        with tarfile.open(archive) as stream:
            stream.extractall(archive.parent, filter='data')
    tool_build = CACHE / 'tool-build/bison'
    tool_build.mkdir(parents=True, exist_ok=True)
    run([tool_source / 'configure', '--prefix=' + str(tools), '--disable-nls'], cwd=tool_build, log='bison-configure.log', timeout=600)
    run(['make', '-j4'], cwd=tool_build, log='bison-build.log', timeout=900)
    run(['make', 'install'], cwd=tool_build, log='bison-install.log', timeout=120)
print('PASS: GNU Bison 3.8.2 in', tools / 'bin')

# 5. The metal2vulkan translator at its pin (no submodules), blobless; its recorded files must match.
if not (m2v / '.git').exists():
    m2v.parent.mkdir(parents=True, exist_ok=True)
    run(['git', 'init', m2v])
    run(['git', '-C', m2v, 'remote', 'add', 'origin', translator['repository']])
    run(['git', '-C', m2v, 'fetch', '--depth=1', '--filter=blob:none', 'origin', translator['commit']], timeout=600)
    run(['git', '-C', m2v, 'checkout', '--detach', 'FETCH_HEAD'], timeout=900)
if run(['git', '-C', m2v, 'rev-parse', 'HEAD']) != translator['commit']:
    raise SystemExit('The metal2vulkan checkout is not at the pinned commit')
# The committed files (build-native-metal-bridge.py patches the working tree, so a second run must not read that).
for name, digest in translator['files'].items():
    blob = subprocess.check_output(['git', '-C', str(m2v), 'show', 'HEAD:' + name], timeout=120)
    if hashlib.sha256(blob).hexdigest() != digest:
        raise SystemExit('metal2vulkan file differs from its recorded hash: ' + name)
print('PASS: metal2vulkan %s' % translator['commit'][:12])
