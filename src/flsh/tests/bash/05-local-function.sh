x=global
f() { local x=local; echo "in f: $x"; }
f
echo "after f: $x"
g() { local x; echo "in g: [${x-unset}]"; x=set-in-g; echo "in g: $x"; }
g
echo "after g: $x"
h() { local x=h-local; k; echo "after k in h: $x"; }
k() { echo "in k sees: $x"; x=changed-by-k; }
h
echo "after h: $x"
unset y
m() { local y=1; echo "m: $y"; }
m
echo "after m: [${y-unset}]"
n() { local a=1 b=2; echo "$a $b"; }
n
echo "[${a-unset}][${b-unset}]"
o() { local x=outer-o; p; echo "o: $x"; }
p() { local x=inner-p; echo "p: $x"; }
o
rec() { local d=$1; if [ "$d" -gt 0 ]; then rec $(($d-1)); fi; echo "d=$d"; }
rec 3
q() { local s='a b'; echo "$s"; }
q
r() { local v=$1; shift; echo "$v|$*"; }
r a b c
function fn1 { echo "function keyword"; }
fn1
function fn2 { echo "args: $#"; }
fn2 a b
function fn3() { echo "function with parens"; }
fn3
function fn4
{
  echo "brace on next line"
}
fn4
function fn5 { local z=1; echo "z=$z"; return 4; }
fn5; echo "st=$?"
fn6() { local x=1; (local x=2; echo "sub $x"); echo "$x"; }
fn6
cnt=0
inc() { local c=$cnt; c=$((c+1)); cnt=$c; }
inc; inc; inc
echo "cnt=$cnt"
lv() { local IFS=:; set -- $1; echo $#; }
lv a:b:c
echo "$(printf '%s' "$IFS" | od -An -c | tr -s ' ')"
vv() { local v; v=$(echo hi); echo "$v"; }
vv
e() { local e1=$(echo cs); echo "$e1"; }
e
ff() { local arr=x; arr=$arr$arr; echo "$arr"; }
ff
echo "[${arr-unset}]"
w=outer
sh1() { local w=inner; ( echo "sub sees $w" ); }
sh1
echo "$w"
st() { local v=$(exit 3); echo "local st=$?"; }
st
