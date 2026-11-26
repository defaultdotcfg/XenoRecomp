"""Emit rt_tables.c: dyld bind slots, import stub dispatch table, default shims."""
import os, re


def cname(n):
    return re.sub(r'\W', '_', n)


def emit(m, outdir):
    defd = {}
    for nm, t, sc, desc, v in m.syms:
        if (t & 0x0e) == 0x0e and v:
            defd.setdefault(nm, v)
    stub_by_name = {n: a for a, n in m.stubs.items()}
    slots = {}
    for a, n in m.ptrslots.items():
        slots[a] = (n, 0)
    for a, n, ad in m.binds:
        slots[a] = (n, ad)
    with open(os.path.join(outdir, 'rt_tables.c'), 'w') as f:
        f.write('#include "armrt.h"\n#include "imports.h"\n\n')
        f.write('/* external symbol slots: *(u32*)addr = value(name) + addend; value 0 => runtime allocates an object named `name` */\n')
        f.write('const struct { uint32_t addr, val, addend; const char *name; } rt_slots[] = {\n')
        for a in sorted(slots):
            n, ad = slots[a]
            if n in defd:
                f.write('  { 0x%x, 0x%x, %d, "%s" },\n' % (a, defd[n], ad, n))
            elif n in stub_by_name:
                f.write('  { 0x%x, 0x%x, %d, "%s" },\n' % (a, stub_by_name[n], ad, n))
            else:
                f.write('  { 0x%x, 0, %d, "%s" },\n' % (a, ad, n))
        f.write('  { 0, 0, 0, 0 }\n};\n\n')
        f.write('const struct { uint32_t addr; guest_fn fn; const char *name; } rt_stub_table[] = {\n')
        for a, n in sorted(m.stubs.items()):
            f.write('  { 0x%x, imp_%s, "%s" },\n' % (a, cname(n), n))
        f.write('};\nconst unsigned rt_stub_count = %d;\n' % len(m.stubs))
    with open(os.path.join(outdir, 'rt_defaults.c'), 'w') as f:
        f.write('#include "armrt.h"\n#include "imports.h"\nvoid rt_unimpl(CPU *c, const char *name);\n')
        # imports the runtime doesn't implement fall back to rt_unimpl: MSVC links them as /alternatename,
        # clang/gcc (MinGW) as weak definitions that a runtime definition overrides
        names = sorted(set(m.stubs.values()))
        f.write('#if defined(_MSC_VER) && !defined(__clang__)\n')
        for n in names:
            cn = cname(n)
            f.write('void imp_default_%s(CPU *c) { rt_unimpl(c, "%s"); }\n' % (cn, n))
            f.write('#pragma comment(linker, "/alternatename:imp_%s=imp_default_%s")\n' % (cn, cn))
        f.write('#else\n')
        for n in names:
            f.write('__attribute__((weak)) void imp_%s(CPU *c) { rt_unimpl(c, "%s"); }\n' % (cname(n), n))
        f.write('#endif\n')
