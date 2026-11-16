"""ロゴの案を SVG で書き出す。

劣化した部分は、図形を 64x64 のマスで描いたうえで、フィルターと同じ JPEG の計算 (YCbCr への変換、色差の 2x2 の間引き、
8x8 の DCT、Annex K の量子化テーブル、逆 DCT) をかけて作る。外部のライブラリは使わない。
使い方: python generate-logos.py <出力先のフォルダー>
"""

import math
import sys
from pathlib import Path

SIZE = 512
GRID = 64
CELL = SIZE // GRID

LUMA = [
    16, 11, 10, 16, 24, 40, 51, 61, 12, 12, 14, 19, 26, 58, 60, 55,
    14, 13, 16, 24, 40, 57, 69, 56, 14, 17, 22, 29, 51, 87, 80, 62,
    18, 22, 37, 56, 68, 109, 103, 77, 24, 35, 55, 64, 81, 104, 113, 92,
    49, 64, 78, 87, 103, 121, 120, 101, 72, 92, 95, 98, 112, 100, 103, 99,
]
CHROMA = [17, 18, 24, 47, 99, 99, 99, 99, 18, 21, 26, 66, 99, 99, 99, 99,
          24, 26, 56, 99, 99, 99, 99, 99, 47, 66, 99, 99, 99, 99, 99, 99] + [99] * 32


def scaled(table, quality):
    scale = 5000 // quality if quality < 50 else 200 - quality * 2
    return [min(255, max(1, (v * scale + 50) // 100)) for v in table]


BASIS = [[(1 / math.sqrt(2) if u == 0 else 1.0) / 2 * math.cos((2 * x + 1) * u * math.pi / 16)
          for x in range(8)] for u in range(8)]


def dct_quantize(plane, width, height, table, shift):
    """plane の 8x8 のブロックに DCT・量子化・逆 DCT をかける。格子を shift だけずらし、外は端の値を複製する"""
    out = [row[:] for row in plane]
    for by in range(-shift, height, 8):
        for bx in range(-shift, width, 8):
            block = [[plane[min(max(by + y, 0), height - 1)][min(max(bx + x, 0), width - 1)] for x in range(8)]
                     for y in range(8)]
            coef = [[sum(BASIS[v][y] * BASIS[u][x] * block[y][x] for y in range(8) for x in range(8))
                     for u in range(8)] for v in range(8)]
            for v in range(8):
                for u in range(8):
                    q = table[v * 8 + u]
                    coef[v][u] = math.floor(coef[v][u] / q + 0.5) * q
            for y in range(8):
                for x in range(8):
                    py, px = by + y, bx + x
                    if 0 <= py < height and 0 <= px < width:
                        out[py][px] = sum(BASIS[v][y] * BASIS[u][x] * coef[v][u] for v in range(8) for u in range(8))
    return out


def crunch(image, quality, passes):
    """image は GRID x GRID の (r, g, b)。フィルターと同じ順の処理を passes 回かける"""
    shifts = [0, 6, 10, 4, 14, 2, 12, 8]
    n = len(image)
    for i in range(passes):
        ys = [[0.299 * r + 0.587 * g + 0.114 * b - 128 for (r, g, b) in row] for row in image]
        half = n // 2
        cb = [[0.0] * half for _ in range(half)]
        cr = [[0.0] * half for _ in range(half)]
        for y in range(half):
            for x in range(half):
                px = [image[2 * y + dy][2 * x + dx] for dy in (0, 1) for dx in (0, 1)]
                r, g, b = (sum(p[c] for p in px) / 4 for c in range(3))
                cb[y][x] = -0.168736 * r - 0.331264 * g + 0.5 * b
                cr[y][x] = 0.5 * r - 0.418688 * g - 0.081312 * b
        ys = dct_quantize(ys, n, n, scaled(LUMA, quality), shifts[i])
        cb = dct_quantize(cb, half, half, scaled(CHROMA, quality), shifts[i] // 2)
        cr = dct_quantize(cr, half, half, scaled(CHROMA, quality), shifts[i] // 2)
        result = []
        for y in range(n):
            row = []
            for x in range(n):
                lum = ys[y][x] + 128
                b_, r_ = cb[y // 2][x // 2], cr[y // 2][x // 2]
                rgb = (lum + 1.402 * r_, lum - 0.344136 * b_ - 0.714136 * r_, lum + 1.772 * b_)
                row.append(tuple(min(255, max(0, math.floor(c + 0.5))) for c in rgb))
            result.append(row)
        image = result
    return image


def landscape(x, y):
    """simple-mosaic のロゴと同じ風景 (空・太陽・丘)。x と y は 0-512 の座標"""
    t = y / SIZE
    color = (round(0x78 + (0x28 - 0x78) * t), round(0xC8 + (0x46 - 0xC8) * t), round(0xFF + (0xA0 - 0xFF) * t))
    if (x - 300) ** 2 + (y - 230) ** 2 < 110 ** 2:
        color = (0xFF, 0xD6, 0x5A)
    hill = 380 + 40 * math.sin(math.pi * x / 368) if x < 368 else 420 - 60 * (x - 368) / 144
    if y > hill:
        s = (y - 340) / 172
        color = (round(0x46 + (0x1E - 0x46) * s), round(0xAA + (0x6E - 0xAA) * s), round(0x6E + (0x46 - 0x6E) * s))
    return color


def sample(scene):
    return [[scene(x * CELL + CELL / 2, y * CELL + CELL / 2) for x in range(GRID)] for y in range(GRID)]


def cells_svg(image, x0=0, y0=0, cell=CELL, clip=None):
    """マスの色を、同じ色が横に続く範囲ごとに 1 つの rect にまとめて描く"""
    parts = []
    for y, row in enumerate(image):
        x = 0
        while x < len(row):
            end = x
            while end + 1 < len(row) and row[end + 1] == row[x]:
                end += 1
            r, g, b = row[x]
            parts.append(f'<rect x="{x0 + x * cell}" y="{y0 + y * cell}" width="{(end - x + 1) * cell}" '
                         f'height="{cell}" fill="#{r:02X}{g:02X}{b:02X}"/>')
            x = end + 1
    body = "".join(parts)
    return f'<g clip-path="url(#{clip})">{body}</g>' if clip else body


def crisp_landscape():
    path = " ".join(f"L {x},{380 + 40 * math.sin(math.pi * x / 368):.1f}" for x in range(0, 369, 8))
    return (
        '<rect width="512" height="512" fill="url(#sky)"/>'
        '<circle cx="300" cy="230" r="110" fill="#FFD65A"/>'
        f'<path d="M 0,512 {path} L 512,360 L 512,512 Z" fill="url(#hill)"/>'
    )


DEFS = (
    '<clipPath id="frame"><rect width="512" height="512" rx="72"/></clipPath>'
    '<linearGradient id="sky" x1="0" y1="0" x2="0" y2="512" gradientUnits="userSpaceOnUse">'
    '<stop offset="0" stop-color="#78C8FF"/><stop offset="1" stop-color="#2846A0"/></linearGradient>'
    '<linearGradient id="hill" x1="0" y1="340" x2="0" y2="512" gradientUnits="userSpaceOnUse">'
    '<stop offset="0" stop-color="#46AA6E"/><stop offset="1" stop-color="#1E6E46"/></linearGradient>'
)


def svg(title, desc, body, extra_defs=""):
    return (
        f'<svg xmlns="http://www.w3.org/2000/svg" width="512" height="512" viewBox="0 0 512 512" role="img" '
        f'aria-labelledby="title description"><title id="title">{title}</title>'
        f'<desc id="description">{desc}</desc><defs>{DEFS}{extra_defs}</defs>{body}</svg>\n'
    )


def logo_split(crunched):
    """案 A: simple-mosaic と同じ風景の右半分だけが JPEG で崩れている"""
    defs = '<clipPath id="right"><rect x="256" width="256" height="512"/></clipPath>'
    body = (f'<g clip-path="url(#frame)">{crisp_landscape()}{cells_svg(crunched, clip="right")}'
            '<rect x="252" width="8" height="512" fill="#FFFFFF" opacity="0.85"/></g>')
    return svg("Simple JPEG Crunch", "A landscape whose right half is broken into JPEG blocks.", body, defs)


def logo_full(crunched):
    """案 B: 風景の全体が JPEG で崩れている"""
    body = f'<g clip-path="url(#frame)">{cells_svg(crunched)}</g>'
    return svg("Simple JPEG Crunch", "A landscape fully broken into JPEG blocks.", body)


def logo_file(crunched):
    """案 C: 画像のファイルのアイコン。中の風景が崩れている"""
    inner = 320
    cell = inner // GRID
    defs = ('<clipPath id="page"><path d="M 96,40 L 336,40 L 416,120 L 416,472 L 96,472 Z"/></clipPath>')
    body = (
        '<rect width="512" height="512" rx="72" fill="#F4F1EA"/>'
        '<path d="M 96,40 L 336,40 L 416,120 L 416,472 L 96,472 Z" fill="#FFFFFF" stroke="#2B2B2B" '
        'stroke-width="12" stroke-linejoin="round"/>'
        f'<g clip-path="url(#page)">{cells_svg(crunched, 96, 120, cell)}</g>'
        '<path d="M 336,40 L 336,120 L 416,120 Z" fill="#D9D4C7" stroke="#2B2B2B" stroke-width="12" '
        'stroke-linejoin="round"/>'
        '<path d="M 96,40 L 336,40 L 416,120 L 416,472 L 96,472 Z" fill="none" stroke="#2B2B2B" '
        'stroke-width="12" stroke-linejoin="round"/>'
    )
    return svg("Simple JPEG Crunch", "An image file icon whose picture is broken into JPEG blocks.", body, defs)


def logo_stack(stages):
    """案 D: 3 枚の画像が重なり、手前ほど崩れている"""
    size = 300
    cell = size // GRID
    body = '<rect width="512" height="512" rx="72" fill="#22252B"/>'
    for i, image in enumerate(stages):
        x = 56 + i * 52
        y = 56 + i * 52
        body += (f'<rect x="{x - 8}" y="{y - 8}" width="{size + 16}" height="{size + 16}" rx="10" fill="#FFFFFF"/>'
                 f'{cells_svg(image, x, y, cell)}')
    return svg("Simple JPEG Crunch", "Three stacked pictures that get more broken toward the front.", body)


def sheet(names):
    """各案を明るい背景と暗い背景で、512px と 64px で並べた比較の一覧"""
    width = 40 + len(names) * 600
    parts = [f'<svg xmlns="http://www.w3.org/2000/svg" width="{width}" height="1300" viewBox="0 0 {width} 1300">',
             '<rect width="100%" height="650" fill="#F5F5F5"/><rect y="650" width="100%" height="650" fill="#1E1E1E"/>']
    for i, name in enumerate(names):
        x = 40 + i * 600
        for row, y in enumerate((40, 690)):
            color = "#222222" if row == 0 else "#EEEEEE"
            parts.append(f'<image href="{name}" x="{x}" y="{y}" width="512" height="512"/>')
            parts.append(f'<image href="{name}" x="{x}" y="{y + 530}" width="64" height="64"/>')
            parts.append(f'<text x="{x + 90}" y="{y + 570}" font-family="sans-serif" font-size="28" '
                         f'fill="{color}">{name}</text>')
    parts.append("</svg>\n")
    return "".join(parts)


def main():
    out = Path(sys.argv[1])
    out.mkdir(parents=True, exist_ok=True)
    scene = sample(landscape)
    files = {
        "logo-a-split.svg": logo_split(crunch(scene, 8, 3)),
        "logo-b-full.svg": logo_full(crunch(scene, 12, 3)),
        "logo-c-file.svg": logo_file(crunch(scene, 10, 3)),
        "logo-d-stack.svg": logo_stack([crunch(scene, 60, 1), crunch(scene, 20, 2), crunch(scene, 6, 4)]),
    }
    for name, text in files.items():
        (out / name).write_bytes(text.encode("utf-8"))
    (out / "logo-candidates.svg").write_bytes(sheet(list(files)).encode("utf-8"))


if __name__ == "__main__":
    main()
