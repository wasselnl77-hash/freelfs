echo $'a\nb'
echo $'a\tb'
echo $'\x41\x42'
echo $'\101\102'
echo $'\x41'$'\x42'
echo $'it\'s'
echo $'back\\slash'
echo $'q\"uote'
echo $'plain'
echo $'a b  c'
x=$'l1\nl2'
echo "$x"
printf '%s' $'a\tb' | od -An -c | tr -s ' '
printf '%s' $'\x41\101A' | od -An -c | tr -s ' '
printf '%s\n' $'one\ttwo' $'three\nfour'
echo $'' end
echo "$'not ansi'"
echo '$'"'"'not ansi'"'"
echo $'\0101' | od -An -c | tr -s ' '
y=$'x\ty'
echo "${y}" | od -An -c | tr -s ' '
echo ${#y}
echo $'\x7e'
echo $'\176'
echo $'\60\61'
echo $'a\\nb'
echo $'\x4a\x4B'
set -- $'a b' c
echo $#
for w in $'x\ty' z; do printf '[%s]' "$w"; done; echo
case $'\n' in $'\n') echo "newline matches";; esac
IFS=$'\n'
set -- $(printf 'a b\nc d\n')
echo $#
unset IFS
echo $'tab\there' | tr '\t' '|'
echo -n $'no newline'; echo
