root=$(mktemp -d) || exit 1
cd "$root" || exit 1
mkdir d
x=outer
( x=inner; echo "in: $x" )
echo "out: $x"
( cd d; echo "in: ${PWD#$root}" )
echo "out: ${PWD#$root}|"
( unset x; echo "in: [${x-unset}]" )
echo "out: $x"
( y=new; export y; echo "in: $y" )
echo "out: [${y-unset}]"
( exit 3 ); echo "st=$?"
( false ); echo "st=$?"
( echo a; echo b ) | tr a-z A-Z
( echo sub; exit 2 ) ; echo "st=$?"
z=$( x=cs; echo $x ); echo "$z $x"
( ( x=deep; echo $x ); echo $x )
( set -- a b c; echo $# )
set -- 1 2
( shift; echo "in: $#" )
echo "out: $#"
( f() { echo "fn in subshell"; }; f )
command -v f >/dev/null && echo leaked || echo "fn not leaked"
( umask 077; umask )
( trap 'echo sub-exit-trap' EXIT; echo body )
( exit 0 ) && echo and-ok
( cd /; pwd )
echo "${PWD#$root}|"
{ x=brace; }
echo "$x"
{ cd d; }
echo "${PWD#$root}"
cd "$root"
x=1
( x=2 ) &
wait
echo "bg x=$x"
(
  a=1
  b=2
  echo $((a+b))
)
echo "[${a-unset}]"
( read v <<EOF
heredoc-in-sub
EOF
echo "$v" )
echo "[${v-unset}]"
( alias_test() { :; } ; echo ok )
( set -e; false; echo not-reached ); echo "st=$?"
( exit 300 ) ; echo "st=$?"
( echo -n >/dev/null; echo "echo-n ok" )
cd /
rm -rf "$root"
