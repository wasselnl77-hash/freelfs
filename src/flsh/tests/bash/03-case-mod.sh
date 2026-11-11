x=hello
echo "${x^}"
echo "${x^^}"
y=WORLD
echo "${y,}"
echo "${y,,}"
z='mIxEd CaSe'
echo "${z^}"
echo "${z^^}"
echo "${z,}"
echo "${z,,}"
e=
echo "[${e^}][${e^^}][${e,}][${e,,}]"
unset u
echo "[${u^}][${u,,}]"
n='123abc'
echo "${n^}" "${n^^}"
s='already UPPER'
echo "${s,,}"
echo "${s^^}"
set -- foo bar
echo "${1^}" "${2^^}"
w='two words'
echo "${w^}" "${w^^}"
echo "${x^^}${x,,}"
t=abc
u2=${t^^}
echo "$u2"
v=${u2,,}
echo "$v"
echo "${t^^}-${t^}-${u2,}-${u2,,}"
sp=' lead'
echo "[${sp^}]"
d='9lives'
echo "${d^}"
