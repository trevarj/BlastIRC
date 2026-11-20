#!/usr/bin/env python3
"""Rasterize the Short Fuse Club component atlas. Run in the icon-studio Nix shell."""
import json
from io import BytesIO
from pathlib import Path
from subprocess import run

from PIL import Image, ImageChops, ImageFilter

ROOT = Path(__file__).resolve().parent
TILE_SIZE = 96
COLUMNS = 16
BODIES = ('Moonshot', 'Low Orbit', 'Cut Corner', 'Dewdrop', 'High Rise')
FACES = ('Excited', 'Startled', 'Nervous', 'Queasy')
GEAR = ('Bare', 'Headphones', 'Pixel headband', 'Arcade cap', 'Goggles')
K, W = '#292634', '#fff1ce'


def path(d, c='none', s=K, w=3):
    return f'<path d="{d}" fill="{c}" stroke="{s}" stroke-width="{w}" stroke-linejoin="round" stroke-linecap="round"/>'


def el(x, y, rx, ry, c, s=K, w=3):
    return f'<ellipse cx="{x}" cy="{y}" rx="{rx}" ry="{ry}" fill="{c}" stroke="{s}" stroke-width="{w}"/>'


def edge(d, c, w):
    return path(d, 'none', c, w)


def solid(d, c):
    return path(d, c, 'none', 0)


def dot(x, y, r, c):
    return el(x, y, r, r, c, 'none', 0)


def group(svg, transform):
    return f'<g transform="{transform}">{svg}</g>'


def shade(c):
    # Same shell response in every channel under a fixed cool cel shadow.
    return '#b9b6c0' if c == '#fff' else '#13101a'


def shell(d, c, shadow):
    clip = f'<defs><clipPath id="body"><path d="{d}"/></clipPath></defs>'
    return path(d, c) + clip + f'<g clip-path="url(#body)">{solid(shadow, shade(c))}</g>' + edge(d, K, 3)


# Outline, shadow cut, graphic highlight. All three follow the shell's geometry.
SHELLS = (
    ('M60 28 A40 40 0 1 1 60 108 A40 40 0 1 1 60 28Z',
     'M15 70 Q37 106 75 87 Q99 75 97 46 L112 115 H10Z',
     'M33 49 Q37 41 45 38'),
    ('M33 48 H85 C102 48 111 59 111 75 C111 92 100 101 85 101 H33 C18 101 9 91 9 75 C9 58 19 48 33 48Z',
     'M4 76 Q12 90 30 90 H84 Q102 90 114 67 V110 H2Z',
     'M19 64 Q23 55 33 55 H42'),
    ('M43 31 H75 L98 52 V85 L77 106 H43 L21 85 V52Z',
     'M75 31 L88 56 V80 L72 95 H46 L21 85 L24 113 H105 V31Z',
     'M29 54 L45 39 H52'),
    ('M57 29 Q61 22 65 31 C71 46 78 48 88 63 C107 91 89 108 60 108 C33 108 15 93 30 68 Q40 51 50 42Z',
     'M22 82 Q34 103 65 96 Q90 90 85 58 L108 91 L86 114 H19Z',
     'M34 74 Q37 65 42 60'),
    ('M33 36 C33 23 87 23 87 36 V98 C87 112 33 112 33 98Z',
     'M78 25 V93 Q65 105 28 97 V116 H94 V24Z',
     'M39 50 V61'),
)


def body(i, c):
    d, shadow, highlight = SHELLS[i]
    art = shell(d, c, shadow) + edge(highlight, W, 3.5)
    if i == 1:
        art += edge('M27 53 Q10 71 24 90 M93 53 Q109 70 96 90', K, 2.4)
    elif i == 2:
        art += edge('M75 34 L88 55 L95 53 M87 81 L96 85 M73 95 L76 103', K, 2)
    elif i == 4:
        art += edge('M34 36 C42 45 78 45 86 36 M34 92 Q60 101 86 92', K, 3)
    return art


def fuse(i):
    x, y, lean = [(52,37,-1), (76,49,1), (52,38,-1), (60,37,-1), (53,37,-1)][i]
    stem = 'M0 2 C2 -9 16 -12 18 -23'
    star = path('M18 -34 L21 -27 30 -27 25 -20 27 -14 19 -16 13 -11 12 -19 5 -23 14 -26Z', '#ff963e', K, 2.8)
    star += solid('M18 -29 L20 -24 24 -23 20 -21 17 -17 16 -22 13 -23 17 -25Z', '#fff0a1')
    star = group(star, 'translate(20 -17) scale(1.24) translate(-18 22)')
    return group(edge(stem, K, 10) + edge(stem, '#f0bc74', 5) + star, f'translate({x} {y}) scale({lean} 1)')


# Expression centers and scale in each shell's local drawing.
SOCKETS = [(60,66,1.03), (60,72,1.02), (60,65,1.01), (60,76,.96), (60,66,.83)]


def face(i, m):
    fg, fill = K, W
    x, y, scale = SOCKETS[i]
    a = ''
    for n, u in enumerate((-15, 15)):
        if m == 0:
            a += edge(f'M{u-6} 0 q6 -10 12 -1', fg, 4.4)
        elif m == 1:
            a += el(u,-1,6.4,7,fill,fg,3.1) + dot(u+1,-.4,2.8,fg)
        elif m == 2:
            a += el(u,1,5.4,4.6,fill,fg,2.5) + dot(u+2,1,2.7,fg)
            a += edge(f'M{u-6} -5 l9 -5' if n == 0 else f'M{u-4} -10 l9 4', fg, 3.2)
        else:
            a += path(f'M{u-7} -2 l14 2 q-5 8 -11 4Z', fill, fg, 2.8)
    mouth = [path('M-9 6 Q0 9 11 3 Q10 17 1 17 -5 17 -9 6Z',fg,fg,2)
             + solid('M-3 13 Q2 9 7 11 3 17 0 15Z','#d98282'),
             el(0,11,4.8,6.8,fg,fg,2),
             path('M-9 13 Q-2 6 8 9 L9 14 Q3 12 -8 16Z',fill,fg,3),
             edge('M-11 10 Q-7 16 -2 11 T8 12 l3 1',fg,4.4)][m]
    if m == 2:
        a += path('M24 -10 q-6 8 -2 10 q7 2 2 -10Z', '#99d5df', K, 1.6)
    return group(a + mouth, f'translate({x} {y}) scale({scale})')


def gear(i, g):
    if g == 0:
        return '', ''
    if g == 1:
        x, y, w, h = [(60,66,40,42), (60,74,51,35), (60,66,39,42), (60,78,35,43), (60,66,28,44)][i]
        band = f'M{x-w} {y} Q{x-w} {y-h} {x} {y-h} T{x+w} {y}'
        cup = path('M-5 -10 Q0 -13 5 -10 L7 7 Q6 11 -4 11 L-7 8Z','#526675',K,2.5) + edge('M-1 -6 l2 10','#9ce2d4',4)
        return edge(band,K,8) + edge(band,'#a0dedb',4), ''.join(group(cup, f'translate({p} {y})') for p in (x-w, x+w))
    if g == 2:
        x, y, w = [(60,47,1.08), (60,55,1.41), (60,46,1), (60,54,.62), (60,46,.86)][i]
        band = path('M-32 -2 Q-9 -7 29 -5 L33 1 Q5 0 -30 7Z','#8864ad',K,2.5)
        band += solid('M-13 -4 h7 v7 h-7z M-6 -5 h7 v4 h-7z','#f6d2ee')
        return '', group(band, f'translate({x} {y}) scale({w} 1)')
    if g == 3:  # Asymmetric peaked cap, pitched away from the spark.
        x, y, s, angle = [(68,37,.94,12), (48,51,1,-7), (68,36,.9,7), (67,39,.78,18), (66,35,.86,8)][i]
        cap = path('M-23 0 Q-24 -15 -13 -19 Q1 -24 10 -18 Q17 -12 17 -2Z', '#d77a61')
        cap += edge('M-9 -17 Q-4 -14 -3 -6', '#ffc890', 3)
        cap += path('M-23 -1 Q-3 -8 15 -5 Q26 -6 32 -1 Q31 3 19 4 Q-3 -1 -22 5Z', '#d7a270', K, 2.8)
        return '', group(cap, f'translate({x} {y}) rotate({angle}) scale({s})')
    # Frames surround the expression rather than hiding it with an opaque visor.
    x, y, s = SOCKETS[i]
    frames = edge('M-33 -6 l7 5 M-5 -1 q5 -5 10 0 M26 -1 l7 -5', K, 4.8)
    for u in (-15, 15):
        lens = 'M-11 -8 Q-4 -12 10 -8 L11 -2 Q10 8 3 9 Q-8 11 -11 3Z'
        frames += group(path(lens, '#c5dcd3', K, 5) + edge(lens,'#d3a665',2.5), f'translate({u} 0)')
    return '', group(frames, f'translate({x} {y}) scale({s})')


def render(svg, rim=None):
    if not svg:
        return Image.new('RGBA', (TILE_SIZE, TILE_SIZE))
    source = f'<svg xmlns="http://www.w3.org/2000/svg" width="480" height="480" viewBox="0 0 120 120">{svg}</svg>'
    png = run(['rsvg-convert'], input=source.encode(), stdout=-1, check=True).stdout
    art = Image.open(BytesIO(png)).convert('RGBA')
    if rim:
        color, size = rim
        outline = Image.new('RGBA', art.size, color)
        outline.putalpha(art.getchannel('A').filter(ImageFilter.MaxFilter(size)))
        art = Image.alpha_composite(outline, art)
    # Area averaging in premultiplied space, then back to straight alpha. Avoid
    # negative-lobe ringing, which would tint pixels outside the shell.
    return art.resize((TILE_SIZE, TILE_SIZE), Image.Resampling.BOX)


def main():
    layers = {k: [] for k in ('fuse', 'body', 'tint', 'face', 'backGear', 'frontGear')}
    rim = ('#a8a2ab', 9)
    for i in range(len(BODIES)):
        layers['fuse'].append(render(fuse(i), rim))
        black, white = (render(body(i, c), rim) for c in ('#000', '#fff'))
        assert black.getchannel('A').tobytes() == white.getchannel('A').tobytes()
        # PNG stores grayscale response in RGB, even though only red is read.
        mask = ImageChops.subtract(white.getchannel('R'), black.getchannel('R'))
        layers['body'].append(black)
        layers['tint'].append(mask.convert('RGBA'))
        for m in range(len(FACES)):
            layers['face'].append(render(face(i, m), ('#d8c9af', 7)))
        for g in range(len(GEAR)):
            back, front = gear(i, g)
            layers['backGear'].append(render(back))
            layers['frontGear'].append(render(front))

    count = sum(map(len, layers.values()))
    atlas = Image.new('RGBA', (COLUMNS*TILE_SIZE, ((count+COLUMNS-1)//COLUMNS)*TILE_SIZE))
    layout = {}
    index = 0
    for group_name, images in layers.items():
        layout[group_name] = dict(offset=index, count=len(images))
        for image in images:
            atlas.paste(image, ((index % COLUMNS)*TILE_SIZE, (index // COLUMNS)*TILE_SIZE))
            index += 1
    atlas.save(ROOT / 'atlas.png')
    provenance = {
        'version': 3,
        'collection': 'Short Fuse Club',
        'origin': 'Original fictional BlastIRC bomb mascots, drawn as procedural SVG paths by GPT-6.2-Astra. No stock assets or fonts.',
        'design_reference': 'output/avatar-design/bomb-shell-cast.png (native-size contact sheet; not required to regenerate)',
        'generator': 'assets/foundry/generate.py; pinned icon-studio Nix shell, librsvg and Pillow',
        'bodies': BODIES, 'faces': FACES, 'gear': GEAR,
        'render': '120-unit drawing at 480px, area-averaged to 96px straight-alpha RGBA. Composite fuse, backGear, tinted body, frontGear, face.',
        'tint': 'Add round(tint.red * nicknameRGB / 255) to black-shell body RGB, clamped, retaining body alpha. Tint response is white-shell red minus black-shell red.',
        'indices': 'Fuse, body and tint use body index. Face uses body * faceKinds + face. Back/front gear use body * gearKinds + gear.',
        'atlas': dict(file='atlas.png', width=atlas.width, height=atlas.height, tile_size=TILE_SIZE, columns=COLUMNS, tile_count=count, layout=layout),
    }
    (ROOT / 'provenance.json').write_text(json.dumps(provenance, indent=2) + '\n')
    constants = dict(tileSize=TILE_SIZE, columns=COLUMNS, tileCount=count, width=atlas.width, height=atlas.height,
                     bodyKinds=len(BODIES), faceKinds=len(FACES), gearKinds=len(GEAR))
    for name, span in layout.items():
        constants[name+'Offset'] = span['offset']
        constants[name+'Count'] = span['count']
    header = '// Generated by assets/foundry/generate.py; do not edit.\n#pragma once\n#include <cstddef>\n\nnamespace blast::foundryAtlas {\n'
    header += ''.join(f'inline constexpr std::size_t {name} = {value};\n' for name, value in constants.items())
    header += '} // namespace blast::foundryAtlas\n'
    (ROOT.parents[1] / 'src/ui/foundry_atlas_data.hpp').write_text(header)


if __name__ == '__main__':
    main()
