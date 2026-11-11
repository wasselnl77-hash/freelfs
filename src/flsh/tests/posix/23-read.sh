root=$(mktemp -d) || exit 1
echo "a b c d" | { read x y; echo "[$x][$y]"; }
echo "a b c d" | { read x y z; echo "[$x][$y][$z]"; }
echo "a b" | { read x y z; echo "[$x][$y][$z]"; }
echo "a b c d" | { read x; echo "[$x]"; }
echo "  lead and trail  " | { read x; echo "[$x]"; }
echo "  lead and trail  " | { read x y; echo "[$x][$y]"; }
echo "  a   b   c  " | { read x y; echo "[$x][$y]"; }
echo "a:b:c" | { IFS=: read x y z; echo "[$x][$y][$z]"; }
echo "a:b:c:d" | { IFS=: read x y; echo "[$x][$y]"; }
echo "a::c" | { IFS=: read x y z; echo "[$x][$y][$z]"; }
echo ":a:" | { IFS=: read x y z; echo "[$x][$y][$z]"; }
echo " a : b " | { IFS=' :' read x y; echo "[$x][$y]"; }
echo "a b c" | { IFS= read x; echo "[$x]"; }
echo "  a b  " | { IFS= read x; echo "[$x]"; }
echo 'back\slash' | { read x; echo "[$x]"; }
echo 'back\slash' | { read -r x; echo "[$x]"; }
printf 'line\\\ncontinued\n' | { read x; echo "[$x]"; }
printf 'line\\\ncontinued\n' | { read -r x; echo "[$x]"; }
printf 'a\\ b c\n' | { read x y; echo "[$x][$y]"; }
printf 'a\\ b c\n' | { read -r x y; echo "[$x][$y]"; }
printf 'a\\:b:c\n' | { IFS=: read x y; echo "[$x][$y]"; }
printf 'l1\nl2\n' | { read x; read y; echo "[$x][$y]"; }
printf 'l1\nl2\n' | { read x; echo "[$x]"; cat; }
printf 'noeol' | { read x; echo "st=$? [$x]"; }
printf '' | { read x; echo "st=$? [$x]"; }
printf '\n' | { read x; echo "st=$? [$x]"; }
printf 'a b\n' | { read x; echo "st=$? [$x]"; }
printf 'x y z\n' | { read a b c d; echo "[$a][$b][$c][$d]"; }
printf '*\n' | { read x; echo "$x"; }
printf '$HOME\n' | { read x; echo "$x"; }
printf '"quoted" '"'single'"'\n' | { read x; echo "$x"; }
printf 'a\tb\tc\n' | { read x y z; echo "[$x][$y][$z]"; }
printf 'a\tb\tc\n' | { IFS=: read x; echo "[$x]" | od -An -c | tr -s ' '; }
printf '1 2\n3 4\n' | while read a b; do echo "$b $a"; done
printf 'k=v\nk2=v2\n' | while IFS== read k v; do echo "$k -> $v"; done
printf 'a b\n\nc d\n' | while read a b; do echo "[$a][$b]"; done
printf 'x\n' > "$root/in"
read v < "$root/in"; echo "[$v]"
printf 'one\ntwo\nthree\n' > "$root/in3"
{ read a; read b; read c; echo "$a-$b-$c"; } < "$root/in3"
exec 3< "$root/in3"
read a <&3; read b <&3
echo "$a $b"
exec 3<&-
read a b <<EOF
from heredoc
EOF
echo "[$a][$b]"
read a <<EOF
 trimmed 
EOF
echo "[$a]"
echo "a b c" | { read -r first rest; echo "[$first][$rest]"; }
echo "a   b   c" | { read -r first rest; echo "[$first][$rest]"; }
echo "a b c   " | { read x y; echo "[$x][$y]"; }
echo 'a\' | { read x; echo "st=$? [$x]"; }
echo "x" | { read REPLY_TEST; echo "$REPLY_TEST"; }
echo "1,2;3" | { IFS=',;' read a b c; echo "$a$b$c"; }
echo ",," | { IFS=, read a b c; echo "[$a][$b][$c]"; }
echo "a,b,,c" | { IFS=, read a b c d; echo "[$a][$b][$c][$d]"; }
echo "a" | { read a b c; echo "[$a][$b][$c]"; }
rm -rf "$root"
