#!/usr/bin/env python3
"""
Seed a simulator SD card with five books, a recents list, and optionally a
Home theme.

Usage: make_sim_sd.py SD_DIR [THEME]

SD_DIR   folder to fill; the simulator uses it when started with
         CROSSPOINT_SIM_SD=SD_DIR
THEME    optional uiTheme number written to settings.json
         (0 Classic, 1 Lyra, 2 Lyra Extended, 3 RoundedRaff, 4 Cover Grid,
         5 Tabs + Recents, 6 Tabs + Carousel)

Existing books are kept. recent.json is rewritten every run, with empty cover
paths: Cover Grid and the tab homes fill them in, while the other themes show
a cover only after the book has been opened once.
"""

import io
import json
import sys
import zipfile
from pathlib import Path

if len(sys.argv) < 2 or any(arg in ("-h", "--help") for arg in sys.argv[1:]):
    print(__doc__.strip())
    sys.exit(0)

try:
    from PIL import Image, ImageDraw, ImageFont
except ImportError:
    print("Please install Pillow: pip install Pillow")
    sys.exit(1)

BOOKS = [
    ("Dune", "Frank Herbert", (30, 30, 30)),
    ("Neuromancer", "William Gibson", (90, 90, 90)),
    ("Foundation", "Isaac Asimov", (150, 150, 150)),
    ("Hyperion", "Dan Simmons", (60, 60, 60)),
    ("Solaris", "Stanislaw Lem", (120, 120, 120)),
]


def cover(title, author, shade):
    img = Image.new("RGB", (600, 900), shade)
    d = ImageDraw.Draw(img)
    font = ImageFont.load_default(size=80)
    small = ImageFont.load_default(size=44)
    d.rectangle([40, 40, 560, 860], outline="white", width=8)
    for i in range(0, 900, 60):
        d.line([(0, i), (600, i + 300)], fill=(255, 255, 255), width=3)
    d.rectangle([60, 330, 540, 570], fill="white")
    d.text((300, 410), title, fill="black", font=font, anchor="mm")
    d.text((300, 510), author, fill="black", font=small, anchor="mm")
    buf = io.BytesIO()
    img.save(buf, "JPEG", quality=85)
    return buf.getvalue()


def epub(path, title, author, shade):
    chapter = "".join(
        f"<p>Paragraph {i} of {title}. " + "Lorem ipsum dolor sit amet. " * 20 + "</p>" for i in range(60)
    )
    opf = f"""<?xml version="1.0"?>
<package xmlns="http://www.idpf.org/2007/opf" version="2.0" unique-identifier="id">
<metadata xmlns:dc="http://purl.org/dc/elements/1.1/"><dc:title>{title}</dc:title><dc:creator>{author}</dc:creator>
<dc:identifier id="id">{title}</dc:identifier><dc:language>en</dc:language><meta name="cover" content="cover"/></metadata>
<manifest><item id="cover" href="cover.jpg" media-type="image/jpeg"/>
<item id="c1" href="c1.xhtml" media-type="application/xhtml+xml"/>
<item id="ncx" href="toc.ncx" media-type="application/x-dtbncx+xml"/></manifest>
<spine toc="ncx"><itemref idref="c1"/></spine></package>"""
    ncx = f"""<?xml version="1.0"?><ncx xmlns="http://www.daisy.org/z3986/2005/ncx/" version="2005-1">
<head/><docTitle><text>{title}</text></docTitle><navMap><navPoint id="p1" playOrder="1">
<navLabel><text>Start</text></navLabel><content src="c1.xhtml"/></navPoint></navMap></ncx>"""
    container = (
        '<?xml version="1.0"?><container version="1.0" xmlns="urn:oasis:names:tc:opendocument:xmlns:container">'
        '<rootfiles><rootfile full-path="OEBPS/content.opf" media-type="application/oebps-package+xml"/>'
        "</rootfiles></container>"
    )
    with zipfile.ZipFile(path, "w") as z:
        z.writestr("mimetype", "application/epub+zip", compress_type=zipfile.ZIP_STORED)
        z.writestr("META-INF/container.xml", container)
        z.writestr("OEBPS/content.opf", opf)
        z.writestr("OEBPS/toc.ncx", ncx)
        z.writestr("OEBPS/cover.jpg", cover(title, author, shade))
        z.writestr(
            "OEBPS/c1.xhtml",
            f'<?xml version="1.0"?><html xmlns="http://www.w3.org/1999/xhtml"><body><h1>{title}</h1>{chapter}</body></html>',
        )


def main():
    sd = Path(sys.argv[1])
    theme = int(sys.argv[2]) if len(sys.argv) > 2 else None
    (sd / "books").mkdir(parents=True, exist_ok=True)
    (sd / ".crosspoint").mkdir(exist_ok=True)
    recents = []
    for title, author, shade in BOOKS:
        path = sd / "books" / f"{title}.epub"
        if not path.exists():
            epub(path, title, author, shade)
        recents.append({"path": f"/books/{title}.epub", "title": title, "author": author, "coverBmpPath": ""})
    (sd / ".crosspoint" / "recent.json").write_text(json.dumps({"books": recents}))
    if theme is not None:
        settings = sd / ".crosspoint" / "settings.json"
        data = json.loads(settings.read_text()) if settings.exists() else {}
        data["uiTheme"] = theme
        settings.write_text(json.dumps(data))
    print(f"Seeded {sd} with {len(BOOKS)} books")


if __name__ == "__main__":
    main()
