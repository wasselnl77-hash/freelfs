x=hello
echo ${#x}
unset y
echo ${#y}
y=
echo ${#y}
f=/usr/local/lib/libfoo.so.1.2
echo "${f%.*}"
echo "${f%%.*}"
echo "${f#*/}"
echo "${f##*/}"
echo "${f%/*}"
echo "${f%%/*}"
echo "${f#/usr}"
echo "${f##*.}"
echo "${f%.so*}"
echo "${f%%.so*}"
echo "${f#*local}"
echo "${f#nomatch}"
echo "${f%nomatch}"
echo "${f%?}"
echo "${f%??}"
echo "${f#?}"
echo "${f%[0-9]}"
echo "${f%%[0-9]*}"
echo "${f##*[!0-9]}"
echo "${f%.[0-9]*}"
echo "${f%%.[0-9]*}"
# quoted patterns are literal
p='*'
a='a*b*c'
echo "${a%$p}"
echo "${a%"$p"}"
echo "${a%%"*"*}"
echo "${a#"a*"}"
echo "${a##*"*"}"
q='b*c'
echo "${a%"$q"}"
echo "${a%$q}"
echo ${a%"*"c}
echo ${a#a'*'}
# pattern from variable
suffix='.c'
n=prog.c
echo "${n%$suffix}"
echo "${n%"$suffix"}"
echo "${n#}"
echo "${n%%}"
# positional
set -- abc def
echo "${1#a}" "${2%f}"
echo ${#1} ${#2} ${#}
set -- a b c d e f g h i j k
echo ${#10}
echo "${#}"
# unset with trim
unset u
echo "[${u#x}][${u%x}]"
long=aXbXcXd
echo "${long#*X}" "${long##*X}" "${long%X*}" "${long%%X*}"
echo "${long#a?b}"
echo "${long%c?d}"
# trim with brace-containing / quoted-in-word
w='{x}'
echo "${w#\{}"
echo "${w%\}}"
