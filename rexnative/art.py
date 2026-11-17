"""Steam artwork for a game's Deck shortcut, made from one of its own captures.

  python -m rexnative art WORK/GAME CAPTURE.png [--title "Name"]

Writes <work>/deck/art/: portrait.png (600x900, the library's capsule), grid.png (920x430, the
wide capsule), hero.png (1920x620, the game page's banner) and icon.png (256x256), which the Deck
kit sends with the game (art/) and install.sh hands to steam_shortcuts.py (--art-dir). CAPTURE is
a frame of the game, best its title screen: a renderer capture (live.ps1 frame, the kit's
deck/try-*.png) or a window capture (live.ps1 shot, whose frame and title bar are cut off). The
capsule shows the capture over a blurred fill of itself, with the game's name under it.
Needs Pillow.
"""
import os

PORTRAIT, GRID, HERO, ICON = (600, 900), (920, 430), (1920, 620), (256, 256)


def _client(im):
    """A window capture's client area: the 16:9 picture under the title bar, inside the frame."""
    w, h = im.size
    if abs(w / h - 16 / 9) < 0.01:
        return im
    cw = w - 16 if w > 16 else w  # Windows 10/11: 8 px of frame each side
    ch = round(cw * 9 / 16)
    top = max(0, h - 8 - ch)
    return im.crop((8, top, 8 + cw, top + ch)) if ch <= h else im


def _cover(im, size, focus=0.5):
    """`im` scaled to fill `size`, cut around `focus` (0 top, 1 bottom) of the height."""
    from PIL import Image
    w, h = im.size
    s = max(size[0] / w, size[1] / h)
    im = im.resize((max(size[0], round(w * s)), max(size[1], round(h * s))), Image.LANCZOS)
    x = (im.width - size[0]) // 2
    y = round((im.height - size[1]) * focus)
    return im.crop((x, y, x + size[0], y + size[1]))


def _font(size):
    from PIL import ImageFont
    windir = os.environ.get('WINDIR', 'C:/Windows')
    for name in ('segoeuib.ttf', 'arialbd.ttf', 'DejaVuSans-Bold.ttf'):
        for d in (os.path.join(windir, 'Fonts'), '/usr/share/fonts/TTF', '/usr/share/fonts/truetype/dejavu'):
            p = os.path.join(d, name)
            if os.path.exists(p):
                return ImageFont.truetype(p, size)
    return ImageFont.load_default()


def _wrap(draw, text, font, width):
    lines, line = [], ''
    for word in text.split():
        trial = (line + ' ' + word).strip()
        if line and draw.textlength(trial, font=font) > width:
            lines.append(line)
            line = word
        else:
            line = trial
    if line:
        lines.append(line)
    return lines


def portrait(im, title):
    from PIL import Image, ImageDraw, ImageEnhance, ImageFilter
    back = _cover(im, PORTRAIT).filter(ImageFilter.GaussianBlur(18))
    back = ImageEnhance.Brightness(back).enhance(0.45)
    front = im.resize((PORTRAIT[0], round(PORTRAIT[0] * im.height / im.width)), Image.LANCZOS)
    y = 250
    back.paste(front, (0, y))
    d = ImageDraw.Draw(back)
    size = 64
    while True:
        font = _font(size)
        lines = _wrap(d, title, font, PORTRAIT[0] - 60)
        if len(lines) <= 3 or size <= 36:
            break
        size -= 6
    ty = y + front.height + 40
    for line in lines:
        tw = d.textlength(line, font=font)
        d.text(((PORTRAIT[0] - tw) / 2, ty), line, font=font, fill=(255, 255, 255),
               stroke_width=3, stroke_fill=(0, 0, 0))
        ty += round(size * 1.2)
    return back


def make(work, capture, title=None, log=print):
    from PIL import Image
    if not title:
        from .deck import title_of
        title = title_of(work)
    im = _client(Image.open(capture).convert('RGB'))
    out = os.path.join(work, 'deck', 'art')
    os.makedirs(out, exist_ok=True)
    portrait(im, title).save(os.path.join(out, 'portrait.png'), optimize=True)
    _cover(im, GRID).save(os.path.join(out, 'grid.png'), optimize=True)
    _cover(im, HERO, focus=0.4).save(os.path.join(out, 'hero.png'), optimize=True)
    side = min(im.size)
    icon = im.crop(((im.width - side) // 2, 0, (im.width + side) // 2, side))
    icon.resize(ICON, Image.LANCZOS).save(os.path.join(out, 'icon.png'), optimize=True)
    log('%s: %s (from %s)' % (title, out, os.path.basename(capture)))
    return out
