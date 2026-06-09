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
# (ld is a larger port -- see NOTES.md -- and is added once the assembler
#  exists so it can be verified end to end.)
tools: dirs binutils cpp-tool

# ---------------------------------------------------------------------
# Binary utilities (single .c file each)
# ---------------------------------------------------------------------

binutils:
	${HOSTCC} ${O} ${COMPAT} -Icross -o ${BIN}/${PREFIX}-nm    nm/nm.c
	${HOSTCC} ${O} ${COMPAT} -Icross -o ${BIN}/${PREFIX}-size  size/size.c
	${HOSTCC} ${O} ${COMPAT} -Icross -o ${BIN}/${PREFIX}-strip strip/strip.c

# ---------------------------------------------------------------------
# libucbpath -- UCB library/include path search (openl, openlp) used by
# cpp (and later ld) to find headers and -lx libraries relative to $PATH.
# ---------------------------------------------------------------------

UCB_OBJS = libucbpath/concat.o libucbpath/openl.o \
	   libucbpath/openlp.o libucbpath/ucbpath.o

libucbpath/concat.o:  libucbpath/concat.c
	${HOSTCC} ${O} ${COMPAT} -c -o $@ libucbpath/concat.c
libucbpath/openl.o:   libucbpath/openl.c
	${HOSTCC} ${O} ${COMPAT} -c -o $@ libucbpath/openl.c
libucbpath/openlp.o:  libucbpath/openlp.c
	${HOSTCC} ${O} ${COMPAT} -c -o $@ libucbpath/openlp.c
libucbpath/ucbpath.o: libucbpath/ucbpath.c
	${HOSTCC} ${O} ${COMPAT} -c -o $@ libucbpath/ucbpath.c

# ---------------------------------------------------------------------
# cpp (C preprocessor) -- cpp.c + cpy.y (#if expression grammar) + yylex.c
# Both units need -Dpdp11=1 so the COFF=128 signed-char tables agree.
# ---------------------------------------------------------------------

CPPFLAGS_CPP = ${O} ${COMPAT} -Dunix=1 -Dpdp11=1

cpp-tool: ${BIN}/${PREFIX}-cpp

${BIN}/${PREFIX}-cpp: cpp/cpp.o cpp/cpy.o ${UCB_OBJS}
	${HOSTCC} ${O} -o $@ cpp/cpp.o cpp/cpy.o ${UCB_OBJS}

cpp/cpp.o: cpp/cpp.c
	${HOSTCC} ${CPPFLAGS_CPP} -c -o cpp/cpp.o cpp/cpp.c

cpp/cpy.o: cpp/cpy.c cpp/yylex.c
	${HOSTCC} ${CPPFLAGS_CPP} -c -o cpp/cpy.o cpp/cpy.c

cpp/cpy.c: cpp/cpy.y
	cd cpp; ${YACC} cpy.y; mv y.tab.c cpy.c

# =====================================================================
# Tests
# =====================================================================

test: tools
	sh tests/run.sh

# regenerate golden .expected files after an intentional behaviour change
test-update: tools
	sh tests/run.sh -u

# =====================================================================
# Housekeeping
# =====================================================================

clean:
	rm -f as/*.o c0/*.o c1/*.o c2/*.o cpp/*.o cpp/cpy.c libucbpath/*.o

distclean: clean
	rm -f ${BIN}/${PREFIX}-nm ${BIN}/${PREFIX}-size \
	      ${BIN}/${PREFIX}-strip ${BIN}/${PREFIX}-cpp
