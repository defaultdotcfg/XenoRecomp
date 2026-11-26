"""Static ARMv7 Thumb-2 (+VFPv3-D32, NEON) -> C lifter for a thin 32-bit Mach-O (Xenowerk).
Uses Capstone's operand detail for decoding; semantics are generated here.

Usage: python lift_t2.py <thin-armv7-macho> <outdir>
Outputs outdir/f_NNN.c chunks, func_table.c, imports.h, protos.h, rt_tables.c, rt_defaults.c, report.txt
"""
import sys, os, re, struct, collections, bisect
import capstone
from capstone import arm as A
from macho import Macho, load_imports
import neon_lift
from common import REGN, Unsupported

M32 = 0xffffffff

# capstone condition codes: EQ=1 NE=2 HS=3 LO=4 MI=5 PL=6 VS=7 VC=8 HI=9 LS=10 GE=11 LT=12 GT=13 LE=14 AL=15
CONDS = {1: 'c->z', 2: '!c->z', 3: 'c->c', 4: '!c->c', 5: 'c->n', 6: '!c->n', 7: 'c->v', 8: '!c->v',
         9: '(c->c && !c->z)', 10: '(!c->c || c->z)', 11: '(c->n == c->v)', 12: '(c->n != c->v)',
         13: '(!c->z && c->n == c->v)', 14: '(c->z || c->n != c->v)'}
AL = 15


def inv_cc(cc):
    return cc + 1 if cc & 1 else cc - 1


NAMEMAP = {'movs': 'mov', 'adds': 'add', 'subs': 'sub', 'fmstat': 'vmrs', 'fconsts': 'vmov', 'fconstd': 'vmov',
           'fmdhr': 'vmov', 'fmrdh': 'vmov', 'fmdlr': 'vmov', 'fmrdl': 'vmov', 'fmrs': 'vmov', 'fmsr': 'vmov',
           'fmrrs': 'vmov', 'fmsrr': 'vmov', 'fmrrd': 'vmov', 'fmdrr': 'vmov', 'vstmdb': 'vstmdb', 'bxns': 'trap', 'blxns': 'trap',
           'cdp': 'trap', 'cdp2': 'trap', 'ldc': 'trap', 'ldc2': 'trap', 'ldcl': 'trap', 'ldc2l': 'trap', 'stc': 'trap', 'stc2': 'trap',
           'stcl': 'trap', 'stc2l': 'trap', 'mcr': 'trap', 'mcr2': 'trap', 'mrc2': 'trap', 'mcrr': 'trap', 'mcrr2': 'trap', 'mrrc': 'trap', 'mrrc2': 'trap'}




class Ins:
    """one lifted instruction"""
    def __init__(self, addr, size):
        self.addr = addr
        self.size = size
        self.L = []           # C statements (unconditional form; the caller wraps them with the IT/branch condition)
        self.targets = []     # labels needed
        self.falls = True     # may fall through to addr+size
        self.cond = AL
        self.tag = ''
        self.ender = False    # unconditional terminator (no fallthrough even if conditional wrapper absent)
        self.in_it = False


class Lifter:
    def __init__(self, m):
        self.m = m
        self.cs = capstone.Cs(capstone.CS_ARCH_ARM, capstone.CS_MODE_THUMB)
        self.cs.detail = True
        self.stats = collections.Counter()
        self.unsup = collections.Counter()
        self.entries = set()
        self.new_entries = set()
        d = sorted((a, a + l, k) for a, l, k in m.dice)
        self.dice = d
        self.dice_starts = [x[0] for x in d]
        self.jt = {a: (l, k) for a, l, k in m.dice if k in (2, 3, 4, 5)}
        self.names = {}
        self.ex = {}
        self.cur_const = None
        self.prev_const = None

    # ----------------------------------------------------------- helpers
    def in_data(self, a):
        i = bisect.bisect_right(self.dice_starts, a) - 1
        return i >= 0 and self.dice[i][0] <= a < self.dice[i][1]

    def rn(self, reg):
        return REGN[self.cs.reg_name(reg)]

    def rv(self, reg, X):
        n = self.rn(reg)
        return ('0x%xu' % ((X.addr + 4) & M32)) if n == 15 else 'R(%d)' % n

    def u16(self, a):
        return struct.unpack_from('<H', self.m.buf, a)[0]

    def call_stmt(self, tgt, X):
        m = self.m
        if m.stub_lo <= tgt < m.stub_hi:
            base = tgt - (tgt - m.stub_lo) % 16
            name = m.stubs.get(base, '?')
            return 'imp_%s(c);' % re.sub(r'\W', '_', name)
        t = tgt & ~1
        if t in self.entries:
            return 'f_%08x(c);' % t
        if m.text['addr'] <= t < m.text['addr'] + m.text['size']:
            self.new_entries.add(t)
        return 'arm_dispatch(c, 0x%xu);' % (tgt if tgt & 1 else tgt)

    def is_noreturn_stub(self, tgt):
        m = self.m
        if not (m.stub_lo <= tgt < m.stub_hi):
            return False
        nm = m.stubs.get(tgt - (tgt - m.stub_lo) % 16, '')
        return bool(re.search(r'__throw|_abort$|^_exit$|__cxa_throw|_Unwind_Resume|__stack_chk_fail|objc_exception_throw|_longjmp|___assert|_pthread_exit|__cxa_rethrow|__cxa_pure_virtual|___cxa_call_unexpected', nm))

    # ----------------------------------------------------------- shifts
    def shift_expr(self, kind, v, amt):
        """immediate shift of expression v. returns (value_expr, carry_expr|None). amt is a python int."""
        if kind == 'lsl':
            if amt == 0:
                return v, None
            if amt >= 32:
                return '0u', ('(%s & 1u)' % v if amt == 32 else '0u')
            return '(%s << %d)' % (v, amt), '((%s >> %d) & 1u)' % (v, 32 - amt)
        if kind == 'lsr':
            if amt == 0:
                amt = 32
            if amt >= 32:
                return '0u', ('(%s >> 31)' % v if amt == 32 else '0u')
            return '(%s >> %d)' % (v, amt), '((%s >> %d) & 1u)' % (v, amt - 1)
        if kind == 'asr':
            if amt == 0 or amt >= 32:
                return '(uint32_t)((int32_t)%s >> 31)' % v, '(%s >> 31)' % v
            return '(uint32_t)((int32_t)%s >> %d)' % (v, amt), '((%s >> %d) & 1u)' % (v, amt - 1)
        if kind == 'ror':
            if amt == 0:
                return v, None
            amt &= 31
            if amt == 0:
                return v, '(%s >> 31)' % v
            return 'ror32(%s, %d)' % (v, amt), '((%s >> %d) & 1u)' % (v, amt - 1)
        if kind == 'rrx':
            return '((c->c << 31) | (%s >> 1))' % v, '(%s & 1u)' % v
        raise Unsupported('shift ' + kind)

    SFT = {A.ARM_SFT_ASR: 'asr', A.ARM_SFT_LSL: 'lsl', A.ARM_SFT_LSR: 'lsr', A.ARM_SFT_ROR: 'ror', A.ARM_SFT_RRX: 'rrx'}

    def imm_carry(self, ins, X, o):
        """carry-out of a Thumb-2 modified immediate (only for 32-bit encodings); None = carry unchanged"""
        if ins.size != 4:
            return None
        hw1, hw2 = self.u16(X.addr), self.u16(X.addr + 2)
        imm12 = (((hw1 >> 10) & 1) << 11) | (((hw2 >> 12) & 7) << 8) | (hw2 & 0xff)
        if (imm12 >> 10) == 0:
            return None
        return '%du' % ((o.imm & M32) >> 31)

    def op2(self, o, ins, X, logic):
        """second operand: returns (expr, carry_expr|None)"""
        if o.type == A.ARM_OP_IMM:
            c = self.imm_carry(ins, X, o) if logic else None
            return '0x%xu' % (o.imm & M32), c
        if o.type == A.ARM_OP_REG:
            v = self.rv(o.reg, X)
            st = o.shift.type
            if st == 0:
                return v, None
            if st in (A.ARM_SFT_ASR_REG, A.ARM_SFT_LSL_REG, A.ARM_SFT_LSR_REG, A.ARM_SFT_ROR_REG, A.ARM_SFT_RRX_REG):
                raise Unsupported('register-shifted operand')
            kind = self.SFT[st]
            X.L.append('t3 = %s;' % v)
            return self.shift_expr(kind, 't3', o.shift.value)
        raise Unsupported('op2 type %d' % o.type)

    # ----------------------------------------------------------- destination write
    def wr_dest(self, d, expr, X):
        """assign expr to core register d; writes to PC become a branch"""
        if d == 15:
            X.L.append('t0 = %s; arm_dispatch(c, t0); return;' % expr)
            X.falls = False
            X.ender = True
        else:
            X.L.append('R(%d) = %s;' % (d, expr))

    S_SET = {'adds', 'adcs', 'subs', 'sbcs', 'rsbs', 'ands', 'orrs', 'eors', 'bics', 'orns', 'movs', 'mvns', 'lsls', 'lsrs', 'asrs',
             'rors', 'rrxs', 'muls'}

    def uf(self, ins, nm, X):
        """does this instruction update the flags? (Capstone's update_flags also fires for adc/sbc that merely read C;
        16-bit flag-setting forms do not set flags inside an IT block)"""
        if nm in ('cmp', 'cmn', 'tst', 'teq'):
            return True
        if ins.mnemonic.split('.')[0] not in self.S_SET:
            return False
        if X.in_it and ins.size == 2:
            return False
        return True

    # ============================================================ integer ALU
    def h_alu(self, ins, nm, X):
        ops = ins.operands
        nodest = nm in ('cmp', 'cmn', 'tst', 'teq')
        S = self.uf(ins, nm, X) or nodest
        logic = nm in ('and', 'orr', 'eor', 'bic', 'orn', 'mov', 'mvn', 'tst', 'teq')
        L = X.L
        if nodest:
            d = None
            a, b = ops[0], ops[1]
        elif nm in ('mov', 'mvn', 'movw'):
            d = self.rn(ops[0].reg); a = None; b = ops[1]
        elif len(ops) == 2:
            d = self.rn(ops[0].reg); a = ops[0]; b = ops[1]
        else:
            d = self.rn(ops[0].reg); a = ops[1]; b = ops[2]
        if a is not None and nm in ('addw', 'subw') and self.rn(a.reg) == 15 and b.type == A.ARM_OP_IMM:
            v = ((X.addr + 4) & ~3) + (b.imm if nm == 'addw' else -b.imm)
            self.cur_const = (d, v & M32)
            return self.wr_dest(d, '0x%xu' % (v & M32), X)
        if a is not None:
            L.append('t1 = %s;' % self.rv(a.reg, X))
        e2, cexp = self.op2(b, ins, X, logic and S)
        L.append('t2 = %s;' % e2)
        if cexp is not None:
            L.append('t3 = %s;' % cexp)      # carry is computed from the pre-write state
        base = nm.rstrip('s') if nm in ('adds', 'subs') else nm
        res = None
        if nm in ('add', 'addw'):
            if S:
                res = 'adc_s(c, t1, t2, 0)'
            else:
                res = 't1 + t2'
        elif nm == 'adc':
            res = 'adc_s(c, t1, t2, c->c)' if S else 't1 + t2 + c->c'
        elif nm in ('sub', 'subw'):
            res = 'adc_s(c, t1, ~t2, 1)' if S else 't1 - t2'
        elif nm == 'sbc':
            res = 'adc_s(c, t1, ~t2, c->c)' if S else 't1 + ~t2 + c->c'
        elif nm == 'rsb':
            res = 'adc_s(c, t2, ~t1, 1)' if S else 't2 - t1'
        elif nm == 'cmp':
            L.append('(void)adc_s(c, t1, ~t2, 1);'); X.tag = nm; return
        elif nm == 'cmn':
            L.append('(void)adc_s(c, t1, t2, 0);'); X.tag = nm; return
        elif nm in ('and', 'tst'):
            res = 't1 & t2'
        elif nm in ('orr',):
            res = 't1 | t2'
        elif nm in ('eor', 'teq'):
            res = 't1 ^ t2'
        elif nm == 'bic':
            res = 't1 & ~t2'
        elif nm == 'orn':
            res = 't1 | ~t2'
        elif nm in ('mov', 'movw'):
            res = 't2'
        elif nm == 'mvn':
            res = '~t2'
        else:
            raise Unsupported('alu ' + nm)
        if nm in ('tst', 'teq'):
            L.append('t0 = %s; SET_NZ(t0);' % res)
            if cexp is not None:
                L.append('c->c = t3;')
            X.tag = nm
            return
        logic_flags = S and logic
        if logic_flags:
            L.append('t0 = %s;' % res)
            self.wr_dest(d, 't0', X)
            L.append('SET_NZ(t0);')
            if cexp is not None:
                L.append('c->c = t3;')
        else:
            self.wr_dest(d, res, X)
        X.tag = nm

    def h_movt(self, ins, nm, X):
        ops = ins.operands
        d = self.rn(ops[0].reg)
        X.L.append('R(%d) = (R(%d) & 0xffffu) | 0x%xu;' % (d, d, (ops[1].imm & 0xffff) << 16))

    def h_adr(self, ins, nm, X):
        ops = ins.operands
        d = self.rn(ops[0].reg)
        v = ((X.addr + 4) & ~3) + ops[1].imm
        self.cur_const = (d, v & M32)
        self.wr_dest(d, '0x%xu' % ((v + (0x600000 if 0x4000 <= v < 0x10000 else 0)) & M32), X)   # code below 64 KiB is mirrored at +0x600000

    def h_shift(self, ins, nm, X):
        ops = ins.operands
        L = X.L
        S = self.uf(ins, nm, X)
        d = self.rn(ops[0].reg)
        if nm == 'rrx':
            L.append('t1 = %s;' % self.rv(ops[1].reg, X))
            val, car = self.shift_expr('rrx', 't1', 1)
        elif len(ops) == 3 and ops[2].type == A.ARM_OP_IMM:
            L.append('t1 = %s;' % self.rv(ops[1].reg, X))
            val, car = self.shift_expr(nm, 't1', ops[2].imm)
        else:
            if len(ops) == 3:
                src, amt = ops[1], ops[2]
            else:
                src, amt = ops[0], ops[1]
            typ = {'lsl': 0, 'lsr': 1, 'asr': 2, 'ror': 3}[nm]
            L.append('t1 = sh_reg(c, %d, %s, %s);' % (typ, self.rv(src.reg, X), self.rv(amt.reg, X)))
            val, car = 't1', 'c->sc'
        if S:
            L.append('t2 = %s;' % val)
            if car is not None:
                L.append('t3 = %s;' % car)
            self.wr_dest(d, 't2', X)
            L.append('SET_NZ(t2);')
            if car is not None:
                L.append('c->c = t3;')
        else:
            self.wr_dest(d, val, X)

    # ------------------------------------------------------------ multiply / misc ALU
    def h_mul(self, ins, nm, X):
        ops = ins.operands
        L = X.L
        r = [self.rv(o.reg, X) for o in ops]
        d = self.rn(ops[0].reg)
        S = self.uf(ins, nm, X)
        if nm == 'mul':
            if len(ops) == 2:
                L.append('R(%d) = %s * %s;' % (d, r[0], r[1]))
            else:
                L.append('R(%d) = %s * %s;' % (d, r[1], r[2]))
            if S:
                L.append('SET_NZ(R(%d));' % d)
        elif nm == 'mla':
            L.append('R(%d) = %s * %s + %s;' % (d, r[1], r[2], r[3]))
        elif nm == 'mls':
            L.append('R(%d) = %s - %s * %s;' % (d, r[3], r[1], r[2]))
        elif nm in ('umull', 'smull', 'umlal', 'smlal'):
            lo, hi = self.rn(ops[0].reg), self.rn(ops[1].reg)
            if nm[0] == 's':
                L.append('u0 = (uint64_t)((int64_t)(int32_t)%s * (int64_t)(int32_t)%s);' % (r[2], r[3]))
            else:
                L.append('u0 = (uint64_t)%s * (uint64_t)%s;' % (r[2], r[3]))
            if nm.endswith('lal'):
                L.append('u0 += ((uint64_t)R(%d) << 32) | R(%d);' % (hi, lo))
            L.append('R(%d) = (uint32_t)u0; R(%d) = (uint32_t)(u0 >> 32);' % (lo, hi))
        elif nm == 'umaal':
            lo, hi = self.rn(ops[0].reg), self.rn(ops[1].reg)
            L.append('u0 = (uint64_t)%s * (uint64_t)%s + R(%d) + R(%d); R(%d) = (uint32_t)u0; R(%d) = (uint32_t)(u0 >> 32);' % (r[2], r[3], lo, hi, lo, hi))
        elif nm in ('smmul', 'smmulr'):
            rnd = ' + 0x80000000ll' if nm.endswith('r') else ''
            L.append('R(%d) = (uint32_t)(((int64_t)(int32_t)%s * (int64_t)(int32_t)%s%s) >> 32);' % (d, r[1], r[2], rnd))
        elif nm in ('smmla', 'smmlar', 'smmls', 'smmlsr'):
            rnd = ' + 0x80000000ll' if nm.endswith('r') else ''
            sg = '-' if nm.startswith('smmls') else '+'
            L.append('R(%d) = (uint32_t)(((((int64_t)(int32_t)%s) << 32) %s (int64_t)(int32_t)%s * (int64_t)(int32_t)%s%s) >> 32);' % (d, r[3], sg, r[1], r[2], rnd))
        elif nm == 'udiv':
            L.append('R(%d) = %s ? %s / %s : 0;' % (d, r[2], r[1], r[2]))
        elif nm == 'sdiv':
            L.append('R(%d) = %s ? (uint32_t)((int32_t)%s / (int32_t)%s) : 0;' % (d, r[2], r[1], r[2]))
        else:
            m = re.match(r'sm(ul|la)(b|t)(b|t)$', nm)
            mw = re.match(r'sm(ul|la)w(b|t)$', nm)
            if m:
                x = (lambda reg, t: ('(int32_t)(int16_t)(%s >> 16)' if t == 't' else '(int32_t)(int16_t)%s') % reg)
                prod = '(uint32_t)(%s * %s)' % (x(r[1], m.group(2)), x(r[2], m.group(3)))
                if m.group(1) == 'la':
                    prod += ' + ' + r[3]
                L.append('R(%d) = %s;' % (d, prod))
            elif mw:
                y = ('(int32_t)(int16_t)(%s >> 16)' if mw.group(2) == 't' else '(int32_t)(int16_t)%s') % r[2]
                prod = '(uint32_t)(((int64_t)(int32_t)%s * (int64_t)%s) >> 16)' % (r[1], y)
                if mw.group(1) == 'la':
                    prod += ' + ' + r[3]
                L.append('R(%d) = %s;' % (d, prod))
            elif nm in ('smlad', 'smladx', 'smuad', 'smuadx'):
                b = ('ror32(%s, 16)' % r[2]) if nm.endswith('x') else r[2]
                L.append('t1 = %s;' % b)
                e = '(uint32_t)((int32_t)(int16_t)%s * (int32_t)(int16_t)t1 + (int32_t)(int16_t)(%s >> 16) * (int32_t)(int16_t)(t1 >> 16))' % (r[1], r[1])
                if nm.startswith('smla'):
                    e += ' + ' + r[3]
                L.append('R(%d) = %s;' % (d, e))
            else:
                raise Unsupported('mul ' + nm)

    def h_bits(self, ins, nm, X):
        ops = ins.operands
        L = X.L
        d = self.rn(ops[0].reg)
        if nm == 'bfc':
            lsb, w = ops[1].imm, ops[2].imm
            mask = ((1 << w) - 1) << lsb
            L.append('R(%d) &= 0x%xu;' % (d, (~mask) & M32))
        elif nm == 'bfi':
            lsb, w = ops[2].imm, ops[3].imm
            mask = ((1 << w) - 1) << lsb
            L.append('R(%d) = (R(%d) & 0x%xu) | ((%s << %d) & 0x%xu);' % (d, d, (~mask) & M32, self.rv(ops[1].reg, X), lsb, mask))
        elif nm == 'ubfx':
            lsb, w = ops[2].imm, ops[3].imm
            L.append('R(%d) = (%s >> %d) & 0x%xu;' % (d, self.rv(ops[1].reg, X), lsb, (1 << w) - 1))
        elif nm == 'sbfx':
            lsb, w = ops[2].imm, ops[3].imm
            L.append('R(%d) = (uint32_t)((int32_t)(%s << %d) >> %d);' % (d, self.rv(ops[1].reg, X), 32 - lsb - w, 32 - w))
        elif nm == 'clz':
            L.append('R(%d) = clz_(%s);' % (d, self.rv(ops[1].reg, X)))
        elif nm == 'rbit':
            L.append('R(%d) = rbit_(%s);' % (d, self.rv(ops[1].reg, X)))
        elif nm == 'rev':
            L.append('R(%d) = bswap32_(%s);' % (d, self.rv(ops[1].reg, X)))
        elif nm == 'rev16':
            L.append('R(%d) = rev16_(%s);' % (d, self.rv(ops[1].reg, X)))
        elif nm == 'revsh':
            L.append('R(%d) = revsh_(%s);' % (d, self.rv(ops[1].reg, X)))
        else:
            raise Unsupported('bits ' + nm)

    def h_ext(self, ins, nm, X):
        """uxtb/uxth/sxtb/sxth and the accumulate forms uxtab/uxtah/sxtab/sxtah; optional ror"""
        ops = ins.operands
        d = self.rn(ops[0].reg)
        acc = nm.endswith('ab') or nm.endswith('ah')
        src = ops[2] if acc else ops[1]
        v = self.rv(src.reg, X)
        if src.shift.type == A.ARM_SFT_ROR and src.shift.value:
            v = 'ror32(%s, %d)' % (v, src.shift.value)
        k = {'uxtab': 'uxtb', 'uxtah': 'uxth', 'sxtab': 'sxtb', 'sxtah': 'sxth'}.get(nm, nm)
        e = {'uxtb': '(%s & 0xffu)', 'uxth': '(%s & 0xffffu)', 'sxtb': '(uint32_t)(int32_t)(int8_t)(%s)', 'sxth': '(uint32_t)(int32_t)(int16_t)(%s)'}[k] % v
        if acc:
            e = '%s + %s' % (self.rv(ops[1].reg, X), e)
        X.L.append('R(%d) = %s;' % (d, e))

    def h_sat(self, ins, nm, X):
        ops = ins.operands
        d = self.rn(ops[0].reg)
        sat = ops[1].imm
        src = ops[2]
        v = self.rv(src.reg, X)
        if src.shift.type in (A.ARM_SFT_LSL, A.ARM_SFT_ASR) and src.shift.value:
            v = ('(%s << %d)' if src.shift.type == A.ARM_SFT_LSL else '(uint32_t)((int32_t)%s >> %d)') % (v, src.shift.value)
        X.L.append('R(%d) = %s(c, %s, %d);' % (d, 'ssat_' if nm == 'ssat' else 'usat_', v, sat))

    def h_pkh(self, ins, nm, X):
        ops = ins.operands
        d = self.rn(ops[0].reg)
        a = self.rv(ops[1].reg, X)
        b = self.rv(ops[2].reg, X)
        sh = ops[2].shift
        if nm == 'pkhbt':
            if sh.type == A.ARM_SFT_LSL and sh.value:
                b = '(%s << %d)' % (b, sh.value)
            X.L.append('R(%d) = (%s & 0xffffu) | (%s & 0xffff0000u);' % (d, a, b))
        else:
            if sh.type == A.ARM_SFT_ASR:
                b = '(uint32_t)((int32_t)%s >> %d)' % (b, sh.value or 32)
            X.L.append('R(%d) = (%s & 0xffff0000u) | (%s & 0xffffu);' % (d, a, b))

    def h_uxtb16(self, ins, nm, X):
        ops = ins.operands
        d = self.rn(ops[0].reg)
        v = self.rv(ops[1].reg, X)
        if ops[1].shift.type == A.ARM_SFT_ROR and ops[1].shift.value:
            v = 'ror32(%s, %d)' % (v, ops[1].shift.value)
        X.L.append('R(%d) = %s & 0x00ff00ffu;' % (d, v))

    # ============================================================ loads / stores
    LDST = {'ldr': (4, 0), 'ldrb': (1, 0), 'ldrh': (2, 0), 'ldrsb': (1, 1), 'ldrsh': (2, 1),
            'ldrt': (4, 0), 'ldrbt': (1, 0), 'ldrht': (2, 0), 'ldrsbt': (1, 1), 'ldrsht': (2, 1),
            'str': (4, 0), 'strb': (1, 0), 'strh': (2, 0), 'strt': (4, 0), 'strbt': (1, 0), 'strht': (2, 0)}

    def mem_ea(self, ins, mo, X, count_post):
        """returns (ea_expr, base_reg, new_base_expr|None). Emits nothing."""
        mem = mo.mem
        base = self.rn(mem.base)
        bexpr = ('0x%xu' % (((X.addr + 4) & ~3) & M32)) if base == 15 else 'R(%d)' % base
        post = getattr(ins, 'post_index', False)
        # operand after the memory operand = post-index offset
        ops = ins.operands
        mi = ops.index(mo)
        extra = ops[mi + 1] if mi + 1 < len(ops) else None
        if post and extra is not None:
            if extra.type == A.ARM_OP_IMM:
                nb = 't1 + 0x%xu' % (extra.imm & M32)
            else:
                sub = extra.subtracted
                nb = 't1 %s %s' % ('-' if sub else '+', self.rv(extra.reg, X))
            return 't1', base, nb, bexpr
        if mem.index:
            idx = self.rv(mem.index, X)
            sh = mo.shift.value if mo.shift.type == A.ARM_SFT_LSL else mem.lshift
            if sh:
                idx = '(%s << %d)' % (idx, sh)
            ea = '%s %s %s' % (bexpr, '-' if mem.scale < 0 else '+', idx)
        else:
            ea = '%s + 0x%xu' % (bexpr, mem.disp & M32) if mem.disp else bexpr
        return ea, base, ('t1' if ins.writeback else None), bexpr

    def h_ldst(self, ins, nm, X):
        ops = ins.operands
        L = X.L
        size, sgn = self.LDST[nm]
        load = nm.startswith('ldr')
        mo = next(o for o in ops if o.type == A.ARM_OP_MEM)
        rt = ops[0]
        if load and self.rn(mo.mem.base) == 15 and not mo.mem.index and not ins.writeback:
            ea = (((X.addr + 4) & ~3) + mo.mem.disp) & M32
            raw = bytes(self.m.buf[ea:ea + size])
            v = int.from_bytes(raw, 'little', signed=bool(sgn)) & M32
            return self.wr_dest(self.rn(rt.reg), '0x%xu' % v, X)
        ea, base, nb, bexpr = self.mem_ea(ins, mo, X, False)
        post = getattr(ins, 'post_index', False)
        if post:
            L.append('t1 = %s;' % bexpr)
            L.append('t0 = t1;')
        else:
            L.append('t1 = %s;' % ea)
            L.append('t0 = t1;')
        t = self.rn(rt.reg)
        if load:
            fn = {(4, 0): 'ld32', (2, 0): 'ld16', (1, 0): 'ld8', (2, 1): 'lds16', (1, 1): 'lds8'}[(size, sgn)]
            if t == 15:
                # load into pc = indirect branch / return (pop {pc} written as ldr pc, [sp], #4)
                X.falls = False; X.ender = True
                L.append('t2 = %s(t0);' % fn)
                if nb is not None and base != 15:
                    L.append('R(%d) = %s;' % (base, nb))
                if base == 13 and post:
                    L.append('return;')
                else:
                    L.append('arm_dispatch(c, t2); return;')
                return
            L.append('t2 = %s(t0);' % fn)
            if nb is not None and base != t:
                L.append('R(%d) = %s;' % (base, nb))
            L.append('R(%d) = t2;' % t)
        else:
            fn = {4: 'st32', 2: 'st16', 1: 'st8'}[size]
            L.append('%s(t0, %s);' % (fn, self.rv(rt.reg, X)))
            if nb is not None:
                L.append('R(%d) = %s;' % (base, nb))

    def h_ldstd(self, ins, nm, X):
        ops = ins.operands
        L = X.L
        mo = next(o for o in ops if o.type == A.ARM_OP_MEM)
        r1, r2 = ops[0], ops[1]
        if nm == 'ldrd' and self.rn(mo.mem.base) == 15 and not mo.mem.index and not ins.writeback:
            ea = (((X.addr + 4) & ~3) + mo.mem.disp) & M32
            lo, hi = struct.unpack_from('<II', self.m.buf, ea)
            L.append('R(%d) = 0x%xu; R(%d) = 0x%xu;' % (self.rn(r1.reg), lo, self.rn(r2.reg), hi))
            return
        ea, base, nb, bexpr = self.mem_ea(ins, mo, X, False)
        post = getattr(ins, 'post_index', False)
        L.append('t1 = %s;' % (bexpr if post else ea))
        L.append('t0 = t1;')
        if nm == 'ldrd':
            L.append('t2 = ld32(t0); t3 = ld32(t0 + 4);')
            if nb is not None:
                L.append('R(%d) = %s;' % (base, nb))
            L.append('R(%d) = t2; R(%d) = t3;' % (self.rn(r1.reg), self.rn(r2.reg)))
        else:
            L.append('st32(t0, %s); st32(t0 + 4, %s);' % (self.rv(r1.reg, X), self.rv(r2.reg, X)))
            if nb is not None:
                L.append('R(%d) = %s;' % (base, nb))

    def h_ldrex(self, ins, nm, X):
        ops = ins.operands
        mo = next(o for o in ops if o.type == A.ARM_OP_MEM)
        ea = 'R(%d) + 0x%xu' % (self.rn(mo.mem.base), mo.mem.disp & M32)
        sz = {'ldrex': 'ld32', 'ldrexb': 'ld8', 'ldrexh': 'ld16'}[nm]
        X.L.append('R(%d) = %s(%s);' % (self.rn(ops[0].reg), sz, ea))

    def h_strex(self, ins, nm, X):
        ops = ins.operands
        mo = next(o for o in ops if o.type == A.ARM_OP_MEM)
        ea = 'R(%d) + 0x%xu' % (self.rn(mo.mem.base), mo.mem.disp & M32)
        sz = {'strex': 'st32', 'strexb': 'st8', 'strexh': 'st16'}[nm]
        X.L.append('%s(%s, %s); R(%d) = 0;' % (sz, ea, self.rv(ops[1].reg, X), self.rn(ops[0].reg)))

    def h_ldstm(self, ins, nm, X):
        ops = ins.operands
        L = X.L
        if nm in ('push', 'pop'):
            base = 13; regs = ops; wb = True
            db = nm == 'push'
        else:
            base = self.rn(ops[0].reg); regs = ops[1:]; wb = ins.writeback
            db = nm in ('stmdb', 'ldmdb')
        rl = [self.rn(o.reg) for o in regs]
        n = len(rl)
        load = nm in ('pop', 'ldm', 'ldmdb')
        L.append('t1 = R(%d);' % base)
        if db:
            L.append('t0 = t1 - %d;' % (4 * n))
        else:
            L.append('t0 = t1;')
        pc_loaded = False
        for i, r in enumerate(rl):
            if load:
                if r == 15:
                    pc_loaded = True
                    L.append('t2 = ld32(t0 + %d);' % (4 * i))
                else:
                    L.append('R(%d) = ld32(t0 + %d);' % (r, 4 * i))
            else:
                L.append('st32(t0 + %d, %s);' % (4 * i, ('0x%xu' % ((X.addr + 4) & M32)) if r == 15 else 'R(%d)' % r))
        if wb and not (load and base in rl):
            L.append('R(%d) = %s;' % (base, 't0' if db else 't1 + %d' % (4 * n)))
        if pc_loaded:
            X.falls = False; X.ender = True
            if base == 13 and nm == 'pop':
                L.append('return;')
            else:
                L.append('arm_dispatch(c, t2); return;')

    # ============================================================ branches
    def in_func(self, t, X):
        return X.fstart <= t < X.fend

    def branch_to(self, tgt, X, call=False):
        """unconditional jump/tail-call to absolute address tgt (thumb bit already stripped for local targets)"""
        t = tgt & ~1
        m = self.m
        if (t in self.entries and t != X.fstart) or (m.stub_lo <= tgt < m.stub_hi):
            X.L.append('%s return;' % self.call_stmt(tgt, X))
        elif self.in_func(t, X):
            X.L.append('goto L_%x;' % t)
            X.targets.append(t)
        else:
            X.L.append('%s return;' % self.call_stmt(tgt, X))

    def h_b(self, ins, nm, X):
        tgt = ins.operands[0].imm
        self.branch_to(tgt, X)
        X.falls = False

    def h_bl(self, ins, nm, X):
        tgt = ins.operands[0].imm
        m = self.m
        X.L.append('R(14) = 0x%xu; %s' % (((X.addr + ins.size) | 1) & M32, self.call_stmt(tgt, X).replace('imp__setjmp(c);', 'XW_SETJMP(c);')))
        if self.is_noreturn_stub(tgt):
            X.falls = False

    def h_blx(self, ins, nm, X):
        o = ins.operands[0]
        if o.type == A.ARM_OP_IMM:
            return self.h_bl(ins, nm, X)
        X.L.append('t0 = %s; R(14) = 0x%xu; arm_dispatch(c, t0);' % (self.rv(o.reg, X), ((X.addr + ins.size) | 1) & M32))

    def h_bx(self, ins, nm, X):
        n = self.rn(ins.operands[0].reg)
        X.falls = False
        X.ender = True
        if n == 14:
            X.L.append('return;')
        else:
            X.L.append('arm_dispatch(c, R(%d)); return;' % n)

    def h_cbz(self, ins, nm, X):
        n = self.rn(ins.operands[0].reg)
        tgt = ins.operands[1].imm
        cond = 'R(%d) %s 0' % (n, '==' if nm == 'cbz' else '!=')
        X.L.append('if (%s) {' % cond)
        sv = len(X.L)
        self.branch_to(tgt, X)
        X.L.append('}')
        X.falls = True

    def h_tb(self, ins, nm, X):
        mem = ins.operands[0].mem
        idx = self.rn(mem.index)
        if self.rn(mem.base) == 15:
            tbl = (X.addr + 4) & M32
        elif self.prev_const and self.prev_const[0] == self.rn(mem.base):
            tbl = self.prev_const[1]
        else:
            raise Unsupported('table branch with unknown base')
        n, kind = self.jt.get(tbl, (None, None))
        if n is None:
            raise Unsupported('table branch without data-in-code length')
        cases = []
        cnt = n // (1 if nm == 'tbb' else 2)
        for i in range(cnt):
            if nm == 'tbb':
                off = self.m.buf[tbl + i]
            else:
                off = struct.unpack_from('<H', self.m.buf, tbl + 2 * i)[0]
            t = ((X.addr + 4) + 2 * off) & M32
            cases.append((i, t))
            X.targets.append(t)
        X.L.append('switch (R(%d)) {' % idx)
        for i, t in cases:
            X.L.append('case %d: goto L_%x;' % (i, t))
        X.L.append('default: break; }')
        X.falls = False
        X.ender = True

    # ============================================================ system / misc
    def h_nop(self, ins, nm, X):
        X.L.append(';')

    def h_trap(self, ins, nm, X):
        X.L.append('arm_trap(0x%xu, "%s");' % (X.addr, nm))
        X.falls = False
        X.ender = True

    def h_svc(self, ins, nm, X):
        X.L.append('arm_svc(c, 0x%x);' % ins.operands[0].imm)

    def h_mrc(self, ins, nm, X):
        # mrc p15, 0, rt, c13, c0, 3  (user read-only thread id)
        ops = ins.operands
        t = self.rn(ops[2].reg)
        if ins.op_str.replace(' ', '').endswith('c13,c0,#3') or ins.op_str.replace(' ', '').endswith('c13,c0,3'):
            X.L.append('R(%d) = c->tls;' % t)
        else:
            raise Unsupported('mrc ' + ins.op_str)

    # ============================================================ VFP
    @staticmethod
    def fp(name):
        m = re.match(r'([sdq])(\d+)$', name)
        return m.group(1), int(m.group(2))

    def freg(self, o):
        return self.fp(self.cs.reg_name(o.reg))

    def vx(self, o, X=None):
        k, n = self.freg(o)
        return {'s': 'VS(%d)', 'd': 'VD(%d)'}[k] % n

    def h_vldstr(self, ins, nm, X):
        ops = ins.operands
        k, n = self.freg(ops[0])
        mem = ops[1].mem
        base = self.rn(mem.base)
        bexpr = ('0x%xu' % (((X.addr + 4) & ~3) & M32)) if base == 15 else 'R(%d)' % base
        ea = '%s + 0x%xu' % (bexpr, mem.disp & M32) if mem.disp else bexpr
        if nm == 'vldr' and base == 15:
            a0 = (((X.addr + 4) & ~3) + mem.disp) & M32
            if k == 's':
                X.L.append('VU(%d) = 0x%xu;' % (n, struct.unpack_from('<I', self.m.buf, a0)[0]))
            else:
                X.L.append('VQ(%d) = 0x%xull;' % (n, struct.unpack_from('<Q', self.m.buf, a0)[0]))
            return
        if nm == 'vldr':
            X.L.append('%s = %s(%s);' % (('VU(%d)' % n) if k == 's' else 'VQ(%d)' % n, 'ld32' if k == 's' else 'ld64', ea))
        else:
            X.L.append('%s(%s, %s);' % ('st32' if k == 's' else 'st64', ea, ('VU(%d)' % n) if k == 's' else 'VQ(%d)' % n))

    def h_vldstm(self, ins, nm, X):
        ops = ins.operands
        L = X.L
        if nm in ('vpush', 'vpop'):
            base = 13; regs = ops; wb = True; db = nm == 'vpush'
        else:
            base = self.rn(ops[0].reg); regs = ops[1:]; wb = ins.writeback; db = nm in ('vstmdb', 'vldmdb')
        load = nm in ('vpop', 'vldmia', 'vldmdb')
        rl = [self.freg(o) for o in regs]
        sz = 4 if rl[0][0] == 's' else 8
        n = len(rl) * sz
        L.append('t1 = R(%d);' % base)
        L.append('t0 = t1 - %d;' % n if db else 't0 = t1;')
        for i, (k, r) in enumerate(rl):
            slot = ('VU(%d)' % r) if k == 's' else 'VQ(%d)' % r
            if load:
                L.append('%s = %s(t0 + %d);' % (slot, 'ld32' if k == 's' else 'ld64', i * sz))
            else:
                L.append('%s(t0 + %d, %s);' % ('st32' if k == 's' else 'st64', i * sz, slot))
        if wb:
            L.append('R(%d) = %s;' % (base, 't0' if db else 't1 + %d' % n))

    def h_vmov(self, ins, nm, X):
        ops = ins.operands
        L = X.L
        mn = ins.mnemonic
        o0 = ops[0]
        # ARM core <-> VFP
        def isreg(o): return o.type == A.ARM_OP_REG
        def iscore(o): return isreg(o) and self.cs.reg_name(o.reg) in REGN
        if len(ops) == 2 and iscore(ops[0]) and isreg(ops[1]) and ops[1].vector_index < 0:
            k, n = self.freg(ops[1])
            if k == 's':
                L.append('R(%d) = VU(%d);' % (self.rn(ops[0].reg), n)); return
        if len(ops) == 2 and isreg(ops[0]) and not iscore(ops[0]) and iscore(ops[1]) and ops[0].vector_index < 0:
            k, n = self.freg(ops[0])
            if k == 's':
                L.append('VU(%d) = %s;' % (n, self.rv(ops[1].reg, X))); return
        if len(ops) == 3 and iscore(ops[0]) and iscore(ops[1]) and isreg(ops[2]):       # vmov rt, rt2, dm
            k, n = self.freg(ops[2])
            L.append('R(%d) = (uint32_t)VQ(%d); R(%d) = (uint32_t)(VQ(%d) >> 32);' % (self.rn(ops[0].reg), n, self.rn(ops[1].reg), n)); return
        if len(ops) == 3 and isreg(ops[0]) and not iscore(ops[0]) and iscore(ops[1]):    # vmov dm, rt, rt2
            k, n = self.freg(ops[0])
            L.append('VQ(%d) = (uint64_t)%s | ((uint64_t)%s << 32);' % (n, self.rv(ops[1].reg, X), self.rv(ops[2].reg, X))); return
        if len(ops) == 4 and iscore(ops[0]):                                             # vmov rt, rt2, sm, sm1
            k, n = self.freg(ops[2])
            L.append('R(%d) = VU(%d); R(%d) = VU(%d);' % (self.rn(ops[0].reg), n, self.rn(ops[1].reg), n + 1)); return
        if len(ops) == 4 and not iscore(ops[0]):                                         # vmov sm, sm1, rt, rt2
            k, n = self.freg(ops[0])
            L.append('VU(%d) = %s; VU(%d) = %s;' % (n, self.rv(ops[2].reg, X), n + 1, self.rv(ops[3].reg, X))); return
        # VFP register/immediate moves
        if ('.f32' in mn or '.f64' in mn) and (self.cs.reg_name(ops[0].reg)[0] == 's' or '.f64' in mn):
            k, n = self.freg(ops[0])
            if ops[1].type == A.ARM_OP_FP:
                v = ops[1].fp
                if k == 's':
                    import struct as _s
                    bits = _s.unpack('<I', _s.pack('<f', v))[0]
                    L.append('VU(%d) = 0x%xu;' % (n, bits))
                else:
                    bits = struct.unpack('<Q', struct.pack('<d', v))[0]
                    L.append('VQ(%d) = 0x%xull;' % (n, bits))
            else:
                k2, n2 = self.freg(ops[1])
                L.append('%s = %s;' % (('VU(%d)' % n) if k == 's' else 'VQ(%d)' % n, ('VU(%d)' % n2) if k2 == 's' else 'VQ(%d)' % n2))
            return
        # NEON forms (vmov.i32 qd,#imm / vmov dd, dm / vmov.32 dd[x], rt / vmov.u16 rt, dn[x])
        return neon_lift.vmov(self, ins, nm, X)

    def h_vmrs(self, ins, nm, X):
        ops = ins.operands
        dst = self.cs.reg_name(ops[0].reg)
        if dst == 'apsr_nzcv':
            X.L.append('c->n = c->fn; c->z = c->fz; c->c = c->fc; c->v = c->fv;')
        else:
            X.L.append('R(%d) = fpscr_get_(c);' % self.rn(ops[0].reg))

    def h_vmsr(self, ins, nm, X):
        ops = ins.operands
        X.L.append('fpscr_set_(c, %s);' % self.rv(ops[1].reg, X))

    def h_vfp_arith(self, ins, nm, X):
        ops = ins.operands
        k, d = self.freg(ops[0])
        if k == 'q' or (k == 'd' and '.f64' not in ins.mnemonic):
            return neon_lift.arith(self, ins, nm, X)
        T = 'float' if k == 's' else 'double'
        fx = lambda o: self.vx(o)
        if nm in ('vneg', 'vabs', 'vsqrt'):
            src = fx(ops[1])
            e = {'vneg': '-%s', 'vabs': ('fabsf(%s)' if k == 's' else 'fabs(%s)'), 'vsqrt': ('sqrtf(%s)' if k == 's' else 'sqrt(%s)')}[nm] % src
            X.L.append('%s = %s;' % (fx(ops[0]), e))
            return
        a, b = fx(ops[1]), fx(ops[2]) if len(ops) > 2 else None
        dd = fx(ops[0])
        e = {'vadd': '%s = %s + %s;', 'vsub': '%s = %s - %s;', 'vmul': '%s = %s * %s;', 'vdiv': '%s = %s / %s;',
             'vnmul': '%s = -(%s * %s);',
             'vmla': '%s = %s + (%s * %s);', 'vmls': '%s = %s - (%s * %s);',
             'vnmla': '%s = -%s - (%s * %s);', 'vnmls': '%s = -%s + (%s * %s);'}[nm]
        if nm in ('vmla', 'vmls', 'vnmla', 'vnmls'):
            X.L.append('{ %s p = (%s)(%s * %s); %s = %s; }' % (T, T, a, b, dd, {'vmla': '%s + p' % dd, 'vmls': '%s - p' % dd, 'vnmla': '-%s - p' % dd, 'vnmls': '-%s + p' % dd}[nm]))
        else:
            X.L.append(e % (dd, a, b))

    def h_vcmp(self, ins, nm, X):
        ops = ins.operands
        k, n = self.freg(ops[0])
        a = self.vx(ops[0])
        if ops[1].type == A.ARM_OP_REG:
            b = self.vx(ops[1])
        else:
            b = '0.0'
        X.L.append('vcmp_(c, %s, %s);' % (a, b))

    def h_vcvt(self, ins, nm, X):
        ops = ins.operands
        mn = ins.mnemonic.split('.')
        # vcvt.<dst>.<src> [dd, sm] (+ fixed-point imm)
        if len(mn) < 3:
            raise Unsupported('vcvt ' + ins.mnemonic)
        dt, st = mn[1], mn[2]
        k0, n0 = self.freg(ops[0])
        k1, n1 = self.freg(ops[1])
        fixed = ops[2].imm if len(ops) > 2 and ops[2].type == A.ARM_OP_IMM else None
        def src(): return self.vx(ops[1])
        def dst(): return self.vx(ops[0])
        L = X.L
        if 'q' in (k0, k1) or (k0 == 'd' and k1 == 'd' and 'f64' not in (dt, st)):
            return neon_lift.vcvt(self, ins, nm, X)
        if dt == 'f32' and st == 'f64':
            L.append('%s = (float)%s;' % (dst(), src()))
        elif dt == 'f64' and st == 'f32':
            L.append('%s = (double)%s;' % (dst(), src()))
        elif dt in ('f32', 'f64') and st in ('s32', 'u32'):
            cast = 'int32_t' if st == 's32' else 'uint32_t'
            if fixed is not None:
                # vcvt.f32.s32 sd, sd, #fbits ; operand register holds raw integer
                L.append('%s = (%s)((%s)%s / %s);' % (dst(), 'float' if dt == 'f32' else 'double', 'double', '(%s)VU(%d)' % (cast, n1) if k1 == 's' else '(%s)VQ(%d)' % (cast, n1), repr(float(1 << fixed))))
            else:
                v = '(%s)VU(%d)' % (cast, n1)
                L.append('%s = (%s)%s;' % (dst(), 'float' if dt == 'f32' else 'double', v))
        elif dt in ('s32', 'u32') and st in ('f32', 'f64'):
            fn = 'f2s_' if dt == 's32' else 'f2u_'
            if fixed is not None:
                L.append('VU(%d) = %s((double)%s * %s);' % (n0, fn, src(), repr(float(1 << fixed))))
            else:
                rnd = 'rintf' if False else ''
                if nm == 'vcvtr':
                    L.append('VU(%d) = %s(nearbyint((double)%s));' % (n0, fn, src()))
                else:
                    L.append('VU(%d) = %s((double)%s);' % (n0, fn, src()))
        else:
            raise Unsupported('vcvt ' + ins.mnemonic)

    # ============================================================ decode driver
    def handler(self, nm):
        if nm in ('add', 'addw', 'adc', 'sub', 'subw', 'sbc', 'rsb', 'cmp', 'cmn', 'and', 'orr', 'eor', 'bic', 'orn', 'mov', 'movw', 'mvn', 'tst', 'teq'):
            return self.h_alu
        if nm == 'movt': return self.h_movt
        if nm == 'adr': return self.h_adr
        if nm in ('lsl', 'lsr', 'asr', 'ror', 'rrx'): return self.h_shift
        if nm in ('mul', 'mla', 'mls', 'umull', 'smull', 'umlal', 'smlal', 'umaal', 'smmul', 'smmulr', 'smmla', 'smmlar', 'smmls', 'smmlsr',
                  'udiv', 'sdiv', 'smlad', 'smladx', 'smuad', 'smuadx') or re.match(r'sm(ul|la)(bb|bt|tb|tt|wb|wt)$', nm):
            return self.h_mul
        if nm in ('bfc', 'bfi', 'ubfx', 'sbfx', 'clz', 'rbit', 'rev', 'rev16', 'revsh'): return self.h_bits
        if nm in ('uxtb', 'uxth', 'sxtb', 'sxth', 'uxtab', 'uxtah', 'sxtab', 'sxtah'): return self.h_ext
        if nm in ('ssat', 'usat'): return self.h_sat
        if nm in ('pkhbt', 'pkhtb'): return self.h_pkh
        if nm == 'uxtb16': return self.h_uxtb16
        if nm in self.LDST: return self.h_ldst
        if nm in ('ldrd', 'strd'): return self.h_ldstd
        if nm in ('ldrex', 'ldrexb', 'ldrexh'): return self.h_ldrex
        if nm in ('strex', 'strexb', 'strexh'): return self.h_strex
        if nm in ('push', 'pop', 'ldm', 'stm', 'ldmdb', 'stmdb'): return self.h_ldstm
        if nm == 'b': return self.h_b
        if nm == 'bl': return self.h_bl
        if nm == 'blx': return self.h_blx
        if nm == 'bx': return self.h_bx
        if nm in ('cbz', 'cbnz'): return self.h_cbz
        if nm in ('tbb', 'tbh'): return self.h_tb
        if nm in ('nop', 'hint', 'yield', 'wfe', 'wfi', 'sev', 'dmb', 'dsb', 'isb', 'pld', 'pldw', 'pli', 'clrex', 'dbg'): return self.h_nop
        if nm in ('trap', 'udf', 'bkpt'): return self.h_trap
        if nm == 'svc': return self.h_svc
        if nm == 'mrc': return self.h_mrc
        if nm in ('vldr', 'vstr'): return self.h_vldstr
        if nm in ('vldmia', 'vldmdb', 'vstmia', 'vstmdb', 'vpush', 'vpop'): return self.h_vldstm
        if nm == 'vmov': return self.h_vmov
        if nm == 'vmrs': return self.h_vmrs
        if nm == 'vmsr': return self.h_vmsr
        if nm in ('vcmp', 'vcmpe'): return self.h_vcmp
        if nm in ('vcvt', 'vcvtr'): return self.h_vcvt
        if nm in ('vadd', 'vsub', 'vmul', 'vdiv', 'vmla', 'vmls', 'vnmla', 'vnmls', 'vnmul', 'vneg', 'vabs', 'vsqrt'):
            return self.h_vfp_arith
        h = neon_lift.handler(self, nm)
        if h:
            return lambda ins, nm, X: h(self, ins, nm, X)
        return None

    # ---------------------------------------------------------------- function
    def lift_function(self, fstart, fend):
        m = self.m
        cs = self.cs
        insns = []          # Ins in address order
        a = fstart
        itq = []            # remaining IT conditions
        buf = m.buf
        while a < fend:
            if self.in_data(a):
                a += 2
                continue
            code = bytes(buf[a:a + 4])
            dec = list(cs.disasm(code, a, 1))
            if not dec:
                X = Ins(a, 2)
                X.L = ['arm_trap(0x%xu, "undecodable");' % a]
                X.falls = False
                X.tag = 'UNDEC'
                self.unsup['undecodable'] += 1
                insns.append(X)
                a += 2
                itq = []
                continue
            ins = dec[0]
            X = Ins(a, ins.size)
            X.fstart, X.fend = fstart, fend
            self.prev_const, self.cur_const = self.cur_const, None
            nm = cs.insn_name(ins.id)
            if nm is None:
                nm = ins.mnemonic
            nm = NAMEMAP.get(nm, nm)
            if ins.mnemonic.split('.')[0] in ('addw', 'subw'):
                nm = ins.mnemonic.split('.')[0]
            # IT state
            if nm == 'it':
                pat = ins.mnemonic[2:]
                first = ins.cc
                itq = [first] + [first if ch == 't' else inv_cc(first) for ch in pat]
                X.L = [';']
                X.tag = 'it'
                insns.append(X)
                a += ins.size
                continue
            if itq:
                X.cond = itq.pop(0)
                X.in_it = True
            else:
                X.cond = ins.cc if nm == 'b' and ins.cc != 0 else AL
            if X.cond == 0:
                X.cond = AL
            try:
                h = self.handler(nm)
                if h is None:
                    raise Unsupported('insn ' + nm)
                h(ins, nm, X)
                X.tag = X.tag or nm
            except Unsupported as e:
                X.L = ['arm_trap(0x%xu, "unsupported %s");' % (a, str(e).replace('"', "'"))]
                X.falls = False
                X.targets = []
                X.tag = 'UNSUP'
                self.unsup[str(e)[:60]] += 1
                self.ex.setdefault(str(e)[:60], []).append((a, ins.mnemonic + ' ' + ins.op_str))
            except (KeyError, IndexError, AttributeError, ValueError) as e:
                X.L = ['arm_trap(0x%xu, "lift error %s");' % (a, ins.mnemonic)]
                X.falls = False
                X.targets = []
                X.tag = 'UNSUP'
                self.unsup['error %s %s' % (nm, type(e).__name__)] += 1
                self.errs.append((a, ins.mnemonic, ins.op_str, repr(e))) if hasattr(self, 'errs') else None
            X.text = '%s %s' % (ins.mnemonic, ins.op_str)
            insns.append(X)
            a += ins.size
        # labels
        labels = set(t for X in insns for t in X.targets)
        have = {X.addr for X in insns}
        out = ['void f_%08x(CPU *c) {' % fstart,
               '  uint32_t t0 = 0, t1 = 0, t2 = 0, t3 = 0; uint64_t u0 = 0; (void)t0; (void)t1; (void)t2; (void)t3; (void)u0;']
        for X in insns:
            if X.addr in labels:
                out.append('L_%x:;' % X.addr)
            body = X.L
            if X.cond != AL:
                out.append('  if (%s) { %s }' % (CONDS[X.cond], ' '.join(body)))
            else:
                for ln in body:
                    out.append('  ' + ln)
            self.stats[X.tag] += 1
        for t in labels:
            if t not in have:
                out.append('L_%x: arm_trap(0x%xu, "branch to non-code");' % (t, t))
        last = insns[-1] if insns else None
        if last is not None and last.falls:
            out.append('  arm_trap(0x%xu, "fell off end of function");' % fend)
        out.append('}')
        return '\n'.join(out), len(insns)


def main():
    path, outdir = sys.argv[1:3]
    os.makedirs(outdir, exist_ok=True)
    m = Macho(path)
    load_imports(m)
    starts, thumb = m.func_starts()
    lo, hi = m.text['addr'], m.text['addr'] + m.text['size']
    ents = sorted(set(s for s in starts if lo <= s < hi))
    L = Lifter(m)
    L.errs = []
    L.entries = set(ents)
    only = None
    if len(sys.argv) > 3:
        only = int(sys.argv[3])
    chunks, cur = [], []
    total = 0
    for i, e in enumerate(ents):
        end = ents[i + 1] if i + 1 < len(ents) else hi
        try:
            src, n = L.lift_function(e, end)
        except Exception as ex:
            import traceback
            traceback.print_exc()
            src, n = 'void f_%08x(CPU *c) { arm_trap(0x%xu, "lift failure"); }' % (e, e), 0
        total += n
        cur.append(src)
        if len(cur) >= 120:
            chunks.append(cur); cur = []
        if i % 2000 == 0:
            print('lifted', i, '/', len(ents), flush=True)
    if cur:
        chunks.append(cur)
    # second round: functions reached only via blx/dispatch that are not in LC_FUNCTION_STARTS
    extra = sorted(e for e in L.new_entries if e not in L.entries)
    print('extra entries discovered:', len(extra), [hex(x) for x in extra[:10]])
    for i, ch in enumerate(chunks):
        with open(os.path.join(outdir, 'f_%03d.c' % i), 'w') as f:
            f.write('#include "armrt.h"\n#include "imports.h"\n#include "protos.h"\n\n')
            f.write('\n\n'.join(ch)); f.write('\n')
    cname = lambda n: re.sub(r'\W', '_', n)
    imps = sorted({cname(n) for n in m.stubs.values()})
    with open(os.path.join(outdir, 'imports.h'), 'w') as f:
        f.write('#include "armrt.h"\n')
        for n in imps:
            f.write('void imp_%s(CPU *c);\n' % n)
    with open(os.path.join(outdir, 'protos.h'), 'w') as f:
        f.write('#include "armrt.h"\n')
        for e in ents:
            f.write('void f_%08x(CPU *c);\n' % e)
    with open(os.path.join(outdir, 'func_table.c'), 'w') as f:
        f.write('#include "armrt.h"\n#include "protos.h"\n\nconst struct { uint32_t addr; guest_fn fn; } arm_func_table[] = {\n')
        for e in ents:
            f.write('  { 0x%x, f_%08x },\n' % (e, e))
        f.write('};\nconst unsigned arm_func_count = %d;\n' % len(ents))
    import tables_t2
    tables_t2.emit(m, outdir)
    rep = ['functions: %d  instructions: %d' % (len(ents), total), '', 'UNSUPPORTED (%d total):' % sum(L.unsup.values())]
    for k, v in L.unsup.most_common(60):
        rep.append('  %6d  %s' % (v, k))
    for k, v in L.ex.items():
        rep.append('  e.g. %s: %s' % (k, [(hex(a), t) for a, t in v[:6]]))
    rep.append('')
    rep.append('tags: ' + str(L.stats.most_common(120)))
    open(os.path.join(outdir, 'report.txt'), 'w').write('\n'.join(rep))
    print('\n'.join(rep[:70]))


if __name__ == '__main__':
    main()
