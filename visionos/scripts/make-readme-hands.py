#!/usr/bin/env python3
# The README's bare-hand gesture drawings (docs/images/hands): TrevorbiltKit's hand art (drawn white,
# to be tinted, with its touch spark and motion arrows apart) with a skin tone baked in, since a
# picture on a web page can't pick one when it's shown. The launcher's Controls tab picks a tone at
# random each time it appears; here each drawing gets a different one of the same 24 (Crayola's
# Colors of the World skin tones, TrevorbiltKit's SkinTone.colorsOfTheWorld), so the page's hands
# are anyone's too.
#
#   visionos/scripts/make-readme-hands.py
import os
import re

HERE = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
KIT = os.path.join(HERE, "visionos/TrevorbiltKit/Sources/TrevorbiltKit/Resources/Hands.xcassets")
OUT = os.path.join(HERE, "docs/images/hands")

# The drawings the README uses, each with its tone (name, 0xRRGGBB, from SkinTone.swift).
DRAWINGS = {
    "hand-pinch-middle-left-move": ("Medium Golden", 0xDEA26C),
    "hand-pinch-middle-left": ("Extra Deep Almond", 0x6E5046),
    "hand-pinch-little-right-turn": ("Light Almond", 0xE6B9B3),
    "hand-pinch-index-right": ("Deep Golden", 0x8D5B28),
    "hand-swing-right": ("Light Medium Golden", 0xF0C9A2),
    "hand-pinch-index-left": ("Deep Rose", 0xB86F69),
    "hand-pinch-ring-left": ("Medium Almond", 0xD19C7D),
    "hand-pinch-little-left": ("Very Deep Almond", 0x88605E),
}


def hex_colour(rgb):
    return f"#{rgb:06X}"


def multiply(rgb, grey):
    """The kit's crease is light grey, multiplied by the tone: a darker shade of it."""
    k = grey / 255
    return tuple(round(((rgb >> s) & 0xFF) * k) for s in (16, 8, 0))


def svg(name):
    with open(os.path.join(KIT, f"{name}.imageset", f"{name}.svg")) as f:
        return f.read()


def main():
    os.makedirs(OUT, exist_ok=True)
    for name, (tone, rgb) in DRAWINGS.items():
        hand = svg(name)
        shade = "#%02X%02X%02X" % multiply(rgb, 0xD2)
        hand = hand.replace("#FFFFFF", hex_colour(rgb)).replace("#D2D2D2", shade)
        marks_path = os.path.join(KIT, f"{name}-marks.imageset", f"{name}-marks.svg")
        if os.path.exists(marks_path):
            with open(marks_path) as f:
                marks = re.search(r"<svg[^>]*>(.*)</svg>", f.read(), re.S).group(1)  # all of it
            hand = hand.replace("</svg>", marks + "</svg>")
        hand = hand.replace("<svg ", f"<svg role=\"img\" ", 1).replace(
            ">", f"><!-- TrevorbiltKit's {name}, in Crayola's {tone} -->", 1)
        with open(os.path.join(OUT, f"{name}.svg"), "w") as f:
            f.write(hand)
        print(f"{name}.svg: {tone}")


if __name__ == "__main__":
    main()
