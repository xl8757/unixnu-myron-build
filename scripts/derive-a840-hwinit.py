#!/usr/bin/env python3
"""Derive the myron Adreno 840 (A8XX) hw_init facts from the Linux sources into package/MyronA840HwInit.h.

Offsets come from a6xx.xml, resolved the way Linux's generated headers name them: REG_<first chip of the variants>_<name>,
or the domain name when a register has no variants. Each one must also cover A8XX. Masks are the A8XX_* macros of
a8xx_gpu.c, expanded from the xml bitsets. Tables are a840_protect_regs and a840_nonctxt_regs of a6xx_catalog.c. The UBWC
words are a8xx_set_ubwc_config with kaanapali's ubwc_6_0_hbb16 (drivers/soc/qcom/ubwc_config.c, include/linux/soc/qcom/ubwc.h).

    derive-a840-hwinit.py [--src DIR] [--check]

DIR holds a6xx.xml, a6xx_catalog.c and a8xx_gpu.c, flat or in the kernel tree layout (default .cache/adreno-source)."""
import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
OUT = ROOT / 'package/MyronA840HwInit.h'
CHIPS = ['A2XX', 'A3XX', 'A4XX', 'A5XX', 'A6XX', 'A7XX', 'A8XX']
PIPES = {'PIPE_NONE': 0, 'PIPE_BR': 1, 'PIPE_BV': 2, 'PIPE_LPAC': 3, 'PIPE_AQE0': 4, 'PIPE_AQE1': 5, 'PIPE_DDE_BR': 6,
         'PIPE_DDE_BV': 7}   # adreno_common.xml enum adreno_pipe

# hw_init (a8xx_gpu.c) and the reads the probe reports, under the names a8xx_gpu.c / a6xx_gpu.c use.
GPU_REGS = [
    'REG_A8XX_CP_APERTURE_CNTL_HOST', 'REG_A6XX_GBIF_HALT', 'REG_A8XX_RBBM_GBIF_HALT', 'REG_A8XX_RBBM_GBIF_HALT_ACK',
    'REG_A6XX_RBBM_SECVID_TSB_CNTL', 'REG_A6XX_RBBM_SECVID_TSB_TRUSTED_BASE', 'REG_A6XX_RBBM_SECVID_TSB_TRUSTED_SIZE',
    'REG_A6XX_RBBM_SECVID_TRUST_CNTL', 'REG_A8XX_RBBM_PERFCTR_GPU_BUSY_MASKED', 'REG_A8XX_UCHE_CCHE_GC_GMEM_RANGE_MIN',
    'REG_A8XX_SP_HLSQ_GC_GMEM_RANGE_MIN', 'REG_A8XX_UCHE_TRAP_BASE', 'REG_A8XX_UCHE_WRITE_THRU_BASE',
    'REG_A8XX_UCHE_CCHE_TRAP_BASE', 'REG_A8XX_UCHE_CCHE_WRITE_THRU_BASE', 'REG_A8XX_RBBM_PERFCTR_CNTL',
    'REG_A8XX_RBBM_SLICE_PERFCTR_CNTL', 'REG_A8XX_GRAS_NC_MODE_CNTL', 'REG_A8XX_RB_CCU_NC_MODE_CNTL',
    'REG_A8XX_RB_CMP_NC_MODE_CNTL', 'REG_A6XX_SP_NC_MODE_CNTL', 'REG_A6XX_TPL1_NC_MODE_CNTL', 'REG_A8XX_UCHE_CLIENT_PF',
    'REG_A8XX_RBBM_INTERFACE_HANG_INT_CNTL', 'REG_A8XX_RBBM_SLICE_INTERFACE_HANG_INT_CNTL', 'REG_A8XX_CP_PROTECT_CNTL_PIPE',
    'REG_A6XX_RB_CONTEXT_SWITCH_GMEM_SAVE_RESTORE_ENABLE', 'REG_A8XX_CP_APRIV_CNTL_PIPE',
    'REG_A8XX_CP_INTERRUPT_STATUS_MASK_PIPE', 'REG_A8XX_CP_HW_FAULT_STATUS_MASK_PIPE',
    'REG_A8XX_CP_INTERRUPT_STATUS_MASK_GLOBAL', 'REG_A8XX_RBBM_INT_0_MASK', 'REG_A8XX_CP_SQE_INSTR_BASE',
    'REG_A8XX_CP_AQE_INSTR_BASE_0', 'REG_A6XX_CP_RB_BASE', 'REG_A6XX_CP_RB_CNTL', 'REG_A6XX_CP_RB_RPTR_ADDR',
    'REG_A8XX_CP_RB_RPTR_ADDR_BV', 'REG_A8XX_CP_SQE_CNTL', 'REG_A8XX_RB_GC_GMEM_PROTECT', 'REG_A6XX_GBIF_CX_CONFIG',
    'REG_A6XX_CP_RB_RPTR', 'REG_A6XX_CP_RB_WPTR', 'REG_A8XX_CP_RB_RPTR_BV', 'REG_A8XX_RBBM_STATUS', 'REG_A8XX_RBBM_STATUS1',
    'REG_A8XX_RBBM_GFX_STATUS', 'REG_A8XX_RBBM_INT_0_STATUS', 'REG_A8XX_RBBM_INT_CLEAR_CMD',
    'REG_A8XX_CP_INTERRUPT_STATUS_GLOBAL', 'REG_A8XX_CP_INTERRUPT_STATUS_PIPE', 'REG_A8XX_CP_HW_FAULT_STATUS_PIPE',
    'REG_A8XX_CP_SQE_STATUS_PIPE', 'REG_A8XX_CP_PROTECT_STATUS_PIPE', 'REG_A8XX_CP_ALWAYS_ON_COUNTER',
]
GPU_ARRAYS = ['REG_A8XX_CP_PROTECT_GLOBAL', 'REG_A8XX_CP_PROTECT_PIPE', 'REG_A8XX_CP_SCRATCH_GLOBAL',
              'REG_A8XX_CP_PERFCTR_CP_SEL']
CX_MISC_REGS = ['REG_A8XX_CX_MISC_SLICE_ENABLE_FINAL', 'REG_A8XX_CX_MISC_SW_FUSE_FREQ_LIMIT_STATUS',
                'REG_A7XX_CX_MISC_SW_FUSE_VALUE']
MASKS = ['A8XX_INT_MASK', 'A8XX_APRIV_MASK', 'A8XX_BR_APRIV_MASK', 'A8XX_CP_GLOBAL_INT_MASK',
         'A8XX_CP_INTERRUPT_STATUS_MASK_PIPE', 'A8XX_CP_HW_FAULT_STATUS_MASK_PIPE']
# a8xx_set_cp_protect's cntl, written in the function body instead of a macro.
PROTECT_CNTL = ['A8XX_CP_PROTECT_CNTL_PIPE_ACCESS_PROT_EN', 'A8XX_CP_PROTECT_CNTL_PIPE_ACCESS_FAULT_ON_VIOL_EN',
                'A8XX_CP_PROTECT_CNTL_PIPE_LAST_SPAN_INF_RANGE', 'A8XX_CP_PROTECT_CNTL_PIPE_HALT_SQE_RANGE__MASK']


def find(src, name):
    hits = sorted(src.rglob(name))
    if not hits:
        raise SystemExit('missing source ' + name + ' under ' + str(src))
    return hits[0].read_text(encoding='utf-8')


def chip_range(variants):
    """The chips a variants attribute covers, as indices into CHIPS."""
    covered = set()
    for part in variants.split(','):
        part = part.strip()
        if '-' in part:
            left, right = part.split('-', 1)
            low = CHIPS.index(left) if left else 0
            high = CHIPS.index(right) if right else len(CHIPS) - 1
            covered.update(range(low, high + 1))
        else:
            covered.add(CHIPS.index(part))
    return covered


def attrs(text):
    return dict(re.findall(r'(\w+)="([^"]*)"', text))


class Domain:
    def __init__(self, name, body):
        self.name = name
        self.items = []   # (kind, attributes)
        self.bitsets = {}
        for m in re.finditer(r'<bitset\s+([^>]*)>(.*?)</bitset>', body, re.S):
            self.bitsets[attrs(m.group(1))['name']] = fields(m.group(2))
        flat = re.sub(r'<bitset\s+[^>]*>.*?</bitset>', '', body, flags=re.S)
        for m in re.finditer(r'<(reg32|reg64|array)\s+([^>]*?)/?>', flat):
            self.items.append((m.group(1), attrs(m.group(2))))

    def prefix(self, a):
        v = a.get('variants')
        return re.split(r'[-,]', v)[0] if v else self.name

    def resolve(self, macro, kinds):
        """REG_<prefix>_<name> -> the attributes of the one matching register (or array)."""
        hits = []
        for kind, a in self.items:
            if kind in kinds and 'REG_%s_%s' % (self.prefix(a), a.get('name')) == macro:
                hits.append(a)
        if len(hits) != 1:
            raise SystemExit('%s: %d matches in domain %s' % (macro, len(hits), self.name))
        a = hits[0]
        if 'variants' in a and CHIPS.index('A8XX') not in chip_range(a['variants']):
            raise SystemExit(macro + ' does not cover A8XX')
        return a


def fields(body):
    out = {}
    for m in re.finditer(r'<bitfield\s+([^>]*?)/?>', body):
        a = attrs(m.group(1))
        if 'variants' in a and CHIPS.index('A8XX') not in chip_range(a['variants']):
            continue
        low, high = (int(a['pos']),) * 2 if 'pos' in a else (int(a['low']), int(a['high']))
        out[a['name']] = (low, high)
    return out


def domains(xml):
    return {attrs(m.group(1))['name']: Domain(attrs(m.group(1))['name'], m.group(2))
            for m in re.finditer(r'<domain\s+([^>]*)>(.*?)</domain>', xml, re.S)}


def field_macros(dom):
    """Generated bitfield macro prefix -> bitset fields: non-inline bitsets by their own name, inline ones (a register's
    type) as <prefix>_<register>."""
    out = {name: f for name, f in dom.bitsets.items()}
    for kind, a in dom.items:
        if kind != 'array' and a.get('type') in dom.bitsets:
            out['%s_%s' % (dom.prefix(a), a['name'])] = dom.bitsets[a['type']]
    return out


def bit_value(token, macros):
    for prefix in sorted(macros, key=len, reverse=True):
        if not token.startswith(prefix + '_'):
            continue
        rest = token[len(prefix) + 1:]
        mask = rest.endswith('__MASK')
        name = rest[:-6] if mask else rest
        if name in macros[prefix]:
            low, high = macros[prefix][name]
            if not mask and low != high:
                raise SystemExit(token + ' is a multi-bit field')
            return ((1 << (high - low + 1)) - 1) << low
    raise SystemExit('unknown bitfield macro ' + token)


def expand(name, defines, macros):
    value = 0
    for token in re.findall(r'[A-Z][A-Z0-9_]+', defines[name]):
        value |= expand(token, defines, macros) if token in defines else bit_value(token, macros)
    return value


def c_value(expr):
    """BIT()/PIPE/hex expressions of the catalog tables."""
    expr = re.sub(r'BIT\((\w+)\)', lambda m: '(1<<%s)' % PIPES.get(m.group(1), m.group(1)), expr)
    if not re.fullmatch(r'[0-9a-fA-FxX()<| ]+', expr):
        raise SystemExit('unparsed table value ' + expr)
    return eval(expr, {'__builtins__': {}})


def main():
    argv = sys.argv[1:]
    src = Path(argv[argv.index('--src') + 1]) if '--src' in argv else ROOT / '.cache/adreno-source'
    doms = domains(find(src, 'a6xx.xml'))
    gpu, cx = doms['A6XX'], doms['A6XX_CX_MISC']
    catalog, a8xx = find(src, 'a6xx_catalog.c'), find(src, 'a8xx_gpu.c')
    regs = [(m, int(gpu.resolve(m, ('reg32', 'reg64'))['offset'], 16)) for m in GPU_REGS]
    arrays = []
    for m in GPU_ARRAYS:
        a = gpu.resolve(m, ('array',))
        arrays.append((m, int(a['offset'], 16), int(a['stride']), int(a['length'])))
    misc = [(m, int(cx.resolve(m, ('reg32', 'reg64'))['offset'], 16)) for m in CX_MISC_REGS]

    defines = {}
    for m in re.finditer(r'#define (A8XX_[A-Z0-9_]+)\s*\\\n((?:.*\\\n)*.*\n)', a8xx):
        defines[m.group(1)] = m.group(2)
    macros = field_macros(gpu)
    masks = [(n, expand(n, defines, macros)) for n in MASKS]
    protect_cntl = 0
    for token in PROTECT_CNTL:
        protect_cntl |= bit_value(token, macros)
    if 'A8XX_CP_PROTECT_CNTL_PIPE_HALT_SQE_RANGE__MASK' not in a8xx:
        raise SystemExit('a8xx_set_cp_protect changed')

    block = re.search(r'a840_protect_regs\[\]\s*=\s*\{(.*?)\};', catalog, re.S).group(1)
    protect = []
    for kind, reg, length in re.findall(r'A6XX_PROTECT_(NORDWR|RDONLY)\((0x[0-9a-fA-F]+),\s*(0x[0-9a-fA-F]+)\)', block):
        protect.append(((1 << 31) if kind == 'NORDWR' else 0) | ((int(length, 16) & 0x3fff) << 18) | (int(reg, 16) & 0x3ffff))
    if len(protect) != block.count('A6XX_PROTECT_') or len(protect) > arrays[0][3]:
        raise SystemExit('unparsed a840_protect_regs')

    block = re.search(r'a840_nonctxt_regs\[\]\s*=\s*\{(.*?)\n\};', catalog, re.S).group(1)
    block = re.sub(r'/\*.*?\*/', '', block, flags=re.S)
    nonctxt = []
    for reg, value, pipes in re.findall(r'\{\s*(REG_A[678]XX_[A-Z0-9_]+),\s*([^,]+),\s*([^}]+)\}', block):
        nonctxt.append((reg, int(gpu.resolve(reg, ('reg32', 'reg64'))['offset'], 16), c_value(value.strip()),
                        c_value(pipes.strip())))
    if len(nonctxt) != block.count('REG_'):
        raise SystemExit('unparsed a840_nonctxt_regs')
    gmem_protect = [v for r, _, v, _ in nonctxt if r == 'REG_A8XX_RB_GC_GMEM_PROTECT']

    # a8xx_set_ubwc_config with ubwc_6_0_hbb16: UBWC 6.0, highest bank bit 16, no swizzle flags.
    version, hbb_bit = 0x60000000, 16
    hbb = hbb_bit - 13
    hbb_hi, hbb_lo = hbb >> 2, hbb & 3
    l2_dis = l3_dis = 0                                        # qcom_ubwc_swizzle: LVL2 | LVL3 for every version > 1.0
    yuvnotcomptofc = int(version == 0x60000000)
    rgba8888_lossless = int(0x40000000 <= version < 0x50000000)
    fp16compoptdis = int(version < 0x40030000)
    rgb565_predicator = int(version >= 0x40000000)
    amsbc, ubwc_mode, min_acc_len_64b, uavflagprd_inv = int(version >= 0x30000000), int(version == 0x10000000), 0, 2
    mode = 5                                                   # qcom_ubwc_version_tag: >= 6.0
    ubwc = [
        ('GRAS_NC_MODE_CNTL', hbb << 5 | l3_dis << 4 | l2_dis << 3),
        ('RB_CCU_NC_MODE_CNTL', yuvnotcomptofc << 6 | l3_dis << 5 | l2_dis << 4 | hbb_hi << 3 | hbb_lo << 1),
        ('RB_CMP_NC_MODE_CNTL', mode << 15 | yuvnotcomptofc << 6 | rgba8888_lossless << 4 | fp16compoptdis << 3 |
         rgb565_predicator << 2 | amsbc << 1 | min_acc_len_64b),
        ('SP_NC_MODE_CNTL', l3_dis << 13 | l2_dis << 12 | hbb_hi << 10 | uavflagprd_inv << 4 | min_acc_len_64b << 3 |
         hbb_lo << 1 | ubwc_mode),
        ('TPL1_NC_MODE_CNTL', l3_dis << 7 | l2_dis << 6 | hbb_hi << 4 | min_acc_len_64b << 3 | hbb_lo << 1 | ubwc_mode),
    ]

    lines = ['// SPDX-License-Identifier: BSD-2-Clause-Patent',
             '// Generated by scripts/derive-a840-hwinit.py from Linux a6xx.xml, a6xx_catalog.c (a840_protect_regs,',
             '// a840_nonctxt_regs), a8xx_gpu.c (interrupt/APRIV/protect masks) and ubwc_config.c (kaanapali: ubwc_6_0_hbb16).',
             '// Register offsets are dwords from the GPU base (CX_MISC ones from the cx_misc block); MYRON_A8XX_* are the values of',
             '// the a8xx_gpu.c macros of the same name. Do not edit by hand.',
             '#ifndef MYRON_A840_HWINIT_H', '#define MYRON_A840_HWINIT_H', '#include <stdint.h>']
    for m, off in regs:
        lines.append('#define A840_%s 0x%05x' % (m[9:], off))
    for m, off, stride, length in arrays:
        lines.append('#define A840_%s(i) (0x%05x + (i) * %d)   /* %d entries */' % (m[9:], off, stride, length))
    for m, off in misc:
        lines.append('#define A840_%s 0x%05x' % (m[9:], off))
    for name, value in masks:
        lines.append('#define MYRON_%s 0x%08x' % (name, value))
    lines.append('#define MYRON_A8XX_CP_PROTECT_CNTL 0x%08x   /* a8xx_set_cp_protect cntl */' % protect_cntl)
    lines.append('#define A840_APERTURE_PIPE(pipe) ((uint32_t)(pipe) << 12)   /* CP_APERTURE_CNTL_HOST PIPEID 15:12 */')
    for name, value in PIPES.items():
        lines.append('#define A840_%s %d' % (name, value))
    lines.append('#define A840_GMEM_PROTECT 0x%08x   /* the RB_GC_GMEM_PROTECT entry below, written after the secure exit */'
                 % gmem_protect[0])
    for name, value in ubwc:
        lines.append('#define A840_UBWC_%s 0x%08x' % (name, value))
    lines.append('// a840_protect_regs in order: CP_PROTECT_GLOBAL(i), the last one also to CP_PROTECT_PIPE(15) of BR and BV.')
    lines.append('static const uint32_t MyronA840Protect[] = {')
    lines += ['    0x%08x,' % v for v in protect]
    lines.append('};')
    lines.append('#define MYRON_A840_PROTECT_COUNT %d' % len(protect))
    lines.append('// a840_nonctxt_regs: {offset, value, pipe mask (BIT(PIPE_x))}, written per pipe through the aperture.')
    lines.append('static const uint32_t MyronA840Nonctxt[][3] = {')
    lines += ['    {0x%05x, 0x%08x, 0x%02x},   /* %s */' % (o, v, p, r[4:]) for r, o, v, p in nonctxt]
    lines.append('};')
    lines.append('#define MYRON_A840_NONCTXT_COUNT %d' % len(nonctxt))
    lines.append('#endif')
    text = '\n'.join(lines) + '\n'
    if '--check' in argv:
        if not OUT.exists() or OUT.read_text(encoding='utf-8') != text:
            raise SystemExit('MyronA840HwInit.h is stale')
        print('PASS: MyronA840HwInit.h matches the sources')
        return
    with open(OUT, 'w', encoding='utf-8', newline='\n') as f:
        f.write(text)
    print('PASS: %d registers, %d arrays, %d protect entries, %d non-context writes' % (
        len(regs) + len(misc), len(arrays), len(protect), len(nonctxt)))


main()
