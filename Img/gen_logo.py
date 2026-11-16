#!/usr/bin/env python3
"""Generate fbktop's logo.svg and icon.svg from the same block-letter art as the
in-app banner (Global::Banner_src in src/btop.cpp).

Each terminal cell is drawn as a 10x20 box: '█' cells are filled in the banner's
red row colors, the box-drawing shadow characters become grey double lines.

usage: python3 Img/gen_logo.py   (writes Img/logo.svg and Img/icon.svg)
Render PNGs with a cairo-based renderer (ImageMagick's built-in SVG renderer drops the strokes):
  rsvg-convert -w 48 -h 48 Img/icon.svg -o Img/icon.png && rsvg-convert -w 500 Img/logo.svg -o Img/logo.png
"""
from pathlib import Path

LETTERS = {
    "F": ["███████╗", "██╔════╝", "█████╗  ", "██╔══╝  ", "██║     ", "╚═╝     "],
    "B": ["██████╗ ", "██╔══██╗", "██████╔╝", "██╔══██╗", "██████╔╝", "╚═════╝ "],
    "K": ["██╗  ██╗", "██║ ██╔╝", "█████╔╝ ", "██╔═██╗ ", "██║  ██╗", "╚═╝  ╚═╝"],
    "T": ["████████╗", "╚══██╔══╝", "   ██║   ", "   ██║   ", "   ██║   ", "   ╚═╝   "],
    "O": [" ██████╗ ", "██╔═══██╗", "██║   ██║", "██║   ██║", "╚██████╔╝", " ╚═════╝ "],
    "P": ["██████╗ ", "██╔══██╗", "██████╔╝", "██╔═══╝ ", "██║     ", "╚═╝     "],
}
RED = ["#E62525", "#CD2121", "#B31D1D", "#9A1919", "#801414", "#801414"]
GREY = ["#585858", "#4e4e4e", "#444444", "#3a3a3a", "#303030", "#262626"]
CW, CH = 10, 20  # cell size


def rows_for(word: str) -> list[str]:
    return ["".join(LETTERS[c][i] for c in word).rstrip() for i in range(6)]


def shadow_paths(ch: str, x0: float, y0: float) -> list[str]:
    x1, y1 = x0 + CW, y0 + CH
    xa, xb = x0 + CW * 0.3, x0 + CW * 0.7
    ya, yb = y0 + CH * 0.4, y0 + CH * 0.6
    segs = {
        "═": [[(x0, ya), (x1, ya)], [(x0, yb), (x1, yb)]],
        "║": [[(xa, y0), (xa, y1)], [(xb, y0), (xb, y1)]],
        "╗": [[(x0, ya), (xb, ya), (xb, y1)], [(x0, yb), (xa, yb), (xa, y1)]],
        "╔": [[(x1, ya), (xa, ya), (xa, y1)], [(x1, yb), (xb, yb), (xb, y1)]],
        "╝": [[(x0, yb), (xb, yb), (xb, y0)], [(x0, ya), (xa, ya), (xa, y0)]],
        "╚": [[(x1, yb), (xa, yb), (xa, y0)], [(x1, ya), (xb, ya), (xb, y0)]],
    }[ch]
    return ["M " + " L ".join(f"{x:g},{y:g}" for x, y in seg) for seg in segs]


def art_svg(rows: list[str], ox: float, oy: float) -> str:
    out = []
    for r, row in enumerate(rows):
        y = oy + r * CH
        # merge runs of blocks into single rects
        c = 0
        while c < len(row):
            if row[c] == "█":
                start = c
                while c < len(row) and row[c] == "█":
                    c += 1
                out.append(f'<rect x="{ox + start * CW:g}" y="{y:g}" width="{(c - start) * CW:g}" height="{CH:g}" fill="{RED[r]}"/>')
                continue
            if row[c] != " ":
                for d in shadow_paths(row[c], ox + c * CW, y):
                    out.append(f'<path d="{d}" fill="none" stroke="{GREY[r]}" stroke-width="2" stroke-linecap="square"/>')
            c += 1
    return "\n  ".join(out)


def main() -> None:
    here = Path(__file__).resolve().parent

    logo_rows = rows_for("FBKTOP")
    w = max(len(r) for r in logo_rows) * CW
    h = len(logo_rows) * CH
    (here / "logo.svg").write_text(
        f'<svg xmlns="http://www.w3.org/2000/svg" width="{w}" height="{h}" viewBox="0 0 {w} {h}">\n'
        f'  {art_svg(logo_rows, 0, 0)}\n</svg>\n', encoding="utf-8")

    icon_rows = LETTERS["F"]
    size, art_w, art_h = 128, 8 * CW, 6 * CH
    scale = 0.75
    ox, oy = (size - art_w * scale) / 2 + 3, (size - art_h * scale) / 2
    (here / "icon.svg").write_text(
        f'<svg xmlns="http://www.w3.org/2000/svg" width="{size}" height="{size}" viewBox="0 0 {size} {size}">\n'
        f'  <rect x="4" y="4" width="{size - 8}" height="{size - 8}" rx="18" fill="#141414" stroke="#2c2c2c" stroke-width="2"/>\n'
        f'  <g transform="translate({ox:g},{oy:g}) scale({scale:g})">\n  {art_svg(icon_rows, 0, 0)}\n  </g>\n</svg>\n', encoding="utf-8")


if __name__ == "__main__":
    main()
