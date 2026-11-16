#!/usr/bin/env python3
"""gens64: translates the NASM output of the Starscream generator (Gens RR r341, Main68k/Sub68k Star.c,
options -hog -name <prefix>) into C with the same behaviour, instruction by instruction.

The x86 registers become globals (eax ... edi, esp pointing into a private stack buffer), the flags a
local FL struct per function with the semantics of tools/star2c/x86ops.h (checked against the real
instructions). Code regions become C functions:
  - subroutines (targets of `call`): void functions, `ret` returns;
  - continuations (instruction handlers, shared tails such as r_bra_b, the exec loop): functions that
    return the next continuation; `jmp dword[__jmptbl+ebx*4]` returns the handler for the opcode,
    a `ret` in exec ends the trampoline in main68k_exec().
Context, jump table and the API (init, GetContext, SetContext, GetContextSize) are emitted directly.

Usage: star2c.py input.asm output.c [--sub]
"""
import re
import sys

R32 = ['eax', 'ecx', 'edx', 'ebx', 'esp', 'ebp', 'esi', 'edi']
R16 = {'ax': 'eax', 'cx': 'ecx', 'dx': 'edx', 'bx': 'ebx', 'sp': 'esp', 'bp': 'ebp', 'si': 'esi', 'di': 'edi'}
R8L = {'al': 'eax', 'cl': 'ecx', 'dl': 'edx', 'bl': 'ebx'}
R8H = {'ah': 'eax', 'ch': 'ecx', 'dh': 'edx', 'bh': 'ebx'}
SIZES = {'byte': 8, 'word': 16, 'dword': 32}
MNEMONICS = set('''mov movsx movzx lea xchg add sub adc sbb cmp and or xor test inc dec neg not shl sal shr sar
rol ror rcl rcr mul imul div idiv push pop pushad popad lahf daa das ret call jmp lodsd lodsb
js jns jz je jnz jne jb jc jnae jnb jnc jae jbe jna ja jnbe jl jnge jge jnl jle jng jg jnle jo jno
seto setno setc setb setnc setae setz sete setnz setne sets setns setbe seta setl setge setle setg'''.split())
DIRECTIVES = set('section bits extern global times align db dw dd resb %assign %rep %endrep %if %endif end'.split())
COND = {
    'o': 'F.o', 'no': '!F.o', 'c': 'F.c', 'b': 'F.c', 'nae': 'F.c', 'nc': '!F.c', 'nb': '!F.c', 'ae': '!F.c',
    'z': 'F.z', 'e': 'F.z', 'nz': '!F.z', 'ne': '!F.z', 's': 'F.s', 'ns': '!F.s',
    'be': '(F.c | F.z)', 'na': '(F.c | F.z)', 'a': '!(F.c | F.z)', 'nbe': '!(F.c | F.z)',
    'l': '(F.s != F.o)', 'nge': '(F.s != F.o)', 'ge': '(F.s == F.o)', 'nl': '(F.s == F.o)',
    'le': '(F.z | (F.s != F.o))', 'ng': '(F.z | (F.s != F.o))', 'g': '!(F.z | (F.s != F.o))',
    'nle': '!(F.z | (F.s != F.o))',
}


def die(msg):
    sys.stderr.write('star2c: ' + msg + '\n')
    sys.exit(1)


def num(s):
    s = s.strip()
    neg = s.startswith('-')
    if neg:
        s = s[1:].strip()
    if re.fullmatch(r'0[xX][0-9a-fA-F]+', s):
        v = int(s, 16)
    elif re.fullmatch(r'[0-9][0-9a-fA-F]*[hH]', s):
        v = int(s[:-1], 16)
    elif re.fullmatch(r'[0-9]+', s):
        v = int(s)
    else:
        return None
    return -v if neg else v


def split_ops(s):
    out, depth, cur = [], 0, ''
    for ch in s:
        if ch == '[' or ch == '(':
            depth += 1
        elif ch == ']' or ch == ')':
            depth -= 1
        if ch == ',' and depth == 0:
            out.append(cur.strip())
            cur = ''
        else:
            cur += ch
    if cur.strip():
        out.append(cur.strip())
    return out


class Op:
    """operand: kind 'reg' (name), 'imm' (value or label), 'mem' (size, terms)"""

    def __init__(self, kind, **kw):
        self.kind = kind
        self.__dict__.update(kw)

    def __repr__(self):
        return '%s%r' % (self.kind, self.__dict__)


def parse_op(s):
    s = s.strip()
    size = None
    m = re.match(r'^(byte|word|dword|near|short)\b\s*(.*)$', s)
    while m:
        if m.group(1) in SIZES:
            size = SIZES[m.group(1)]
        s = m.group(2).strip()
        m = re.match(r'^(byte|word|dword|near|short)\b\s*(.*)$', s)
    if s.startswith('['):
        if not s.endswith(']'):
            die('bad memory operand ' + s)
        return Op('mem', size=size, terms=parse_expr(s[1:-1]))
    if s in R32:
        return Op('reg', name=s, w=32, base=s, part='full')
    if s in R16:
        return Op('reg', name=s, w=16, base=R16[s], part='x')
    if s in R8L:
        return Op('reg', name=s, w=8, base=R8L[s], part='l')
    if s in R8H:
        return Op('reg', name=s, w=8, base=R8H[s], part='h')
    v = num(s)
    if v is not None:
        return Op('imm', value=v & 0xFFFFFFFF, size=size)
    return Op('imm', label=s, value=None, size=size)   # label or symbol expression


def parse_expr(e):
    """sum of terms -> list of ('const', v) / ('reg', name, scale) / ('sym', name)"""
    e = e.replace(' ', '')
    terms = []
    # evaluate parenthesised constant sub-expressions like (0x18*4)
    def paren(m):
        inner = m.group(1)
        v = eval_const(inner)
        return '%d' % v
    while '(' in e:
        e = re.sub(r'\(([^()]*)\)', paren, e)
    parts = re.findall(r'[+-]?[^+-]+', e)
    for p in parts:
        sign = -1 if p.startswith('-') else 1
        p = p.lstrip('+-')
        if '*' in p:
            a, b = p.split('*')
            if a in R32:
                terms.append(('reg', a, int(num(b)) * sign))
            elif b in R32:
                terms.append(('reg', b, int(num(a)) * sign))
            else:
                terms.append(('const', eval_const(p) * sign))
        elif p in R32:
            terms.append(('reg', p, sign))
        elif num(p) is not None:
            terms.append(('const', num(p) * sign))
        else:
            if sign != 1:
                die('negative symbol in ' + e)
            terms.append(('sym', p))
    return terms


def eval_const(e):
    e = e.replace(' ', '')
    tot = 0
    for p in re.findall(r'[+-]?[^+-]+', e):
        sign = -1 if p.startswith('-') else 1
        p = p.lstrip('+-')
        v = 1
        for f in p.split('*'):
            n = num(f)
            if n is None:
                die('not a constant: ' + e)
            v *= n
        tot += sign * v
    return tot


class Line:
    def __init__(self, n, labels, mn, ops, raw):
        self.n, self.labels, self.mn, self.ops, self.raw = n, labels, mn, ops, raw


def read_asm(path):
    text = open(path, encoding='latin-1').read().splitlines()
    lines = []
    pending = []
    section = None
    lastglobal = ''
    data = []      # (labels, directive, args) in .data
    for n, raw in enumerate(text, 1):
        s = raw.split(';', 1)[0].strip()
        if not s:
            continue
        labels = []
        while True:
            m = re.match(r'^([A-Za-z_.$?][\w.$@?]*):\s*(.*)$', s)
            if not m:
                break
            labels.append(m.group(1))
            s = m.group(2).strip()
        tok = s.split(None, 1)
        if tok and tok[0] not in MNEMONICS and tok[0] not in DIRECTIVES and len(tok) == 1 and section == '.text':
            # label without colon (NASM accepts it)
            labels.append(tok[0])
            s = ''
            tok = []
        # resolve local labels
        full = []
        for l in labels:
            if l.startswith('.'):
                full.append(lastglobal + l)
            else:
                lastglobal = l
                full.append(l)
        if tok and tok[0] == 'section':
            section = tok[1].strip()
            continue
        if section == '.text':
            pending += full
            if not tok:
                continue
            mn = tok[0]
            args = tok[1] if len(tok) > 1 else ''
            if mn in ('bits', 'extern', 'global', 'times', 'align'):
                continue
            if mn == 'db' and args.strip() == '3Eh':
                continue   # DS prefix
            if mn in DIRECTIVES:
                lines.append(Line(n, pending, '.' + mn, args, raw))
                pending = []
                continue
            if mn not in MNEMONICS:
                die('line %d: unknown mnemonic %s' % (n, mn))
            ops = [parse_op(o) for o in split_ops(args)]
            # resolve local label operands of jumps
            for o in ops:
                if o.kind == 'imm' and o.value is None and o.label.startswith('.'):
                    o.label = lastglobal + o.label
            lines.append(Line(n, pending, mn, ops, raw))
            pending = []
        else:
            if tok and tok[0] in ('bits', 'extern', 'global', 'times', 'align', 'end'):
                if full:
                    data.append((full, None, None))
                continue
            if not tok:
                if full:
                    data.append((full, None, None))
                continue
            # "name dd 0,0" or "dd ..." with labels
            if tok[0] in ('dd', 'db', 'dw', 'resb'):
                data.append((full, tok[0], tok[1] if len(tok) > 1 else ''))
            else:
                t2 = s.split(None, 2)
                if len(t2) >= 2 and t2[1] in ('dd', 'db', 'dw', 'resb'):
                    data.append((full + [t2[0]], t2[1], t2[2] if len(t2) > 2 else ''))
                else:
                    die('line %d: cannot parse data %r' % (n, raw))
    if pending:
        lines.append(Line(len(text) + 1, pending, '.end', '', ''))
    return lines, data


# ------------------------------------------------------------------------------------------------

class Gen:
    def __init__(self, lines, data, sub):
        self.lines = lines
        self.data = data
        self.sub = sub
        self.out = []
        self.ctx = {}       # symbol -> offset in the context block
        self.ctx_init = []
        self.prefix = None
        self.handlers = []  # jump table compressed entries (label, count)

    # ---- data ----
    def layout_data(self):
        off = None
        cur = 0
        init = bytearray()
        for labels, d, args in self.data:
            for l in labels:
                if l.endswith('context') and l.startswith('_'):
                    self.prefix = l[1:-len('context')]
                    off = 0
                if l == '__jmptblcomp':
                    off = None
                if off is not None:
                    self.ctx[l] = cur
                if l == 'contextend':
                    off = None
            if off is None:
                if d == 'dd':
                    for a in split_ops(args):
                        m = re.fullmatch(r'([A-Za-z_][\w]*)-top(?:\+(\d+))?', a.replace(' ', ''))
                        if m:
                            self.handlers.append((m.group(1), (int(m.group(2)) >> 24) + 1 if m.group(2) else 1))
                continue
            if d is None:
                continue
            vals = split_ops(args)
            size = {'dd': 4, 'dw': 2, 'db': 1}[d]
            for v in vals:
                x = num(v)
                if x is None:
                    die('context initializer ' + v)
                init += (x & ((1 << (8 * size)) - 1)).to_bytes(size, 'little')
                cur += size
        if self.prefix is None:
            die('no context')
        self.ctx_size = self.ctx['contextend']
        self.ctx_init = bytes(init[:self.ctx_size])
        if sum(c for _, c in self.handlers) != 0x10000:
            die('jump table covers %d opcodes' % sum(c for _, c in self.handlers))

    # ---- symbols ----
    def symaddr(self, s):
        if s in self.ctx:
            return 'CA(%d)' % self.ctx[s]
        if s == 'Ram_68k':
            return 'GA(Ram_68k)'
        if s == '_Ram_68k_2MB':	# gens64: 2 MB RAM flag (Mem_M68k_asm.c, docs/overclock.md)
            return 'GA(&Ram_68k_2MB)'
        if s == '_Gens64_Ram_Write_Hook':	# gens64: RAM writes through M68K_WB/WW (Mem_M68k_asm.c)
            return 'GA(&Gens64_Ram_Write_Hook)'
        if s.startswith('_hook_'):
            return 'GA(&%s)' % s[1:]
        if s == '__jmptbl':
            return 'GA(jmptbl)'
        die('unknown data symbol ' + s)

    def addr(self, terms):
        parts = []
        const = 0
        for t in terms:
            if t[0] == 'const':
                const += t[1]
            elif t[0] == 'reg':
                parts.append(t[1] if t[2] == 1 else '%s * %d' % (t[1], t[2]) if t[2] > 0 else '- %s * %d' % (t[1], -t[2]))
            else:
                parts.append(self.symaddr(t[1]))
        if const or not parts:
            parts.append('0x%Xu' % (const & 0xFFFFFFFF))
        e = ' + '.join(parts).replace('+ -', '-')
        return '(u32)(%s)' % e

    # ---- operands ----
    def width(self, ops, default=None):
        for o in ops:
            if o.kind == 'reg':
                return o.w
        for o in ops:
            if o.kind == 'mem' and o.size:
                return o.size
        for o in ops:
            if o.kind == 'imm' and o.size:
                return o.size
        if default:
            return default
        die('cannot tell the operand size')

    def rd(self, o, w):
        if o.kind == 'reg':
            b = o.base
            if o.part == 'full':
                return b
            if o.part == 'x':
                return '(%s & 0xFFFF)' % b
            if o.part == 'l':
                return '(%s & 0xFF)' % b
            return '((%s >> 8) & 0xFF)' % b
        if o.kind == 'imm':
            if o.value is None:
                die('label used as a value: ' + o.label)
            return '0x%Xu' % (o.value & ((1 << w) - 1) if w < 32 else o.value)
        return 'RD%d(%s)' % (w, self.addr(o.terms))

    def wr(self, o, w, v):
        if o.kind == 'reg':
            b = o.base
            if o.part == 'full':
                return '%s = %s;' % (b, v)
            if o.part == 'x':
                return '%s = (%s & 0xFFFF0000u) | ((%s) & 0xFFFF);' % (b, b, v)
            if o.part == 'l':
                return '%s = (%s & 0xFFFFFF00u) | ((%s) & 0xFF);' % (b, b, v)
            return '%s = (%s & 0xFFFF00FFu) | (((%s) & 0xFF) << 8);' % (b, b, v)
        if o.kind == 'mem':
            return 'WR%d(%s, %s);' % (w, self.addr(o.terms), v)
        die('write to immediate')

    # ---- regions ----
    def analyse(self):
        L = self.lines
        self.pos = {}
        for i, ln in enumerate(L):
            for l in ln.labels:
                self.pos[l] = i
        self.calls = set()
        self.jumps = set()
        for i, ln in enumerate(L):
            if ln.mn == 'call' and ln.ops[0].kind == 'imm' and ln.ops[0].label in self.pos:
                self.calls.add(ln.ops[0].label)
        exported = [l for l in self.pos if l.startswith('_' + self.prefix)]
        self.exported = {l[len(self.prefix) + 1:]: l for l in exported}
        handwritten = ('init', 'GetContext', 'SetContext', 'GetContextSize')
        self.skip = set()
        for h in handwritten:
            # the region from this proc to the next exported proc is not translated
            self.skip.add(self.exported[h])
        entries = set(self.calls) | set(exported) | set(h for h, _ in self.handlers)
        changed = True
        while changed:
            changed = False
            starts = sorted(set(self.pos[e] for e in entries))
            owner = {}
            si = 0
            cur = None
            for i in range(len(L)):
                while si < len(starts) and starts[si] <= i:
                    cur = starts[si]
                    si += 1
                owner[i] = cur
            for i, ln in enumerate(L):
                if ln.mn.startswith('j') and ln.ops and ln.ops[0].kind == 'imm':
                    t = ln.ops[0].label
                    if t not in self.pos:
                        die('line %d: unknown jump target %s' % (ln.n, t))
                    if owner[self.pos[t]] != owner[i] and self.pos[t] not in starts:
                        entries.add(t)
                        changed = True
        self.starts = sorted(set(self.pos[e] for e in entries))
        self.entry_at = {}
        for e in entries:
            self.entry_at.setdefault(self.pos[e], []).append(e)
        self.owner = owner
        # kinds
        self.kind = {}
        for p in self.starts:
            names = self.entry_at[p]
            if any(n in self.calls for n in names):
                k = 'sub'
            elif any(n == self.exported.get('exec') for n in names):
                k = 'cont'
            elif any(n in exported for n in names):
                k = 'sub'
            else:
                k = 'cont'
            self.kind[p] = k
        # continuation regions reached by fallthrough from exec are continuations as well (already)

    def fname(self, p):
        names = sorted(self.entry_at[p], key=lambda n: (n.startswith('_'), len(n)))
        return 'R_' + re.sub(r'[^\w]', '_', names[0])

    def lname(self, l):
        return 'L_' + re.sub(r'[^\w]', '_', l)

    # ---- code ----
    def find_tails(self):
        """the hog-mode dispatch tail (fetch, hook, jmp [__jmptbl+ebx*4]) is the same block of instructions
        after every handler: translate each such block once (T_dispatchN) and jump there"""
        L = self.lines
        blocks = {}
        for i, ln in enumerate(L):
            if ln.mn == 'mov' and ln.raw.strip() == 'mov bx,[esi]':
                j = i
                while j < len(L) and not (L[j].mn == 'jmp' and L[j].ops and L[j].ops[0].kind == 'mem') and L[j].mn != 'ret':
                    j += 1
                if j >= len(L) or L[j].mn == 'ret':
                    continue
                if any(x.labels for x in L[i + 1:j + 1]):
                    continue
                key = tuple(x.raw.strip() for x in L[i:j + 1])
                blocks.setdefault(key, []).append(i)
        self.tail_at = {}
        self.tails = []
        for key, ps in blocks.items():
            if len(ps) < 8:
                continue
            k = len(self.tails)
            self.tails.append((L[ps[0]:ps[0] + len(key)], len(key)))
            for p in ps:
                self.tail_at[p] = k

    def gen_tail(self):
        out = []
        for k, (lines, n) in enumerate(self.tails):
            out.append('static void *T_dispatch%d(void)\n{\n\tFL F = Fg;\n' % k)
            for ln in lines:
                code, term = self.instr(ln, 'cont', -1)
                out.append('\t' + code.replace('\n', '\n\t') + '\n')
            out.append('}\n')
        return ''.join(out)

    def gen_region(self, p, end):
        L = self.lines
        kind = self.kind[p]
        fn = self.fname(p)
        out = []
        ret_t = 'static void *' if kind == 'cont' else 'static void '
        out.append('%s%s(void)\n{\n\tFL F = Fg;\n' % (ret_t, fn))
        terminal = False
        skip_to = -1
        for i in range(p, end):
            ln = L[i]
            if i <= skip_to:
                continue
            for l in ln.labels:
                out.append('%s:;\n' % self.lname(l))
            if i in self.tail_at and kind == 'cont' and i + self.tails[self.tail_at[i]][1] <= end:
                out.append('\tFg = F; return (void *)T_dispatch%d;\n' % self.tail_at[i])
                skip_to = i + self.tails[self.tail_at[i]][1] - 1
                terminal = True
                continue
            if ln.mn.startswith('.'):
                if ln.mn == '.end':
                    continue
                die('line %d: directive %s in code' % (ln.n, ln.mn))
            code, terminal = self.instr(ln, kind, i)
            out.append('\t' + code.replace('\n', '\n\t') + '\n')
        if not terminal:
            # fall through into the next region
            if end >= len(L):
                die('code runs off the end')
            out.append('\t' + self.transfer(kind, end, None) + '\n')
        out.append('}\n')
        return ''.join(out)

    def transfer(self, kind, tpos, cond):
        """jump to the region starting at tpos (cross-region)"""
        tk = self.kind[tpos]
        tf = self.fname(tpos)
        if kind == 'cont':
            if tk != 'cont':
                die('continuation jumps into subroutine %s' % tf)
            s = 'Fg = F; return (void *)%s;' % tf
        else:
            if tk != 'sub':
                die('subroutine jumps into continuation %s' % tf)
            s = 'Fg = F; %s(); return;' % tf
        if cond:
            return 'if (%s) { %s }' % (cond, s)
        return s

    def jump(self, ln, kind, i, cond):
        t = ln.ops[0].label
        tp = self.pos[t]
        if self.owner[tp] == self.owner[i]:
            lab = self.lname(t)
            return ('if (%s) goto %s;' % (cond, lab)) if cond else 'goto %s;' % lab
        if tp not in self.entry_at:
            die('line %d: jump into the middle of another region (%s)' % (ln.n, t))
        return self.transfer(kind, tp, cond)

    def call_ext(self, name):
        p = self.prefix
        if name in ('M68K_RB', 'M68K_RW', 'S68K_RB'):
            return 'eax = %s(RD32(esp));' % name
        if name == 'S68K_RW':
            return 'gens64_s68k_ecx_set = 0; eax = S68K_RW(RD32(esp)); if (gens64_s68k_ecx_set) ecx = gens64_s68k_ecx;'
        if name in ('M68K_WB', 'M68K_WW', 'S68K_WB', 'S68K_WW'):
            return '%s(RD32(esp), RD32(esp + 4));' % name
        if name == 'Int_Ack':
            return 'eax = (eax & 0xFFFFFF00u) | (STAR_INT_ACK() & 0xFF);'
        if name.startswith('_hook_'):
            return '%s();' % name[1:]
        die('unknown external call ' + name)

    def instr(self, ln, kind, i):
        mn, ops = ln.mn, ln.ops
        G = self
        if mn == 'mov':
            w = self.width(ops)
            return self.wr(ops[0], w, self.rd(ops[1], w)), False
        if mn in ('movsx', 'movzx'):
            wd = ops[0].w
            ws = ops[1].w if ops[1].kind == 'reg' else ops[1].size
            v = self.rd(ops[1], ws)
            if mn == 'movsx':
                v = '(u32)(s32)(%s)(%s)' % ('s8' if ws == 8 else 's16', v)
            return self.wr(ops[0], wd, v), False
        if mn == 'lea':
            return self.wr(ops[0], 32, self.addr(ops[1].terms)), False
        if mn == 'xchg':
            w = self.width(ops)
            return '{ u32 t = %s; %s %s }' % (self.rd(ops[0], w), self.wr(ops[0], w, self.rd(ops[1], w)), self.wr(ops[1], w, 't')), False
        if mn in ('add', 'adc', 'sub', 'sbb', 'cmp'):
            w = self.width(ops)
            f = 'x_add' if mn in ('add', 'adc') else 'x_sub'
            cin = 'F.c' if mn in ('adc', 'sbb') else '0'
            e = '%s(%d, %s, %s, %s, &F)' % (f, w, self.rd(ops[0], w), self.rd(ops[1], w), cin)
            if mn == 'cmp':
                return '%s;' % e, False
            return '{ u32 r = %s; %s }' % (e, self.wr(ops[0], w, 'r')), False
        if mn in ('and', 'or', 'xor', 'test'):
            w = self.width(ops)
            c = {'and': '&', 'test': '&', 'or': '|', 'xor': '^'}[mn]
            e = 'x_logic(%d, %s %s %s, &F)' % (w, self.rd(ops[0], w), c, self.rd(ops[1], w))
            if mn == 'test':
                return '%s;' % e, False
            return '{ u32 r = %s; %s }' % (e, self.wr(ops[0], w, 'r')), False
        if mn in ('inc', 'dec', 'neg'):
            w = self.width(ops)
            return '{ u32 r = x_%s(%d, %s, &F); %s }' % (mn, w, self.rd(ops[0], w), self.wr(ops[0], w, 'r')), False
        if mn == 'not':
            w = self.width(ops)
            return self.wr(ops[0], w, '~%s' % self.rd(ops[0], w)), False
        if mn in ('shl', 'sal', 'shr', 'sar', 'rol', 'ror', 'rcl', 'rcr'):
            w = self.width(ops[:1])
            f = {'sal': 'shl'}.get(mn, mn)
            cnt = self.rd(ops[1], 8)
            return '{ u32 r = x_%s(%d, %s, %s, &F); %s }' % (f, w, self.rd(ops[0], w), cnt, self.wr(ops[0], w, 'r')), False
        if mn in ('mul', 'imul'):
            w = self.width(ops)
            if w != 16:
                die('line %d: %s only 16 bit' % (ln.n, mn))
            return ('{ u32 r = x_%s16(eax, %s, &F); eax = (eax & 0xFFFF0000u) | (r & 0xFFFF); '
                    'edx = (edx & 0xFFFF0000u) | (r >> 16); }' % (mn, self.rd(ops[0], 16))), False
        if mn in ('div', 'idiv'):
            w = self.width(ops)
            if w != 32:
                die('line %d: %s only 32 bit' % (ln.n, mn))
            return 'x_%s32(%s);' % (mn, self.rd(ops[0], 32)), False
        if mn == 'push':
            v = self.rd(ops[0], 32)
            return '{ u32 v = %s; esp -= 4; WR32(esp, v); }' % v, False
        if mn == 'pop':
            if ops[0].kind == 'mem':
                return '{ u32 v = RD32(esp); esp += 4; %s }' % self.wr(ops[0], 32, 'v'), False
            return '%s = RD32(esp); esp += 4;' % ops[0].base if ops[0].part == 'full' else die('pop partial'), False
        if mn == 'pushad':
            return ('{ u32 s = esp; esp -= 32; WR32(esp + 28, eax); WR32(esp + 24, ecx); WR32(esp + 20, edx); '
                    'WR32(esp + 16, ebx); WR32(esp + 12, s); WR32(esp + 8, ebp); WR32(esp + 4, esi); WR32(esp, edi); }'), False
        if mn == 'popad':
            return ('edi = RD32(esp); esi = RD32(esp + 4); ebp = RD32(esp + 8); ebx = RD32(esp + 16); '
                    'edx = RD32(esp + 20); ecx = RD32(esp + 24); eax = RD32(esp + 28); esp += 32;'), False
        if mn == 'lahf':
            return 'eax = (eax & 0xFFFF00FFu) | (x_lahf(&F) << 8);', False
        if mn in ('daa', 'das'):
            return 'eax = (eax & 0xFFFFFF00u) | x_%s(eax & 0xFF, &F);' % mn, False
        if mn.startswith('set'):
            c = COND[mn[3:]]
            return self.wr(ops[0], 8, '(%s ? 1 : 0)' % c), False
        if mn == 'jmp':
            o = ops[0]
            if o.kind == 'mem':
                t = o.terms
                if [x for x in t if x[0] == 'sym'] == [('sym', '__jmptbl')]:
                    if kind != 'cont':
                        die('dispatch from a subroutine')
                    return 'Fg = F; return (void *)jmptbl[ebx & 0xFFFF];', True
                die('line %d: indirect jump' % ln.n)
            return self.jump(ln, kind, i, None), True
        if mn.startswith('j'):
            return self.jump(ln, kind, i, COND[mn[1:]]), False
        if mn == 'call':
            o = ops[0]
            if o.kind == 'reg':
                return '((void (*)(void))GP(%s))();' % o.base, False
            t = o.label
            if t in self.pos:
                tp = self.pos[t]
                if self.kind.get(tp) != 'sub':
                    die('call to a non-subroutine %s' % t)
                return 'esp -= 4; WR32(esp, 0); Fg = F; %s(); F = Fg;' % self.fname(tp), False
            return self.call_ext(t), False
        if mn == 'ret':
            if kind == 'sub':
                return 'esp += 4; Fg = F; return;', True
            return 'esp += 4; Fg = F; return 0;', True
        die('line %d: cannot translate %s' % (ln.n, ln.raw.strip()))

    def run(self, out_path):
        self.layout_data()
        self.analyse()
        self.find_tails()
        self.entry_labels = set()
        L = self.lines
        p = self.prefix
        o = []
        o.append('/* Generated by tools/star2c/star2c.py from the Starscream output (do not edit). */\n')
        o.append(PRELUDE.replace('@P@', p).replace('@CTXSIZE@', str(self.ctx_size)))
        if self.sub:
            o.append('extern u32 gens64_s68k_ecx_set, gens64_s68k_ecx;\n')
            o.append('u32 S68K_RB(u32);\nu32 S68K_RW(u32);\nvoid S68K_WB(u32, u32);\nvoid S68K_WW(u32, u32);\n')
            hk = '_cd'
        else:
            o.append('u32 M68K_RB(u32);\nu32 M68K_RW(u32);\nvoid M68K_WB(u32, u32);\nvoid M68K_WW(u32, u32);\n')
            o.append('#ifndef STAR_INT_ACK\n#define STAR_INT_ACK Int_Ack\n#endif\nu32 STAR_INT_ACK(void);\n')
            hk = ''
        for h in ('exec', 'read_byte', 'read_word', 'read_dword', 'write_byte', 'write_word', 'write_dword'):
            o.append('void hook_%s%s(void);\n' % (h, hk))
        for h in ('pc', 'address', 'value'):
            o.append('extern u32 hook_%s%s;\n' % (h, hk))
        o.append('u8 %scontext[%d] __attribute__((aligned(8))) = {%s};\n' % (
            p, self.ctx_size, ', '.join('0x%02X' % b for b in self.ctx_init)))
        o.append('static void *(*jmptbl[0x10000])(void);\n')
        # prototypes
        for s in self.starts:
            fn = self.fname(s)
            if self.entry_at[s][0] in self.skip or any(n in self.skip for n in self.entry_at[s]):
                continue
            o.append('static void %s%s(void);\n' % ('*' if self.kind[s] == 'cont' else '', fn))
        o.append(self.gen_tail())
        # regions
        bounds = self.starts + [len(L)]
        first = self.pos[self.exported['init']] if 'init' in self.exported else None
        skipping = False
        for k, s in enumerate(self.starts):
            names = self.entry_at[s]
            if any(n in self.skip for n in names):
                skipping = True
            elif any(n.startswith('_' + p) for n in names):
                skipping = False
            if skipping:
                continue
            o.append(self.gen_region(s, bounds[k + 1]))
        # jump table
        o.append('static void *(*const handlers[])(void) = {\n')
        names = []
        for h, c in self.handlers:
            names.append(self.fname(self.pos[h]))
        o.append(',\n'.join('\t' + n for n in names))
        o.append('\n};\nstatic const unsigned short counts[] = {\n')
        o.append(',\n'.join('\t%d' % c for _, c in self.handlers))
        o.append('\n};\n')
        o.append(API.replace('@P@', p).replace('@N@', str(len(self.handlers))))
        # exported wrappers
        for name, lab in sorted(self.exported.items()):
            if lab in self.skip:
                continue
            o.append(self.wrapper(name, lab))
        open(out_path, 'w').write(''.join(o))

    def wrapper(self, name, lab):
        p = self.prefix
        fn = self.fname(self.pos[lab])
        sig = {
            'exec': ('u32', ['u32']), 'reset': ('u32', []), 'interrupt': ('u32', ['u32', 'u32']),
            'flushInterrupts': ('void', []), 'fetch': ('u32', ['u32']), 'readOdometer': ('u32', []),
            'tripOdometer': ('u32', []), 'controlOdometer': ('u32', ['u32']), 'releaseTimeslice': ('void', []),
            'releaseCycles': ('eax', []), 'addCycles': ('void', ['u32']), 'readPC': ('u32', []),
        }[name]
        rt, args = sig
        params = ', '.join('u32 a%d' % i for i in range(len(args))) or 'void'
        cname = '%s%s' % (p, name)
        if rt == 'eax':
            params = 'u32 a_eax'   # (the asm took the argument in eax)
        s = []
        s.append('%s %s(%s)\n{\n' % ('u32' if rt == 'u32' else 'void', cname, params))
        s.append('\tu32 r, s[8];\n\tFL sf = Fg;\n\tSAVE_REGS(s);\n\tif (!esp)\n\t\tesp = GA(stack_mem + sizeof(stack_mem));\n')
        if rt == 'eax':
            s.append('\teax = a_eax;\n')
        for i in reversed(range(len(args))):
            s.append('\tesp -= 4; WR32(esp, a%d);\n' % i)
        s.append('\tesp -= 4; WR32(esp, 0);\n')
        if self.kind[self.pos[lab]] == 'cont':
            s.append('\t{\n\t\tvoid *(*f)(void) = %s;\n\t\twhile (f)\n\t\t\tf = (void *(*)(void))f();\n\t}\n' % fn)
        else:
            s.append('\t%s();\n' % fn)
        if args:
            s.append('\tesp += %d;\n' % (4 * len(args)))
        s.append('\tr = eax;\n\tRESTORE_REGS(s);\n\tFg = sf;\n')
        s.append('\treturn r;\n}\n' if rt == 'u32' else '\t(void)r;\n}\n')
        return ''.join(s)


PRELUDE = r'''
#include <string.h>
typedef unsigned char u8;
typedef signed char s8;
typedef unsigned short u16;
typedef short s16;
typedef unsigned int u32;
typedef int s32;
typedef unsigned long long u64;
typedef long long s64;
#include "x86ops.h"
#include "gens64_addr.h"

/* the emulated x86 registers and memory operands hold 32-bit host addresses (gaddr, gens64_addr.h) */
#define RD8(a) (*(u8 *)GP(a))
#define RD16(a) (*(u16 *)GP(a))
#define RD32(a) (*(u32 *)GP(a))
#define WR8(a, v) (*(u8 *)GP(a) = (u8)(v))
#define WR16(a, v) (*(u16 *)GP(a) = (u16)(v))
#define WR32(a, v) (*(u32 *)GP(a) = (u32)(v))
#define CA(o) GA(@P@context + (o))

extern u8 Ram_68k[];
extern u8 Ram_68k_2MB;
extern u8 Gens64_Ram_Write_Hook;
extern u8 @P@context[];
static u32 eax, ecx, edx, ebx, esp, ebp, esi, edi;
static FL Fg;
static u8 stack_mem[16384] __attribute__((aligned(16)));
#define SAVE_REGS(s) (s[0] = eax, s[1] = ecx, s[2] = edx, s[3] = ebx, s[4] = esp, s[5] = ebp, s[6] = esi, s[7] = edi)
#define RESTORE_REGS(s) (eax = s[0], ecx = s[1], edx = s[2], ebx = s[3], esp = s[4], ebp = s[5], esi = s[6], edi = s[7])

/* div/idiv r32 (edx:eax / src); the asm faulted (#DE) on a quotient overflow: left unchanged here */
static void x_div32(u32 s)
{
	u64 n = (u64)edx << 32 | eax, q;
	if (!s || (q = n / s) > 0xFFFFFFFFu)
		return;
	edx = (u32)(n % s);
	eax = (u32)q;
}
static void x_idiv32(u32 s)
{
	s64 n = (s64)((u64)edx << 32 | eax), q;
	if (!s || (n == (s64)0x8000000000000000ull && (s32)s == -1))
		return;
	q = n / (s32)s;
	if (q > 0x7FFFFFFF || q < -(s64)0x80000000)
		return;
	edx = (u32)(n % (s32)s);
	eax = (u32)q;
}
'''

API = r'''
int @P@init(void)
{
	int i, j, k = 0;
	for (i = 0; i < @N@; i++)
		for (j = 0; j < counts[i]; j++)
			jmptbl[k++] = handlers[i];
	return 0;
}
int @P@GetContextSize(void)
{
	return (int)sizeof(@P@context);
}
void @P@GetContext(void *c)
{
	memcpy(c, @P@context, sizeof(@P@context));
}
void @P@SetContext(void *c)
{
	memcpy(@P@context, c, sizeof(@P@context));
}
/* debugging aid: the emulated x86 registers */
void @P@x86regs(u32 *r)
{
	SAVE_REGS(r);
}
'''


def main():
    args = [a for a in sys.argv[1:] if not a.startswith('--')]
    sub = '--sub' in sys.argv
    if len(args) != 2:
        die('usage: star2c.py input.asm output.c [--sub]')
    lines, data = read_asm(args[0])
    Gen(lines, data, sub).run(args[1])


if __name__ == '__main__':
    main()
