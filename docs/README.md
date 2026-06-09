# Porting guides

This directory documents how each tool of the 2.8BSD PDP-11 toolchain was
ported to build and run on a modern 64-bit Linux host while still producing
PDP-11 a.out objects. There is one guide per tool; read them in pipeline
order.

## The toolchain

```
source.c ─cpp─▶ ─c0─▶ ─c1─▶ [c2] ─as─▶ file.o ─ld─▶ a.out
                └────── ritchie cc ──────┘
```

| Guide | Tool(s) | Role | Status |
|-------|---------|------|--------|
| [cross-headers.md](cross-headers.md) | `cross/` a.out.h, ar.h | fixed-width on-disk structs for LP64 hosts | done |
| [binutils.md](binutils.md) | `nm`, `size`, `strip` | inspect/modify a.out | done |
| [cpp.md](cpp.md) | `cpp` + `libucbpath` | C preprocessor | done |
| [c0.md](c0.md) | `c0` | compiler pass 1 (parser) | done |
| [c1.md](c1.md) | `c1` + codegen table | compiler pass 2 (PDP-11 codegen) | done |
| [c2.md](c2.md) | `c2` | peephole optimizer | builds; optimizer not yet reliable |
| [as.md](as.md) | `as` | assembler (C reimplementation) | core works; numeric local labels todo |
| [ld.md](ld.md) | `ld` | link editor | links objects; verified by relocated disassembly |
| [cc.md](cc.md) | `cc` | compiler driver | -S works; -c/-o need as/ld |

## Porting philosophy

The sources are the authentic 2.8BSD (1981) Ritchie cc toolchain. Changes
are kept minimal and are one of a small number of recurring kinds. Every
guide is organised around the same checklist because the same problems
recur — the compiler passes especially, being pre-1977 C, hit all of them:

1. **Host vs. target word size.** The PDP-11 is 16-bit (`int`=2, `ptr`=2,
   `long`=4); the host is LP64 (`int`=4, `ptr`=8, `long`=8). Target type
   sizes in the compiler are explicit constants (`SZINT`, `SZPTR`, …) and
   need no change, but anything that conflated host and target widths does.
2. **On-disk struct layout.** a.out/archive structures are read and written
   raw. They must match the PDP-11 byte layout exactly — see
   [cross-headers.md](cross-headers.md). PDP-11 and x86-64 are both
   little-endian, so only field widths and packing differ, not byte order.
3. **Pre-modern C syntax.** Ancient compound-assignment operators
   (`=+`, `=<<`, …), old-style initialisers without `=`, and the old-C
   "global struct members" idiom (a member reachable through any struct
   pointer by name/offset).
4. **LP64 pointer truncation.** K&R functions and calls with no prototype
   default to an `int` return/parameter and silently truncate 64-bit
   pointers. This is the single most common runtime crash.
5. **K&R varargs.** Walking a variadic argument list by taking the address
   of the last named parameter assumes stack-contiguous arguments — false
   on the x86-64 register ABI; rewrite with `<stdarg.h>`.

A condensed checklist of these for the compiler passes is in the project
memory note `ritchie-cc-porting-patterns`.

## Build & test

```sh
make tools     # build everything ported so far into usr/bin/
make test      # build, then run the regression suite (tests/)
```

`HOSTCC` builds K&R C with `-std=gnu89 -fno-strict-aliasing -fwrapv
-fcommon` plus warning suppressions; the compiler passes add
`-fms-extensions` for the anonymous-union node supersets.
