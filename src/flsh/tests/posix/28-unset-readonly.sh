x=1
unset x
echo "[${x-unset}]"
x=
unset x
echo "[${x-unset}]"
unset nonexistent_var; echo "st=$?"
a=1 b=2
unset a b
echo "[${a-u}][${b-u}]"
f() { echo f; }
unset -f f
command -v f >/dev/null 2>&1 || echo "f gone"
f() { echo f1; }
g() { echo g1; }
unset -f f g
command -v f g >/dev/null 2>&1 || echo "both gone"
unset -f nonexistent_fn; echo "unset -f missing st=$?"
v=1
f() { echo "fn"; }
unset v
echo "fn still: $(f)"
v=var
unset -v v
echo "[${v-unset}]"
v=1
f() { echo fn-and-var; }
readonly r=5
echo "$r"
( r=6 ) 2>/dev/null; echo "assign ro in subshell nonzero: $([ $? -ne 0 ] && echo y)"
( unset r ) 2>/dev/null; echo "unset ro nonzero: $([ $? -ne 0 ] && echo y)"
echo "$r"
( readonly q=1; q=2; echo reached ) 2>/dev/null; echo "st nonzero: $([ $? -ne 0 ] && echo y)"
y=10
readonly y
echo "$y"
( y=11 ) 2>/dev/null; echo "st nonzero: $([ $? -ne 0 ] && echo y)"
readonly -p >/dev/null; echo "readonly -p st=$?"
( readonly z=1; export z; echo "$z" )
readonly lst
( readonly w=7; for w in 1; do :; done ) 2>/dev/null; echo "for ro nonzero: $([ $? -ne 0 ] && echo y)"
( readonly w=7; read w <<EOF
x
EOF
) 2>/dev/null; echo "read ro nonzero: $([ $? -ne 0 ] && echo y)"
( readonly w=7; eval 'w=8' ) 2>/dev/null; echo "eval ro nonzero: $([ $? -ne 0 ] && echo y)"
unset -v nonexistent2; echo "st=$?"
p1=1; p2=2
unset p1 p2; echo "[${p1-}${p2-}]"
set -- a b
echo "$#"
PS_test=1; unset PS_test; echo "[${PS_test-gone}]"
n=1
unset n
n=2
echo "$n"
IFS=:; unset IFS
set -- "a b"
x=$1; echo ${x}
