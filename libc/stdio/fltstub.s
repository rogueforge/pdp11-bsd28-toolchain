/ Minimal stubs for the floating-point print helpers doprnt references.
/ %e/%f/%g are not supported by this minimal libc; these are never called
/ unless such a conversion is used.  They just satisfy the linker.
.globl	pfloat
.globl	pscien
.globl	pgen
pfloat:
pscien:
pgen:
	rts	pc
