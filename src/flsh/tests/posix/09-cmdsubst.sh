echo $(echo hello)
echo `echo hello`
echo "$(echo a; echo b)"
echo $(echo a; echo b)
x=$(printf 'abc\n\n\n')
echo "[$x]"
x=$(printf '\n\nabc')
echo "[$x]"
x=$(printf 'a\n\nb\n')
echo "[$x]"
x=$(printf 'a b  \n')
echo "[$x]"
echo "[$(echo)]"
echo "[$(true)]"
echo $(echo $(echo $(echo deep)))
echo "$(echo "$(echo "$(echo nested)")")"
echo `echo \`echo inner\``
echo "`echo "dq in bq"`"
echo `echo 'sq in bq'`
echo `echo \$HOME | sed 's/.*/X/'`
echo "$(echo ')')"
echo "$(echo "(")"
echo "$(case x in x) echo case-paren;; esac)"
echo "$( (echo subshell) )"
echo $(echo a)$(echo b)
echo "$(echo a)$(echo b)"
x=$(false); echo "st=$?"
x=$(true); echo "st=$?"
x=$(exit 3); echo "st=$?"
x=$(echo hi; exit 4); echo "st=$? x=$x"
echo $(exit 5); echo "st=$?"
false; echo "$(echo $?)"
x=y$(false)z; echo "st=$? x=$x"
true; x=$(false) || echo "or-branch"
if x=$(exit 2); then echo yes; else echo "no $?"; fi
y=1
x=$(y=2; echo $y); echo "$x $y"
x=$(cd /; pwd); echo "$x"
x=$(echo 'a  b'); echo "$x"; echo $x
x=$(echo '*' ); echo "$x"
x="$(printf '%s' '  keep  ')"; echo "[$x]"
set -- $(echo a b c)
echo $#
set -- "$(echo a b c)"
echo $#
echo "$(printf 'a\tb')" | od -An -c | tr -s ' '
echo $(printf 'a\tb\n')
n=$(printf '%s\n' 1 2 3 | wc -l); echo $((n+0))
printf "%s\n" "$(printf "%s" "a\\b")"
echo "$(printf '%s' "\\")"
echo $(( $(echo 3) * 2 ))
f() { echo "in f: $1"; }
echo "$(f arg)"
v=$(f arg2; f arg3)
echo "$v"
echo "$(echo 'a'"b"'c')"
echo "a$(echo X)b"
echo '$(echo no)'
echo "$(
echo multi
echo line
)"
echo "$(cat <<EOT
heredoc in subst
EOT
)"
echo $(
true
)end
# comment with ) in subst
echo "$(echo a # comment
echo b)"
