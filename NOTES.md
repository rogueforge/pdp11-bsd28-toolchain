# Development Notes

A 2.8BSD PDP-11 cross-toolchain that runs on modern 64-bit Linux and
produces PDP-11 a.out binaries, built from the authentic 2.8BSD (1981)
sources. Sibling project to `~/vax-bsd42-toolchain`, but the compiler is
**Ritchie's `cc`** (the c0/c1/c2 pass compiler), *not* PCC.

## Source origin

All sources imported unmodified from `~/bsd/2.8/usr/kernel/src`:

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

## Reference assets on this machine

- `~/bsd/2.8/usr/kernel/src` — full 2.8BSD system source (pristine).
- `~/bsd/2.9/usr/src/cmd/{c,as,cpp,ld}` — 2.9BSD equivalents (cross-ref).
- `~/pdp11-tools` — GNU binutils 2.43 pdp11-aout (as/ld/ar/nm) — oracle only.
- `~/vax-bsd42-toolchain` — the sibling project this one mirrors.
