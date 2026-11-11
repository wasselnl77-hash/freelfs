#!/usr/bin/env python3
"""Write a PostScript program for a small multi-page report with bookmarks,
an internal link and some drawn figures; ghostscript turns it into a PDF:

    python3 -I mkpdf.py > test.ps && gs -q -sDEVICE=pdfwrite -o test.pdf test.ps

Every word on the pages is made up here (nothing is copied from anywhere)."""
import random

random.seed(7)
words = ("harbour tide lantern copper meadow signal compass orchard timber valley anchor "
         "ridge ferry quarry canal beacon market bridge pasture lighthouse granite estuary "
         "kettle ledger passage thicket cargo mooring terrace foundry weather survey chart "
         "morning current shelter lantern harvest crossing summit pilot channel mirror").split()


def sentence():
    n = random.randint(8, 16)
    s = " ".join(random.choice(words) for _ in range(n))
    return s[0].upper() + s[1:] + "."


def paragraph():
    return " ".join(sentence() for _ in range(random.randint(3, 6)))


def esc(s):
    return s.replace("\\", "\\\\").replace("(", "\\(").replace(")", "\\)")


chapters = [("Overview", 1), ("The harbour survey", 1), ("Method", 2), ("Results", 2),
            ("Tides and currents", 1), ("Appendix", 1)]

ps = []
out = ps.append
out("%!PS-Adobe-3.0")
out("<< /PageSize [595 842] >> setpagedevice")
out("/Body /Times-Roman findfont 11 scalefont def")
out("/Head /Helvetica-Bold findfont 20 scalefont def")
out("/Sub /Helvetica-Bold findfont 13 scalefont def")
out("/Small /Helvetica findfont 9 scalefont def")
out("[ /Title (Harbour survey, field notes) /Author (FreeLFS test) /DOCINFO pdfmark")


def wrap(text, width_chars=88):
    lines, cur = [], ""
    for w in text.split():
        if len(cur) + len(w) + 1 > width_chars:
            lines.append(cur)
            cur = w
        else:
            cur = (cur + " " + w).strip()
    if cur:
        lines.append(cur)
    return lines


pageno = 0


def new_page(title=None):
    global pageno
    if pageno:
        out("showpage")
    pageno += 1
    out("Small setfont 0.5 setgray 72 40 moveto (Harbour survey, field notes) show")
    out("500 40 moveto (%d) show 0 setgray" % pageno)


def text_block(y, lines):
    for ln in lines:
        out("Body setfont 72 %d moveto (%s) show" % (y, esc(ln)))
        y -= 14
    return y


# page 1: title page
new_page()
out("Head setfont 72 700 moveto (Harbour survey) show")
out("Sub setfont 72 676 moveto (Field notes and results) show")
out("Body setfont 72 640 moveto (Contents) show")
y = 620
for i, (name, lvl) in enumerate(chapters):
    out("Body setfont %d %d moveto (%s) show" % (72 + 14 * (lvl - 1), y, esc(name)))
    out("[ /Rect [%d %d 300 %d] /Page %d /View [/XYZ null null null] /Subtype /Link /Border [0 0 0] /ANN pdfmark"
        % (72 + 14 * (lvl - 1), y - 3, y + 11, i + 1))
    y -= 18
y = text_block(y - 20, wrap(paragraph()))
y = text_block(y - 8, wrap(paragraph()))
out("[ /Title (Overview) /Page 1 /View [/XYZ null null null] /OUT pdfmark")

for i, (name, lvl) in enumerate(chapters[1:], start=2):
    new_page()
    cnt = " /Count 2" if name == "The harbour survey" else ""
    out("[ /Title (%s)%s /Page %d /View [/XYZ null null null] /OUT pdfmark" % (esc(name), cnt, i))
    out("Head setfont 72 760 moveto (%d  %s) show" % (i - 1, esc(name)))
    y = 730
    for _ in range(3):
        y = text_block(y, wrap(paragraph()))
        y -= 10
    if i % 2 == 0:
        # a small bar chart, drawn
        out("0.25 0.6 0.4 setrgbcolor")
        for k in range(8):
            h = random.randint(30, 120)
            out("%d 330 %d %d rectfill" % (90 + k * 50, 28, h))
        out("0 setgray 72 320 moveto 450 320 lineto 0.5 setlinewidth stroke")
        out("Small setfont 72 306 moveto (Figure %d: readings at eight stations) show" % (i - 1))
        y = 290
    y = text_block(y, wrap(paragraph()))
    y = text_block(y - 10, wrap(paragraph()))
    if i >= 3:
        # the word the search test looks for, on a few pages
        out("Body setfont 72 150 moveto (The lighthouse log was checked against the ferry timetable.) show")

out("showpage")
print("\n".join(ps))
