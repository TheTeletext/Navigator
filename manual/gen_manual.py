#!/usr/bin/env python3
# gen_manual.py - Teletext Navigator User's Guide: help\src\*.txt (the same
# source as TELETEXT.HLP, read with the viewer's hlpsrc.py parser) plus
# manual\src\*.txt (what only the printed manual has), manual\figures.txt
# (where each screenshot goes) -> build\body.typ, compiled by manual.typ.
#
# Part of Teletext Navigator. Copyright (C) 2026 Dimitri Papadopoulos and
# The Teletext. GNU Affero General Public License v3.0 or later.
#
# Every text run becomes a Typst STRING literal, so nothing in the prose is
# read as Typst markup ("//", "--", "#", "*" stay as written).
import os, re, sys, glob, struct, subprocess

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
VIEWER = os.path.join(os.path.dirname(ROOT), "mgt-winhelp")
sys.path.insert(0, os.path.join(VIEWER, "tools"))
import hlpsrc  # noqa: E402

BUILD = os.path.join(HERE, "build")
DPI = 160            # every screenshot at the same scale: 160 pixels to the inch

# Chapters in order: (browse sequence, chapter title or None for the
# overview topic's own title). Ribbon order for the services.
CHAPTERS = [
    ("intro", None), ("install", None), ("general", "Getting Started"),
    ("home", None), ("rip", None), ("nabu", None), ("vtx", None), ("web", None),
    ("gopher", None), ("arpa", None), ("wais", None), ("archie", None), ("ftp", None),
    ("news", None), ("irc", None), ("cusm", None), ("term", None), ("gwb", None),
    ("helpuse", None), ("howto", None),
]
MERGE = {"howto": "helpuse"}   # the viewer's How to Use Help joins the Help chapter

def s(text):
    """a Typst string literal"""
    return '"' + text.replace("\\", "\\\\").replace('"', '\\"') + '"'

def png_size(path):
    with open(path, "rb") as f:
        head = f.read(24)
    return struct.unpack(">II", head[16:24])

def inline(text, known):
    """hlpsrc inline markup -> a sequence of Typst code expressions"""
    out = []
    pos = 0
    pat = re.compile(r"\[\[([^|\]]+)\|([^\]]+)\]\]|\{\{([^|}]+)\|([^}]+)\}\}|\*\*(.+?)\*\*|``(.+?)``|\{bmc ([^}]+)\}")
    for m in pat.finditer(text):
        if m.start() > pos:
            out.append("#" + s(text[pos:m.start()]))
        if m.group(1):
            ctx, t = m.group(1).strip(), m.group(2)
            if ctx in known:
                out.append("#xref(<%s>, %s)" % (ctx, s(t)))
            else:
                out.append("#" + s(t))
        elif m.group(3):
            out.append("#term(%s)" % s(m.group(4)))
        elif m.group(5):
            out.append("#strong(%s)" % s(m.group(5)))
        elif m.group(6):
            out.append("#raw(%s)" % s(m.group(6)))
        else:
            out.append("#icon(%s)" % s(m.group(7).strip()))
        pos = m.end()
    if pos < len(text):
        out.append("#" + s(text[pos:]))
    return "".join(out)

def only_link(text):
    m = re.fullmatch(r"\s*\[\[([^|\]]+)\|([^\]]+)\]\]\s*", text)
    return (m.group(1).strip(), m.group(2)) if m else None

def body(topic, known):
    out = []
    prev = None
    for b in hlpsrc.blocks(topic["body"]):
        kind, text = b[0], b[-1]
        # a blank line ends a list or paragraph; consecutive items stay tight
        if prev is not None and not (kind == prev and kind in ("li", "num", "code")):
            out.append("\n")
        if kind == "p":
            out.append(inline(text, known) + "\n")
        elif kind == "li":
            ol = only_link(text)
            if ol and ol[0] in known:
                out.append("#seeline(<%s>, %s)\n" % (ol[0], s(ol[1])))
            else:
                out.append("- " + inline(text, known) + "\n")
        elif kind == "num":
            out.append("+ " + inline(text, known) + "\n")
        elif kind == "h":
            out.append("=== " + inline(text, known) + "\n")
        else:
            out.append("#codeline(%s)\n" % s(text))
        prev = kind
    out.append("\n")
    return "".join(out)

def main():
    os.makedirs(BUILD, exist_ok=True)
    help_srcs = sorted(glob.glob(os.path.join(ROOT, "help", "src", "*.txt")))
    man_srcs = sorted(glob.glob(os.path.join(HERE, "src", "*.txt")))
    howto = os.path.join(VIEWER, "help", "src", "ttwinhlp.txt")
    meta, topics = hlpsrc.parse(help_srcs + man_srcs + [howto])

    # figures: ctx | shot | caption
    figs = {}
    for line in open(os.path.join(HERE, "figures.txt"), encoding="utf-8"):
        line = line.strip()
        if not line or line.startswith(";;"):
            continue
        ctx, shot, cap = [x.strip() for x in line.split("|", 2)]
        figs.setdefault(ctx, []).append((shot, cap))

    # page layout: sections that start on a new page (manual\layout.txt)
    breaks = set()
    lay = os.path.join(HERE, "layout.txt")
    if os.path.exists(lay):
        for line in open(lay, encoding="utf-8"):
            line = line.strip()
            if line and not line.startswith(";;"):
                breaks.add(line)

    # icons used in the help (BMP) -> PNG for Typst
    for bmp in glob.glob(os.path.join(ROOT, "help", "src", "*.BMP")):
        png = os.path.join(BUILD, os.path.basename(bmp)[:-4] + ".png")
        if not os.path.exists(png) or os.path.getmtime(png) < os.path.getmtime(bmp):
            subprocess.run(["magick", bmp, png], check=True)

    seqs = {}
    popups = []
    for t in topics:
        if t["kind"] == "popup":
            popups.append(t)
            continue
        seq = t["browse"].split(":")[0] if t["browse"] else ""
        seq = MERGE.get(seq, seq)
        if seq:
            seqs.setdefault(seq, []).append(t)
    placed = [t for c, _ in CHAPTERS if c not in MERGE for t in seqs.get(c, [])]
    known = {t["ctx"] for t in placed} | {"glossary"}

    out = ["// generated by gen_manual.py; edit help\\src, manual\\src, manual\\figures.txt\n#import \"../lib.typ\": *\n"]
    # the foreword: front matter, before the contents
    front = ["// generated by gen_manual.py\n#import \"../lib.typ\": *\n"]
    for t in seqs.get("foreword", []):
        front.append("#foreword(%s)[\n" % s(t["title"]))
        front.append(body(t, set()))
        front.append("]\n")
    open(os.path.join(BUILD, "front.typ"), "w", encoding="utf-8").write("".join(front))

    used_figs = set()
    for seq, title in CHAPTERS:
        if seq in MERGE:
            continue
        ts = seqs.get(seq, [])
        if not ts:
            sys.exit("chapter %s has no topics" % seq)
        first = ts[0]
        ctitle = title or first["title"]
        icon = first.get("icon") or meta["seqicon"].get(seq, "")
        out.append("#chapter(%s, icon: %s)\n= %s <ch_%s>\n\n" % (s(ctitle), s(icon) if icon else "none", inline(ctitle, set()), seq))
        for i, t in enumerate(ts):
            if title or i > 0:
                if t["ctx"] in breaks:
                    out.append("#pagebreak(weak: true)\n")
                out.append("== %s <%s>\n" % (inline(t["title"], set()), t["ctx"]))
            else:
                out.append("#metadata(none) <%s>\n" % t["ctx"])
            for k in [k.strip() for k in t["keys"].split(";") if k.strip()]:
                out.append("#idx(%s)" % s(k))
            out.append("\n")
            out.append(body(t, known))
            for shot, cap in figs.get(t["ctx"], []):
                p = os.path.join(HERE, "shots", shot + ".png")
                if not os.path.exists(p):
                    print("WARNING: missing screenshot", shot)
                    continue
                w, h = png_size(p)
                out.append("#shot(%s, %s, %.4f)\n\n" % (s("shots/" + shot + ".png"), s(cap), w / DPI))
                used_figs.add(t["ctx"])
    for ctx in figs:
        if ctx not in used_figs and ctx not in {t["ctx"] for t in placed}:
            print("WARNING: figure for unknown topic", ctx)

    # glossary, alphabetical
    out.append("#numbered.update(false)\n#chapicon.update(none)\n#heading(level: 1, numbering: none)[Glossary] <glossary>\n\n")
    for t in sorted(popups, key=lambda t: t["title"].lower()):
        txt = " ".join(" ".join(t["body"]).split())
        out.append("#gloss(%s)[%s]\n\n" % (s(t["title"]), inline(txt, known)))
    open(os.path.join(BUILD, "body.typ"), "w", encoding="utf-8").write("".join(out))
    print("body.typ: %d chapters, %d sections, %d glossary terms, %d figures" % (
        len([c for c in CHAPTERS if c[0] not in MERGE]), len(placed), len(popups),
        sum(len(v) for v in figs.values())))

if __name__ == "__main__":
    main()
