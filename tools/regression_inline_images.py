#!/usr/bin/env python3
"""Host-side regression checks for inline EPUB image handling.

This deliberately mirrors small, pure logic slices from the firmware so we can
catch parser/pagination/draw regressions without ESP32 hardware. It is not a
substitute for hardware-in-the-loop validation of heap, SD, decoder, or display
behavior.
"""

from __future__ import annotations

import html
import re
import sys
from dataclasses import dataclass
from urllib.parse import unquote

IMG_MARKER_BYTE = "\x01"
IMG_MARKER_PREFIX = f"{IMG_MARKER_BYTE}IMG|"
IMG_CONT_MARKER = f"{IMG_MARKER_BYTE}IMGCONT{IMG_MARKER_BYTE}"


def is_marker(line: str) -> bool:
    return len(line) > 5 and line.startswith(IMG_MARKER_PREFIX)


def is_continuation(line: str) -> bool:
    return line == IMG_CONT_MARKER


@dataclass(frozen=True)
class ImageMarker:
    path: str
    width: int
    height: int
    lines: int


def parse_enriched_marker(line: str) -> ImageMarker | None:
    if not is_marker(line):
        return None
    payload = line[5:]
    end = payload.find(IMG_MARKER_BYTE)
    if end >= 0:
        payload = payload[:end]
    parts = payload.split("|")
    if len(parts) != 4:
        return None
    path, width, height, lines = parts
    try:
        marker = ImageMarker(path, int(width), int(height), int(lines))
    except ValueError:
        return None
    if not marker.path or marker.width <= 0 or marker.height <= 0 or marker.lines <= 0:
        return None
    return marker


def draw_line_advance(lines: list[str]) -> tuple[int, list[tuple[str, int, int]]]:
    """Mirror ui_reader_draw image/continuation line-advance behavior.

    Returns total line advances plus event tuples of (kind, consumed, skipped).
    """
    i = 0
    advanced = 0
    events: list[tuple[str, int, int]] = []
    while i < len(lines):
        line = lines[i]
        if is_marker(line):
            marker = parse_enriched_marker(line)
            if marker:
                consumed = max(1, marker.lines)
                advanced += consumed
                skipped = 0
                while i + 1 < len(lines) and is_continuation(lines[i + 1]):
                    i += 1
                    skipped += 1
                events.append(("image", consumed, skipped))
            else:
                advanced += 1
                events.append(("bad-image", 1, 0))
        elif is_continuation(line):
            advanced += 1
            events.append(("stray-continuation", 1, 0))
        else:
            advanced += 1
            events.append(("text", 1, 0))
        i += 1
    return advanced, events


_ATTR_RE = re.compile(
    r"(?P<name>[A-Za-z0-9_:-]+)(?:\s*=\s*(?:\"(?P<dq>[^\"]*)\"|'(?P<sq>[^']*)'|(?P<bare>[^\s>]+)))?"
)


def extract_image_attr(tag_content: str) -> str:
    """Mirror exact src/href/xlink:href matching from epub.cpp."""
    for match in _ATTR_RE.finditer(tag_content):
        name = match.group("name").lower()
        if name not in {"src", "href", "xlink:href"}:
            continue
        value = match.group("dq")
        if value is None:
            value = match.group("sq")
        if value is None:
            value = match.group("bare")
        if value is None:
            value = ""
        value = re.split(r"[?#]", value, maxsplit=1)[0]
        value = unquote(html.unescape(value)).strip()
        if value:
            return value
    return ""


def image_markers_from_html(fragment: str) -> list[str]:
    """Small fixture extractor matching the firmware's img/image marker intent."""
    markers: list[str] = []
    for match in re.finditer(r"<\s*(img|image)\b([^>]*)>", fragment, flags=re.I):
        src = extract_image_attr(match.group(2))
        if src:
            markers.append(f"{IMG_MARKER_PREFIX}{src}{IMG_MARKER_BYTE}")
    return markers


def check(name: str, condition: bool, detail: str = "") -> None:
    if not condition:
        raise AssertionError(f"{name} failed" + (f": {detail}" if detail else ""))
    print(f"ok - {name}")


def main() -> int:
    # Regression: image continuation markers must not be double-counted in draw.
    lines = [
        f"{IMG_MARKER_PREFIX}/cache/raw4_a_100x60.r4|100|60|3{IMG_MARKER_BYTE}",
        IMG_CONT_MARKER,
        IMG_CONT_MARKER,
        "following text",
    ]
    advanced, events = draw_line_advance(lines)
    check("draw skips image continuation markers", advanced == 4, f"advanced={advanced}, events={events}")
    check("draw recorded skipped continuations", events[0] == ("image", 3, 2), str(events))

    # Regression: malformed image markers stay safe and consume one line.
    advanced, events = draw_line_advance([f"{IMG_MARKER_PREFIX}bad{IMG_MARKER_BYTE}", "text"])
    check("malformed marker fallback is one line", advanced == 2, f"advanced={advanced}, events={events}")

    # Regression: stranded continuation markers are harmless.
    advanced, events = draw_line_advance([IMG_CONT_MARKER, "text"])
    check("stray continuation remains navigable", advanced == 2, f"advanced={advanced}, events={events}")

    # Regression: exact attribute scanner ignores src substrings in other names/values.
    check(
        "img parser ignores data-srcset before real src",
        extract_image_attr('data-srcset="bad.jpg 1x" src="good.jpg"') == "good.jpg",
    )
    check(
        "img parser ignores srcset before real src",
        extract_image_attr('srcset="bad.jpg 1x" src="good.jpg"') == "good.jpg",
    )
    check(
        "img parser ignores src in alt text",
        extract_image_attr('alt="src text" src="good.jpg"') == "good.jpg",
    )
    check(
        "img parser supports href",
        extract_image_attr('href="images/cover.jpg?cache=1#frag"') == "images/cover.jpg",
    )
    check(
        "img parser supports xlink href and decoding",
        extract_image_attr('xlink:href="Images/My%20Cover.jpg"') == "Images/My Cover.jpg",
    )

    markers = image_markers_from_html(
        '<p>front</p><img data-srcset="bad.jpg 1x" alt="src text" src="Images/My%20Cover.jpg?x=1#y"><p>body</p>'
    )
    check("html fixture emits decoded image marker", markers == [f"{IMG_MARKER_PREFIX}Images/My Cover.jpg{IMG_MARKER_BYTE}"], str(markers))

    print("all inline image host regressions passed")
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except AssertionError as exc:
        print(f"FAIL: {exc}", file=sys.stderr)
        raise SystemExit(1)
