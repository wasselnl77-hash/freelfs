unset x
echo "1:${x-y}"
echo "2:${x:-y}"
x=
echo "3:${x-y}"
echo "4:${x:-y}"
x=val
echo "5:${x-y}"
echo "6:${x:-y}"
unset x
echo "7:${x=assigned}"
echo "7b:$x"
unset x
echo "8:${x:=assigned2}"
echo "8b:$x"
x=
echo "9:${x=nope}"
echo "9b:[$x]"
echo "10:${x:=yes}"
echo "10b:[$x]"
unset x
echo "11:[${x+y}]"
echo "12:[${x:+y}]"
x=
echo "13:[${x+y}]"
echo "14:[${x:+y}]"
x=a
echo "15:[${x+y}]"
echo "16:[${x:+y}]"
echo "17:${x:+"quoted  $x"}"
unset x y
echo "18:${x-$y}"
y=Y
echo "19:${x-$y}"
echo "20:${x:-${y:-z}}"
unset y
echo "21:${x:-${y:-z}}"
echo "22:${x:-a b}"
set -- ${x:-a   b}
echo $#
set -- "${x:-a   b}"
echo $#
echo "23:${1:-none}"
set --
echo "24:${1:-none}"
echo "26:${x:-"a'b"}"
echo "27:${x:-'q'}"
unset x
: ${x:=init}
echo "28:$x"
