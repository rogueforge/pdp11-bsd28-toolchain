# pdp11-bsd28-toolchain

A **2.8BSD PDP-11 cross-toolchain that runs on modern 64-bit Linux** and
produces classic PDP-11 `a.out` binaries — built from the authentic 2.8BSD
(1981) sources, ported to an LP64 host. The compiler is **Ritchie's `cc`**
(the `c0`/`c1`/`c2` multi-pass compiler), *not* PCC.

```
source.c ─cpp─▶ ─c0─▶ ─c1─▶ [c2] ─as─▶ file.o ─ld─▶ a.out
                └────── Ritchie cc ──────┘
```

Everything is vendored in this repository; the build depends on nothing
outside the tree. A user-mode PDP-11 simulator (`apsim`) is included so the
output can be executed and verified on the host.

## Quick start

```sh
make            # build the whole toolchain into ./usr/bin/
make test       # build, then run the regression suite (16 tests)

# compile a C program to a PDP-11 a.out and run it in the simulator
echo 'int main(){ printf("hello, pdp-11!\n"); return 0; }' > hi.c
./usr/bin/pdp11-bsd28-cc hi.c -o hi
./usr/bin/pdp11-bsd28-apsim hi
#  -> hello, pdp-11!
```

## Prerequisites

- A C compiler for the host (`cc`/`gcc`/`clang`) and `make`.
- A 64-bit little-endian Linux host (developed on x86-64 / WSL2).
- Nothing else — no PDP-11 hardware, no external BSD tree, no cross-libs.

Override the host compiler if needed: `make HOSTCC=gcc`. The install prefix
defaults to the repo root (`DESTDIR=.`); set `DESTDIR=/some/where` to build
the `usr/bin`, `usr/lib`, `usr/include` tree elsewhere.

## What gets built

All tools are installed to `${DESTDIR}/usr/bin/` with a `pdp11-bsd28-` prefix:

| Tool | Role |
|------|------|
| `pdp11-bsd28-cc`   | compiler driver (runs cpp → c0 → c1 → as → ld) |
| `pdp11-bsd28-cpp`  | C preprocessor |
| `pdp11-bsd28-c0/c1/c2` | Ritchie cc passes (parser, PDP-11 codegen, peephole) |
| `pdp11-bsd28-as`   | assembler (C reimplementation; emits 2.8BSD a.out) |
| `pdp11-bsd28-ld`   | link editor |
| `pdp11-bsd28-ar` / `-ranlib` | archiver and `__.SYMDEF` table of contents |
| `pdp11-bsd28-nm` / `-size` / `-strip` | a.out inspection tools |
| `pdp11-bsd28-apsim` | host-side user-mode PDP-11 simulator (runs the output) |

Plus the runtime in `${DESTDIR}/usr/lib/`: `crt0.o` and `libc.a` (a
substantial slice of the authentic 2.8BSD libc — 107 members: startup and
long-arithmetic helpers, the string/memory/numeric `gen/` routines, `qsort`,
buffered `stdio` including file I/O (`fopen`/`fgets`/`fputs`/`fseek`/…), and
~40 syscall stubs).  See [docs/libc.md](docs/libc.md).

## Using the toolchain

The driver resolves its sibling passes and `libc.a` relative to its own
location, so you can run it from anywhere:

```sh
B=$(pwd)/usr/bin/pdp11-bsd28

$B-cc  prog.c -o prog        # compile + link a runnable a.out
$B-cc  -c prog.c             # compile only -> prog.o
$B-cc  -S prog.c             # stop after codegen -> prog.s
$B-as  -o prog.o prog.s      # assemble
$B-ld  -o prog crt0.o prog.o -lc
$B-nm    prog                # list the symbol table
$B-size  prog                # text/data/bss sizes
$B-apsim prog                # execute it (stdout + exit status flow back)
```

`cc` accepts K&R / pre-1977 C (the C the 2.8BSD compiler itself is written
in): old-style function definitions, implicit `int`, etc.

## Repository layout

```
cc/  cpp/  c0/  c1/  c2/      compiler driver + passes
as/                           assembler (C reimplementation)
ld/  ar/  ranlib/             link editor, archiver, ranlib
nm/  size/  strip/            binutils
libc/                         crt0, csv, syscall stubs, stdio, malloc
cross/                        host build headers (a.out.h, ar.h, whoami.h, ...)
include/                      target system headers (installed to usr/include)
sim/                          apsim, the PDP-11 simulator
libucbpath/                   $PATH/relocatable-path helpers used by cc/cpp/ld
tests/                        regression suite (sh + golden files)
docs/                         one porting guide per tool
NOTES.md                      design notes, source provenance, porting log
```

## How it works / porting notes

The sources are authentic 2.8BSD; the changes needed to build them on an LP64
host fall into a small number of recurring classes (16-bit vs 64-bit widths,
on-disk a.out/archive struct layout, pre-modern C syntax, K&R varargs,
pointer-in-`int` truncation). Each tool has a porting guide in
[`docs/`](docs/), and the overall design and source provenance are in
[`NOTES.md`](NOTES.md).

## Status

The full pipeline works end to end: `cc hello.c -o hello` produces a runnable
classic 2.8BSD `a.out`, verified by execution in `apsim`, including buffered
`printf` (`%d %s %c %x %o`). The `c2` peephole optimizer works too — `cc -O`
runs it and produces correct, smaller code (loops, recursion, calls, and the
stdio path are covered by the test suite). It is opt-in via `-O`.

## License / provenance

The 2.8BSD sources are under the BSD license (see [`LICENSE`](LICENSE)).
2.8BSD was released by the University of California, Berkeley; the historical
Unix sources it derives from were made freely redistributable by Caldera.
