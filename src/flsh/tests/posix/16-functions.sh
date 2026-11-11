f() { echo "f: $# $1 $2"; }
f a b c
f
set -- x y
f p
echo "outer: $1 $2 $#"
g() { set -- changed; echo "g: $1"; }
g
echo "after g: $1 $2"
r() { return 3; }
r; echo "r=$?"
r2() { return; }
false; r2; echo "r2=$?"
r3() { false; return; }
r3; echo "r3=$?"
r4() { true; }
r4; echo "r4=$?"
r5() { (exit 9); }
r5; echo "r5=$?"
fact() { if [ $1 -le 1 ]; then echo 1; else echo $(( $1 * $(fact $(($1-1))) )); fi; }
fact 5
fib() { if [ $1 -lt 2 ]; then echo $1; else echo $(( $(fib $(($1-1))) + $(fib $(($1-2))) )); fi; }
fib 10
countdown() { [ $1 -le 0 ] && return 0; echo $1; countdown $(($1-1)); }
countdown 3
h() { echo first; }
h
h() { echo second; }
h
h() { echo "third $1"; }
h x
unset -f h
command -v h >/dev/null || echo "h gone"
v=global
setv() { v=changed; }
setv
echo $v
nested() { inner() { echo "inner defined by nested"; }; inner; }
nested
inner
args() { for a in "$@"; do printf '[%s]' "$a"; done; echo; }
args "a b" "" c
args
wrap() { args "$@"; }
wrap "x y" z
wrap2() { args "$*"; }
wrap2 "x y" z
shifter() { shift; echo "$# $*"; }
shifter a b c
echo $#
retv() { echo out; return 2; }
x=$(retv); echo "x=$x st=$?"
out=$(retv; echo "after:$?")
echo "$out"
cond() { return 1; }
if cond; then echo yes; else echo no; fi
cond || echo "or"
! cond && echo "negated"
fn_with_redir() { echo hello; } >/dev/null
fn_with_redir; echo "st=$?"
fn_subshell() ( x=in-subshell; echo $x )
x=outer
fn_subshell
echo $x
fn_compound() if true; then echo "if-body"; fi
fn_compound
noargs() { echo "$#"; }
noargs ""
noargs "" ""
noargs $nonexistent
e() { echo "e: $0" >/dev/null; echo done; }
e
fn1() { fn2; }
fn2() { echo "fn2 called from fn1"; }
fn1
rec() { [ "$1" -gt 0 ] || return 0; printf '%s' "$1"; rec $(($1-1)); }
rec 5; echo
fl() { local_like=1; }
fl; echo "$local_like"
ret_in_loop() { for i in 1 2 3; do [ $i = 2 ] && return 7; done; }
ret_in_loop; echo "st=$?"
exit_status_pass() { false; }
exit_status_pass; echo "$?"
posargs_restore() { set -- 1 2 3; echo "$#"; }
set -- a b
posargs_restore
echo "$# $*"
