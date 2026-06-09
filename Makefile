#
# Top-level Makefile for the 2.8BSD Ritchie-cc PDP-11 cross-toolchain
#
# Uses only features available in BSD make: no GNU pattern rules (%),
# no $(shell), no $(wildcard), no $(patsubst).
#
# Status: incremental.  Implemented targets are listed under `tools`.
# Compiler passes (c0/c1/c2), cpp, the assembler, and libc are added as
# they are ported -- see NOTES.md for the roadmap.
#

# Configuration -- override DESTDIR on the command line if needed
DESTDIR	= .
PREFIX	= pdp11-bsd28
BIN	= ${DESTDIR}/usr/bin
LIB	= ${DESTDIR}/usr/lib
INC	= ${DESTDIR}/usr/include

# Host compiler flags for building K&R C with modern GCC
HOSTCC	= cc
YACC	= yacc -Wno-yacc -Wno-other -Wno-conflicts-sr
O	= -O
COMPAT	= -std=gnu89 -Wno-int-conversion -Wno-incompatible-pointer-types \
	  -Wno-unused-result -Wno-int-to-pointer-cast -Wno-pointer-to-int-cast \
	  -Wno-format -Wno-endif-labels -Wno-builtin-declaration-mismatch \
	  -Wno-return-type -Wno-implicit-int -Wno-implicit-function-declaration \
	  -fno-strict-aliasing -fwrapv -fcommon

# =====================================================================
# Toolchain
# =====================================================================

all: tools

dirs:
	mkdir -p ${BIN} ${LIB} ${INC}

# Implemented so far.  More are appended as passes are ported.
tools: dirs binutils

# ---------------------------------------------------------------------
# Binary utilities (single .c file each)
# ---------------------------------------------------------------------

binutils:
	${HOSTCC} ${O} ${COMPAT} -Icross -o ${BIN}/${PREFIX}-nm    nm/nm.c
	${HOSTCC} ${O} ${COMPAT} -Icross -o ${BIN}/${PREFIX}-size  size/size.c
	${HOSTCC} ${O} ${COMPAT} -Icross -o ${BIN}/${PREFIX}-strip strip/strip.c
	${HOSTCC} ${O} ${COMPAT} -Icross -o ${BIN}/${PREFIX}-ld    ld/ld.c

# =====================================================================
# Housekeeping
# =====================================================================

clean:
	rm -f as/*.o c0/*.o c1/*.o c2/*.o cpp/*.o

distclean: clean
	rm -f ${BIN}/${PREFIX}-nm ${BIN}/${PREFIX}-size \
	      ${BIN}/${PREFIX}-strip ${BIN}/${PREFIX}-ld
