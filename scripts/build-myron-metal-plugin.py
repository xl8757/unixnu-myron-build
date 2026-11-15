#!/usr/bin/env python3
"""Build the Adreno Metal plugin bundle and probe daemon for myron: scripts/build-adreno-metal-plugin.py with two changes.

1. No Turnip replay step. That step (scripts/gen-turnip-replay.py) records Turnip's first submission on the host simulation of the
   A740 and embeds it as build/turnip-smoke/TurnipReplay.h, which the plugin's user test then submits to the GPU as recorded
   (NativeAdrenoUserTest.h, only when the header exists). An A740 command stream is not one the A840 runs, and the host simulation is
   a large build of its own, so myron leaves the header out: the user test runs its raw submissions and then Turnip on the real chip.
2. The plugin's Turnip debug flags come from the file NativeAdrenoMetal.tudebug beside the bundle, when there is one: a line
   NAME=VALUE sets that environment variable (IR3_SHADER_DEBUG for the shader compiler, ...), a line without '=' is TU_DEBUG;
   otherwise upstream's "sysmem,nocb". The A840 hung in a draw with both CP threads busy and no fault (boot-myron-29), the signature
   sheng's A740 had before those two flags; the file lets a boot image try other flags (syncdraw, flushall, nolrz, noubwc, ...)
   without a rebuild, since the GPU addon that carries it can be rewritten on its own. The plugin source is compiled from a patched
   copy in build/myron-metal-plugin-src; drivers/NativeMetal is not modified.

Everything else (the Metal bridge, Turnip in the bundle, the 4 KiB ad-hoc signatures, trust.json, the launchd job plists) is the
upstream script's. Needs scripts/build-turnip-darwin.py --target ios first."""
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
upstream = ROOT / 'scripts/build-adreno-metal-plugin.py'
text = upstream.read_text()
replay = "subprocess.run(['python3', ROOT / 'scripts/gen-turnip-replay.py'], check=True)\n"
source = "'-x', 'objective-c++', ROOT / 'drivers/NativeMetal/NativeAdrenoMetalPlugin.mm',"
for needle in (replay, source):
    if text.count(needle) != 1:
        raise SystemExit('build-adreno-metal-plugin.py changed where this wrapper edits it; review the wrapper: ' + needle.strip())

plugin = (ROOT / 'drivers/NativeMetal/NativeAdrenoMetalPlugin.mm').read_text()
# The upstream plugin's default flags moved from "sysmem,nocb" to "nocb" when it began choosing tile or system memory per pass.
flags = next((candidate for candidate in (
    '        setenv("TU_DEBUG", "sysmem,nocb", 0);\n',
    '        setenv("TU_DEBUG", "nocb", 0);\n') if plugin.count(candidate) == 1), None)
if flags is None:
    raise SystemExit('NativeAdrenoMetalPlugin.mm no longer sets TU_DEBUG as recorded here; review the wrapper')
fallback = 'sysmem,nocb' if 'sysmem,nocb' in flags else 'nocb'
patched_flags = '''        // myron (build-myron-metal-plugin.py): NativeAdrenoMetal.tudebug beside the bundle. A line NAME=VALUE sets that environment
        // variable (IR3_SHADER_DEBUG, ...), a line without '=' is the TU_DEBUG flags; without one, upstream's flags.
        {
            char tudebug[1024], flags[256] = {}, line[256];
            if (FILE *file = requestedPath("NativeAdrenoMetal.tudebug", tudebug, sizeof tudebug) ? fopen(tudebug, "r") : nullptr) {
                while (fgets(line, sizeof line, file)) {
                    line[strcspn(line, "\\r\\n")] = 0;
                    if (char *equals = strchr(line, '=')) {*equals = 0; if (line[0]) setenv(line, equals + 1, 1);}
                    else if (line[0]) strlcpy(flags, line, sizeof flags);
                }
                fclose(file);
            }
            setenv("TU_DEBUG", flags[0] ? flags : "__MYRON_FALLBACK__", 0);
        }
'''.replace('__MYRON_FALLBACK__', fallback)
copy = ROOT / 'build/myron-metal-plugin-src/NativeAdrenoMetalPlugin.mm'
copy.parent.mkdir(parents=True, exist_ok=True)
copy.write_text(plugin.replace(flags, patched_flags))
# The copy's own directory has none of the plugin's headers: the original directory goes on the include path.
text = text.replace(replay, '').replace(source, "'-I', ROOT / 'drivers/NativeMetal', '-x', 'objective-c++', ROOT / %r," %
                                        copy.relative_to(ROOT).as_posix())
(ROOT / 'build/turnip-smoke/TurnipReplay.h').unlink(missing_ok=True)
exec(compile(text, str(upstream), 'exec'), {'__name__': '__main__', '__file__': str(upstream)})
if (ROOT / 'build/turnip-smoke/TurnipReplay.h').exists():
    raise SystemExit('A Turnip replay header appeared during the build')
print('PASS: myron plugin build without the A740 Turnip replay, Turnip flags from NativeAdrenoMetal.tudebug')
