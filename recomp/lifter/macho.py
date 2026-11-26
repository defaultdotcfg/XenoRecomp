"""Minimal 32-bit little-endian Mach-O loader shared by the Xenowerk (ARMv7/Thumb-2) tools."""
import struct


def uleb(d, o):
    r = s = 0
    while True:
        b = d[o]; o += 1
        r |= (b & 0x7f) << s; s += 7
        if not b & 0x80:
            return r, o


class Macho:
    def __init__(self, path):
        d = self.d = open(path, 'rb').read()
        assert d[:4] == b'\xce\xfa\xed\xfe', 'not a thin 32-bit Mach-O'
        self.cputype, self.cpusub = struct.unpack('<II', d[4:12])
        ncmds = struct.unpack('<I', d[16:20])[0]
        off = 28
        self.segs, self.sects, self.dice, self.fstarts = [], [], [], None
        self.symtab = self.dysym = None
        self.dyldinfo = None
        self.entry = None
        for _ in range(ncmds):
            cmd, sz = struct.unpack('<2I', d[off:off + 8])
            if cmd == 1:
                name = d[off + 8:off + 24].rstrip(b'\0').decode()
                vm, vs, fo, fs = struct.unpack('<4I', d[off + 24:off + 40])
                nsec = struct.unpack('<I', d[off + 48:off + 52])[0]
                self.segs.append((name, vm, vs, fo, fs))
                so = off + 56
                for _ in range(nsec):
                    sn = d[so:so + 16].rstrip(b'\0').decode()
                    sg = d[so + 16:so + 32].rstrip(b'\0').decode()
                    addr, size, foff = struct.unpack('<3I', d[so + 32:so + 44])
                    flags, r1, r2 = struct.unpack('<3I', d[so + 56:so + 68])
                    self.sects.append(dict(seg=sg, name=sn, addr=addr, size=size, off=foff, flags=flags, r1=r1, r2=r2))
                    so += 68
            elif cmd == 2:
                self.symtab = struct.unpack('<4I', d[off + 8:off + 24])
            elif cmd == 0xb:
                self.dysym = struct.unpack('<18I', d[off + 8:off + 80])
            elif cmd == 0x26:
                self.fstarts = struct.unpack('<2I', d[off + 8:off + 16])
            elif cmd == 0x29:
                dof, dsz = struct.unpack('<2I', d[off + 8:off + 16])
                for i in range(dsz // 8):
                    a, ln, kind = struct.unpack('<IHH', d[dof + 8 * i:dof + 8 * i + 8])
                    self.dice.append((a, ln, kind))
            elif cmd in (0x22, 0x80000022):
                self.dyldinfo = struct.unpack('<10I', d[off + 8:off + 48])
            elif cmd == 5:   # LC_UNIXTHREAD
                self.entry = struct.unpack('<I', d[off + 8 + 8 + 4 * 15:off + 8 + 8 + 4 * 16])[0]
            off += sz
        top = max(vm + vs for _, vm, vs, _, _ in self.segs)
        self.top = top
        self.buf = bytearray(top)
        for name, vm, vs, fo, fs in self.segs:
            self.buf[vm:vm + fs] = d[fo:fo + fs]
        self.text = self.section('__text')
        self.base = min(vm for n, vm, vs, fo, fs in self.segs if n == '__TEXT')
        self.dice = [(a + self.base, l, k) for a, l, k in self.dice]   # DATA_IN_CODE offsets are relative to the mach header

    def section(self, name):
        for s in self.sects:
            if s['name'] == name:
                return s

    def u32(self, a):
        return struct.unpack('<I', self.buf[a:a + 4])[0]

    def func_starts(self):
        """sorted list of function addresses (thumb bit stripped) and a set of those with the thumb bit"""
        off, size = self.fstarts
        o, end = off, off + size
        addr = self.base
        out = []
        thumb = set()
        while o < end:
            v, o = uleb(self.d, o)
            if v == 0:
                break
            addr += v
            out.append(addr & ~1)
            if addr & 1:
                thumb.add(addr & ~1)
        return out, thumb

    def symbols(self):
        symoff, nsyms, stroff, _ = self.symtab
        out = []
        for i in range(nsyms):
            sx, t, sc, desc, v = struct.unpack('<IBBHI', self.d[symoff + 12 * i:symoff + 12 * i + 12])
            nm = self.d[stroff + sx:self.d.index(b'\0', stroff + sx)].decode('latin1')
            out.append((nm, t, sc, desc, v))
        return out


# ---------------------------------------------------------------- imports / dyld binds
def sleb(d, o):
    r = s = 0
    while True:
        b = d[o]; o += 1
        r |= (b & 0x7f) << s; s += 7
        if not b & 0x80:
            if b & 0x40:
                r -= 1 << s
            return r, o


def parse_binds(m, off, size):
    """dyld bind opcode stream -> [(addr, symbol, addend)] (32-bit pointers)"""
    d = m.d
    out = []
    o, end = off, off + size
    seg_addrs = [vm for _, vm, _, _, _ in m.segs]
    sym = ''; addend = 0; addr = 0; typ = 1
    while o < end:
        b = d[o]; o += 1
        op, imm = b & 0xf0, b & 0x0f
        if op == 0x00:
            if size and o >= end:
                break
        elif op in (0x10, 0x30):
            pass
        elif op == 0x20:
            _, o = uleb(d, o)
        elif op == 0x40:
            e = d.index(b'\0', o); sym = d[o:e].decode('latin1'); o = e + 1
        elif op == 0x50:
            typ = imm
        elif op == 0x60:
            addend, o = sleb(d, o)
        elif op == 0x70:
            v, o = uleb(d, o); addr = seg_addrs[imm] + v
        elif op == 0x80:
            v, o = uleb(d, o); addr = (addr + v) & 0xffffffff
        elif op == 0x90:
            out.append((addr, sym, addend)); addr += 4
        elif op == 0xa0:
            v, o = uleb(d, o); out.append((addr, sym, addend)); addr = (addr + 4 + v) & 0xffffffff
        elif op == 0xb0:
            out.append((addr, sym, addend)); addr += 4 + imm * 4
        elif op == 0xc0:
            cnt, o = uleb(d, o); skip, o = uleb(d, o)
            for _ in range(cnt):
                out.append((addr, sym, addend)); addr += 4 + skip
        else:
            raise ValueError('bad bind opcode %x at %x' % (b, o))
    return out


def load_imports(m):
    """fills m.stubs {stub addr: name}, m.ptrslots {slot addr: name}, m.binds [(addr, name, addend)]"""
    syms = m.symbols()
    m.syms = syms
    indoff, nind = m.dysym[12], m.dysym[13]
    ind = struct.unpack('<%dI' % nind, m.d[indoff:indoff + 4 * nind])
    m.stubs, m.ptrslots = {}, {}
    for s in m.sects:
        ty = s['flags'] & 0xff
        if ty == 8:      # S_SYMBOL_STUBS
            ss = s['r2']
            for i in range(s['size'] // ss):
                si = ind[s['r1'] + i]
                if si < 0x40000000:
                    m.stubs[s['addr'] + i * ss] = syms[si][0]
            m.stub_lo, m.stub_hi = s['addr'], s['addr'] + s['size']
        elif ty in (6, 7):   # non-lazy / lazy symbol pointers
            for i in range(s['size'] // 4):
                si = ind[s['r1'] + i]
                if si < 0x40000000:
                    m.ptrslots[s['addr'] + 4 * i] = syms[si][0]
    m.binds = []
    if m.dyldinfo:
        ro, rs, bo, bs, wo, ws, lo, ls, eo, es = m.dyldinfo
        for off, size in ((bo, bs), (wo, ws), (lo, ls)):
            if size:
                m.binds += parse_binds(m, off, size)
    # classic external relocations, if any
    return m
