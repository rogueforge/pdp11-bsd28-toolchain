# apsim (verification simulator)

`sim/apsim.c` is a small **host-side user-mode PDP-11 simulator** used to run
the toolchain's output and verify it end to end.  It is NOT part of the
produced toolchain -- it is a test aid, like the GNU `objdump` oracle.

It loads a classic 2.8BSD a.out (0407/0410/0411) into a 64 KB address space,
executes PDP-11 instructions (the full general-register set, all eight
addressing modes, double/single-operand, branches, JSR/RTS/SOB, the EIS
MUL/DIV/ASH, condition codes, and the **FP11 floating-point unit** -- the six
accumulators as host doubles converting to/from DEC F/D format, the float
arithmetic/compare/convert ops, and the FP status), and emulates the 2BSD `sys` traps
the libc stubs issue -- both the direct form (`sys exit`) and the indirect
form (`sys 0; argblock`) used by write/read/etc.  Implemented calls: exit,
write, read, open, close, creat, lseek (enough for stdio-free programs).

```
make sim                       # builds usr/bin/<prefix>-apsim
<prefix>-cc hello.c -o hello
<prefix>-apsim hello           # runs it; exit status = the program's
<prefix>-apsim -t hello        # + per-instruction trace
```

`tests/cc/endtoend.sh` compiles, links and runs several programs through it
(return value -> exit status, `write` output, recursion).
