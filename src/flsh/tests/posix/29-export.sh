X=notexported
env | grep '^X=' ; echo "st=$?"
export X
env | grep '^X='
export Y=direct
env | grep '^Y='
Y=changed
env | grep '^Y='
export Z
env | grep '^Z=' ; echo "Z unset st=$?"
Z=now
env | grep '^Z='
unset Z
env | grep '^Z=' ; echo "after unset st=$?"
W=pre env | grep '^W='
echo "[${W-unset}]"
W=a V=b env | grep '^[WV]=' | sort
echo "[${W-unset}][${V-unset}]"
export A=1 B=2
env | grep '^[AB]=' | sort
export S='with space'
env | grep '^S='
export E=
env | grep '^E='
f() { export FV=infn; }
f
env | grep '^FV='
( export SUBV=1; env | grep '^SUBV=' )
env | grep '^SUBV=' ; echo "leak st=$?"
export -p >/dev/null; echo "export -p st=$?"
M=1
export M
M=2
env | grep '^M='
export N=1
N=2
env | grep '^N='
LC_FOO=bar env | grep '^LC_FOO='
P=1
( P=2; env | grep '^P=' )
export P
( P=3; env | grep '^P=' )
echo "$P"
x=1 y=2
echo "$x $y"
FOO=bar sh -c 'echo "child:$FOO"'
FOO=bar; sh -c 'echo "child2:[${FOO-}]"'
export FOO; sh -c 'echo "child3:$FOO"'
PATH_BAK=$PATH
PATH=/usr/bin:/bin
env | grep -c '^PATH=' | tr -d ' '
export NEWLINE='a
b'
env | grep -A1 '^NEWLINE=' | tr '\n' '|'; echo
export Q1="quo'te"
env | grep '^Q1='
sh -c 'echo "$Q1"'
unset Q1
sh -c 'echo "[${Q1-gone}]"'
export T1=x
T1=y sh -c 'echo "$T1"'
echo "$T1"
EV=1 true
echo "[${EV-unset}]"
