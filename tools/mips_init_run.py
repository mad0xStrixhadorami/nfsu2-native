#!/usr/bin/env python3
"""Replay a straight-line static initializer of SLES_527.25 (R5900) in a tiny interpreter and dump
the memory it writes. Data extraction only (ISSUES.md #128): calls are stubbed (v0 = a0), so
anything a callee would compute is missing. Usage: mips_init_run.py ELF FUNC_HEX OUT.json"""
import json, struct, sys
import capstone
from capstone import mips

GP = 0x5695F0  # from Ghidra: lwc1 -0x7664($gp) in 0x1572f8 resolves to 0x561f8c

elf = open(sys.argv[1], 'rb').read()
ph = struct.unpack_from('<I', elf, 0x1c)[0]
_, off, base, _, filesz, memsz = struct.unpack_from('<IIIIII', elf, ph)
mem = bytearray(memsz + 0x400000)
mem[:filesz] = elf[off:off + filesz]
written = {}

def u32(a): return struct.unpack_from('<I', mem, a - base)[0]
def store(a, data):
    mem[a - base:a - base + len(data)] = data
    for k in range(len(data)): written[a + k] = 1

md = capstone.Cs(capstone.CS_ARCH_MIPS, capstone.CS_MODE_MIPS64 + capstone.CS_MODE_LITTLE_ENDIAN)
md.detail = True
md.skipdata = True
R = [0] * 32
F = [0] * 32  # raw 32-bit float registers
R[28] = GP
R[29] = base + memsz + 0x200000  # stack
R[31] = 0xdead0000
def reg(op): return md.reg_name(op.reg)
def gi(name):  # gpr index
    return {'zero':0,'at':1,'v0':2,'v1':3,'a0':4,'a1':5,'a2':6,'a3':7,'t0':8,'t1':9,'t2':10,'t3':11,'t4':12,'t5':13,'t6':14,'t7':15,
            's0':16,'s1':17,'s2':18,'s3':19,'s4':20,'s5':21,'s6':22,'s7':23,'t8':24,'t9':25,'k0':26,'k1':27,'gp':28,'sp':29,'fp':30,'s8':30,'ra':31}[name]
def fi(name): return int(name[1:])
def s16(x): return x - 0x10000 if x & 0x8000 else x

cache = {}
def insn(pc):
    if pc not in cache:
        cache[pc] = next(md.disasm(bytes(mem[pc - base:pc - base + 4]), pc), None)
    return cache[pc]

def step(pc):
    """Executes the instruction at pc; returns a branch target or None."""
    i = insn(pc)
    m, ops = i.mnemonic, i.operands
    def r(k): return R[gi(reg(ops[k]))]
    def setr(k, v):
        n = gi(reg(ops[k]))
        if n: R[n] = v & 0xffffffff
    def mema(k): return (R[gi(md.reg_name(ops[k].mem.base))] + ops[k].mem.disp) & 0xffffffff
    if m in ('nop', 'sync', 'sync.p'): return None
    if m == 'lui': setr(0, ops[1].imm << 16)
    elif m in ('addiu', 'daddiu'): setr(0, r(1) + ops[2].imm)
    elif m == 'ori': setr(0, r(1) | (ops[2].imm & 0xffff))
    elif m == 'andi': setr(0, r(1) & (ops[2].imm & 0xffff))
    elif m in ('addu', 'daddu'): setr(0, r(1) + r(2))
    elif m in ('subu', 'dsubu'): setr(0, r(1) - r(2))
    elif m == 'or': setr(0, r(1) | r(2))
    elif m in ('move',): setr(0, r(1))
    elif m == 'sll': setr(0, r(1) << ops[2].imm)
    elif m == 'srl': setr(0, r(1) >> ops[2].imm)
    elif m == 'slti': setr(0, int(struct.unpack('<i', struct.pack('<I', r(1)))[0] < ops[2].imm))
    elif m == 'sw': store(mema(1), struct.pack('<I', r(0)))
    elif m == 'sh': store(mema(1), struct.pack('<H', r(0) & 0xffff))
    elif m == 'sb': store(mema(1), bytes([r(0) & 0xff]))
    elif m == 'lw': setr(0, u32(mema(1)))
    elif m == 'lhu': setr(0, struct.unpack_from('<H', mem, mema(1) - base)[0])
    elif m == 'lbu': setr(0, mem[mema(1) - base])
    elif m == 'swc1': store(mema(1), struct.pack('<I', F[fi(reg(ops[0]))]))
    elif m == 'lwc1': F[fi(reg(ops[0]))] = u32(mema(1))
    elif m == 'mtc1': F[fi(reg(ops[1]))] = r(0)
    elif m == 'mfc1': setr(0, F[fi(reg(ops[1]))])
    elif m == 'mov.s': F[fi(reg(ops[0]))] = F[fi(reg(ops[1]))]
    elif m == 'sd': store(mema(1), struct.pack('<II', r(0), 0))
    elif m == 'ld': setr(0, u32(mema(1)))
    elif m == 'sq': store(mema(1), struct.pack('<IIII', r(0), 0, 0, 0))
    elif m == 'jal':
        return ('call', ops[0].imm)
    elif m == 'jalr': return ('call', None)
    elif m == 'j': return ('jump', ops[0].imm)
    elif m == 'b': return ('jump', ops[0].imm)
    elif m == 'jr': return ('ret', None)
    elif m in ('bne', 'beq', 'bnel', 'beql'):
        t = (r(0) != r(1)) if m.startswith('bne') else (r(0) == r(1))
        return ('br', ops[2].imm if t else None, m.endswith('l'))
    elif m in ('bgez', 'bltz', 'blez', 'bgtz', 'bnez', 'beqz'):
        x = struct.unpack('<i', struct.pack('<I', r(0)))[0]
        t = {'bgez': x >= 0, 'bltz': x < 0, 'blez': x <= 0, 'bgtz': x > 0, 'bnez': x != 0, 'beqz': x == 0}[m]
        return ('br', ops[1].imm if t else None, False)
    else:
        raise RuntimeError('unhandled %s %s at %x' % (m, i.op_str, pc))
    return None

pc = int(sys.argv[2], 16)
steps = 0
while steps < 2_000_000:
    steps += 1
    ctl = step(pc)
    if ctl is None:
        pc += 4
        continue
    kind = ctl[0]
    if kind == 'br' and ctl[2] and ctl[1] is None:  # branch-likely not taken: skip delay slot
        pc += 8
        continue
    step(pc + 4)  # delay slot
    if kind == 'call':
        R[2] = R[4]  # stub: return the object (constructors)
        pc += 8
    elif kind == 'ret':
        break
    elif kind == 'jump' or (kind == 'br' and ctl[1] is not None):
        pc = ctl[1]
    else:
        pc += 8
print('steps', steps, 'bytes written', len(written), file=sys.stderr)
# Coalesce written ranges.
runs, start, prev = [], None, None
for a in sorted(written):
    if start is None: start = prev = a
    elif a == prev + 1: prev = a
    else: runs.append((start, prev + 1)); start = prev = a
if start is not None: runs.append((start, prev + 1))
json.dump({'gp': GP, 'runs': [[s, e, mem[s - base:e - base].hex()] for s, e in runs]}, open(sys.argv[3], 'w'))
for s, e in runs: print('%08x..%08x %6d' % (s, e, e - s), file=sys.stderr)
