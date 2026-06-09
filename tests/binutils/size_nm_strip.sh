# size/nm/strip against a committed real 2.8BSD PDP-11 object (dkleave.o).
. "$ROOT/tests/lib.sh"

obj="$FIX/dkleave.o"

# --- size: exact segment sizes (cross-checked against GNU pdp11-aout-size)
line=`"$BIN-size" "$obj" | tail -1`
set -- $line
check_eq "size text" 128 "$1"
check_eq "size data" "+" "$2"      # layout: "128 +\t0 +\t0 =..."
check_eq "size data val" 0 "$3"
check_eq "size bss val" 0 "$5"

# --- nm: decodes the 2.8 symbol table (16 entries) and emits 6-digit octal
n=`"$BIN-nm" "$obj" | wc -l | tr -d ' '`
check_eq "nm symbol count" 16 "$n"
check_contains "nm format" "`"$BIN-nm" "$obj" | head -1`" " "

# --- strip: drops the symbol table and sets the no-relocation flag
tmp=`mktemp`
cp "$obj" "$tmp"
"$BIN-strip" "$tmp" || fail "strip exited nonzero"
after=`"$BIN-nm" "$tmp" 2>&1`
check_contains "strip removed names" "$after" "no name list"
rm -f "$tmp"

exit 0
