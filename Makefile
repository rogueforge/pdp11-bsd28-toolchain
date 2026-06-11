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

# Target (PDP-11) system headers, installed to usr/include where cpp's
# getincdir() finds them (resolved relative to the cpp binary, the same
# scheme cc/ld use) -- mirrors the vax project's `headers' target.  The full
# authentic 2.8BSD header set lives in include/ (and include/sys/); install it
# all so the libc sources find <ctype.h>, <errno.h>, <signal.h>, <sys/...>,
# etc. instead of leaking to the host's /usr/include.
headers: dirs
	mkdir -p ${INC}/sys
	for f in include/*.h;     do cp -f $$f ${INC}/;     done
	for f in include/sys/*.h; do cp -f $$f ${INC}/sys/; done
	chmod -R u+w ${INC}

# Implemented so far.  More are appended as passes are ported.
# (ld is a larger port -- see NOTES.md -- and is added once the assembler
#  exists so it can be verified end to end.)
tools: dirs binutils cpp-tool c0-tool c1-tool c2-tool as-tool ld-tool ar-tool ranlib-tool cc-tool headers

# ---------------------------------------------------------------------
# Binary utilities (single .c file each)
# ---------------------------------------------------------------------

binutils:
	${HOSTCC} ${O} ${COMPAT} -Icross -o ${BIN}/${PREFIX}-nm    nm/nm.c
	${HOSTCC} ${O} ${COMPAT} -Icross -o ${BIN}/${PREFIX}-size  size/size.c
	${HOSTCC} ${O} ${COMPAT} -Icross -o ${BIN}/${PREFIX}-strip strip/strip.c
	${HOSTCC} ${O} ${COMPAT} -Icross -o ${BIN}/${PREFIX}-das   das/das.c
	${HOSTCC} ${O} ${COMPAT} -Icross -DPREFIX='"${PREFIX}"' -o ${BIN}/${PREFIX}-dcc dcc/dcc.c

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
# c2 -- peephole optimizer (operates on c1's assembly).  `cc -O' runs it and
# the libc is built with -O (above), matching 2.8's `compall'.  Verified
# byte-identical to the linked rogue3.4 binary across the C library (strlen,
# fread/fwrite, strcmp, ... -- 33 objects to the byte).  See docs/c2.md.
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
# libc + crt0 -- the authentic 2.8BSD C library: program startup
# (crt0/csv/cerror), the long-arithmetic helpers c1 calls (lmul/ldiv/...),
# the gen/ string-memory-numeric-util routines, buffered stdio (printf plus
# fopen/fgets/fputs/getc/putc/ungetc/fseek/...), and the sys/ syscall stubs.
# C sources go through our cc; .s through our as (syscall stubs get the
# syscall-number header prepended).  ranlib builds the __.SYMDEF table so the
# archive members can be in any order.  crt0.o is installed separately (cc
# links it explicitly).  Floating-point printing, the varargs sprintf/scanf
# family, the passwd/group database, networking, and the overlay/profiling
# build variants are out of scope (see NOTES.md).
# ---------------------------------------------------------------------
ASM  = ${BIN}/${PREFIX}-as
CC   = ${BIN}/${PREFIX}-cc
LSYS = libc/include/sys.s

# gen/ C sources (string, memory, numeric, char-class, small utilities)
LIBC_GENC = strcmp strlen strcpy strcat strncmp strncpy strncat index rindex \
	    atoi atol abs ctype_ getenv mktemp perror errlst qsort swab isatty \
	    isapipe stty sleep malloc calloc atof ecvt
# stdio/ C sources (the full buffered-I/O layer minus the scanf/db family)
LIBC_STDIOC = printf fprintf data strout flsbuf fputc fopen freopen fdopen \
	      fgets fputs fgetc getchar putchar puts gets ungetc fseek ftell rew \
	      setbuf clrerr filbuf findiop endopen rdwr getw putw sprintf gcvt scanf doscan
# sys/ syscall stubs (assembled with the sys.s number defs)
LIBC_SYS = write read open close creat lseek exit sbrk unlink fstat stat lstat \
	   dup pipe fork wait getpid getuid getgid setuid setgid access chmod \
	   chown chdir link kill ioctl umask time stime sync alarm pause nice \
	   times mknod chroot execv execl
# crt/ long-arithmetic + profiler-counter helpers (c1 emits calls to these)
LIBC_ARITH = lmul ldiv lrem almul aldiv alrem mcount

# Full object list for the archive (with directory prefixes).
LIBC_OBJS = $(addprefix libc/gen/,$(addsuffix .o,${LIBC_GENC})) \
	    libc/gen/cuexit.o libc/gen/setjmp.o libc/gen/abort.o libc/gen/modf.o libc/gen/ldexp.o libc/gen/frexp.o \
	    $(addprefix libc/stdio/,$(addsuffix .o,${LIBC_STDIOC})) \
	    libc/stdio/doprnt.o libc/stdio/fltpr.o \
	    $(addprefix libc/sys/,$(addsuffix .o,${LIBC_SYS})) \
	    libc/crt/csv.o libc/crt/cerror.o \
	    $(addprefix libc/crt/,$(addsuffix .o,${LIBC_ARITH}))

libc: as-tool ar-tool ranlib-tool cc-tool headers dirs
	${ASM} -o ${LIB}/crt0.o ${LSYS} libc/csu/crt0.s
	# C sources (cc refuses `-o foo.o', so compile in-place via a subshell).
	# -O runs c2, as the authentic 2.8 libc `compall' does (`cc -c -O' on every
	# file); this is what makes the library byte-identical to 2.8's.
	cd libc/gen   && ${CURDIR}/${CC} -O -c $(addsuffix .c,${LIBC_GENC})
	cd libc/stdio && ${CURDIR}/${CC} -O -c $(addsuffix .c,${LIBC_STDIOC})
	# hand-written asm in gen/ and stdio/ (cuexit does `sys exit')
	${ASM} -o libc/gen/cuexit.o   ${LSYS} libc/gen/cuexit.s
	${ASM} -o libc/gen/setjmp.o            libc/gen/setjmp.s
	${ASM} -o libc/gen/abort.o             libc/gen/abort.s
	${ASM} -o libc/gen/modf.o              libc/gen/modf.s
	${ASM} -o libc/gen/ldexp.o             libc/gen/ldexp.s
	${ASM} -o libc/gen/frexp.o             libc/gen/frexp.s
	${ASM} -o libc/stdio/doprnt.o          libc/stdio/doprnt.s
	# fltpr: the authentic float-print hooks (pfloat/pscien/pgen) doprnt calls
	# for %f/%e/%g; it also defines `fltused' (replacing the old fltstub).
	${ASM} -o libc/stdio/fltpr.o           libc/stdio/fltpr.s
	# syscall stubs
	for f in ${LIBC_SYS}; do ${ASM} -o libc/sys/$$f.o ${LSYS} libc/sys/$$f.s; done
	# crt: csv + cerror + long arithmetic
	${ASM} -o libc/crt/csv.o               libc/crt/csv.s
	${ASM} -o libc/crt/cerror.o   ${LSYS} libc/crt/cerror.s
	for f in ${LIBC_ARITH}; do ${ASM} -o libc/crt/$$f.o libc/crt/$$f.s; done
	# archive + table of contents
	rm -f ${LIB}/libc.a
	${BIN}/${PREFIX}-ar rc ${LIB}/libc.a ${LIBC_OBJS}
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
