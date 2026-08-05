#!/usr/bin/env python3
"""Reduce a Xenos pixel shader's swizzle chains by simulating its register file.

WHY THIS EXISTS. Writing a native pass (docs/native-renderer.md) means restating
what a title shader computes, and the hard part is not the arithmetic -- it is the
CHANNEL ROTATION. The base-pass material 0x1f1a3f779667a02a has ten consecutive
`mad r0.xyz_, ..., r0.zxyy` accumulations, each rotating by one, plus constants
read as `c3.zyx` against textures fetched with destination swizzle `zxy_`.
Composing that by hand is how a native pass ships subtly wrong: every permutation
in that shader cancels, and there is no way to *see* that from the listing.

So: apply every instruction to symbolic expressions instead of numbers, intern
common subexpressions, and print the straight-line program that results. What
cancels disappears; what does not cancel -- above all the ORDER of an accumulation
chain, because the sequencer rounds after every step -- is left standing.

    tools/ucode_reduce.py scratch/shaders/bound_out/ps_<hash>.ucode.txt
    tools/ucode_reduce.py --inputs r0=lmuv,r1=uv ps.ucode.txt   # name interpolators
    tools/ucode_reduce.py --selftest

Get a disassembly with:
    scratch/build/tools/xenos_translate/xenos_translate --raw OUTDIR \\
        scratch/shaders/bound/ps_<hash>.ucode

WHAT IT IS NOT. Not a translator and not a substitute for the A/B gate. It has no
control flow (a shader with `loop`/`jmp`/predication is REFUSED, not
approximated), no memexport, and no fetch or output epilogue -- the texture sign
decode, the exponent bias and the gamma encode are the runtime's, and
`tools/verify_native_pass.sh` is what says whether the shader you write from this
is right. Its output is a reading aid with a stated blind spot, not evidence.
"""
import argparse
import re
import sys

IDX = {'x': 0, 'y': 1, 'z': 2, 'w': 3}

# Instructions this tool models. Anything else in an exec block makes it REFUSE:
# a reduction that silently skipped an instruction would read exactly like a
# correct one, and the shader written from it would be wrong in a way the listing
# could not show.
VECTOR_OPS = {'add', 'mul', 'mad', 'dp3', 'dp3_sat', 'dp2add', 'dp2add_sat',
              'add_sat', 'mul_sat', 'mad_sat', 'max', 'min', 'cndgt', 'floor',
              'frc', 'trunc'}
SCALAR_OPS = {'rsq', 'rcp', 'log', 'exp', 'sqrt', 'adds', 'maxs', 'mins',
              'mulsc', 'muls', 'muls_prev', 'adds_prev', 'subsc', 'subsc_sat',
              'addsc', 'addsc_sat',
              'sin', 'cos', 'rcp_sat', 'rsq_sat', 'exp_sat', 'log_sat'}
# Structural lines that carry no arithmetic.
STRUCTURAL = {'exec', 'exece', 'alloc', 'cnop', 'serialize', 'nop'}
# Anything here means the shader has control flow this tool does not model.
CONTROL = {'loop', 'endloop', 'jmp', 'call', 'ret', 'label', 'setp_ne', 'setp_eq',
           'kill', 'killne', 'killgt', 'kill_gt', 'kill_eq', 'kill_ge', 'kill_ne',
           'kills_gt', 'kills_eq', 'kills_ge', 'kills_ne', 'pred_setne', 'pred_seteq'}


class Reg:
    def __init__(self, comps):
        self.c = list(comps)


def dest_mask(dst):
    """Split a destination into (name, mask), where a BARE register is a FULL write.

    `mul r9, r3.xyyz, c255.xyzx` writes all four components; there is no dot and no
    mask. Treating the empty mask as "writes nothing" is not a parse quirk -- it
    silently drops the instruction, every later read of that register falls back to
    its INPUT value, and the tool prints a tidy program that is wrong. That is
    exactly what happened on ps_d99a15450a08043a before this existed, which is why
    read_before_write() below now reports the tell.
    """
    name, _, mask = dst.partition('.')
    return name, (mask or 'xyzw')


class Sim:
    def __init__(self, inputs):
        self.regs = {}
        for i in range(64):
            name = inputs.get('r%d' % i, 'r%d' % i)
            self.regs[i] = Reg(['%s.%s' % (name, ch) for ch in 'xyzw'])
        self.consts = {}
        self.oc = Reg(['oC0.%s' % ch for ch in 'xyzw'])
        self.ps = 'PS'
        self.pool = {}
        self.order = []
        self.fetches = []
        self.written = set()   # register indices any instruction has written
        self.read_first = []   # registers read before ever being written

    def const(self, n):
        if n not in self.consts:
            self.consts[n] = Reg(['c%d.%s' % (n, ch) for ch in 'xyzw'])
        return self.consts[n]

    def intern(self, expr):
        if expr in self.pool:
            return self.pool[expr]
        name = 't%d' % len(self.order)
        self.pool[expr] = name
        self.order.append((name, expr))
        return name

    def operand(self, tok, pos):
        neg = tok.startswith('-')
        if neg:
            tok = tok[1:]
        absv = tok.startswith('r_abs[')
        if absv:
            num, swz = tok[len('r_abs['):].split('].')
            base = self.regs[int(num)]
        else:
            name, _, swz = tok.partition('.')
            base = self.const(int(name[1:])) if name[0] == 'c' else self.regs[int(name[1:])]
        swz = swz or 'xyzw'
        swz = swz + swz[-1] * (4 - len(swz))
        if not absv and tok[0] == 'r':
            n = int(name[1:])
            if n not in self.written and n not in [x for x, _ in self.read_first]:
                self.read_first.append((n, name))
        v = base.c[IDX[swz[pos]]]
        if absv:
            v = 'abs(%s)' % v
        if neg:
            v = '-(%s)' % v
        return v

    def target(self, name):
        if name.startswith('oC'):
            return self.oc
        self.written.add(int(name[1:]))
        return self.regs[int(name[1:])]


def parse(path):
    """Yield (op, dest, [srcs]) for every arithmetic line, in issue order."""
    out = []
    for raw in open(path):
        line = re.sub(r'/\*.*?\*/', '', raw)
        line = line.split('//')[0].strip()
        if not line:
            continue
        line = re.sub(r'^\(p0\)\s*', '(p0) ', line)
        # co-issue continuation: "+ muls_prev r0.x___, r8.y"
        for part in [p.strip() for p in re.split(r'\s\+\s', line)]:
            # A co-issued scalar op is printed on its own continuation line
            # beginning with '+', not only after an inline ' + '. Missing that is
            # not a cosmetic bug: it drops the scalar pipe entirely, and the
            # scalar pipe is where this shader's lightmap constants are built.
            part = part.lstrip('+').strip()
            if not part:
                continue
            head = part.split()[0]
            if head in STRUCTURAL or head.startswith('label'):
                continue
            body = part
            pred = body.startswith('(p0)')
            if pred:
                body = body[len('(p0)'):].strip()
            op = body.split()[0]
            rest = body[len(op):].strip()
            args = [a.strip() for a in rest.split(',') if a.strip()]
            out.append((op, args, pred))
    return out


def run(path, inputs, allow_control=False):
    sim = Sim(inputs)
    prog = parse(path)
    refused = []
    for op, args, pred in prog:
        if pred:
            refused.append('predicated instruction `%s` (this tool has no predication)' % op)
            continue
        if op in CONTROL:
            refused.append('control flow `%s`' % op)
            continue
        if op.startswith('tfetch') or op.startswith('vfetch'):
            # A fetch's destination swizzle names the SOURCE component for each
            # destination position -- dest.x = result.z for `r2.zxy_`. Verified
            # against a translated module (OpVectorShuffle ... 6 4 5 3).
            dst, coord = args[0], args[1]
            src = args[2] if len(args) > 2 else '?'
            name, mask = dest_mask(dst)
            tex = 'tex(%s, %s)' % (src, coord)
            sim.fetches.append((tex, dst))
            tgt = sim.target(name)
            new = list(tgt.c)
            for posn, ch in enumerate(mask):
                if ch != '_':
                    new[posn] = sim.intern('%s.%s' % (tex, ch))
            tgt.c[:] = new
            continue
        if op in SCALAR_OPS:
            dst = args[0]
            name, mask = dest_mask(dst)
            srcs = args[1:]
            v = scalar(sim, op, srcs)
            v = sim.intern(v)
            sim.ps = v
            posn = next((i for i, ch in enumerate(mask) if ch != '_'), None)
            if posn is not None:
                sim.target(name).c[posn] = v
            continue
        if op in VECTOR_OPS:
            dst = args[0]
            name, mask = dest_mask(dst)
            srcs = args[1:]
            tgt = sim.target(name)
            new = list(tgt.c)
            for posn, ch in enumerate(mask):
                if ch == '_':
                    continue
                new[posn] = sim.intern(vector(sim, op, srcs, posn))
            tgt.c[:] = new
            continue
        refused.append('unmodelled instruction `%s`' % op)

    return sim, refused


def sat(op, expr):
    return 'saturate(%s)' % expr if op.endswith('_sat') else expr


def vector(sim, op, srcs, posn):
    o = lambda i, p=None: sim.operand(srcs[i], posn if p is None else p)
    base = op[:-4] if op.endswith('_sat') else op
    if base == 'add':
        return sat(op, '(%s + %s)' % (o(0), o(1)))
    if base == 'mul':
        return sat(op, '(%s * %s)' % (o(0), o(1)))
    if base == 'mad':
        return sat(op, '(%s * %s + %s)' % (o(0), o(1), o(2)))
    if base == 'max':
        return sat(op, 'max(%s, %s)' % (o(0), o(1)))
    if base == 'min':
        return sat(op, 'min(%s, %s)' % (o(0), o(1)))
    if base == 'cndgt':
        return sat(op, '(%s > 0 ? %s : %s)' % (o(0), o(1), o(2)))
    if base in ('floor', 'frc', 'trunc'):
        return sat(op, '%s(%s)' % (base, o(0)))
    if base == 'dp3':
        return sat(op, '((%s * %s + %s * %s) + %s * %s)' % (
            o(0, 0), o(1, 0), o(0, 1), o(1, 1), o(0, 2), o(1, 2)))
    if base == 'dp2add':
        return sat(op, '((%s * %s + %s * %s) + %s)' % (
            o(0, 0), o(1, 0), o(0, 1), o(1, 1), o(2, 0)))
    raise SystemExit('vector op %s reached the dispatcher unhandled' % op)


def scalar(sim, op, srcs):
    base = op[:-4] if op.endswith('_sat') else op
    if base in ('rsq', 'rcp', 'log', 'exp', 'sqrt', 'sin', 'cos'):
        fn = {'rsq': 'inversesqrt', 'rcp': '1.0/', 'log': 'log2', 'exp': 'exp2',
              'sqrt': 'sqrt', 'sin': 'sin', 'cos': 'cos'}[base]
        v = sim.operand(srcs[0], 0)
        return sat(op, ('(1.0 / %s)' % v) if base == 'rcp' else '%s(%s)' % (fn, v))
    if base in ('adds', 'maxs', 'mins'):
        # A one-source scalar op reading TWO components of the same register.
        a, b = sim.operand(srcs[0], 0), sim.operand(srcs[0], 1)
        fn = {'adds': '(%s + %s)', 'maxs': 'max(%s, %s)', 'mins': 'min(%s, %s)'}[base]
        return sat(op, fn % (a, b))
    if base in ('mulsc', 'subsc', 'addsc'):
        a, b = sim.operand(srcs[0], 0), sim.operand(srcs[1], 0)
        fmt = {'mulsc': '(%s * %s)', 'subsc': '(%s - %s)', 'addsc': '(%s + %s)'}[base]
        return sat(op, fmt % (a, b))
    if base in ('muls_prev', 'adds_prev'):
        b = sim.operand(srcs[0], 0)
        return sat(op, ('(%s * %s)' if base == 'muls_prev' else '(%s + %s)')
                   % (sim.ps, b))
    if base == 'muls':
        a, b = sim.operand(srcs[0], 0), sim.operand(srcs[0], 1)
        return sat(op, '(%s * %s)' % (a, b))
    raise SystemExit('scalar op %s reached the dispatcher unhandled' % op)


def report(path, inputs):
    sim, refused = run(path, inputs)
    print('== %s ==' % path)
    if sim.fetches:
        print('-- texture fetches, in issue order (binding numbers follow THIS order) --')
        for tex, dst in sim.fetches:
            print('   %-28s -> %s' % (tex, dst))
        print()
    if refused:
        # THE NEGATIVE, LOUD. A reduction that skipped instructions and printed a
        # tidy program anyway is worse than no reduction at all.
        print('REFUSING to present this as a reduction: %d instruction(s) were not'
              % len(refused))
        print('modelled, so the program below is INCOMPLETE and every expression')
        print('downstream of them is wrong:')
        for r in sorted(set(refused)):
            print('   - %s' % r)
        print()
    # THE TELL FOR A DROPPED INSTRUCTION. Every register read before it is written
    # must be an INTERPOLATOR: the pixel shader's inputs are r0..rN and nothing
    # else arrives unwritten. If a register appears here that the shader clearly
    # computes, an instruction that wrote it was not modelled -- which is how a
    # tidy, wrong program gets printed.
    if sim.read_first:
        names = ', '.join(n for _, n in sorted(sim.read_first))
        print('-- read before written: %s --' % names)
        print('   These must ALL be interpolators. If one of them is a register the')
        print('   shader computes, an instruction that wrote it was dropped and')
        print('   everything downstream of it is wrong.')
        print()
    print('-- straight-line program (%d expressions) --' % len(sim.order))
    for name, expr in sim.order:
        print('%-6s = %s' % (name, expr))
    print()
    print('-- outputs --')
    for ch in 'xyzw':
        print('oC0.%s = %s' % (ch, sim.oc.c[IDX[ch]]))
    if refused:
        return 1
    print()
    print('-- what this does NOT cover --')
    print('   The texture sign decode, the integer scale, the render target')
    print('   exponent bias and the gamma encode are the RUNTIME\'s epilogue, not')
    print('   the microcode\'s, and none of them appear above. Read them off the')
    print('   translated module (GEARS_DRAW_SPV_DUMP) and gate the result with')
    print('   tools/verify_native_pass.sh -- this listing is a reading aid.')
    return 0


SELFTEST_SRC = """/*    0.0 */       exec
/*    1   */          mul r0.xyz_, r1.zxyy, c1.zxyy
/*    2   */          mul r0.xyz_, r0.yzxx, c255.xxxx
/*    0.1 */       exece
/*    3   */          mul oC0.xyz_, r0.xyzz, c3.xyzz
              +       maxs oC0.___w, r2.ww
"""
# The two rotations above compose to the IDENTITY -- instruction 1 sends
# (x,y,z) <- (z,x,y) and instruction 2 sends it back -- so oC0.x must end up
# depending on r1.x, c1.x and c3.x and on NOTHING from r1.y or r1.z. That is the
# property this whole tool exists to establish, so it is the property the
# self-test asserts, on a case small enough to verify by hand:
#   1: r0 = (r1.z*c1.z, r1.x*c1.x, r1.y*c1.y)
#   2: r0 = (r0.y, r0.z, r0.x) * c255.x = (r1.x*c1.x, r1.y*c1.y, r1.z*c1.z) * c255.x
#   3: oC0.xyz = r0.xyz * c3.xyz


def selftest():
    """A rotation that MUST cancel, and a refusal that MUST happen."""
    import os
    import tempfile
    bad = 0
    with tempfile.TemporaryDirectory() as d:
        p = os.path.join(d, 't.ucode.txt')
        open(p, 'w').write(SELFTEST_SRC)
        sim, refused = run(p, {})
        got = sim.oc.c[0]
        expr = dict(sim.order)
        # Resolve the interned names back to a tree so the test asserts on the
        # ALGEBRA, not on temporary numbering.
        seen = set()
        def expand(e):
            for _ in range(50):
                names = sorted(set(re.findall(r'\bt\d+\b', e)), key=len, reverse=True)
                if not names:
                    break
                for n in names:
                    e = re.sub(r'\b%s\b' % n, '(%s)' % expr[n], e)
            return e
        full = expand(got)
        ok = ('r1.x' in full and 'c1.x' in full and 'c3.x' in full)
        print('   %-4s rotation cancels on oC0.x (finds r1.x, c1.x, c3.x)'
              % ('ok' if ok else 'FAIL'))
        bad += not ok
        cross = 'r1.y' in full or 'r1.z' in full
        print('   %-4s oC0.x does NOT pull in r1.y or r1.z' % ('ok' if not cross else 'FAIL'))
        bad += cross
        okw = sim.oc.c[3] in expr and 'r2.w' in expr[sim.oc.c[3]]
        print('   %-4s the co-issued scalar reaches oC0.w' % ('ok' if okw else 'FAIL'))
        bad += not okw
        print('   %-4s nothing was refused on a clean shader' % ('ok' if not refused else 'FAIL'))
        bad += bool(refused)

        # THE BARE DESTINATION. `mul r5, r1.xyzw, c1.xyzw` writes all four
        # components with no dot and no mask. Treating that as "writes nothing"
        # printed a tidy, WRONG program for ps_d99a15450a08043a -- every later
        # read of r5 fell back to its input value -- and nothing refused. This is
        # that bug as a test.
        p3 = os.path.join(d, 'v.ucode.txt')
        open(p3, 'w').write('''/*    0.0 */       exec
/*    1   */          mul r5, r1.xyzw, c1.xyzw
/*    0.1 */       exece
/*    2   */          mul oC0.xyz_, r5.xyzz, c2.xyzz
''')
        sim3, _ = run(p3, {})
        e3 = dict(sim3.order)
        def expand3(e):
            for _ in range(50):
                names = sorted(set(re.findall(r'\bt\d+\b', e)), key=len, reverse=True)
                if not names:
                    break
                for n in names:
                    e = re.sub(r'\b%s\b' % n, '(%s)' % e3[n], e)
            return e
        f3 = expand3(sim3.oc.c[0])
        okbare = 'c1.x' in f3
        print('   %-4s a BARE destination (`mul r5, ...`) writes all four components'
              % ('ok' if okbare else 'FAIL'))
        bad += not okbare
        # ...and the register must NOT then appear as read-before-written.
        okrbw = 5 not in [n for n, _ in sim3.read_first]
        print('   %-4s r5 is not reported read-before-written after that write'
              % ('ok' if okrbw else 'FAIL'))
        bad += not okrbw

        # MUST REFUSE: a shader with control flow.
        p2 = os.path.join(d, 'u.ucode.txt')
        open(p2, 'w').write(SELFTEST_SRC + '/*  4 */ loop i31, L8\n')
        _, refused2 = run(p2, {})
        print('   %-4s a shader with `loop` is REFUSED' % ('ok' if refused2 else 'FAIL'))
        bad += not refused2
    print('%d of 7 cases pass' % (7 - bad))
    return 1 if bad else 0


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('ucode', nargs='?', help='a .ucode.txt from xenos_translate')
    ap.add_argument('--inputs', default='',
                    help='rename input registers, e.g. r0=lmuv,r1=uv,r3=eye')
    ap.add_argument('--selftest', action='store_true',
                    help='prove the reduction cancels a rotation AND refuses control flow')
    a = ap.parse_args()
    if a.selftest:
        return selftest()
    if not a.ucode:
        ap.error('a .ucode.txt is required (or --selftest)')
    inputs = dict(kv.split('=', 1) for kv in a.inputs.split(',') if '=' in kv)
    return report(a.ucode, inputs)


if __name__ == '__main__':
    sys.exit(main())
