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

all: tools libc sim

dirs:
	mkdir -p ${BIN} ${LIB} ${INC}

# Implemented so far.  More are appended as passes are ported.
# (ld is a larger port -- see NOTES.md -- and is added once the assembler
#  exists so it can be verified end to end.)
tools: dirs binutils cpp-tool c0-tool c1-tool c2-tool as-tool ld-tool ar-tool ranlib-tool cc-tool

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

# ---------------------------------------------------------------------
# c0 -- compiler pass 1 (parser / front end).  Emits intermediate code
# read by c1.  -fms-extensions for the tnode anonymous-union superset.
# ---------------------------------------------------------------------

C0FLAGS = ${O} ${COMPAT} -fms-extensions -Icross -Ic0

C0_OBJS = c0/c00.o c0/c01.o c0/c02.o c0/c03.o c0/c04.o c0/c05.o

c0-tool: ${BIN}/${PREFIX}-c0

${BIN}/${PREFIX}-c0: ${C0_OBJS}
	${HOSTCC} ${O} -o $@ ${C0_OBJS}

c0/c00.o: c0/c00.c c0/c0.h; ${HOSTCC} ${C0FLAGS} -c -o $@ c0/c00.c
c0/c01.o: c0/c01.c c0/c0.h; ${HOSTCC} ${C0FLAGS} -c -o $@ c0/c01.c
c0/c02.o: c0/c02.c c0/c0.h; ${HOSTCC} ${C0FLAGS} -c -o $@ c0/c02.c
c0/c03.o: c0/c03.c c0/c0.h; ${HOSTCC} ${C0FLAGS} -c -o $@ c0/c03.c
c0/c04.o: c0/c04.c c0/c0.h; ${HOSTCC} ${C0FLAGS} -c -o $@ c0/c04.c
c0/c05.o: c0/c05.c c0/c0.h; ${HOSTCC} ${C0FLAGS} -c -o $@ c0/c05.c

# ---------------------------------------------------------------------
# c1 -- compiler pass 2 (PDP-11 code generator).  c10-c13 + the codegen
# table.  The table is authored in table.s (a template language); cvopt
# expands it to assembler, and mktab (host helper) converts that to C so
# it can be compiled and linked in instead of assembled for the PDP-11.
#
#   table.s --cvopt--> c1/table.i --mktab--> c1/table.c --cc--> table.o
# ---------------------------------------------------------------------

C1FLAGS = ${O} ${COMPAT} -fms-extensions -Icross -Ic1

C1_OBJS = c1/c10.o c1/c11.o c1/c12.o c1/c13.o c1/table.o

c1-tool: ${BIN}/${PREFIX}-c1

${BIN}/${PREFIX}-c1: ${C1_OBJS}
	${HOSTCC} ${O} -o $@ ${C1_OBJS}

c1/c10.o: c1/c10.c c1/c1.h; ${HOSTCC} ${C1FLAGS} -c -o $@ c1/c10.c
c1/c11.o: c1/c11.c c1/c1.h; ${HOSTCC} ${C1FLAGS} -c -o $@ c1/c11.c
c1/c12.o: c1/c12.c c1/c1.h; ${HOSTCC} ${C1FLAGS} -c -o $@ c1/c12.c
c1/c13.o: c1/c13.c c1/c1.h; ${HOSTCC} ${C1FLAGS} -c -o $@ c1/c13.c
c1/table.o: c1/table.c c1/c1.h; ${HOSTCC} ${C1FLAGS} -c -o $@ c1/table.c

c1/table.c: c1/table.i c1/mktab
	c1/mktab c1/table.i c1/table.c

c1/table.i: c1/table.s c1/cvopt
	c1/cvopt c1/table.s c1/table.i

c1/cvopt: c1/cvopt.c; ${HOSTCC} ${O} ${COMPAT} -o $@ c1/cvopt.c
c1/mktab: c1/mktab.c; ${HOSTCC} ${O} -o $@ c1/mktab.c

# ---------------------------------------------------------------------
# c2 -- peephole optimizer (operates on c1's assembly).  Builds and runs,
# but its optimizer is NOT yet reliable: it over-optimizes the standard
# function prologue (see docs/c2.md / NOTES.md).  cc does NOT invoke it by
# default; -O is experimental.
# ---------------------------------------------------------------------

C2FLAGS = ${O} ${COMPAT} -Ic2

c2-tool: ${BIN}/${PREFIX}-c2

${BIN}/${PREFIX}-c2: c2/c20.o c2/c21.o
	${HOSTCC} ${O} -o $@ c2/c20.o c2/c21.o

c2/c20.o: c2/c20.c c2/c2.h; ${HOSTCC} ${C2FLAGS} -c -o $@ c2/c20.c
c2/c21.o: c2/c21.c c2/c2.h; ${HOSTCC} ${C2FLAGS} -c -o $@ c2/c21.c

# ---------------------------------------------------------------------
# cc -- compiler driver.  Resolves the passes relative to its own install
# location and runs cpp -> c0 -> c1 (-> c2 only with -O, experimental) ->
# as -> ld.  -S stops after c1 and writes assembly; -c/-o need as/ld (not
# yet ported), so those report the missing tool.
# ---------------------------------------------------------------------

# ---------------------------------------------------------------------
# as -- PDP-11 assembler, reimplemented in C (the 2BSD as is pure PDP-11
# assembly).  Accepts 2BSD syntax, emits classic 2.8BSD a.out.  The opcode
# table (as/optab.h) is the authentic table from as19.s.
# ---------------------------------------------------------------------

as-tool: ${BIN}/${PREFIX}-as

${BIN}/${PREFIX}-as: as/as.c as/optab.h
	${HOSTCC} ${O} ${COMPAT} -Ias -o $@ as/as.c

# ---------------------------------------------------------------------
# ld -- link editor.  The authentic 2.8BSD ld.c, ported for LP64: its
# whole I/O model assumes sizeof(int)==2, so on-disk word buffers and the
# header/symbol/archive structs are pinned to 16-bit widths (uint16_t).
# Links classic 2.8BSD a.out objects (what as emits).  Uses openlp for
# -l libraries, so it links the libucbpath objects like cpp.
# ---------------------------------------------------------------------

ld-tool: ${BIN}/${PREFIX}-ld

${BIN}/${PREFIX}-ld: ld/ld.c ${UCB_OBJS}
	${HOSTCC} ${O} ${COMPAT} -Icross -o $@ ld/ld.c ${UCB_OBJS}

# ---------------------------------------------------------------------
# ar -- archiver.  The authentic 2.8BSD ar.c, ported for LP64: the on-disk
# member header is a 26-byte record (V7 ar_hdr) and the magic is a 16-bit
# word -- both assume sizeof(int)==2, so the header struct is pinned to
# fixed widths and packed (cross/ar.h layout) and fstat() is read through
# the host struct stat.  Produces plain (no __.SYMDEF) archives that ld
# scans member-by-member to resolve -l libraries.
# ---------------------------------------------------------------------

ar-tool: ${BIN}/${PREFIX}-ar

${BIN}/${PREFIX}-ar: ar/ar.c
	${HOSTCC} ${O} ${COMPAT} -Icross -o $@ ar/ar.c

# ranlib -- adds a __.SYMDEF table of contents so ld resolves archive members
# regardless of order (the canonical V7/2.9 ranlib, paired with the canonical ar).
ranlib-tool: ${BIN}/${PREFIX}-ranlib

${BIN}/${PREFIX}-ranlib: ranlib/ranlib.c
	${HOSTCC} ${O} ${COMPAT} -Icross -o $@ ranlib/ranlib.c

# ---------------------------------------------------------------------
# cc -- compiler driver.
# ---------------------------------------------------------------------

cc-tool: ${BIN}/${PREFIX}-cc

${BIN}/${PREFIX}-cc: cc/cc.c
	${HOSTCC} ${O} ${COMPAT} -Icross -o $@ cc/cc.c

# ---------------------------------------------------------------------
# libc + crt0 -- the authentic 2.8BSD startup (crt0/csv) and C-library
# syscall stubs, assembled by our as (with the syscall-number header
# prepended) and archived by our ar into usr/lib/libc.a.  crt0.o is
# installed separately (cc links it explicitly).  Members are listed in
# dependency order (referencer before definer) because this ld scans a
# plain archive single-pass; a ranlib/__.SYMDEF table would remove that
# ordering requirement.
# ---------------------------------------------------------------------
ASM  = ${BIN}/${PREFIX}-as
LSYS = libc/include/sys.s

libc: as-tool ar-tool ranlib-tool dirs
	${ASM} -o ${LIB}/crt0.o ${LSYS} libc/csu/crt0.s
	${ASM} -o libc/csv.o      libc/crt/csv.s
	${ASM} -o libc/cleanup.o  libc/gen/cleanup.s
	${ASM} -o libc/cerror.o   ${LSYS} libc/crt/cerror.s
	${ASM} -o libc/cuexit.o   ${LSYS} libc/gen/cuexit.s
	${ASM} -o libc/write.o    ${LSYS} libc/sys/write.s
	${ASM} -o libc/read.o     ${LSYS} libc/sys/read.s
	${ASM} -o libc/open.o     ${LSYS} libc/sys/open.s
	${ASM} -o libc/close.o    ${LSYS} libc/sys/close.s
	${ASM} -o libc/creat.o    ${LSYS} libc/sys/creat.s
	${ASM} -o libc/lseek.o    ${LSYS} libc/sys/lseek.s
	${ASM} -o libc/exit.o     ${LSYS} libc/sys/exit.s
	${ASM} -o libc/sbrk.o     ${LSYS} libc/sys/sbrk.s
	${ASM} -o libc/unlink.o   ${LSYS} libc/sys/unlink.s
	${ASM} -o libc/fstat.o    ${LSYS} libc/sys/fstat.s
	rm -f ${LIB}/libc.a
	${BIN}/${PREFIX}-ar rc ${LIB}/libc.a \
		libc/cuexit.o libc/write.o libc/read.o libc/open.o \
		libc/close.o libc/creat.o libc/lseek.o libc/exit.o \
		libc/sbrk.o libc/unlink.o libc/fstat.o libc/csv.o \
		libc/cleanup.o libc/cerror.o
	${BIN}/${PREFIX}-ranlib ${LIB}/libc.a

# ---------------------------------------------------------------------
# apsim -- host-side user-mode PDP-11 simulator that runs the produced
# a.out (loads text/data, emulates the 2BSD sys traps), to verify end to
# end.  Not part of the produced toolchain; a verification aid.
# ---------------------------------------------------------------------
sim: dirs sim/apsim.c
	${HOSTCC} ${O} -o ${BIN}/${PREFIX}-apsim sim/apsim.c

# =====================================================================
# Tests
# =====================================================================

test: tools libc sim
	sh tests/run.sh

# regenerate golden .expected files after an intentional behaviour change
test-update: tools
	sh tests/run.sh -u

# =====================================================================
# Housekeeping
# =====================================================================

clean:
	rm -f as/*.o c0/*.o c1/*.o c2/*.o cpp/*.o cpp/cpy.c libucbpath/*.o
	rm -f c1/table.i c1/table.c c1/cvopt c1/mktab libc/*.o libc/*/*.o

distclean: clean
	rm -f ${BIN}/${PREFIX}-nm ${BIN}/${PREFIX}-size \
	      ${BIN}/${PREFIX}-strip ${BIN}/${PREFIX}-cpp ${BIN}/${PREFIX}-c0 \
	      ${BIN}/${PREFIX}-c1 ${BIN}/${PREFIX}-c2 ${BIN}/${PREFIX}-as \
	      ${BIN}/${PREFIX}-ld ${BIN}/${PREFIX}-ar ${BIN}/${PREFIX}-cc \
	      ${BIN}/${PREFIX}-ar ${BIN}/${PREFIX}-ranlib \
	      ${BIN}/${PREFIX}-apsim ${LIB}/crt0.o ${LIB}/libc.a
