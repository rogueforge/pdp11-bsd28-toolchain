#!/bin/sh
#
# Regression test runner for the 2.8BSD PDP-11 cross-toolchain.
#
# Usage:
#   tests/run.sh            run all tests, print a summary, exit nonzero on
#                           any failure
#   tests/run.sh -u         update (regenerate) golden .expected files
#   tests/run.sh -v         verbose: show diffs for failures
#   tests/run.sh <pattern>  only run tests whose name matches <pattern>
#
# Test kinds:
#   tests/cpp/*.c        golden tests: preprocess the .c (with -Itests/cpp/
#                        include) and compare to the matching .expected file.
#                        cpp line markers are normalised to basenames so the
#                        goldens are path-independent.
#   tests/binutils/*.sh  shell tests: each sources tests/lib.sh and uses the
#                        check_* helpers; exit status is the verdict.
#   tests/<pass>/*.sh    same convention for c0/c1/as/ld as they land.
#

ROOT=`cd \`dirname "$0"\`/.. && pwd`
BIN="$ROOT/usr/bin/pdp11-bsd28"
FIX="$ROOT/tests/fixtures"
export ROOT BIN FIX

UPDATE=0
VERBOSE=0
PATTERN=

for a in "$@"; do
	case "$a" in
	-u) UPDATE=1 ;;
	-v) VERBOSE=1 ;;
	*)  PATTERN="$a" ;;
	esac
done

PASS=0
FAIL=0
FAILED_NAMES=

# normalise cpp output: rewrite line markers `# 12 "/abs/path/foo.h"` to
# `# 12 "foo.h"` so golden files do not depend on the checkout location.
norm_cpp() {
	sed -E 's,^(# [0-9]+ ")[^"]*/([^"/]+"),\1\2,'
}

match() {
	[ -z "$PATTERN" ] && return 0
	case "$1" in *"$PATTERN"*) return 0 ;; *) return 1 ;; esac
}

report_pass() { PASS=`expr $PASS + 1`; echo "PASS $1"; }
report_fail() {
	FAIL=`expr $FAIL + 1`
	FAILED_NAMES="$FAILED_NAMES $1"
	echo "FAIL $1"
	if [ $VERBOSE -eq 1 ] && [ -n "$2" ]; then
		echo "$2" | sed 's/^/     | /'
	fi
}

# ---- cpp golden tests -------------------------------------------------
run_cpp_tests() {
	for src in "$ROOT"/tests/cpp/*.c; do
		[ -e "$src" ] || continue
		base=`basename "$src" .c`
		name="cpp/$base"
		match "$name" || continue
		exp="${src%.c}.expected"
		got=`"$BIN-cpp" -Dpdp11=1 -I"$ROOT/tests/cpp/include" "$src" 2>&1 | norm_cpp`
		if [ $UPDATE -eq 1 ]; then
			printf '%s\n' "$got" > "$exp"
			echo "UPDATED $name"
			continue
		fi
		if [ ! -f "$exp" ]; then
			report_fail "$name" "no .expected file (run with -u to create)"
		elif [ "$got" = "`cat \"$exp\"`" ]; then
			report_pass "$name"
		else
			report_fail "$name" "`printf '%s\n' \"$got\" | diff \"$exp\" - `"
		fi
	done
}

# ---- shell tests (binutils, and future passes) ------------------------
run_sh_tests() {
	for dir in binutils c0 c1 c2 cc as ld; do
		for t in "$ROOT"/tests/$dir/*.sh; do
			[ -e "$t" ] || continue
			name="$dir/`basename \"$t\" .sh`"
			match "$name" || continue
			out=`sh "$t" 2>&1`
			if [ $? -eq 0 ]; then
				report_pass "$name"
			else
				report_fail "$name" "$out"
			fi
		done
	done
}

run_cpp_tests
run_sh_tests

echo "------------------------------------------------------------"
if [ $UPDATE -eq 1 ]; then
	echo "goldens updated."
	exit 0
fi
echo "passed: $PASS   failed: $FAIL"
[ $FAIL -eq 0 ] || { echo "failing:$FAILED_NAMES"; exit 1; }
exit 0
