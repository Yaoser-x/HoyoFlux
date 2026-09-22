"""Regenerate PNG and multi-resolution ICO assets from the logo geometry."""

from pathlib import Path
from PIL import Image, ImageDraw


ROOT = Path(__file__).resolve().parents[1]
ASSETS = ROOT / "assets"
SCALE = 4
SIZE = 512


def cubic(p0, p1, p2, p3, steps=120):
    points = []
    for i in range(steps + 1):
        t = i / steps
        u = 1.0 - t
        points.append((
            (u**3 * p0[0] + 3 * u * u * t * p1[0] +
             3 * u * t * t * p2[0] + t**3 * p3[0]) * SCALE,
            (u**3 * p0[1] + 3 * u * u * t * p1[1] +
             3 * u * t * t * p2[1] + t**3 * p3[1]) * SCALE,
        ))
    return points


def gradient(size, start, end):
    image = Image.new("RGBA", (size, size))
    pixels = image.load()
    for y in range(size):
        for x in range(size):
            amount = (x + y) / (2 * (size - 1))
            pixels[x, y] = tuple(
                round(start[channel] * (1 - amount) + end[channel] * amount)
                for channel in range(4)
            )
    return image


def stroke_layer(points, width, start, end):
    canvas_size = SIZE * SCALE
    layer = gradient(canvas_size, start, end)
    mask = Image.new("L", (canvas_size, canvas_size), 0)
    draw = ImageDraw.Draw(mask)
    draw.line(points, fill=255, width=width * SCALE)
    radius = width * SCALE // 2
    # Pillow can leave tiny seams at dense floating-point line joins. Filling
    # each sampled join keeps the ribbon solid at every icon size.
    for x, y in points:
        draw.ellipse((x - radius, y - radius, x + radius, y + radius), fill=255)
    layer.putalpha(mask)
    return layer


def build_logo():
    canvas_size = SIZE * SCALE
    image = Image.new("RGBA", (canvas_size, canvas_size), (0, 0, 0, 0))
    background = gradient(canvas_size, (18, 40, 69, 255), (5, 13, 27, 255))
    rounded = Image.new("L", (canvas_size, canvas_size), 0)
    ImageDraw.Draw(rounded).rounded_rectangle(
        (20 * SCALE, 20 * SCALE, 492 * SCALE, 492 * SCALE),
        radius=112 * SCALE,
        fill=255,
    )
    background.putalpha(rounded)
    image.alpha_composite(background)

    image.alpha_composite(stroke_layer(
        cubic((154, 405), (154, 262), (166, 162), (271, 105)),
        58, (61, 124, 255, 255), (71, 230, 255, 255)))
    image.alpha_composite(stroke_layer(
        cubic((358, 107), (326, 203), (326, 310), (360, 407)),
        58, (139, 244, 255, 255), (103, 107, 255, 255)))
    image.alpha_composite(stroke_layer(
        cubic((180, 272), (226, 222), (274, 300), (333, 247)),
        52, (216, 251, 255, 255), (135, 161, 255, 255)))
    glow = ImageDraw.Draw(image)
    glow.ellipse((162 * SCALE, 248 * SCALE, 184 * SCALE, 270 * SCALE),
                 fill=(234, 255, 255, 210))

    return image.resize((SIZE, SIZE), Image.Resampling.LANCZOS)


def main():
    ASSETS.mkdir(parents=True, exist_ok=True)
    logo = build_logo()
    logo.save(ASSETS / "hoyoflux-logo.png", optimize=True)
    logo.save(
        ASSETS / "hoyoflux.ico",
        format="ICO",
        sizes=[(16, 16), (24, 24), (32, 32), (48, 48),
               (64, 64), (128, 128), (256, 256)],
    )


if __name__ == "__main__":
    main()
