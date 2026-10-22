#!/usr/bin/env python3
"""Draw the scoring-matrix tutorial's original, theme-aware concept figures.

These are worked teaching examples, not measurements from the genome cohort.
The small BLOSUM62 extract is from https://ftp.ncbi.nih.gov/blast/matrices/BLOSUM62.
"""
from __future__ import annotations

import html
import math
from pathlib import Path

from theme_palette import theme_svg

OUT = Path(__file__).resolve().parents[1] / "static/figures/blog/scoring-matrices"
C = dict(text="#eef4f9", muted="#a9bccb", grid="#20364a", panel="#0a1826",
         blue="#65b9dd", mint="#74c8a8", orange="#f4894f", purple="#c5a8d6")


def text(x, y, value, size=24, color="text", anchor="start", weight="normal", mono=False):
    family = "monospace" if mono else "Arial, sans-serif"
    return (f'<text x="{x}" y="{y}" font-family="{family}" font-size="{size}" '
            f'font-weight="{weight}" text-anchor="{anchor}" fill="{C.get(color, color)}">'
            f'{html.escape(str(value))}</text>')


def line(x1, y1, x2, y2, color="grid", width=2, extra=""):
    return f'<path d="M{x1} {y1}L{x2} {y2}" fill="none" stroke="{C[color]}" stroke-width="{width}" {extra}/>'


def rect(x, y, w, h, color="panel", radius=14, extra=""):
    return f'<rect x="{x}" y="{y}" width="{w}" height="{h}" rx="{radius}" fill="{C[color]}" {extra}/>'


def tile(x, y, letter, color="mint", size=48, alpha=.14):
    return (rect(x, y, size, size, color, 8, f'fill-opacity="{alpha}"') +
            text(x + size / 2, y + size * .69, letter, size * .52, color, "middle", "bold", True))


def save(name, width, height, title, description, content):
    OUT.mkdir(parents=True, exist_ok=True)
    svg = (f'<svg xmlns="http://www.w3.org/2000/svg" width="{width}" height="{height}" '
           f'viewBox="0 0 {width} {height}" role="img" aria-labelledby="title desc">\n'
           f'<title id="title">{html.escape(title)}</title>\n'
           f'<desc id="desc">{html.escape(description)}</desc>\n'
           + "\n".join(content) + "\n</svg>\n")
    (OUT / name).write_text(theme_svg(svg))


def cover():
    parts = []
    # Decorative matrix thumbnails deliberately contain no numerical data.
    for origin, letters, color in [(55, "ACGT", "mint"), (970, "LIGW", "purple")]:
        for i, letter in enumerate(letters):
            parts.append(text(origin + 45 + i * 34, 54, letter, 24, color, "middle", "bold", True))
            parts.append(text(origin + 8, 87 + i * 34, letter, 24, color, "middle", "bold", True))
            for j in range(4):
                cell_color = color if i == j else ("blue" if (i + j) % 3 else "orange")
                opacity = .45 if i == j else (.13 + .06 * ((i + j) % 3))
                parts.append(rect(origin + 30 + j * 34, 65 + i * 34, 29, 29, cell_color, 5,
                                  f'fill-opacity="{opacity}"'))
    # The two helices are schematic. One base pair changes from A:T to G:C.
    for center, color, mutated in [(470, "blue", False), (745, "mint", True)]:
        for phase in [0, math.pi]:
            coords = [(center + 36 * math.sin((y - 32) * math.pi / 76 + phase), y)
                      for y in range(32, 229, 2)]
            path = " ".join(("M" if i == 0 else "L") + f"{x:.1f},{y}" for i, (x, y) in enumerate(coords))
            parts.append(f'<path d="{path}" fill="none" stroke="{C[color]}" stroke-width="4" stroke-opacity=".65"/>')
        for y in range(42, 223, 18):
            spread = abs(36 * math.sin((y - 32) * math.pi / 76))
            if y == 114:
                parts.append(line(center - spread, y, center + spread, y, "orange", 7,
                                  'stroke-linecap="round"'))
            else:
                parts.append(line(center - spread, y, center + spread, y, color, 2.5,
                                  'stroke-opacity=".3" stroke-linecap="round"'))
        parts.append(tile(center - 67, 92, "G" if mutated else "A", "orange", 31, .16))
        parts.append(tile(center + 38, 92, "C" if mutated else "T", "orange", 31, .16))
    parts.append(line(560, 114, 655, 114, "muted", 2, 'stroke-dasharray="5 7" stroke-opacity=".55"'))
    parts.append(f'<path d="M644 106L655 114L644 122" fill="none" stroke="{C["muted"]}" stroke-width="2" stroke-opacity=".55"/>')
    save("cover.svg", 1200, 260, "Sequence changes and substitution matrices",
         "Two schematic DNA double helices show an A:T base pair replaced by G:C. "
         "A nucleotide matrix thumbnail and a separate protein-letter matrix flank the helices. "
         "The colored matrix cells are decorative, not measured values.", parts)


def identity_blosum():
    parts = [text(24, 42, "A different score for each amino-acid pair", 28, weight="bold"),
             text(24, 80, "The same four protein letters, two scoring schemes", 22, "muted")]
    letters = "LIGW"
    scores = [[4, 2, -4, -2], [2, 4, -4, -3], [-4, -4, 6, -2], [-2, -3, -2, 11]]
    for origin, title, data in [(24, "Identity: +1 / −1", [[1 if i == j else -1 for j in range(4)] for i in range(4)]),
                                (374, "BLOSUM62 extract", scores)]:
        parts.append(text(origin + 156, 133, title, 25, anchor="middle", weight="bold"))
        for i, letter in enumerate(letters):
            parts.append(text(origin + 82 + i * 57, 180, letter, 27, "muted", "middle", "bold", True))
            parts.append(text(origin + 19, 221 + i * 57, letter, 27, "muted", "middle", "bold", True))
            for j in range(4):
                value = data[i][j]
                color = "mint" if value > 0 else "orange"
                x, y = origin + 55 + j * 57, 187 + i * 57
                parts.append(rect(x, y, 53, 53, color, 7, f'fill-opacity="{.13 + .022 * abs(value)}"'))
                parts.append(text(x + 26.5, y + 35, f"{value:+d}".replace("-", "−"), 25, color, "middle", "bold", True))
    parts.extend([line(24, 445, 696, 445),
                  text(24, 485, "L ↔ I", 29, "mint", weight="bold", mono=True),
                  text(185, 485, "+2: observed above chance", 25, "mint"),
                  text(24, 529, "G ↔ W", 29, "orange", weight="bold", mono=True),
                  text(185, 529, "−2: observed below chance", 25, "orange"),
                  text(24, 582, "L leucine · I isoleucine", 23, "muted"),
                  text(24, 616, "G glycine · W tryptophan", 23, "muted"),
                  text(24, 661, "BLOSUM62 entries are rounded half-bit scores.", 22, "muted")])
    save("identity-and-blosum.svg", 720, 690, "Identity scores and BLOSUM62 protein scores",
         "For amino acids L, I, G and W, an identity matrix gives every mismatch minus one. "
         "BLOSUM62 instead gives leucine-isoleucine plus two and glycine-tryptophan minus two. "
         "The matrix rows and columns are L, I, G, W: 4,2,-4,-2; 2,4,-4,-3; "
         "-4,-4,6,-2; -2,-3,-2,11. These are protein scores, not nucleotide scores.", parts)


def pair_counts():
    parts = [text(24, 42, "One alignment column gives 15 pairs", 29, weight="bold"),
             text(24, 80, "Six sequences · unweighted teaching example", 23, "muted")]
    for i, letter in enumerate("AAAAIL"):
        parts.append(tile(42 + i * 108, 119, letter, "mint" if letter == "A" else "blue" if letter == "I" else "purple", 76))
    parts.extend([text(360, 236, "Each pair of sequences contributes once.", 25, anchor="middle"),
                  text(360, 279, "6 × 5 / 2 = 15", 29, "mint", "middle", "bold", True)])
    for i, (label, count, rule, color) in enumerate([
        ("A–A", 6, "4 × 3 / 2", "mint"), ("A–I", 4, "4 × 1", "blue"),
        ("A–L", 4, "4 × 1", "purple"), ("I–L", 1, "1 × 1", "orange")
    ]):
        y = 314 + i * 61
        parts.append(rect(24, y, 672, 51, "panel", 9))
        parts.append(text(43, y + 34, label, 26, color, weight="bold", mono=True))
        parts.append(rect(156, y + 13, count * 32, 25, color, 5, 'fill-opacity=".4"'))
        parts.append(text(400, y + 34, count, 27, color, "middle", "bold", True))
        parts.append(text(666, y + 34, rule, 24, "muted", "end", mono=True))
    parts.extend([text(24, 611, "Observed pair frequency: count / 15", 25, weight="bold"),
                  text(24, 651, "For example, q(A,I) = 4 / 15", 25, "blue", mono=True),
                  text(24, 700, "BLOSUM groups similar sequences before", 23, "muted"),
                  text(24, 734, "counting between groups, reducing redundancy.", 23, "muted")])
    save("column-pair-counts.svg", 720, 765, "Count unordered amino-acid pairs within a column",
         "Six unweighted aligned amino acids A A A A I L yield 15 unordered sequence pairs. "
         "There are six A-A pairs, four A-I pairs, four A-L pairs and one I-L pair. "
         "BLOSUM additionally groups similar sequences and counts between groups to reduce redundancy.", parts)


def seed_chain_align():
    parts = [text(24, 42, "Three stages, different things to score", 29, weight="bold")]
    for y in [79, 315, 579]:
        parts.append(rect(24, y, 672, 214 if y != 315 else 242, extra=f'stroke="{C["grid"]}"'))
    parts.extend([text(44, 117, "1  Find exact seeds", 28, "mint", weight="bold"),
                  text(44, 156, "Shared sequence words supply anchors.", 23, "muted")])
    for row, sequence in enumerate(["ACGTTGCA", "ACGATTGCA"]):
        for i, char in enumerate(sequence):
            exact_positions = [0, 1, 2, 4, 5, 6, 7] if row == 0 else [0, 1, 2, 5, 6, 7, 8]
            parts.append(tile(162 + i * 54, 172 + row * 55, char, "mint" if i in exact_positions else "muted", 45))
    parts.extend([text(44, 205, "Ref.", 22, "muted"), text(44, 260, "Query", 22, "muted"),
                  text(44, 354, "2  Chain anchor coordinates", 28, "blue", weight="bold"),
                  text(44, 393, "Reward coverage and charge for uneven gaps.", 23, "muted")])
    # Two one-dimensional coordinate tracks with corresponding exact seeds.
    parts.extend([line(160, 439, 658, 439, "muted"), line(160, 510, 658, 510, "muted"),
                  text(44, 447, "Ref.", 22, "muted"), text(44, 518, "Query", 22, "muted")])
    for top, bottom in [(190, 190), (290, 265), (445, 425), (615, 590)]:
        parts.append(line(top, 447, bottom, 502, "blue", 2, 'stroke-opacity=".45"'))
        parts.append(rect(top - 18, 430, 36, 18, "blue", 4))
        parts.append(rect(bottom - 18, 501, 36, 18, "blue", 4))
    parts.extend([text(44, 618, "3  Align the selected sequence regions", 27, "orange", weight="bold"),
                  text(44, 657, "Score individual letter pairs and gap runs.", 23, "muted")])
    for row, sequence in enumerate(["ACGT-TGCA", "ACGATTGCA"]):
        for i, char in enumerate(sequence):
            color = "orange" if i == 3 else "purple" if i == 4 else "mint"
            parts.append(tile(162 + i * 54, 674 + row * 55, char, color, 45))
    parts.extend([text(44, 707, "Ref.", 22, "muted"), text(44, 762, "Query", 22, "muted"),
                  text(24, 839, "For DNA: nucleotide match / mismatch scores.", 23, "muted"),
                  text(24, 875, "For proteins: matrices such as PAM or BLOSUM.", 23, "muted")])
    save("seed-chain-alignment.svg", 720, 905, "Where substitution scores enter seed and extend",
         "The first panel finds exact DNA words shared by reference and query. The second "
         "connects anchors using their coordinates and gaps. The third compares individual "
         "letters and gap runs within selected regions. Nucleotide alignment uses DNA scores; "
         "PAM and BLOSUM are protein substitution matrices.", parts)


def gap_penalties():
    parts = [text(24, 42, "Opening a gap and extending a gap", 29, weight="bold"),
             text(24, 81, "Illustrative costs, subtracted from alignment scores", 22, "muted")]
    x0, x1, y0, y1 = 78, 668, 450, 146
    def xp(v):
        return x0 + (v - 1) / 23 * (x1 - x0)
    def yp(v):
        return y0 - v / 50 * (y0 - y1)
    for value in [0, 10, 20, 30, 40, 50]:
        parts.append(line(x0, yp(value), x1, yp(value)))
        parts.append(text(x0 - 16, yp(value) + 8, value, 22, "muted", "end"))
    for value in [1, 5, 10, 15, 20, 24]:
        parts.append(text(xp(value), y0 + 33, value, 21, "muted", "middle"))
    parts.extend([text(x0, 129, "Cost", 23, "muted"), text(375, 528, "Gap length L", 24, "muted", "middle")])
    for color, func in [("purple", lambda v: 6), ("blue", lambda v: 2 * v),
                        ("orange", lambda v: 5 + v), ("mint", lambda v: min(5 + v, 15 + .25 * v))]:
        points = sorted([*range(1, 25), 40 / 3])
        path = " ".join(("M" if i == 0 else "L") + f"{xp(v):.2f},{yp(func(v)):.2f}" for i, v in enumerate(points))
        extra = 'stroke-dasharray="8 8"' if color == "mint" else ''
        parts.append(f'<path d="{path}" fill="none" stroke="{C[color]}" stroke-width="4" stroke-linecap="round" {extra}/>')
    for y, color, label in [(580, "purple", "Constant: 6"), (623, "blue", "Linear: 2L"),
                             (666, "orange", "Affine: 5 + L"), (709, "mint", "Two-piece: min(5 + L, 15 + 0.25L)")]:
        extra = 'stroke-dasharray="8 8"' if color == "mint" else ''
        parts.append(line(30, y - 8, 66, y - 8, color, 4, extra))
        parts.append(text(84, y, label, 24, color))
    parts.extend([rect(24, 746, 672, 127), text(44, 787, "With the single-affine convention:", 24, weight="bold"),
                  text(44, 824, "one 3-base gap costs 8", 24, "orange"),
                  text(44, 857, "three separate 1-base gaps cost 18", 24, "muted")])
    save("gap-penalties.svg", 720, 895, "Constant, linear, affine and two-piece affine gap costs",
         "An illustrative chart plots gap lengths 1 to 24. Constant cost stays at 6, "
         "linear cost is twice the length, and affine cost is 5 plus the length. "
         "A dashed two-piece affine curve takes the smaller of 5 plus length and "
         "15 plus one quarter of length; its slope changes at length 13 and one third. "
         "Under this affine convention a gap of length one costs 6, a gap of length "
         "three costs 8, and three separate gaps of length one cost 18.", parts)


if __name__ == "__main__":
    cover()
    identity_blosum()
    pair_counts()
    seed_chain_align()
    gap_penalties()
    print(f"Wrote 5 concept SVGs to {OUT}")
