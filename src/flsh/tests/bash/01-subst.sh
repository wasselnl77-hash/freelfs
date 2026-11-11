x=hello_world_hello
echo "${x/hello/HI}"
echo "${x//hello/HI}"
echo "${x/#hello/HI}"
echo "${x/%hello/HI}"
echo "${x/#world/HI}"
echo "${x/%world/HI}"
echo "${x/o/0}"
echo "${x//o/0}"
echo "${x//l/}"
echo "${x/l}"
echo "${x//[aeiou]/_}"
echo "${x/h*o/X}"
echo "${x//h*o/X}"
echo "${x/?ello/Y}"
echo "${x//_/ }"
echo "${x/nomatch/Z}"
echo "${x//}"
y='a/b/c'
echo "${y//\//-}"
echo "${y/\//-}"
echo "${y//\//\\}"
p='l'
echo "${x//$p/L}"
echo "${x//"$p"/L}"
q='*'
a='a*b*c'
echo "${a//"$q"/-}"
echo "${a/"$q"/-}"
r=XX
echo "${x/hello/$r}"
echo "${x//hello/<$r>}"
set -- foo bar
echo "${1/o/0}" "${2//a/A}"
unset u
echo "[${u/a/b}]"
e=
echo "[${e/a/b}]"
echo "[${e/#/pre}]"
echo "[${x/#/pre-}]"
echo "[${x/%/-post}]"
s='a b c'
echo "${s// /_}"
echo "${s/ /}"
t='aaa'
echo "${t/a/b}" "${t//a/b}" "${t/#a/b}" "${t/%a/b}"
echo "${t//aa/X}"
w='path/to/file'
echo "${w/to/TO}"
echo "${w//\//:}"
c='a.b.c'
echo "${c//./,}"
echo "${c/./,}"
d='x[1]'
echo "${d/\[1\]/(1)}"
m='a  b'
echo "${m// /}"
n='tab	sep'
echo "${n/	/<TAB>}"
