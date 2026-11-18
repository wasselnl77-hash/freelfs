#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Generate docs/graph/ (an Obsidian vault section) and docs/graph/AGENT-MAP.md.

Run from anywhere:   python3 -I build/gen-graph.py
Reads src/, recipes/, docs/ and the git history. Python 3 stdlib only.
Deterministic: sorted output, no timestamps (INDEX.md names the short hash of HEAD).
Re-running removes the files listed in docs/graph/.generated and writes them again.
Heuristics are documented in build/gen-graph.README.md.
"""

import os
import posixpath
import re
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
GRAPH_REL = "docs/graph"
GRAPH = ROOT / GRAPH_REL
MANIFEST = GRAPH / ".generated"

SKIP_DIRS = {"tests", "corpus", "expected", "data", "completions"}
SRC_EXT = (".c", ".h")
GEN_RE = re.compile(r"-(?:client-)?protocol\.[ch]$")
C_KEYWORDS = {"if", "while", "for", "switch", "return", "sizeof", "defined", "else", "case"}
FUNC_RE = re.compile(r"^([A-Za-z_][\w \t\*]*?[\s\*])([A-Za-z_]\w*)\s*\(")
INCLUDE_RE = re.compile(r'^\s*#\s*include\s*[<"]([^>"\n]+)[>"]', re.M)
EXEC_RE = re.compile(r"\b(?:execl|execlp|execle|execv|execvp|execvpe|execve|system|popen|posix_spawnp?)\s*\(")
STR_RE = re.compile(r'"((?:[^"\\\n]|\\.)*)"')
BUS_RE = re.compile(r'"((?:org\.freedesktop\.(?:portal|impl\.portal|Notifications|appearance|Settings)'
                    r'|org\.freelfs|net\.connman\.iwd|org\.bluez|org\.mpris\.MediaPlayer2)[\w.]*)"')
WL_RE = re.compile(r"\b((?:zxdg|zwp|zwlr|ext|wp|xwayland)_[a-z0-9_]+_v\d+"
                   r"|xdg_(?:wm_base|surface|toplevel|popup|positioner)"
                   r"|fl_(?:shell|screencopy_manager|screencopy_frame)"
                   r"|wl_(?:compositor|shm|seat|output|subcompositor|data_device_manager))(?:_interface)?\b")
PKG_TOKEN_RE = re.compile(r"^[\w.+-]+$")
RULE_RE = re.compile(r"[=\-~#*_]{3,}")

MAIN_DOCS = ["README.md", "docs/manual/README.md", "docs/ROADMAP-NEXT.md", "docs/HARDWARE.md",
             "docs/COMMANDS.md", "docs/CONFIG.md", "docs/TROUBLESHOOTING.md"]


# ---------- helpers ----------

def read_text(path):
    return path.read_bytes().decode("utf-8", "replace")


def git(*args):
    res = subprocess.run(["git", "-C", str(ROOT), "-c", "core.quotepath=off", *args],
                         capture_output=True, check=True)
    return res.stdout.decode("utf-8", "replace")


def shorten(text, limit):
    text = " ".join(text.split())
    if len(text) <= limit:
        return text
    cut = text.rfind(" ", 0, limit - 3)
    if cut < limit // 2:
        cut = limit - 3
    return text[:cut].rstrip(" ,;:") + "..."


def rel_link(frm, to):
    """Relative path (for plain .md links) from note frm to repo file to."""
    return posixpath.relpath(to, posixpath.dirname(frm))


def md(frm, to, text):
    return f"[{text}]({rel_link(frm, to)})"


def wiki(target, alias):
    """Obsidian wikilink; target is a repo-relative path without .md."""
    return f"[[{target}|{alias}]]"


def comp_note(c):
    return f"{GRAPH_REL}/components/{c}.md"


def pkg_note(p):
    return f"{GRAPH_REL}/packages/{p}.md"


def group_note(g):
    return f"{GRAPH_REL}/groups/{g}.md"


def file_note(key):
    return f"{GRAPH_REL}/files/{key[len('src/'):]}.md"


def round_note(n):
    return f"{GRAPH_REL}/rounds/round-{n}.md"


def wl_comp(c):
    return wiki(comp_note(c)[:-3], c)


def wl_pkg(p):
    return wiki(pkg_note(p)[:-3], f"pkg/{p}")


def wl_file(key):
    return wiki(file_note(key)[:-3], key)


def wl_doc(chapter_stem):
    return wiki(f"docs/manual/{chapter_stem}", chapter_stem)


def comp_of(key):
    return key.split("/")[1]


def bus_norm(s):
    s = s.rstrip(".")
    for prefix in ("net.connman.iwd", "org.bluez", "org.mpris.MediaPlayer2"):
        if s == prefix or s.startswith(prefix + "."):
            return prefix
    return s


# ---------- scanning ----------

def collect_files(comps):
    files = {}
    for comp in comps:
        base = ROOT / "src" / comp
        for dirpath, dirnames, filenames in os.walk(base):
            dirnames[:] = sorted(d for d in dirnames if d not in SKIP_DIRS and not d.startswith("."))
            for fn in sorted(filenames):
                if not fn.endswith(SRC_EXT):
                    continue
                path = Path(dirpath) / fn
                rel = path.relative_to(base).as_posix()
                key = f"src/{comp}/{rel}"
                text = read_text(path)
                files[key] = {"key": key, "comp": comp, "rel": rel, "text": text,
                              "lines": len(text.splitlines()),
                              "generated": GEN_RE.search(fn) is not None}
    return files


def functions(text):
    lines = text.splitlines()
    names = []
    for i, ln in enumerate(lines):
        if ln.rstrip().endswith(";"):
            continue
        m = FUNC_RE.match(ln)
        if not m:
            continue
        name = m.group(2)
        first = m.group(1).split()[0]
        if name in C_KEYWORDS or first in ("typedef", "return", "else", "case", "goto", "static_assert"):
            continue
        chunk = "\n".join(lines[i:i + 15])
        b, s = chunk.find("{"), chunk.find(";")
        if b >= 0 and (s < 0 or b < s) and name not in names:
            names.append(name)
    return names


def resolve(key, name, hdr_index, files):
    cand = posixpath.normpath(posixpath.join(posixpath.dirname(key), name))
    if cand in files:
        return [cand]
    return [k for k in hdr_index.get(posixpath.basename(name), []) if k != key]


def readme_summary(comp):
    path = ROOT / "src" / comp / "README"
    if not path.is_file():
        return ""
    for block in re.split(r"\n\s*\n", read_text(path).strip()):
        lines = [l.strip() for l in block.splitlines() if l.strip()]
        if not lines or RULE_RE.fullmatch(lines[0]) and len(lines) == 1:
            continue
        if len(lines) >= 2 and RULE_RE.fullmatch(lines[1]):
            continue  # a title block: "name" + underline
        body = [l for l in lines if not RULE_RE.fullmatch(l)]
        if body:
            return shorten(" ".join(body), 240)
    return ""


def makefile_info(comp):
    """Returns (makefile text, set of pkg-config names and -lX flags, binaries installed)."""
    path = ROOT / "src" / comp / "Makefile"
    if not path.is_file():
        return "", set()
    text = read_text(path)
    joined = re.sub(r"\\\n", " ", text)
    names = set()
    for line in joined.splitlines():
        m = re.match(r"\s*(PKGS|GLPKGS|LIBS|LDLIBS|UI_LIBS|PKG_LIBS)\s*[:+?]?=\s*(.*)$", line)
        if not m:
            continue
        for tok in m.group(2).split():
            if tok.startswith("-l") and len(tok) > 2:
                names.add(tok)
            elif PKG_TOKEN_RE.match(tok):
                names.add(tok)
    for m in re.finditer(r"pkg-config\s+(?:--\w+\s+)+([\w.+\- ]+?)\)", joined):
        for tok in m.group(1).split():
            if PKG_TOKEN_RE.match(tok):
                names.add(tok)
    return text, names


def classify(comp, files, mk_text, pkgnames):
    src = [f for f in files if not f["generated"]]
    alltext = "\n".join(f["text"] for f in src)
    cdir = ROOT / "src" / comp
    services = sorted(cdir.glob("*.service*")) + sorted(cdir.glob("*.socket"))
    desktop = sorted(cdir.glob("*.desktop"))
    if "efi_main" in alltext or '"efi.h"' in alltext:
        return "system"  # UEFI loader: no main(), runs before the OS
    has_bin = re.search(r"/s?bin/", mk_text) is not None
    has_main = re.search(r"^int\s+main\s*\(", alltext, re.M) is not None
    if comp.startswith("lib") or "-shared" in mk_text or (not has_main and not has_bin):
        return "library"
    if services or re.search(r"\bdaemon\s*\(", alltext):
        return "daemon"
    if ("wayland-server" in pkgnames
            or re.search(r"\b(?:mount|umount2?|reboot|sethostname|pivot_root|setuid)\s*\(", alltext)
            or re.search(r"install\s.*/sbin/", mk_text)):
        return "system"
    if desktop or pkgnames & {"flui", "wayland-client"}:
        return "app"
    return "system"


def manual_refs(comps):
    chapters = {}
    for p in sorted((ROOT / "docs" / "manual").glob("*.md")):
        if p.name != "README.md":
            chapters[p.stem] = read_text(p)
    refs = {}
    for c in comps:
        pat = re.compile(r"(?<![\w-])" + re.escape(c) + r"(?![\w-])")
        refs[c] = [stem for stem in sorted(chapters) if pat.search(chapters[stem])]
    return refs


def parse_recipes():
    pk = {}
    for rdir in sorted((ROOT / "recipes").iterdir()):
        rf = rdir / "recipe"
        if not rf.is_file():
            continue
        text = read_text(rf)

        def sc(key):
            m = re.search(r"^" + key + r"=(.*)$", text, re.M)
            return m.group(1).strip().strip("\"'") if m else ""

        def ar(key):
            m = re.search(r"^" + key + r"=\(([^)]*)\)", text, re.M)
            if not m:
                return []
            out = []
            for t in m.group(1).split():
                t = re.sub(r"[<>=].*$", "", t.strip("\"'"))
                if t:
                    out.append(t)
            return out

        pk[rdir.name] = {
            "name": rdir.name, "recipe": f"recipes/{rdir.name}/recipe",
            "pkgname": sc("pkgname"), "version": sc("version") or "?", "release": sc("release") or "?",
            "summary": sc("summary"), "license": " ".join(ar("license")), "url": sc("url"),
            "groups": ar("groups"), "depends": ar("depends"), "makedepends": ar("makedepends"),
            "copies": sorted(set(re.findall(r"startdir/\.\./\.\./src/([\w.+-]+)", text))),
            "srcrefs": re.findall(r"src/([\w.+-]+)", text),
        }
    return pk


# ---------- build ----------

def main():
    comps = sorted(p.name for p in (ROOT / "src").iterdir() if p.is_dir())
    head = git("rev-parse", "--short=7", "HEAD").strip()
    files = collect_files(comps)
    hdr_index = {}
    for key in sorted(files):
        if key.endswith(".h"):
            hdr_index.setdefault(posixpath.basename(key), []).append(key)

    # per-file: functions, local includes (resolved), system include count
    rev = {}
    for key in sorted(files):
        f = files[key]
        f["functions"] = [] if f["generated"] else functions(f["text"])
        deps, sysc = set(), 0
        for m in INCLUDE_RE.finditer(f["text"]):
            targets = [t for t in resolve(key, m.group(1).strip(), hdr_index, files) if t != key]
            if targets:
                deps.update(targets)
            else:
                sysc += 1
        f["deps"] = sorted(deps)
        f["sys"] = sysc
        for d in f["deps"]:
            rev.setdefault(d, []).append(key)

    # Makefile-derived data, binaries
    binmap = {}
    mk = {}
    for c in comps:
        mk_text, names = makefile_info(c)
        mk[c] = (mk_text, names)
        for line in mk_text.splitlines():
            m = re.match(r"^\s*install\s.*?/s?bin/([\w.+-]+)\s*$", line)
            if m:
                binmap[m.group(1)] = c

    desc, tags, uses, libs, cli, bus, wl = {}, {}, {}, {}, {}, {}, {}
    files_of = {c: [] for c in comps}
    for key in sorted(files):
        files_of[files[key]["comp"]].append(key)
    for c in comps:
        mk_text, names = mk[c]
        pkgnames = {n for n in names if not n.startswith("-l")}
        desc[c] = readme_summary(c)
        if not desc[c]:
            m = re.search(r"^#\s*\S+\s*--\s*(.+)$", mk_text, re.M)
            desc[c] = m.group(1).strip() if m else "(no description)"
        tags[c] = classify(c, [files[k] for k in files_of[c]], mk_text, pkgnames)

        uses[c] = {}
        for k in files_of[c]:
            for d in files[k]["deps"]:
                t = comp_of(d)
                if t != c:
                    uses[c].setdefault(t, set()).add("hdr:" + posixpath.basename(d))
        libs[c] = set()
        for n in sorted(names):
            if n.startswith("-l") and n[2:] in comps and n[2:] != c:
                uses[c].setdefault(n[2:], set()).add("pkg")
            elif not n.startswith("-l") and n in comps and n != c:
                uses[c].setdefault(n, set()).add("pkg")
            elif n not in comps:
                libs[c].add(n)

        cli[c], bus[c], wl[c] = {}, set(), set()
        for k in files_of[c]:
            f = files[k]
            if f["generated"]:
                continue
            text = f["text"]
            for m in EXEC_RE.finditer(text):
                end = text.find(";", m.end())
                if end < 0 or end - m.end() > 800:
                    end = m.end() + 800
                for s in STR_RE.findall(text[m.end():end]):
                    words = s.split()
                    if not words:
                        continue
                    name = posixpath.basename(words[0])
                    if name in binmap and binmap[name] != c:
                        cli[c].setdefault(binmap[name], set()).add(name)
            bus[c].update(bus_norm(m.group(1)) for m in BUS_RE.finditer(text))
            wl[c].update(m.group(1) for m in WL_RE.finditer(text))

    # recipes, packages, groups
    pk = parse_recipes()
    pkg_comp = {}
    for p, d in pk.items():
        if p in comps:
            pkg_comp[p] = p
        else:
            cand = [s for s in d["srcrefs"] if s in comps]
            pkg_comp[p] = cand[0] if cand else None
    installed = {c: {"primary": [], "copied": []} for c in comps}
    for p, d in pk.items():
        for c in d["copies"]:
            if c in comps:
                (installed[c]["primary"] if p == c else installed[c]["copied"]).append(p)
    groups = {}
    for p, d in pk.items():
        for g in d["groups"] or ["ungrouped"]:
            groups.setdefault(g, []).append(p)
    for g in groups:
        groups[g].sort()

    manual = manual_refs(comps)

    # git history
    commits = [l.split("\t") for l in git("log", "--format=%H\t%h\t%s").splitlines() if l]
    rounds = {}
    for full, short, subj in commits:
        m = re.match(r"Round (\d+):", subj)
        if m:
            rounds.setdefault(int(m.group(1)), []).append((full, short, subj))
    recent = {}
    for c in comps:
        recent[c] = [l.split("\t") for l in git("log", "-n", "5", "--format=%h\t%s", "--", f"src/{c}").splitlines() if l]

    # reverse relations
    used_by = {c: {} for c in comps}
    for c in comps:
        for t, labels in uses[c].items():
            used_by[t].setdefault(c, set()).update(l.split(":")[0] for l in labels)
        for t, bins in cli[c].items():
            used_by[t].setdefault(c, set()).add("cli")

    out = {}

    # ----- component notes -----
    for c in comps:
        note = comp_note(c)
        L = ["---", f"tags: [component, {tags[c]}]", "---", f"# {c}", "",
             f"#component #{tags[c]}", "", desc[c], ""]
        L += ["## Source files", ""]
        for k in files_of[c]:
            f = files[k]
            extra = " (generated)" if f["generated"] else f" -- {len(f['functions'])} fn" if f["functions"] else ""
            L.append(f"- {wl_file(k)} -- {f['lines']} lines{extra}")
        if not files_of[c]:
            L.append("- none")
        L += ["", "## Uses", ""]
        if uses[c]:
            for t in sorted(uses[c]):
                L.append(f"- component {wl_comp(t)} ({', '.join(sorted(uses[c][t]))})")
        L.append(f"- libraries: {', '.join('`%s`' % x for x in sorted(libs[c])) or 'none'}")
        L += ["", "## Talks to", ""]
        for t in sorted(cli[c]):
            L.append(f"- CLI {wl_comp(t)}: {', '.join('`%s`' % b for b in sorted(cli[c][t]))}")
        if bus[c]:
            L.append("- D-Bus: " + ", ".join(f"`{b}`" for b in sorted(bus[c])))
        if wl[c]:
            L.append("- Wayland: " + ", ".join(f"`{w}`" for w in sorted(wl[c])))
        if not (cli[c] or bus[c] or wl[c]):
            L.append("- none found")
        L += ["", "## Used by", ""]
        for u in sorted(used_by[c]):
            L.append(f"- {wl_comp(u)} ({', '.join(sorted(used_by[c][u]))})")
        if not used_by[c]:
            L.append("- nothing in the repo")
        L += ["", "## Installed by", ""]
        for p in installed[c]["primary"]:
            L.append(f"- {wl_pkg(p)} (recipe `{pk[p]['recipe']}`)")
        for p in installed[c]["copied"]:
            L.append(f"- also copied from src/{c} by {wl_pkg(p)}")
        if not (installed[c]["primary"] or installed[c]["copied"]):
            L.append("- no recipe")
        L += ["", "## Docs", ""]
        L += [f"- {wl_doc(s)}" for s in manual[c]] or ["- no manual chapter names it"]
        L += ["", "## Recent changes", ""]
        L += [f"- `{h}` {s}" for h, s in recent[c]] or ["- none"]
        see = []
        if (ROOT / "src" / c / "README").is_file():
            see.append(md(note, f"src/{c}/README", f"src/{c}/README"))
        see += [md(note, f"docs/manual/{s}.md", s) for s in manual[c][:3]]
        see += [md(note, f"{GRAPH_REL}/AGENT-MAP.md", "AGENT-MAP"), md(note, f"{GRAPH_REL}/INDEX.md", "INDEX")]
        L += ["", "See also: " + " · ".join(see), ""]
        out[note] = "\n".join(L)

    # ----- source file notes -----
    for key in sorted(files):
        f = files[key]
        c = f["comp"]
        note = file_note(key)
        tagline = "#source" + (" #generated" if f["generated"] else "")
        L = ["---", "tags: [source]", "---", f"# {key}", "", tagline, "",
             f"Component: {wl_comp(c)} -- {f['lines']} lines", "", "## Includes", ""]
        L += [f"- {wl_file(d)}" for d in f["deps"]] or ["- none in the repo"]
        if f["sys"]:
            L.append(f"- {f['sys']} system or external headers (not linked)")
        L += ["", "## Functions (best effort)", ""]
        L.append("- " + ", ".join(f"`{n}`" for n in f["functions"]) if f["functions"] else "- none found")
        L += ["", f"See also: {md(note, comp_note(c), c + ' component')} · {md(note, key, key)}", ""]
        out[note] = "\n".join(L)

    # ----- package notes -----
    for p in sorted(pk):
        d = pk[p]
        note = pkg_note(p)
        L = ["---", "tags: [package]", "---", f"# pkg/{p}", "", "#package", ""]
        L.append(f"Version {d['version']}, release {d['release']}. {d['summary']}".rstrip())
        L.append("")
        L.append(f"- License: {d['license'] or '?'}; URL: {d['url'] or 'none'}")
        if d["groups"]:
            L.append("- Groups: " + ", ".join(wiki(group_note(g)[:-3], g) for g in d["groups"]))
        else:
            L.append("- Groups: none")
        if pkg_comp.get(p):
            L.append(f"- Component: {wl_comp(pkg_comp[p])}")
        for label, field in (("Depends", "depends"), ("Makedepends", "makedepends")):
            items = []
            for dep in d[field]:
                items.append(wl_pkg(dep) if dep in pk else f"{dep} (not in recipes/)")
            L.append(f"- {label}: " + (", ".join(items) if items else "none"))
        L += ["", f"Recipe: `{d['recipe']}`" + (f" (pkgname {d['pkgname']})" if d['pkgname'] not in ("", p) else ""), ""]
        see = [md(note, d["recipe"], d["recipe"])]
        if pkg_comp.get(p):
            see.append(md(note, comp_note(pkg_comp[p]), pkg_comp[p] + " component"))
        see.append(md(note, f"{GRAPH_REL}/INDEX.md", "INDEX"))
        L += ["See also: " + " · ".join(see), ""]
        out[note] = "\n".join(L)

    # ----- group notes -----
    for g in sorted(groups):
        note = group_note(g)
        L = ["---", "tags: [package-group]", "---", f"# group: {g}", "", "#package-group", "",
             f"{len(groups[g])} packages:", ""]
        L += [f"- {wl_pkg(p)}" for p in groups[g]]
        L += ["", f"See also: {md(note, GRAPH_REL + '/INDEX.md', 'INDEX')}", ""]
        out[note] = "\n".join(L)

    # ----- round notes -----
    for n in sorted(rounds):
        note = round_note(n)
        L = ["---", "tags: [round]", "---", f"# Round {n}", "", "#round", ""]
        prev_rounds = [m for m in sorted(rounds) if m < n]
        nxt = [m for m in sorted(rounds) if m > n]
        for full, short, subj in rounds[n]:
            L.append(f"Commit `{short}`: {subj}")
            L.append("")
            rows = git("show", "--numstat", "--no-renames", "--format=", full).splitlines()
            entries = []
            for row in rows:
                parts = row.split("\t")
                if len(parts) == 3:
                    entries.append((parts[2], parts[0], parts[1]))
            L.append(f"{len(entries)} files changed.")
            L.append("")
            by_comp, by_pkg, other = {}, {}, []
            for path, add, dele in entries:
                seg = path.split("/")
                if seg[0] == "src" and len(seg) > 2 and seg[1] in comps:
                    by_comp.setdefault(seg[1], []).append((path, add, dele))
                elif seg[0] == "recipes" and len(seg) > 2 and seg[1] in pk:
                    by_pkg.setdefault(seg[1], []).append((path, add, dele))
                else:
                    other.append((path, add, dele))
            L.append("## Components touched")
            L.append("")
            for c in sorted(by_comp):
                tot_a = sum(int(a) for _, a, _ in by_comp[c] if a.isdigit())
                tot_d = sum(int(d) for _, _, d in by_comp[c] if d.isdigit())
                nf = len(by_comp[c])
                L.append(f"- {wl_comp(c)}: {nf} file{'s' if nf != 1 else ''}, +{tot_a} -{tot_d}")
                for path, add, dele in sorted(by_comp[c]):
                    if path in files:
                        L.append(f"  - {wl_file(path)} (+{add} -{dele})")
                    else:
                        L.append(f"  - `{path}` (+{add} -{dele})")
            if not by_comp:
                L.append("- none")
            L += ["", "## Packages touched", ""]
            for p in sorted(by_pkg):
                L.append(f"- {wl_pkg(p)}: " + ", ".join(f"`{x[0]}`" for x in sorted(by_pkg[p])))
            if not by_pkg:
                L.append("- none")
            L += ["", "## Other files", ""]
            L += [f"- `{path}` (+{add} -{dele})" for path, add, dele in sorted(other)] or ["- none"]
            L.append("")
        see = [md(note, GRAPH_REL + "/INDEX.md", "INDEX")]
        if prev_rounds:
            see.append(md(note, round_note(prev_rounds[-1]), f"Round {prev_rounds[-1]}"))
        if nxt:
            see.append(md(note, round_note(nxt[0]), f"Round {nxt[0]}"))
        L += ["See also: " + " · ".join(see), ""]
        out[note] = "\n".join(L)

    # ----- impact (for AGENT-MAP) -----
    def impact(seeds):
        seen, stack = set(seeds), list(seeds)
        while stack:
            k = stack.pop()
            for r in rev.get(k, []):
                if r not in seen:
                    seen.add(r)
                    stack.append(r)
        return sorted({comp_of(k) for k in seen})

    impact_rows = []
    cross_hdrs = set()
    for key in sorted(files):
        for d in files[key]["deps"]:
            if comp_of(d) != comp_of(key) and d.endswith(".h") and not files[d]["generated"]:
                cross_hdrs.add(d)
    for c in comps:
        if tags[c] == "library":
            for key in files_of[c]:
                if key.endswith(".h") and "/" not in key[len(f"src/{c}/"):] and not files[key]["generated"]:
                    cross_hdrs.add(key)
    for key in sorted(cross_hdrs):
        kind = "library header" if tags[comp_of(key)] == "library" else "header"
        impact_rows.append((key, kind, impact([key])))
    families = {}
    for key in sorted(files):
        if files[key]["generated"]:
            stem = re.sub(r"(?:-client)?-protocol\.[ch]$", "", posixpath.basename(key))
            families.setdefault(stem, []).append(key)
    for stem in sorted(families):
        impact_rows.append((f"generated {stem}-protocol.[ch] (wayland XML)", "protocol",
                            impact(families[stem])))
    for xml in sorted(ROOT.glob("src/*/protocol/*.xml")):
        stem = xml.stem
        seeds = families.get(stem, [])
        hit = set(impact(seeds)) if seeds else set()
        iface = stem.replace("-", "_")
        hit |= {c for c in comps if any(w.startswith(iface) for w in wl[c])}
        impact_rows.append((xml.relative_to(ROOT).as_posix(), "protocol XML", sorted(hit)))
    flconf_present = any(posixpath.basename(k).startswith("flconf") for k in files)

    # ----- INDEX -----
    L = ["---", "tags: [index]", "---", "# FreeLFS graph index", "", "#index", "",
         f"Generated by `build/gen-graph.py` at HEAD `{head}`. Open the repo folder as an Obsidian vault "
         f"(graph view: Ctrl+G). Compact version for AI agents: [AGENT-MAP](AGENT-MAP.md).", "",
         "## Main docs", ""]
    note = f"{GRAPH_REL}/INDEX.md"
    for doc in MAIN_DOCS + [p for p in sorted(f"docs/{x.name}" for x in (ROOT / "docs").glob("PLAN*.md"))]:
        if (ROOT / doc).is_file():
            target = doc[:-3] if doc.endswith(".md") else doc
            L.append(f"- [[{target}|{doc}]] -- {md(note, doc, doc)}")
    L += ["", f"## Components ({len(comps)})", ""]
    for c in comps:
        L.append(f"- {wl_comp(c)} #{tags[c]} -- {md(note, comp_note(c), c + '.md')} -- {desc[c]}")
    L += ["", f"## Package groups ({len(groups)})", ""]
    for g in sorted(groups):
        L.append(f"- {wiki(group_note(g)[:-3], g)} ({len(groups[g])} packages) -- {md(note, group_note(g), g + '.md')}")
    L += ["", f"Packages: {len(pk)} notes in {GRAPH_REL}/packages/ (one per recipes/*/recipe).", ""]
    L += ["## Rounds", ""]
    for n in sorted(rounds):
        L.append(f"- {wiki(round_note(n)[:-3], f'Round {n}')} -- {md(note, round_note(n), f'round-{n}.md')}"
                 f" ({len(rounds[n])} commit{'s' if len(rounds[n]) != 1 else ''})")
    L += ["", "## Agent map", "", f"- {md(note, GRAPH_REL + '/AGENT-MAP.md', 'AGENT-MAP.md')}", ""]
    out[note] = "\n".join(L)

    # ----- AGENT-MAP -----
    note = f"{GRAPH_REL}/AGENT-MAP.md"
    total_c = len(comps)
    L = ["# FreeLFS agent map", "",
         "Generated by build/gen-graph.py. Do not edit: re-run the script. Rules: docs/AGENT-RULES.md. "
         "Full graph: docs/graph/INDEX.md.", "",
         "Labels: uses = (hdr) a header of it is #included, (pkg) named in its Makefile. "
         "used-by adds (cli) when it execs that binary. talks-to: CLI = exec of another fl* tool, "
         "D-Bus = bus or interface name, Wayland = protocol interface.", "",
         f"## Components ({total_c})", ""]
    for c in comps:
        keyf = sorted(files_of[c], key=lambda k: (files[k]["generated"], -files[k]["lines"], k))
        keyf = [k for k in keyf if not files[k]["generated"]]
        shown = [files[k]["rel"] for k in keyf[:6]]
        more = len(keyf) - len(shown)
        ulist = []
        for t in sorted(uses[c]):
            kinds = sorted({l.split(":")[0] for l in uses[c][t]})
            ulist.append(f"{t} ({', '.join(kinds)})")
        ub = [f"{u} ({', '.join(sorted(used_by[c][u]))})" for u in sorted(used_by[c])]
        tk = []
        for t in sorted(cli[c]):
            tk.append("cli " + ", ".join(sorted(cli[c][t])))
        if bus[c]:
            tk.append("dbus " + ", ".join(sorted(bus[c])))
        if wl[c]:
            tk.append("wayland " + ", ".join(sorted(wl[c])))
        if pk.get(c):
            rec = f"recipes/{c}/recipe (pkg {c})"
        elif installed[c]["primary"]:
            rec = f"recipes/{installed[c]['primary'][0]}/recipe (pkg {installed[c]['primary'][0]})"
        elif installed[c]["copied"]:
            rec = f"copied by pkg {', '.join(installed[c]['copied'])}"
        else:
            rec = "none"
        mtxt = ", ".join(manual[c][:4]) + (f" (+{len(manual[c]) - 4})" if len(manual[c]) > 4 else "")
        L.append(f"### {c} [{tags[c]}]")
        L.append(desc[c][:200] if desc[c] else "(no description)")
        L.append(f"- key files: {', '.join(shown) or 'none'}" + (f" (+{more})" if more > 0 else ""))
        L.append(f"- uses: {'; '.join(ulist) or 'none in repo'}; libs: {', '.join(sorted(libs[c])) or 'none'}")
        L.append(f"- used-by: {', '.join(ub) or 'nothing in repo'}")
        L.append(f"- talks-to: {'; '.join(tk) or 'none found'}")
        L.append(f"- recipe: {rec}")
        L.append(f"- manual: {mtxt or 'none'}")
    L += ["", "## Change impact", "",
          "Rebuild the listed components when the file changes (includes followed transitively, "
          "plus protocol and link users). Libraries: a change to a public header rebuilds every consumer.", ""]
    for key, kind, hit in impact_rows:
        L.append(f"- {key} ({kind}): {', '.join(hit) or 'none'}")
    if not flconf_present:
        L.append("- flconf: no such file in this repo (colour tokens and config live in src/flui/flpalette.h "
                 "and src/flui/conf.c; see flui)")
    L.append("")
    out[note] = "\n".join(L)

    write_out(out)
    n_files = len(out)
    print(f"gen-graph: wrote {n_files} files to {GRAPH_REL}/ (HEAD {head})")


def write_out(out):
    old = []
    if MANIFEST.is_file():
        old = [l.strip() for l in read_text(MANIFEST).splitlines() if l.strip()]
    old_set = {f"{GRAPH_REL}/{e}" for e in old}
    new_paths = sorted(out)
    for rel in new_paths:
        p = ROOT / rel
        if p.exists() and rel not in old_set:
            sys.exit(f"gen-graph: refusing to overwrite {rel}: it exists and is not in {GRAPH_REL}/.generated")
    dirs = set()
    for e in old:
        if e.startswith("/") or ".." in e.split("/"):
            continue
        p = GRAPH / e
        if p.is_file() or p.is_symlink():
            p.unlink()
        d = p.parent
        while d != GRAPH and GRAPH in d.parents:
            dirs.add(d)
            d = d.parent
    for d in sorted(dirs, key=lambda x: len(x.parts), reverse=True):
        try:
            d.rmdir()
        except OSError:
            pass
    for rel in new_paths:
        p = ROOT / rel
        p.parent.mkdir(parents=True, exist_ok=True)
        p.write_text(out[rel], encoding="utf-8")
    MANIFEST.write_text("".join(rel[len(GRAPH_REL) + 1:] + "\n" for rel in new_paths), encoding="utf-8")


if __name__ == "__main__":
    main()
