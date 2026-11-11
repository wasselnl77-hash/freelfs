root=$(mktemp -d) || exit 1
cd "$root" || exit 1
t() { if "$@"; then echo "T $*"; else echo "F $*"; fi; }
touch file; mkdir dir; : > empty; echo data > nonempty
ln -s file symlink
touch exe_f; chmod 755 exe_f
t test a = a
t test a = b
t test a != b
t test a != a
t test -z ""
t test -z "x"
t test -n "x"
t test -n ""
t test x
t test ""
t test
t test 1 -eq 1
t test 1 -eq 2
t test 1 -ne 2
t test 1 -lt 2
t test 2 -lt 1
t test 2 -le 2
t test 3 -gt 2
t test 2 -ge 3
t test -5 -lt 3
t test 10 -gt 9
t test 007 -eq 7
t test " 5" -eq 5
t test -e file
t test -e nonexistent
t test -f file
t test -f dir
t test -d dir
t test -d file
t test -s empty
t test -s nonempty
t test -r file
t test -w file
t test -x dir
t test -x exe_f
t test -L symlink
t test -L file
t test -h symlink
t test -e symlink
t test -f symlink
t test -p file
t test -S file
t test -b file
t test -c /dev/null
t test -t 99
t test ! -e nonexistent
t test ! -e file
t test ! a = b
t test ! ""
t test ! x
t test ! !
t test -n -a -n
t test a = a -a b = b
t test a = a -a b = c
t test a = b -o b = b
t test a = b -o b = c
t test a = a -o b = c
t test ! a = b -a c = c
t test \( a = a \)
t test \( a = b \)
t test \( a = a -o b = c \) -a d = d
t test \( a = b -o b = b \) -a d = e
t test ! \( a = a \)
t test ! \( a = b -o c = d \)
t test -f file -a -r file
t test -d dir -a -w dir
t test -z "" -a -z ""
t test = =
t test -n = 
t test ! = !
t test -f = -f
t test -e -e
t test -z -z
t test -d -d
t test '(' = '('
t test a -eq 1 2>/dev/null; echo "bad int st nonzero: $([ $? -ge 2 ] && echo y)"
t test 1 -eq 2>/dev/null
echo "--- [ ] form"
t [ a = a ]
t [ a = b ]
t [ -f file ]
t [ -d dir ]
t [ 1 -lt 2 ]
t [ -z "" ]
t [ -n "" ]
t [ "" ]
t [ x ]
t [ ! -f file ]
t [ -f file -a -d dir ]
t [ -f nofile -o -d dir ]
t [ \( 1 -lt 2 \) -a \( 3 -lt 4 \) ]
t [ "a b" = "a b" ]
v="a b"
t [ "$v" = "a b" ]
t [ $v = "a b" ] 2>/dev/null
unset u
t [ -z "$u" ]
t [ -n "$u" ]
t [ "$u" = "" ]
t [ "x$u" = "x" ]
t [ ]
t [ ! ]
t [ -n ]
t [ -z ]
t [ ! -n ]
t [ "!" ]
t [ "(" ]
t [ -e ]
t [ a = ]
t [ = = = ]
t [ -n -n ]
[ 1 -eq 1 ]; echo "st=$?"
[ 1 -eq 2 ]; echo "st=$?"
[ 1 -eq x ] 2>/dev/null; echo "st>1: $([ $? -gt 1 ] && echo y)"
[ -f ] ; echo "st=$?"
[ 2>/dev/null; echo "missing ] st>1: $([ $? -gt 1 ] && echo y)"
cd /
rm -rf "$root"
