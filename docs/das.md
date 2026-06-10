# das (disassembler)

`das/das.c` is the inverse of this toolchain's `as`: a disassembler for the
three 2.8BSD PDP-11 binary formats.

```sh
pdp11-bsd28-das  file        # write <stem>.<object>.dis listings (one per object)
pdp11-bsd28-das -p file      # write a single combined listing to stdout
```

## What it does

- **Object (`.o`)** — disassembles the one object to a single listing.
- **Linked a.out** — *splits it back into per-object listings*.  When `ld`
  links objects it leaves an `N_FN` (037) file-name symbol in the symbol table
  at the text address where each input object's contribution begins; `das`
  uses those to partition the text and writes one `<stem>.<object>.dis` per
  input object (e.g. `prog.crt0.o.dis`, `prog.main.o.dis`, …).  The data and
  bss segments cannot be attributed to objects from the a.out alone, so they
  go to a single `<stem>.DATA.dis`.
- **Archive (`.a`)** — disassembles each object member to its own
  `<stem>.<member>.dis`.

## Symbol labelling

Available debugging symbols label the output:

- text symbols (functions, local `L`-labels) become `name:` labels and resolve
  branch/`jsr`/`jmp` targets,
- data and bss symbols label the data dump and resolve absolute references,
- `pc`/`sp` are printed for r7/r6.

For a *bare* `.o`, references to external symbols (e.g. `jsr pc,*$_helper`)
appear unrelocated, since the relocation has not been applied; in a linked
a.out everything resolves.

## Formats (read explicitly, never by struct overlay, so it's LP64-clean)

- a.out/object header: 8 words (16 bytes) — magic, text, data, bss, syms,
  entry, unused, flag.  Symbol table follows text+data (plus the relocation
  area when `a_flag==0`, i.e. an unstripped `.o`).
- `nlist`: 12 bytes — `n_name[8]` (inline, no string table), `n_type`,
  `n_ovly`, `n_value`.  Types: N_TEXT=02, N_DATA=03, N_BSS=04, N_FN=037,
  +N_EXT=040.
- archive: the binary `ar` format, `ARMAG` 0177545, 26-byte member headers.

## Decoder

A direct decode of the PDP-11 instruction word(s) to mnemonic + operands —
double/single-operand, branches, `jsr`/`rts`/`sob`/`mark`, the EIS
(`mul`/`div`/`ash`/`ashc`/`xor`), the FP11 floating-point set, `sys`/`emt`,
condition-code ops, and all eight addressing modes including immediate
(`$x`), absolute (`*$x`), and PC-relative (resolved to a symbol).  Verified
instruction-for-instruction against the GNU binutils `pdp11-aout` objdump.
