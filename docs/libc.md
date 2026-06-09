# libc + crt0 (runtime)

The C runtime and a minimal libc, assembled from the **authentic 2.8BSD
sources** (`libc/` mirrors `usr/kernel/src/libc/`) by this toolchain's own
`as`, and archived by its `ar` into `usr/lib/libc.a`.  `cc` links a program
as `ld -X crt0.o <objs> -lc`.

## Pieces

- `libc/csu/crt0.s` — C startup `start:`: sets up argc/argv/environ on the
  stack, calls `_main`, passes its return value to `exit`.
- `libc/crt/csv.s` — `csv`/`cret`, the function prologue/epilogue helpers c1
  emits a call to in every function.
- `libc/crt/cerror.s` — error return (`sets errno`, returns -1).
- `libc/gen/cuexit.s` — `exit()` (flush via `__cleanup`, then the exit trap).
- `libc/gen/cleanup.s` — minimal `__cleanup` (no buffered stdio yet).
- `libc/sys/*.s` — syscall stubs (write, read, open, close, creat, lseek,
  exit, sbrk, unlink, fstat), each assembled with `libc/include/sys.s`
  (the syscall-number definitions) prepended -- exactly as 2.8BSD built them
  (`as -o x.o /usr/include/sys.s x.s`).

## Assembler features these sources required

The hand-written assembly exercises syntax `cc` output never used, which the
C-reimplemented `as` had to grow (see docs/as.md): multi-file input (the
prepended `sys.s`), numeric local labels (`1:`/`1f`/`1b`), the `..`
relocation-base placeholder, and the no-operand instructions (`setd`, ...).

## Archive ordering (no ranlib yet)

This `ld` scans a plain `.a` single-pass, so `libc.a` lists members in
dependency order (a referencer before its definer): the leaf helpers
`cleanup.o`/`cerror.o`/`csv.o` come last.  A `ranlib`/`__.SYMDEF` table would
remove this ordering requirement -- a natural next step.

## Build

`make libc` assembles the objects and builds `usr/lib/{crt0.o,libc.a}`.
