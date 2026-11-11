show() { printf '%s:' "$#"; for a in "$@"; do printf '<%s>' "$a"; done; echo; }
x=a:b::c
IFS=:
show $x
x=:a:b:
show $x
x=::a
show $x
x=a::
show $x
x=a:
show $x
x=:
show $x
x=::
show $x
x=
show $x
echo "--- whitespace IFS"
IFS=' 	
'
x='  a  b   c  '
show $x
echo "--- mixed"
IFS=' :'
x=' a : b  c :: d '
show $x
x='a : b'
show $x
x=' : '
show $x
x='a  :  b'
show $x
echo "--- IFS empty"
IFS=''
x='a b:c'
show $x
echo "--- unset IFS"
unset IFS
x='  a   b  '
show $x
IFS=,
echo "--- quoting"
x='a,b'
show "$x"
show $x
show a,b
show "a,b"
y=c,d
show $x$y
show "$x"$y
show ${x}x
echo "--- literal delimiters aren't split"
show a,b,c
IFS=:
show "a:b" a:b
echo "--- cmd subst"
IFS=:
show $(echo a:b:c)
show $(echo 'a b:c')
IFS=' '
show $(echo 'a b:c')
IFS=x
show $(echo 'axbxc')
echo "--- set --"
IFS=,
set -- a,b,c
show $1
show "$1"
echo "--- assignment not split"
IFS=:
v=a:b:c
set -- $v
echo $#
echo "$1-$2-$3"
unset IFS
echo "--- IFS non-ws single"
IFS=,
x=,
show $x
x=a,,b
show $x
x=,,
show $x
unset IFS
echo "--- unquoted empty expansions"
e=
show $e
show "$e"
show $e$e
show x$e
show $e x
