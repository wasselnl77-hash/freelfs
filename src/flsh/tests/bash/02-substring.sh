x=abcdefghij
echo "${x:0}"
echo "${x:3}"
echo "${x:3:4}"
echo "${x:0:1}"
echo "${x:9}"
echo "${x:10}"
echo "[${x:11}]"
echo "${x:2:0}"
echo "${x:2:100}"
echo "${x: -2}"
echo "${x: -3:2}"
echo "${x: -1}"
echo "${x: -10}"
echo "${x:(-4)}"
echo "${x:(-4):2}"
echo "${x:1:-1}" 2>/dev/null
n=3
echo "${x:n}"
echo "${x:$n}"
echo "${x:n:2}"
echo "${x:$n:$((n+1))}"
echo "${x:n+1}"
echo "${x:0:n}"
l=2
echo "${x:n:l}"
echo "${x: -n}"
echo "${x:$((n-1)):1}"
set -- hello world
echo "${1:1:3}" "${2: -3}"
e=
echo "[${e:0}][${e:1}]"
unset u
echo "[${u:0}][${u:0:2}]"
y="with space here"
echo "${y:5:5}"
echo "${y:5}"
z=0123456789
echo "${z:5:1}" "${z:0:5}" "${z: -5:5}"
echo "${#z}"
echo "${z:${#z}-3}"
echo "${z:$((${#z}-3)):2}"
echo "${z:2:3}${z:7:1}"
q='*?'
echo "${q:1}" "${q:0:1}"
i=0
while [ $i -lt 3 ]; do echo "${x:$i:1}"; i=$((i+1)); done
