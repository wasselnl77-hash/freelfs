root=$(mktemp -d) || exit 1
cd "$root" || exit 1
echo one > f
echo two >> f
cat f
cat < f
echo repl > f
cat f
echo noclobber-test > g
set -C
echo x > g 2>/dev/null; echo "noclobber status nonzero: $([ $? -ne 0 ] && echo y)"
echo y >| g; cat g
set +C
errf() { echo out; echo err >&2; }
errf > o 2>&1; cat o
errf 2>&1 >o2 | sed 's/^/pipe:/'; cat o2
errf >o3 2>/dev/null; cat o3
errf 2>e3 >/dev/null; cat e3
{ echo a; echo b; } > grp; cat grp
{ echo c; echo d >&2; } > grp2 2>&1; cat grp2
( echo sub; echo sub2 ) > sf; cat sf
if true; then echo inif; fi > iff; cat iff
for i in 1 2; do echo for$i; done > forf; cat forf
while false; do :; done > /dev/null
exec 3> fd3
echo to3 >&3
echo to3b >&3
exec 3>&-
cat fd3
exec 4< f
read l <&4; echo "read: $l"
read l <&4; echo "read: $l"
exec 4<&-
echo hello > rw
exec 5<> rw
read l <&5; echo "rw read: $l"
exec 5>&-
echo abcdef > rw2
exec 6<>rw2
echo XY >&6
exec 6>&-
cat rw2
printf 'l1\nl2\nl3\n' > lines
while read l; do echo "L:$l"; done < lines
n=0
while read l; do n=$((n+1)); done < lines
echo "count=$n"
while read a b; do echo "$b-$a"; done <<EOF
x y
p q
EOF
cat lines | while read l; do echo "P:$l"; done
cmd=$(cat < lines | tr '\n' ,); echo "$cmd"
echo data > "sp ace"; cat < "sp ace"
x=fn; echo c > $x; cat $x
echo z > nested; { { cat nested; } | tr z Z; }
echo to-dev-null > /dev/null; echo "st=$?"
cat < /nonexistent/file 2>/dev/null; echo "nz=$([ $? -ne 0 ] && echo y)"
echo hi > /nonexistent/dir/f 2>/dev/null; echo "nz=$([ $? -ne 0 ] && echo y)"
echo a > A; echo b > B
cat A B > AB; cat AB
cat < A < B
echo 1>x1; cat x1
echo 2 2>x2; ls x2
echo "x" >&1
echo err 2>&1 >&2 | cat
{ echo 1; echo 2; } | { read a; read b; echo "$b$a"; }
: > empty; wc -c < empty | tr -d ' '
> created; ls created
>> appended; ls appended
echo xx > ord1 > ord2; cat ord2; wc -c < ord1 | tr -d ' '
exec 7>&1
echo via7 >&7
exec 7>&-
f() { echo in-func; } > func.out
f; cat func.out
g() { cat; } < A
g
cd /
rm -rf "$root"
