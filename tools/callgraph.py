#!/usr/bin/env python3
"""Call graph from the Ghidra pseudocode dump (reference/all/<addr>.c, index.tsv).
Direct calls only (FUN_xxxxxxxx and named functions); indirect/virtual calls are not followed.

Usage:
  callgraph.py REFDIR reach ADDR [ADDR...]   total code reachable from these roots
  callgraph.py REFDIR tree ADDR [depth]       print the call tree
"""
import os
import re
import sys


def load(refdir):
    names, sizes = {}, {}
    for line in open(os.path.join(refdir, 'index.tsv')).read().splitlines()[1:]:
        addr, name, size, _ = line.split('\t')
        a = int(addr, 16)
        names[a] = name
        sizes[a] = int(size)
    by_name = {n: a for a, n in names.items()}
    calls = {}
    call_re = re.compile(r'\b([A-Za-z_][A-Za-z0-9_]*)\s*\(')
    for a in names:
        path = os.path.join(refdir, f'{a:08x}.c')
        if not os.path.exists(path):
            continue
        body = open(path).read().split('\n', 3)[-1]
        out = []
        for m in call_re.finditer(body):
            t = by_name.get(m.group(1))
            if t is not None and t != a and t not in out:
                out.append(t)
        calls[a] = out
    return names, sizes, calls


def reach(calls, roots):
    seen, stack = set(), list(roots)
    while stack:
        a = stack.pop()
        if a in seen:
            continue
        seen.add(a)
        stack.extend(calls.get(a, []))
    return seen


def main():
    refdir, mode = sys.argv[1], sys.argv[2]
    names, sizes, calls = load(refdir)
    if mode == 'reach':
        roots = [int(x, 16) for x in sys.argv[3:]]
        s = reach(calls, roots)
        print(f'{len(s)} functions, {sum(sizes[a] for a in s)} bytes of code '
              f'({100 * len(s) / len(names):.1f}% of {len(names)} functions)')
    elif mode == 'tree':
        root = int(sys.argv[3], 16)
        depth = int(sys.argv[4]) if len(sys.argv) > 4 else 2
        seen = set()

        def show(a, d):
            sub = reach(calls, [a])
            print('  ' * d + f'{a:08x} {names[a]:<28} size {sizes[a]:>6}  subtree {len(sub):>5} fn')
            if d < depth and a not in seen:
                seen.add(a)
                for c in calls.get(a, []):
                    show(c, d + 1)
        show(root, 0)


if __name__ == '__main__':
    main()
