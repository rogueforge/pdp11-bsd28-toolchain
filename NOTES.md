# Development Notes

A 2.8BSD PDP-11 cross-toolchain that runs on modern 64-bit Linux and
produces PDP-11 a.out binaries, built from the authentic 2.8BSD (1981)
sources. Sibling project to `~/vax-bsd42-toolchain`, but the compiler is
**Ritchie's `cc`** (the c0/c1/c2 pass compiler), *not* PCC.

## Source origin

The authentic 2.8BSD sources below are **vendored in this repository** — the
build (`make all && make test`) depends on nothing outside the tree.  The
paths are recorded only for provenance: where each file was originally
imported from (unmodified), under 2.8BSD `usr/kernel/src`:

- `cmd/cc.c`            -> `cc/`
- `cmd/cpp/*`          -> `cpp/`
- `cmd/c/c0[0-5].c`    -> `c0/`   (pass 1: parser/front-end)
- `cmd/c/c1[0-3].c`    -> `c1/`   (pass 2: PDP-11 code generator)
- `cmd/c/table.s`, `cvopt.c` -> `c1/` (codegen template table + converter)
- `cmd/c/c2[01].c`     -> `c2/`   (peephole optimizer)
- `cmd/as/as1?.s as2?.s` -> `as/orig/` (assembler, in PDP-11 assembly)
- `cmd/ld.c`           -> `ld/`
- `cmd/{nm,size,strip}.c`, `usr/misc/ar.c` -> tool dirs
- `usr/kernel/include/*` -> `include/`

## Compiler pipeline

```
source.c -> cpp -> c0 -> c1 -> [c2] -> as -> source.o -> ld -> a.out
```

- **c0** (pass 1): C source -> intermediate trees in temp files. Front-end:
  lexing, parsing, declaration handling. `c00`-`c05`.
- **c1** (pass 2): intermediate -> PDP-11 assembly. Code generator driven by
  `table.s`, a template table compiled by `cvopt` into `table.i` (assembly)
  then assembled and linked into the c1 binary. `c10`-`c13`.
- **c2**: peephole optimizer on the assembly (optional; `-O`).
- **as**: PDP-11 assembler, two passes (`as1x` = pass 1, `as2x` = pass 2).
- **ld**: link editor. a.out magics 0405 (overlay/OMAGIC), 0407 (FMAGIC,
  normal), 0410 (NMAGIC, pure text), 0411 (IMAGIC, separate I/D).

## Key decisions

- **Fully authentic backend.** Unlike the VAX project (whose `as`/`ld` were
  already C), 2.8BSD's `as` is pure PDP-11 assembly. We **reimplement `as`
  in C** faithfully — same 2BSD assembler syntax (what c1 emits), full
  PDP-11 instruction set, byte-identical 2BSD a.out output — and use the
  real `ld.c`. No GNU binutils in the produced toolchain. (`~/pdp11-tools`,
  a GNU binutils pdp11-aout build, is available only as a cross-check
  oracle for a.out/relocation correctness.)
- **First milestone:** `hello.c` -> runnable PDP-11 a.out, verified end to
  end (SIMH 2.8BSD / apout).

## Porting challenges (vs. the VAX project)

1. **16-bit target on a 64-bit host.** PDP-11 is `int`=2, `ptr`=2, `long`=4
   — a wider gulf than the VAX's ILP32-on-LP64. The compiler folds
   constants assuming 16-bit wraparound; a.out struct I/O must use
   fixed-width fields (`cross/` headers, as in the VAX project) more
   pervasively.
2. **The assembler.** Written only in PDP-11 assembly; must be reimplemented
   in C. It is on the critical path for three things: assembling **libc**
   (99 `.s` syscall/gen stubs + `crt0.s`), assembling **c1's `table.s`**,
   and assembling **user programs**.
3. **c1's codegen table.** `table.s` is PDP-11 assembly linked into the c1
   binary. `cvopt.c` (C) converts the template form to assembly. For a host
   build the table must become host-buildable data/object.

## Per-tool porting guides

`docs/` has a porting guide for each tool, written as it is ported:
`docs/README.md` (index + the shared porting checklist), then
`cross-headers.md`, `binutils.md`, `cpp.md`, `c0.md`, `c1.md`. New guides
are added for `c2`, `as`, `ld`, and `cc` as those land.

## Testing

`make test` runs `tests/run.sh`, a small regression harness:

- `tests/cpp/*.c` — golden tests: preprocess the input and diff against
  `*.expected` (cpp line markers are normalised to basenames so goldens are
  path-independent). `make test-update` (`run.sh -u`) regenerates goldens
  after an intentional behaviour change.
- `tests/binutils/*.sh` (and `tests/<pass>/*.sh` as passes land) — shell
  tests sourcing `tests/lib.sh`; exit status is the verdict.
- `tests/fixtures/` — committed real 2.8BSD PDP-11 objects (e.g.
  `dkleave.o`) for the binutils tests.

## Known limitations (porting, to revisit)

- **c2 optimizer: done** (was "not yet reliable"). `cc -O` runs it and
  produces correct, smaller code; covered by `tests/cc/optimizer.sh`. The
  porting bugs were the usual host/LP64 classes: `copy()`'s K&R varargs
  stack-walk (`(&ap)[1]`) → `<stdarg.h>`; `alloc()`'s `sbrk` node arena →
  malloc chunks (sbrk corrupts the host heap c2's stdio uses); and the NUL
  bytes c1 emits on the host — glibc's `printf("%c",0)` writes a NUL where
  the PDP-11 `_doprnt` emitted nothing — which c2 now skips in `getlin`, as
  `as` already did.  c2 is opt-in via `-O`.  See docs/c2.md.

## Known limitations (authentic 2.8BSD behaviour, preserved)

- **cpp `defined()` operator leaks `flslvl`.** `#if defined(M)` where `M`
  is a *defined* macro pre-expands to `defined(10)`; yylex then takes the
  numeric path, which skips the `--flslvl` reset that only the identifier
  path performs. The `#if` itself yields the right answer, but the leak
  suppresses the *next* `#if`. Harmless in 1981 code (which used `#ifdef`,
  not the then-new `defined()` operator). Captured by
  `tests/cpp/defined_quirk.c`. Bug-compatible with stock 2.8BSD.

## Build approach (mirrors VAX project)

- Top-level Makefile using only BSD-make features (no GNU `%` rules,
  `$(shell)`, `$(wildcard)`).
- `HOSTCC` builds K&R C with `-std=gnu89 -fno-strict-aliasing -fwrapv
  -fcommon` plus K&R warning suppressions.
- `PREFIX = pdp11-bsd28`; tools install to `usr/bin/pdp11-bsd28-*`.
- Targets: `make tools` / `headers` / `libs` / `all`.

## Roadmap / status

See the session task list. Order of attack:

1. cross/ headers + Makefile skeleton
2. binutils C tools (nm, size, strip, ar, ld) — straightforward host ports
3. cc driver
4. cpp
5. c0 (pass 1)
6. c1 (pass 2) + table
7. c2 (optimizer)
8. **as** — C reimplementation (the linchpin)
9. minimal libc + crt0
10. hello.c -> runnable a.out (first milestone)

## Parity with the sibling VAX toolchain (possible future work)

Compared with `~/vax-bsd42-toolchain`, this project is *ahead* on the test
suite (20 regression tests vs one inline test), the per-tool `docs/` guides,
the `apsim` simulator, and `libucbpath` (relocatable path resolution).  The
VAX project has these we do not:

- **A much fuller libc.** This is the main gap; see "libc completion" below.
- **lint** — the two-pass C checker + its `llib-*.ln` libraries. (2.8BSD does
  ship `cmd/lint`; a real port, multi-week.)
- **libcurses + libtermlib** — terminal UI / termcap. (2.8BSD has both.)
- **Profiling startup** — `mcrt0.o`/`gcrt0.o` from `gen/mon.c` + `sys/profil`.
- **FP startup variants** — `fcrt0.o` etc. (we stub floats, so deferred).
- **A canonical large-program test** (the VAX tree builds `rogue`); ours could
  use one once the libc is fuller.

We deliberately do NOT mirror VAX-only items that don't apply: PCC/MIP (we use
Ritchie cc), and the VAX `/sys` kernel tree.  Networking (inet/, net/) barely
exists on the 2.8BSD PDP-11 and is out of scope.

### libc completion status

`libc.a` now carries the practical core of the authentic 2.8BSD libc: program
startup (crt0/csv/cerror), the long-arithmetic helpers (lmul/ldiv/lrem,
almul/aldiv/alrem, mcount), the string/memory and numeric routines from
`gen/` (strcmp/strlen/strcpy/…, atoi/atol/abs/atof, ctype, qsort, getenv,
perror, …), buffered stdio (printf/fprintf/sprintf + scanf/sscanf + fopen/
fgets/fputs/fgetc/getc/putc/puts/ungetc/fseek/ftell/setbuf and the internals),
the authentic free-list malloc/free/realloc, and the common `sys/` syscall
stubs.  **Floating point is fully supported** (DEC F/D format, not IEEE): the
FP11 codegen assembles and runs in apsim, and printf `%f`/`%e`/`%g`, scanf
`%f`, atof, and ecvt/fcvt/gcvt/modf/ldexp/frexp all work.  Still out of scope
(stubbed or omitted): the passwd/group database, networking, and the
overlay/non-FP/profiling build variants.

## Development-time references (NOT build dependencies)

These were used only while porting — to cross-check behaviour and trace
provenance.  None is required to build, test, or run this toolchain; the repo
is self-contained.

- 2.8BSD `usr/kernel/src` — full 2.8BSD system source (pristine), the import origin.
- 2.9BSD `usr/src/cmd/{c,as,cpp,ld}` — 2.9BSD equivalents (cross-ref).
- GNU binutils 2.43 `pdp11-aout` (as/ld/ar/nm) — disassembly oracle only.
- `vax-bsd42-toolchain` — the sibling project this one mirrors.
