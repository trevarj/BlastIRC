#!/usr/bin/env python3
"""Rebuild the approved Foundry component atlas and native layout constants."""
from hashlib import sha256
import json
from pathlib import Path

from PIL import Image, ImageDraw

ROOT = Path(__file__).resolve().parent
NICKS = 'trev ash cedar ember flux moss nova pixel quark rust spark willow'.split()
SCALE = 3
INK = '#20272b'
CREAM = '#f6dfad'
PALETTES = [
    ('graphite', '#485159', '#78868c', '#30373f'),
    ('copper', '#ae6342', '#e4a46b', '#713d32'),
    ('orange', '#e08332', '#ffc16b', '#9d4d28'),
    ('petrol', '#286e76', '#64a9a5', '#204850'),
    ('steel', '#6e8790', '#a4c0c4', '#405b65'),
    ('ochre', '#b28b3d', '#e7c26b', '#765a31'),
]


def traits(nick):
    # Default RFC1459 byte folding; non-ASCII bytes are preserved.
    folded = nick.translate(str.maketrans(
        'ABCDEFGHIJKLMNOPQRSTUVWXYZ[]\\^', 'abcdefghijklmnopqrstuvwxyz{}|~'))
    seed = sha256(('foundry-concept-1:' + folded).encode()).digest()
    return dict(seed=seed.hex(), body=seed[0] % 4, palette=seed[1] % 6,
                fuse=seed[2] % 4, spark=seed[3] % 3,
                accessory=seed[4] % 5, marking=seed[5] % 4)


def layer(kind, t):
    im = Image.new('RGBA', (96*SCALE, 96*SCALE))
    d = ImageDraw.Draw(im)
    _, base, high, shade = PALETTES[t['palette']]

    def box(xy):
        return tuple(round(v*SCALE) for v in xy)

    def line(points, fill, width=2):
        d.line([(round(x*SCALE), round(y*SCALE)) for x, y in points],
               fill=fill, width=round(width*SCALE), joint='curve')

    def poly(points, fill, outline=None, width=2):
        pts = [(round(x*SCALE), round(y*SCALE)) for x, y in points]
        d.polygon(pts, fill=fill)
        if outline:
            d.line(pts + [pts[0]], fill=outline, width=round(width*SCALE), joint='curve')

    def oval(xy, fill, outline=None, width=2):
        d.ellipse(box(xy), fill=fill, outline=outline, width=round(width*SCALE))

    def rect(xy, fill, radius=3, outline=None, width=2):
        d.rounded_rectangle(box(xy), radius=round(radius*SCALE), fill=fill,
                            outline=outline, width=round(width*SCALE))

    if kind == 'fuse':
        # Short, distinct rope routes; every spark stays inside the canvas.
        routes = [[(48, 32), (49, 21), (60, 15), (68, 16)],
                  [(48, 32), (40, 23), (31, 22), (26, 13)],
                  [(48, 32), (48, 17), (54, 10)],
                  [(48, 32), (60, 27), (68, 28), (75, 20)]]
        route = routes[t['fuse']]
        line(route, INK, 8)
        line(route, '#c6a578', 5)
        x, y = route[1]
        line([(x-2, y-2), (x+2, y+2)], '#806446', 1.5)
        x, y = route[-1]
        shapes = [
            [(0,-8),(2,-3),(7,-5),(4,0),(8,3),(2,4),(0,9),(-3,4),(-8,3),(-4,-1),(-6,-6),(-2,-4)],
            [(0,-8),(3,-2),(8,0),(3,3),(0,8),(-3,3),(-8,0),(-3,-2)],
            [(-1,-8),(4,-3),(2,0),(7,2),(-1,8),(0,3),(-6,2),(-2,-2)],
        ]
        poly([(x+dx, y+dy) for dx, dy in shapes[t['spark']]], '#efad45', INK, 1.5)
        oval((x-2,y-2,x+2,y+2), CREAM)

    if kind == 'body':
        body = t['body']
        # Pale outer edge reads on dark UI; inner ink edge reads on light UI.
        if body == 0:
            oval((14,29,82,91), CREAM)
            oval((16,31,80,89), base, INK)
            d.arc(box((20,35,76,85)), 25, 155, fill=shade, width=7*SCALE)
            d.arc(box((23,37,73,81)), 205, 264, fill=high, width=4*SCALE)
        elif body == 1:
            rect((8,35,88,87), CREAM, 19)
            rect((10,37,86,85), base, 17, INK)
            rect((18,70,78,81), shade, 5)
            line([(22,44),(36,42)], high, 4)
        elif body == 2:
            outer = [(28,29),(68,29),(84,45),(84,74),(69,89),(27,89),(12,74),(12,45)]
            poly(outer, CREAM)
            poly([(29,31),(67,31),(82,46),(82,73),(68,87),(28,87),(14,73),(14,46)], base, INK)
            poly([(16,70),(29,82),(67,82),(80,70),(80,76),(68,85),(28,85),(16,74)], shade)
            line([(21,45),(31,36),(42,36)], high, 4)
        else:
            rect((22,27,74,92), CREAM, 19)
            rect((24,29,72,90), base, 17, INK)
            rect((30,76,66,85), shade, 5)
            line([(31,45),(33,38),(39,35)], high, 4)

        rect((38,27,58,36), INK, 3)
        rect((41,28,55,32), '#a9a49a', 2)

    if kind == 'accessory':
        # A single large wearable or shell fitting, rather than noisy tiny props.
        accessory = t['accessory']
        if accessory == 0:  # Wide inset visor.
            rect((23,47,73,66), INK, 6)
            rect((27,50,69,62), '#c7e5df', 3)
            poly([(29,51),(42,51),(35,61),(29,61)], '#f2f5dd')
            line([(58,52),(65,52)], '#77a5a6', 2)
        elif accessory == 1:  # Riveted lower belt.
            rect((18,62,78,75), INK, 3)
            rect((20,65,76,72), '#ad8053', 2)
            rect((42,61,55,77), CREAM, 2, INK, 1.5)
            rect((46,65,51,72), shade, 1)
            for x in (26, 67):
                oval((x-2,67,x+2,71), CREAM)
        elif accessory == 2:  # Asymmetric bolted side plate.
            poly([(58,43),(75,46),(79,66),(69,79),(57,75)], INK)
            poly([(61,47),(72,49),(75,65),(68,74),(61,72)], '#ad8053')
            for x,y in ((65,52),(68,68)):
                oval((x-2,y-2,x+2,y+2), CREAM)
            line([(64,58),(70,60)], '#654939', 2)
        elif accessory == 3:  # Heavy shoulder collar and central seam.
            line([(23,42),(33,49),(63,49),(74,42)], INK, 11)
            line([(24,41),(34,46),(62,46),(73,41)], '#c5a170', 6)
            line([(48,50),(48,79)], shade, 3)
            rect((43,51,53,58), CREAM, 2, INK, 1)
        else:  # Twin oversized workshop goggles.
            line([(17,55),(79,55)], INK, 6)
            for x in (35,60):
                oval((x-11,44,x+11,67), '#bd9462', INK)
                oval((x-7,48,x+7,63), '#b4dbd4', INK, 1)
                line([(x-4,52),(x,50)], CREAM, 2)
            line([(46,54),(49,54)], CREAM, 2)

    if kind == 'mark':
        # Broad identifying marks, not text, faces, or unread-status-like dots.
        mark = t['marking']
        if t['accessory'] == 1:
            mx, my = 39, 52
        else:
            mx, my = 36, 77
        if mark == 0:
            line([(mx-4,my),(mx,my-4),(mx+4,my)], CREAM, 2)
        elif mark == 1:
            line([(mx-4,my+2),(mx-1,my-3)], CREAM, 2)
            line([(mx+2,my+2),(mx+5,my-3)], CREAM, 2)
        elif mark == 2:
            poly([(mx,my-4),(mx+4,my),(mx,my+4),(mx-4,my)], CREAM)
        else:
            rect((mx-5,my-2,mx+5,my+2), CREAM, 1)
    return im.resize((96,96), Image.Resampling.LANCZOS)


TILE_SIZE = 96
COLUMNS = 16
LAYOUT = {'fuse': (0, 12), 'body': (12, 24),
          'accessory': (36, 30), 'mark': (66, 8)}


def main():
    atlas = Image.new('RGBA', (1536, 480))
    for kind, (offset, count) in LAYOUT.items():
        for index in range(count):
            t = dict(body=0, palette=0, fuse=0, spark=0, accessory=0, marking=0)
            if kind == 'fuse':
                t['fuse'], t['spark'] = divmod(index, 3)
            elif kind == 'body':
                t['body'], t['palette'] = divmod(index, 6)
            elif kind == 'accessory':
                t['accessory'], t['palette'] = divmod(index, 6)
            else:
                t['accessory'], t['marking'] = divmod(index, 4)
            tile = offset + index
            # No mask: copy straight-alpha RGBA pixels unchanged into the atlas.
            atlas.paste(layer(kind, t), ((tile % COLUMNS)*TILE_SIZE,
                                       (tile // COLUMNS)*TILE_SIZE))
    atlas.save(ROOT / 'atlas.png')
    provenance = {
        'method': 'Pillow procedural raster; art/script GPT-6 Astra',
        'version': 1,
        'status': 'User-approved Foundry Bomb direction',
        'references': ['output/imagegen/blastirc-A7-charcoal-inset.png',
                       'output/imagegen/blastirc-A6-duotone.png',
                       'output/imagegen/blastirc-A5-industrial.png'],
        'identity': 'SHA-256 of foundry-concept-1: plus IRC-casefolded nick; default RFC1459 ASCII/punctuation folding; non-ASCII UTF-8 unchanged; finite combinations can collide',
        'trait_counts': dict(body=4, palette=6, fuse=4, spark=3, accessory=5, marking=4),
        'render': 'Each component drawn at 3x then Lanczos resized to straight-alpha RGBA 96x96; source-over fuse, body, accessory, mark at 96px',
        'atlas': dict(file='atlas.png', width=1536, height=480,
                      tile_size=TILE_SIZE, columns=COLUMNS, tile_count=74,
                      layout={k: dict(offset=v[0], count=v[1]) for k,v in LAYOUT.items()}),
        'indices': dict(fuse='fuse*3+spark', body='body*6+palette',
                        accessory='accessory*6+palette',
                        mark='(accessory == 1 ? 1 : 0)*4+marking'),
        'examples': [dict(nick=nick, **traits(nick)) for nick in NICKS],
    }
    (ROOT / 'provenance.json').write_text(json.dumps(provenance, indent=2) + '\n')
    constants = dict(tileSize=TILE_SIZE, columns=COLUMNS, tileCount=74,
                     width=1536, height=480)
    for name, (offset, count) in LAYOUT.items():
        constants[name+'Offset'] = offset
        constants[name+'Count'] = count
    header = '// Generated by assets/foundry/generate.py; do not edit.\n#pragma once\n#include <cstddef>\n\nnamespace blast::foundryAtlas {\n'
    header += ''.join(f'inline constexpr std::size_t {name} = {value};\n'
                      for name, value in constants.items())
    header += '} // namespace blast::foundryAtlas\n'
    (ROOT.parent.parent / 'src/ui/foundry_atlas_data.hpp').write_text(header)


if __name__ == '__main__':
    main()
