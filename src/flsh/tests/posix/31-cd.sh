root=$(cd "$(mktemp -d)" && pwd -P) || exit 1
cd "$root" || exit 1
mkdir -p a/b/c d "sp ace"
rel() { echo "${PWD#$root}|"; }
rel
cd a; rel
cd b; rel
cd c; rel
cd ..; rel
cd ../..; rel
cd ./a/b; rel
cd "$root"; rel
cd a
cd "$root/d"; rel
cd -  >/dev/null; rel
cd - | sed "s|$root|ROOT|"; rel
cd - | sed "s|$root|ROOT|"; rel
echo "OLDPWD=${OLDPWD#$root}"
cd "$root"
cd "sp ace"; rel
cd ..
cd a/b/c
cd ../../../d; rel
cd "$root"
HOME="$root/d"
cd; rel
cd "$root"
cd ~; rel
cd "$root"
cd /; echo "${PWD}"
cd "$root"
cd /nonexistent_dir_zz 2>/dev/null; echo "st nonzero: $([ $? -ne 0 ] && echo y)"
rel
cd "" 2>/dev/null; rel
cd a; echo "PWD ok: $([ "$PWD" = "$root/a" ] && echo y)"
echo "pwd ok: $([ "$(pwd)" = "$root/a" ] && echo y)"
cd "$root"
( cd a; rel )
rel
mkdir -p "$root/link-target"
ln -s "$root/link-target" "$root/lnk"
cd lnk; rel
echo "pwd -L: $(pwd -L | sed "s|$root||")"
echo "pwd -P: $(pwd -P | sed "s|$root||")"
cd ..; rel
cd -P lnk; rel
cd ..; rel
cd "$root"
cd a/b; cd ../c 2>/dev/null; echo "st nonzero: $([ $? -ne 0 ] && echo y)"; rel
cd "$root"
f() { cd d; }
f; rel
cd "$root"
cd a >/dev/null; echo "OLDPWD ok: $([ "$OLDPWD" = "$root" ] && echo y)"
cd "$root"
cd . ; rel
cd ./; rel
cd a/; rel
cd "$root"
CDPATH="$root/a"
cd b >/dev/null; rel
unset CDPATH
cd "$root"
touch file
cd file 2>/dev/null; echo "cd to file nonzero: $([ $? -ne 0 ] && echo y)"
cd "$root"
mkdir noperm; chmod 000 noperm
cd noperm 2>/dev/null; echo "noperm: $([ $? -ne 0 ] && [ "$(id -u)" != 0 ] && echo denied || echo allowed)"
chmod 755 noperm
cd "$root"
pwd -P | sed "s|$root|ROOT|"
cd /
rm -rf "$root"
