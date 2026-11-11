#!/bin/sh
# flstore: the catalog indexer on a small appstream file. No network, no display, no root.
# usage: tests/index.sh PATH-TO-FLSTORE   (a sysroot build may be run through its ld.so: FLSTORE_RUN=...)
# SPDX-License-Identifier: MIT
bin=${1:-./flstore}
dir=$(mktemp -d)
trap 'rm -rf "$dir"' EXIT
fails=0
ok() { echo "ok   $1"; }
bad() { echo "FAIL $1"; fails=$((fails + 1)); }

cat > "$dir/a.xml" <<'EOF'
<?xml version="1.0" encoding="UTF-8"?>
<components version="0.8" origin="flatpak">
  <component type="desktop-application">
    <id>org.example.Paint</id>
    <name>Paint</name>
    <name xml:lang="de">Malen</name>
    <summary>Draw and edit pictures</summary>
    <summary xml:lang="de">Bilder malen</summary>
    <developer_name>Example Team</developer_name>
    <description>
      <p>Paint is a simple drawing program.</p>
      <p xml:lang="de">Ein Malprogramm.</p>
      <ul><li>Layers</li><li>Brushes</li></ul>
      <p>Made for &amp; by everybody.</p>
    </description>
    <categories><category>Graphics</category><category>2DGraphics</category></categories>
    <keywords><keyword>draw</keyword><keyword xml:lang="de">zeichnen</keyword></keywords>
    <project_license>GPL-3.0-or-later</project_license>
    <url type="homepage">https://example.org/paint</url>
    <screenshots>
      <screenshot type="default">
        <image type="source">https://example.org/big.png</image>
        <image type="thumbnail" width="624" height="351">https://example.org/624.png</image>
        <image type="thumbnail" width="752" height="423">https://example.org/752.png</image>
      </screenshot>
      <screenshot><image type="source">https://example.org/src2.png</image></screenshot>
    </screenshots>
    <releases><release timestamp="1700000000" version="1.0"/><release timestamp="1800000000" version="2.0"/></releases>
    <custom><value key="flathub::verification::verified">true</value></custom>
  </component>
  <component type="runtime"><id>org.example.Platform</id><name>Platform</name></component>
  <component type="addon"><id>org.example.Paint.Plugin</id><name>Plugin</name></component>
  <component type="desktop-application">
    <id>org.example.Game</id>
    <name>Game</name>
    <summary>Play</summary>
    <categories><category>ActionGame</category></categories>
  </component>
</components>
EOF
gzip -c "$dir/a.xml" > "$dir/a.xml.gz"
mkdir "$dir/out"
$FLSTORE_RUN "$bin" --build-index "$dir/a.xml.gz" "$dir/out" 2>"$dir/err" || { cat "$dir/err"; bad "indexer ran"; }
[ -s "$dir/out/index.tsv" ] && ok "index.tsv written" || bad "index.tsv written"
[ "$(wc -l < "$dir/out/index.tsv")" = 3 ] && ok "magic line and two apps (runtime and addon left out)" || bad "app count: $(wc -l < "$dir/out/index.tsv")"
grep -q "^org.example.Paint	Paint	Draw and edit pictures	Example Team	8	GPL-3.0-or-later	2.0	1800000000	1	2	" "$dir/out/index.tsv" \
	&& ok "fields: default language only, newest release, category bit, verified, 2 screenshots" || { cat "$dir/out/index.tsv"; bad "Paint line"; }
grep -q "	draw$" "$dir/out/index.tsv" && ok "English keywords only" || bad "keywords"
grep -q "^org.example.Game	Game	Play		1	" "$dir/out/index.tsv" && ok "ActionGame is in Games" || bad "Game category"
grep -q "https://example.org/752.png" "$dir/out/details.bin" && ok "752 px thumbnail preferred" || bad "thumbnail"
grep -q "https://example.org/src2.png" "$dir/out/details.bin" && ok "original used when there is no thumbnail" || bad "source fallback"
grep -q "^- Layers" "$dir/out/details.bin" && grep -q "Made for & by everybody." "$dir/out/details.bin" && ok "description: list items and entities" || bad "description"
! grep -q "Malprogramm" "$dir/out/details.bin" && ok "translated paragraph skipped" || bad "translation leaked"
echo "not xml" | gzip > "$dir/b.xml.gz"
mkdir "$dir/out2"
$FLSTORE_RUN "$bin" --build-index "$dir/b.xml.gz" "$dir/out2" 2>/dev/null && bad "garbage refused" || ok "garbage refused"
[ ! -e "$dir/out2/index.tsv" ] && ok "no index left behind by a failure" || bad "index left behind"
[ $fails = 0 ] && echo "all tests passed" || { echo "$fails test(s) failed"; exit 1; }
