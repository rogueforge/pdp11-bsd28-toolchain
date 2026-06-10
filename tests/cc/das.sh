# das: the PDP-11 disassembler.  Verify it (1) decodes a bare object with
# symbol labels, (2) splits a linked a.out back into per-object listings via
# the N_FN file symbols, and (3) disassembles each archive member to its own
# file.  das writes <stem>.<object>.dis files into the current directory, so
# the test runs inside its scratch dir.
. "$ROOT/tests/lib.sh"

tmp=`mktemp -d`
trap 'rm -rf "$tmp"' EXIT
cd "$tmp"

# --- 1. object disassembly + symbol labelling -----------------------------
cat > m.c <<'EOF'
int sq(n) int n; { return n * n; }
EOF
"$BIN-cc" -c m.c || fail "cc -c m.c failed"
dis=`"$BIN-das" -p m.o`
check_contains "object: function label" "$dis" "_sq:"
check_contains "object: mul decoded"    "$dis" "mul"
check_contains "object: csv prologue"   "$dis" "jsr"

# --- 2. a.out split into per-object listings ------------------------------
cat > p.c <<'EOF'
int helper() { return 7; }
int main() { return helper(); }
EOF
"$BIN-cc" p.c -o p || fail "cc p.c failed"
"$BIN-das" p >/dev/null 2>&1
test -f p.crt0.o.dis || fail "a.out: no per-object crt0.o.dis (split failed)"
grep -l '_main:'   p.*.dis >/dev/null 2>&1 || fail "a.out: _main not in any split file"
grep -l '_helper:' p.*.dis >/dev/null 2>&1 || fail "a.out: _helper not in any split file"

# --- 3. archive: one listing per member -----------------------------------
"$BIN-ar" cr lib.a m.o || fail "ar failed"
"$BIN-das" lib.a >/dev/null 2>&1
test -f lib.a.m.o.dis || fail "archive: member m.o not disassembled"
check_contains "archive member" "`cat lib.a.m.o.dis`" "_sq:"

exit 0
