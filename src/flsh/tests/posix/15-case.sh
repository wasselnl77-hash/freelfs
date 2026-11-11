t() { case $1 in
  a|b) echo "$1:a-or-b";;
  c*) echo "$1:c-star";;
  ?x) echo "$1:?x";;
  [0-9]) echo "$1:digit";;
  [A-Z]*) echo "$1:upper";;
  [!a-z]) echo "$1:single-nonlower";;
  "q*") echo "$1:literal-q-star";;
  *.txt) echo "$1:txt";;
  '') echo "empty";;
  *) echo "$1:default";;
esac; }
for w in a b c cat cx ax 5 Zed 'q*' qxx f.txt '' '!' z zz '?x'; do t "$w"; done
case foo in foo) echo exact;; esac
case foo in bar) echo no;; *) echo fallback;; esac
case foo in bar) echo no;; esac
echo "nomatch-status: $?"
case foo in foo) false;; esac
echo "body-status: $?"
x='a b'
case $x in 'a b') echo space-quoted;; esac
case "$x" in a\ b) echo space-escaped;; esac
case $x in a*) echo glob-a;; esac
p='a*'
case abc in $p) echo var-pattern-glob;; esac
case 'a*' in "$p") echo var-quoted-literal;; esac
case abc in "$p") echo no;; *) echo quoted-no-glob;; esac
case a in a) echo one ;; a) echo two ;; esac
case a in
  b)
    echo b
    ;;
  a)
    echo multi-line-a
    ;;
esac
case a in (a) echo paren-form;; esac
case a in (b|a) echo paren-or;; esac
case x in x) echo last-no-dsemi
esac
case ab in a?) echo q2;; esac
case ab in ?) echo no;; *) echo not-single;; esac
case '' in '') echo empty-match;; esac
case '' in *) echo empty-star;; esac
case '-' in -) echo dash;; esac
case 'a.b' in a.b) echo dot;; esac
case 'a.b' in a?b) echo dot-q;; esac
case '[' in '[') echo bracket;; esac
case 'x]' in x]) echo rbracket;; esac
case b in [abc]) echo cls;; esac
case b in [!abc]) echo no;; *) echo notcls;; esac
case Q in [[:upper:]]) echo posix-upper;; esac
case 5 in [[:digit:]]) echo posix-digit;; esac
case '/' in */*) echo slash;; esac
case '.hid' in *) echo star-matches-dot;; esac
case '/a/b' in /a/*) echo path-star;; esac
case 'a/b' in a?b) echo slash-by-qm;; esac
case $(echo cmd) in cmd) echo cmdsubst-word;; esac
case "$(echo 'x y')" in 'x y') echo cs-spaces;; esac
for i in 1 2 3; do case $i in 2) continue;; esac; echo i$i; done
case a in a) case b in b) echo nested-case;; esac;; esac
case $unset_var in '') echo unset-empty;; esac
case ${unset_var-dflt} in dflt) echo default-word;; esac
