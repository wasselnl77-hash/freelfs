set -- a b c d e
echo $# $1 $5
shift
echo $# $1
shift 2
echo $# $1
shift 0
echo $# $1
set -- 1 2 3
( shift 5 ) 2>/dev/null; echo "shift too far st nonzero: $([ $? -ne 0 ] && echo y)"
echo "$# still"
set --
echo "$#"
set -- a "b c" ""
echo $#
set -- "$@" d
echo $# "$4"
set -- x "$@"
echo "$1 $2"
set -- $(echo p q r)
echo $# $2
set -- --
echo $# 
set -- -- a
echo $# $1
set -- -x
echo "$# $1"
set -- a b c
shift $#
echo "$#"
set -- 1 2 3 4 5 6 7 8 9 10 11 12
echo $10 ${10} ${11} ${12}
shift 9
echo $1 $2 $3
set -- a b c
while [ $# -gt 0 ]; do echo "arg=$1"; shift; done
echo "end $#"
set -- "x y" z
for a in "$@"; do echo "[$a]"; done
set -- a b
x=$1; shift; y=$1
echo "$x $y"
set a b c
echo "$#"
set -f
set +f
echo "$#"
f() { echo "in f: $#"; shift; echo "after shift: $#"; }
set -- 1 2 3
f x y
echo "outer: $#"
set -- a b c
echo "$@" "$*"
set -- ""
echo "$#"
set -- "" ""
echo "$#"
echo "[$1][$2]"
IFS=,
set -- a,b
echo "$#"
set -- $1
echo "$#"
unset IFS
n=2
set -- a b c d
shift $n
echo "$*"
set -- 0
echo $0 >/dev/null
set -- a b c
last=
for last; do :; done
echo "last=$last"
set -- a b c
eval "echo \${$#}"
set -- one
shift
shift 0
echo "ok $#"
