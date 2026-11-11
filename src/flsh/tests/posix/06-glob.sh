root=$(mktemp -d) || exit 1
cd "$root" || exit 1
mkdir sub sub/deep .hid
touch b.txt a.txt c.dat .dot .hid/x sub/s1.txt sub/s2.c sub/deep/d.txt 'sp ace.txt' 'a*star'
echo *
echo .*  | tr ' ' '\n' | grep -v '^\.\.\?$'
echo *.txt
echo [ab]*
echo [!a]*
echo [a-b].txt
echo ?.txt
echo ??.*
echo sub/*
echo sub/*/*
echo */
echo */*.txt
echo */d*
echo ./*.dat
echo nomatch*
echo "*.txt"
echo '*.txt'
echo \*.txt
echo "a"*
echo a"*"
echo a\*
echo .h*/*
echo .hid/*
echo sub/.*  | tr ' ' '\n' | grep -v '/\.\.\?$'
echo "$root"/*.dat | sed "s|$root|ROOT|"
set -f
echo *.txt
echo [ab]*
set +f
echo *.dat
x='*.dat'
echo $x
echo "$x"
set -f
echo $x
set +f
for f in *.txt; do echo "f=$f"; done
for f in nomatch*; do echo "f=$f"; done
p='sp ace*'
echo $p
echo "sp ace"*
case 'a.txt' in *.txt) echo matches ;; esac
echo [[]*
echo [*
echo a.t[x]t
echo a.t[]x]t
echo [a-]*
touch -- 'a]b' ./-d
echo [a]]*
echo -- [-]*
cd /
rm -rf "$root"
