alias hi='echo hello'
hi
hi world
alias ll='echo long'
ll -a
alias | sed "s/^alias //"
alias | grep -c . | tr -d ' ' >/dev/null
alias hi | sed "s/^alias //"
alias hi ll | sed "s/^alias //"
unalias hi
hi 2>/dev/null; echo "after unalias st nonzero: $([ $? -ne 0 ] && echo y)"
alias two='echo a; echo b'
two
alias e='echo'
e   spaced  args
alias trailing='echo '
alias nxt='NEXT'
trailing nxt
alias pipeme='echo piped |'
pipeme tr a-z A-Z
alias q="echo 'quoted alias'"
q
alias dq='echo "dq  alias"'
dq
alias greeting='echo hello,'
greeting world
alias lsx='printf "%s\n"'
lsx a b
f() { hi2; }
alias hi2='echo late-alias'
f 2>/dev/null; echo "late alias in earlier-defined fn nonzero: $([ $? -ne 0 ] && echo y)"
alias hi3='echo hi3'
g() { hi3; }
g
unalias hi3
g
alias x1=true
alias x2='x1 && echo chained'
x2
unalias -a
alias | wc -l | tr -d ' '
alias a1='echo one'
(alias a1 | sed "s/^alias //")
(unalias a1; alias a1 2>/dev/null; echo "sub st nonzero: $([ $? -ne 0 ] && echo y)")
a1
alias redir='echo redirected >'
d=$(mktemp -d)
redir "$d/f"
cat "$d/f"
rm -rf "$d"
alias ifx='if true; then'
ifx echo ok; fi
alias same='same2'
alias same2='echo resolved-chain'
same
alias rec='rec x'
rec 2>/dev/null; echo "recursive alias st nonzero: $([ $? -ne 0 ] && echo y)"
alias semi='echo semi;'
semi echo after-semi
alias nl='echo nl'
nl; nl
alias em=
em echo through-empty-alias
