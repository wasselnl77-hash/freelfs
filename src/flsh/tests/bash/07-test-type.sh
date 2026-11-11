[ a == a ] && echo eq1
[ a == b ] && echo eq2 || echo ne2
[ "a b" == "a b" ] && echo eq3
v=x
[ "$v" == x ] && echo eq4
[ "$v" == y ] || echo ne5
[ -z "" -a a == a ] && echo and6
test a == a && echo eq7
[ 1 -eq 1 ] && [ a == a ] && echo both
[ "" == "" ] && echo empty-eq
[ a != b ] && echo ne
[ ! a == b ] && echo neg
[ a == a -o b == c ] && echo or
type -t cd
type -t echo
type -t test
type -t ls
type -t if
type -t nonexistent_zzz; echo "st=$?"
f() { :; }
type -t f
type -t '['
type -t true
type -t pwd
type -t while
type -t '{'
type -t read
type -t eval
type -t export
type -t unset
type -t set
type -t shift
type -t exit
type -t return
type -t break
type -t trap
type -t wait
type -t umask
type -t getopts
g() { :; }
type -t g
unset -f g
type -t g; echo "st=$?"
echo() { :; }
type -t echo
unset -f echo
type -t echo
type -t /bin/ls; echo "st=$?"
