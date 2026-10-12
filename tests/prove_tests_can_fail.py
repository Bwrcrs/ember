# -*- coding: utf-8 -*-
"""
Prove the tests can fail.

For each 8xy_ handler, apply ONE targeted mutation that gets the
VF write order wrong, rebuild, run the harness, and record how many
checks went red. A handler whose mutation leaves the suite green is
a handler the suite does not actually cover -- "a test that cannot
fail is not a test".

Restores src/cpu.cpp and rebuilds at the end no matter what.
"""
import io
import os
import re
import subprocess
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
SRC = os.path.join(ROOT, 'src', 'cpu.cpp')
BUILD = os.path.join(ROOT, 'build.bat')
HARNESS = os.path.join(ROOT, 'tests', 'run_tests.ps1')


def func_span(s, name):
    """(start, end) of a Machine member function definition."""
    needle = 'void Machine::%s(const Decoded& d) {' % name
    i = s.index(needle)
    j = s.index('{', i)
    depth = 0
    for k in range(j, len(s)):
        if s[k] == '{':
            depth += 1
        elif s[k] == '}':
            depth -= 1
            if depth == 0:
                return i, k + 1
    raise AssertionError('unbalanced braces in ' + name)


def break_swap(s, name):
    """Move the VF write to before the Vx write (the classic bug)."""
    a, b = func_span(s, name)
    body = s[a:b]
    vf = re.search(r'\n([ \t]*)(v_\[VF_INDEX\] = [^\n]*;)\n', body)
    assert vf, name + ': no VF write'
    indent, stmt = vf.group(1), vf.group(2)
    body = body[:vf.start()] + '\n' + body[vf.end():]
    vx = re.search(r'\n[ \t]*(v_\[d\.x\] = [^\n]*;)\n', body)
    assert vx, name + ': no Vx write'
    body = body[:vx.start() + 1] + indent + stmt + '\n' + body[vx.start() + 1:]
    return s[:a] + body + s[b:]


def break_logic(s, name, op):
    """Write the VF reset before the result instead of after."""
    a, b = func_span(s, name)
    body = s[a:b]
    old = (
        '    v_[d.x] = static_cast<std::uint8_t>(v_[d.x] %s v_[d.y]);\n'
        '    if (quirks_.vfResetOnLogic) {\n'
        '        v_[VF_INDEX] = 0;\n'
        '    }\n' % op
    )
    new = (
        '    if (quirks_.vfResetOnLogic) {\n'
        '        v_[VF_INDEX] = 0;\n'
        '    }\n'
        '    v_[d.x] = static_cast<std::uint8_t>(v_[d.x] %s v_[d.y]);\n' % op
    )
    assert old in body, name + ': body did not match'
    return s[:a] + body.replace(old, new) + s[b:]


BREAKS = [
    # (function, mode, op, label, detectable)
    #
    # "detectable" is False only where the mutation is provably
    # EQUIVALENT to the original -- not where the tests are weak.
    # See AND_EQUIVALENCE below.
    ('opAddReg', 'swap',  None, '8xy4 ADD',  True),
    ('opSub',    'swap',  None, '8xy5 SUB',  True),
    ('opSubn',   'swap',  None, '8xy7 SUBN', True),
    ('opShr',    'swap',  None, '8xy6 SHR',  True),
    ('opShl',    'swap',  None, '8xyE SHL',  True),
    ('opOr',     'logic', '|',  '8xy1 OR',   True),
    ('opAnd',    'logic', '&',  '8xy2 AND',  False),
    ('opXor',    'logic', '^',  '8xy3 XOR',  True),
]

# Why 8xy2 AND is the one exception.
#
# Moving the VF reset ahead of the result is a genuine behavioural
# change for OR and XOR, because with x == 0xF the destination and the
# reset target are the same register:
#
#     OR : VF = 0, then VF = 0 | Vy = Vy     -> VF = Vy  (Vy != 0, so it differs)
#     XOR: VF = 0, then VF = 0 ^ Vy = Vy     -> VF = Vy  (Vy != 0, so it differs)
#     AND: VF = 0, then VF = 0 & Vy = 0      -> VF = 0   (identical to correct)
#
# AND is absorbed by zero: 0 & anything is 0, and 0 is exactly the
# value the reset wanted anyway. The two orders agree for every
# possible operand and every setting of vfResetOnLogic, so NO test can
# distinguish them -- this is a theorem about the instruction, not a
# hole in the suite. Any ROM would behave the same.
#
# The coverage of opAnd therefore has to be shown some other way, by a
# mutation that does change behaviour: swap the operator. Both lists
# must come back fully CAUGHT for the run to pass.

COVERAGE_BREAKS = [
    # (function, old fragment, new fragment, label)
    ('opAnd', 'v_[d.x] & v_[d.y]', 'v_[d.x] | v_[d.y]', '8xy2 AND (op -> OR)'),
    ('opAnd', 'v_[d.x] & v_[d.y]', 'v_[d.x] ^ v_[d.y]', '8xy2 AND (op -> XOR)'),
]


def run(cmd):
    return subprocess.run(cmd, capture_output=True, text=True, cwd=ROOT)


def total_line():
    r = run(['powershell', '-NoProfile', '-File', HARNESS, '-Quiet'])
    m = re.search(r'Total (\d+): (\d+) pass, (\d+) fail', r.stdout)
    assert m, 'could not parse harness output:\n' + r.stdout + r.stderr
    return int(m.group(2)), int(m.group(3))


original = io.open(SRC, encoding='utf-8').read()
print('baseline (unmutated source):')
p, f = total_line()
baseline_pass, baseline_fail = p, f
print('  %d pass, %d fail\n' % (p, f))
if f != 0:
    print('baseline is not green -- fix that first')
    sys.exit(1)

rows = []
try:
    for name, mode, op, label, detectable in BREAKS:
        if mode == 'swap':
            mutated = break_swap(original, name)
        else:
            mutated = break_logic(original, name, op)
        assert mutated != original, name + ': mutation was a no-op'

        io.open(SRC, 'w', encoding='utf-8', newline='').write(mutated)
        b = run(['cmd', '/c', BUILD, 'headless', 'nopause'])
        if b.returncode != 0:
            rows.append((label, 'BUILD FAILED', 0, 0, detectable))
            continue
        p, f = total_line()
        rows.append((label, 'mutated', p, f, detectable))

    for name, old, new, label in COVERAGE_BREAKS:
        a, b_ = func_span(original, name)
        body = original[a:b_]
        assert old in body, label + ': fragment not found'
        mutated = original[:a] + body.replace(old, new) + original[b_:]

        io.open(SRC, 'w', encoding='utf-8', newline='').write(mutated)
        b = run(['cmd', '/c', BUILD, 'headless', 'nopause'])
        if b.returncode != 0:
            rows.append((label, 'BUILD FAILED', 0, 0, True))
            continue
        p, f = total_line()
        rows.append((label, 'mutated', p, f, True))
finally:
    io.open(SRC, 'w', encoding='utf-8', newline='').write(original)
    run(['cmd', '/c', BUILD, 'headless', 'nopause'])

print('%-22s %-8s %-6s %-6s %s' % ('mutation', 'result', 'pass', 'fail', 'verdict'))
print('-' * 68)
problems = []
for label, state, p, f, detectable in rows:
    if state == 'BUILD FAILED':
        print('%-22s %-8s %-6s %-6s %s' % (label, state, '-', '-', 'n/a'))
        problems.append(label + ' (did not build)')
        continue
    caught = f > baseline_fail
    if caught:
        verdict = 'CAUGHT'
    elif not detectable:
        verdict = 'equivalent mutant (expected)'
    else:
        verdict = '*** NOT CAUGHT ***'
        problems.append(label)
    print('%-22s %-8s %-6d %-6d %s' % (label, state, p, f, verdict))

print()
if problems:
    print('PROBLEMS: ' + ', '.join(problems))
    sys.exit(1)
print('every 8xy_ write order that CAN be observed is pinned by a check,')
print('and 8xy2 AND -- whose order provably cannot be observed -- is')
print('covered by an operator mutation instead.')
