/ Minimal stubs for the floating-point print helpers doprnt references.
/ %e/%f/%g are not supported by this minimal libc; these are never called
/ unless such a conversion is used.  They just satisfy the linker.
/ fltused: the symbol c1 emits `.globl fltused' for, to pull in float startup
/ (crt0 does the `setd').  Defining it here satisfies the linker.
.globl	fltused
.globl	pfloat
.globl	pscien
.globl	pgen
fltused:
pfloat:
pscien:
pgen:
	rts	pc
