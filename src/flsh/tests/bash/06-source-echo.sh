root=$(mktemp -d) || exit 1
cat > "$root/lib.sh" <<'EOF'
sourced_var=yes
sf() { echo "sf called with $#: $*"; }
echo "in lib: $#"
EOF
source "$root/lib.sh"
echo "$sourced_var"
sf a b
set -- o1 o2
source "$root/lib.sh" x y z
echo "$# $1"
cat > "$root/r.sh" <<'EOF'
return 6
EOF
source "$root/r.sh"; echo "st=$?"
( source "$root/nonexist.sh" ) 2>/dev/null; echo "missing nonzero: $([ $? -ne 0 ] && echo y)"
echo -n "no-newline"; echo
echo -n a b; echo
echo -n; echo "empty-n"
echo -e 'a\nb'
echo -e 'a\tb'
echo -e 'a\\b'
echo -e 'x\x41y'
echo -e '\0101'
echo -e 'one\ctwo'; echo
echo -E 'a\nb'
echo -e "q\"uote"
echo -en 'a\nb'; echo
echo -ne 'c\td'; echo
echo -n -e 'e\tf'; echo
echo -e -n 'g\th'; echo
echo -E -e 'i\tj'
echo -e -E 'k\tl'
echo -nE 'm\tn'; echo
echo -x
echo -z -n
echo -- -n
echo - x
echo -ee 'o\tp'
echo -e
echo -n -n x; echo
echo -n "$(echo sub)"; echo
echo -e '\\n'
echo -e 'trailing\\'
echo 'a\nb'
echo -e '\a' | od -An -c | tr -s ' '
echo -e 'bell\bback' | od -An -c | tr -s ' '
echo -e '\e[0m' | od -An -c | tr -s ' '
echo -e 'A' 2>/dev/null
echo -e 'a\vb' | od -An -c | tr -s ' '
echo -e '\rcr' | od -An -c | tr -s ' '
echo -e '\ffeed' | od -An -c | tr -s ' '
echo -e 'x\0y' | od -An -c | tr -s ' '
rm -rf "$root"
