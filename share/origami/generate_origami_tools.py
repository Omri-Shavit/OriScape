#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Build origami-tools.svg, the resource OriScape's "Origami tools" tab reads, from
origami-library.svg (the diagrammer's style reference).

Every tool's look comes from a labeled element in the library, so restyle the library and rerun:

    python3 generate_origami_tools.py

Needs inkex (share/extensions in this repo, or any Inkscape install's Python).

Output layout:
  <defs>  paper swatches, arrowhead markers and symbols, ready to copy into a document
  <origami:tool .../> per tool, in palette order, with its final style in CSS px
"""

import copy
import math
import os
import re
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.join(HERE, "..", "extensions"))

import inkex  # noqa: E402
from inkex import Style, Transform  # noqa: E402
from lxml import etree  # noqa: E402

LIBRARY = os.path.join(HERE, "origami-library.svg")
OUTPUT = os.path.join(HERE, "origami-tools.svg")

ORIGAMI_NS = "https://github.com/Omri-Shavit/OriScape/ns/origami"
OSB_NS = "http://www.openswatchbook.org/uri/2009/osb"
INK = "#231f20"
ARROW_WIDTH_PX = 0.7 * 96 / 72  # arrows are always 0.7pt

STROKE_KEYS = (
    "stroke", "stroke-width", "stroke-linecap", "stroke-linejoin", "stroke-miterlimit",
    "stroke-dasharray", "stroke-dashoffset", "stroke-opacity",
)

# key, name, category, "more" section, id prefix for drawn objects, library label(s)
TOOLS = [
    ("white", "White", "paper", False, "whitePaper", dict(label="whiteSwatch", color="#ffffff")),
    ("gray25", "Gray25", "paper", False, "gray25Paper", dict(label="gray25Swatch", color="#c8c7c7")),
    ("gray15", "Gray15", "paper", True, "gray15Paper", dict(label="gray15Swatch", color="#deddde")),
    ("gray32", "Gray32", "paper", True, "gray32Paper", dict(label="gray32Swatch", color="#b9b7b8")),
    ("gray40", "Gray40", "paper", True, "gray40Paper", dict(label="gray40Swatch", color="#a7a5a6")),
    ("gray50", "Gray50", "paper", True, "gray50Paper", dict(label="gray50Swatch", color="#918f90")),
    ("gray65", "Gray65", "paper", True, "gray65Paper", dict(label="gray65Swatch", color="#706d6e")),
    ("black", "Black", "paper", True, "blackPaper", dict(label="blackSwatch", color="#231f20")),

    ("valley", "Valley", "lines", False, "valleyFold", dict(label="valleyLine")),
    ("mountain", "Mountain", "lines", False, "mountainFold", dict(label="mountainLine")),
    ("crease", "Crease", "lines", False, "crease", dict(label="creaseLine")),
    ("xray", "X-ray", "lines", False, "xrayLine", dict(label="xrayLine")),
    ("smallValley", "Small valley", "lines", False, "smallValleyFold", dict(label="sValleyLine")),
    ("smallMountain", "Small mountain", "lines", False, "smallMountainFold", dict(label="sMountainLine")),
    ("bold", "Bold", "lines", False, "boldLine", dict(label="boldLine")),
    ("normal", "Normal", "lines", True, "normalLine", dict(label="normalLine")),
    ("whiteLine", "White", "lines", True, "whiteLine", dict(label="whiteLine")),

    ("fold", "Fold", "arrows", False, "foldArrow", dict(end="foldArrowHead")),
    ("unfold", "Unfold", "arrows", False, "unfoldArrow", dict(end="unfoldArrowHead")),
    ("foldUnfold", "Fold and unfold", "arrows", False, "foldUnfoldArrow",
     dict(end="foldArrowHead", start="unfoldArrowHead")),
    ("smallFold", "Small fold", "arrows", True, "smallFoldArrow", dict(end="sFoldArrowHead")),
    ("smallUnfold", "Small unfold", "arrows", True, "smallUnfoldArrow", dict(end="sUnfoldArrowHead")),
    ("smallFoldUnfold", "Small fold and unfold", "arrows", True, "smallFoldUnfoldArrow",
     dict(end="sFoldArrowHead", start="sUnfoldArrowHead")),

    ("dot", "Dot", "symbols", False, "dotSymbol", dict(label="dotSymbol")),
    ("finger", "Finger", "symbols", False, "fingerSymbol", dict(label="fingerSymbol")),
    ("pushIn", "Push in", "symbols", False, "pushInSymbol", dict(label="pushInSymbol")),
    ("turnOver", "Turn over", "symbols", False, "turnOverSymbol", dict(label="turnOverSymbol")),
]


def fmt(value):
    return ("%.4f" % value).rstrip("0").rstrip(".") or "0"


def number(value):
    match = re.match(r"\s*([-+]?[0-9]*\.?[0-9]+(?:[eE][-+]?[0-9]+)?)", str(value))
    return float(match.group(1)) if match else 0.0


class Library:
    def __init__(self, path):
        self.svg = inkex.load_svg(path).getroot()
        self.uu = self.svg.unittouu("1px")
        self.by_label = {}
        for el in self.svg.descendants():
            label = el.get("inkscape:label")
            if label and label not in self.by_label:
                self.by_label[label] = el

    def get(self, label):
        el = self.by_label.get(label)
        if el is None:
            sys.exit("origami-library.svg has no element labeled '%s'" % label)
        return el

    def px_per_unit(self, el):
        t = el.composed_transform()
        return math.sqrt(abs(t.a * t.d - t.b * t.c)) / self.uu

    def stroke_px(self, label):
        el = self.get(label)
        style = Style({k: v for k, v in Style(el.get("style") or "").items() if k in STROKE_KEYS})
        return scale_stroke(style, self.px_per_unit(el))

    def fill(self, label, default):
        el = self.by_label.get(label)
        fill = Style(el.get("style") or "").get("fill") if el is not None else None
        return fill if fill and fill.startswith("#") else default

    def shapes(self, label):
        el = self.get(label)
        if not isinstance(el, inkex.Group):
            return [el]
        return [d for d in el.descendants()
                if isinstance(d, inkex.ShapeElement) and not isinstance(d, inkex.Group)]


def scale_stroke(style, factor):
    out = Style(style)
    if "stroke-width" in out:
        out["stroke-width"] = fmt(number(out["stroke-width"]) * factor)
    dash = out.get("stroke-dasharray")
    if dash and dash.strip() != "none":
        parts = [p for p in re.split(r"[\s,]+", dash.strip()) if p]
        out["stroke-dasharray"] = ",".join(fmt(number(p) * factor) for p in parts)
    if "stroke-dashoffset" in out:
        out["stroke-dashoffset"] = fmt(number(out["stroke-dashoffset"]) * factor)
    return out


def line_style(stroke, **extra):
    style = Style({"fill": "none", "marker-start": "none", "marker-mid": "none",
                   "marker-end": "none", "stroke-dasharray": "none"})
    style.update(stroke)
    style.update(extra)
    return style


def make_swatch(key, name, color):
    grad = etree.Element(inkex.addNS("linearGradient", "svg"))
    grad.set("id", "origami-paper-" + key)
    grad.set(inkex.addNS("swatch", "inkscape"), "solid")
    grad.set("{%s}paint" % OSB_NS, "solid")
    grad.set(inkex.addNS("label", "inkscape"), "Paper " + name)
    stop = etree.SubElement(grad, inkex.addNS("stop", "svg"))
    stop.set("offset", "0")
    stop.set("style", "stop-color:%s;stop-opacity:1" % color)
    return grad


def make_marker(lib, head, at_start):
    """Same structure Object > Objects to Marker produces; drawn in stroke-width units."""
    marker_id = "origami-%s-%s" % (head, "start" if at_start else "end")
    el = lib.get(head)
    path = el.path.transform(el.composed_transform()).transform(Transform(scale=1.0 / lib.uu))
    tip = max(path.end_points, key=lambda p: p.x)
    path = path.translate(-tip.x, -tip.y).transform(Transform(scale=1.0 / ARROW_WIDTH_PX))
    if at_start:
        path = path.transform(Transform(rotate=180))
    style = Style(el.get("style") or "")
    style["stroke-width"] = "1"
    style["stroke-dasharray"] = "none"

    marker = etree.Element(inkex.addNS("marker", "svg"))
    for k, v in (("id", marker_id), ("orient", "auto"), ("refX", "0"), ("refY", "0"),
                 ("style", "overflow:visible"), (inkex.addNS("stockid", "inkscape"), marker_id),
                 (inkex.addNS("isstock", "inkscape"), "true"),
                 (inkex.addNS("label", "inkscape"), head + (" (start)" if at_start else ""))):
        marker.set(k, v)
    child = etree.SubElement(marker, inkex.addNS("path", "svg"))
    child.set("d", str(path))
    child.set("style", str(style))
    return marker_id, marker


def make_symbol(lib, key, name, label, defs_out, pad=1.0):
    """A <symbol> whose content is plain paths in a px box starting at 0,0."""
    shapes = lib.shapes(label)
    boxes, parts = [], []
    for shape in shapes:
        path = shape.path.transform(shape.composed_transform()).transform(
            Transform(scale=1.0 / lib.uu))
        style = Style(shape.get("style") or "")
        factor = lib.px_per_unit(shape)
        style = scale_stroke(style, factor)
        for prop in ("fill", "stroke"):
            match = re.match(r"url\(#([^)]+)\)", style.get(prop, ""))
            if match:
                style[prop] = "url(#%s)" % import_paint(lib, match.group(1), defs_out)
        parts.append((path, style))
        box = path.bounding_box()
        half = number(style.get("stroke-width", 0)) / 2 if style.get("stroke", "none") != "none" else 0
        boxes.append((box.left - half, box.top - half, box.right + half, box.bottom + half))
    left = min(b[0] for b in boxes) - pad
    top = min(b[1] for b in boxes) - pad
    width = max(b[2] for b in boxes) + pad - left
    height = max(b[3] for b in boxes) + pad - top

    symbol = etree.Element(inkex.addNS("symbol", "svg"))
    symbol.set("id", "origami-symbol-" + key)
    symbol.set("viewBox", "0 0 %s %s" % (fmt(width), fmt(height)))
    symbol.set("style", "overflow:visible")
    symbol.set(inkex.addNS("label", "inkscape"), name)
    title = etree.SubElement(symbol, inkex.addNS("title", "svg"))
    title.text = name
    for i, (path, style) in enumerate(parts):
        child = etree.SubElement(symbol, inkex.addNS("path", "svg"))
        child.set("id", "origami-symbol-%s-%d" % (key, i))
        child.set("d", str(path.translate(-left, -top)))
        child.set("style", str(style))
    return symbol, width, height


def import_paint(lib, source_id, defs_out):
    new_id = "origami-lib-" + source_id
    if new_id not in defs_out:
        source = lib.svg.getElementById(source_id)
        grad = copy.deepcopy(source)
        grad.set("id", new_id)
        href = grad.get(inkex.addNS("href", "xlink")) or grad.get("href")
        if href and href.startswith("#"):
            grad.set(inkex.addNS("href", "xlink"), "#" + import_paint(lib, href[1:], defs_out))
        defs_out[new_id] = grad
    return new_id


def main():
    lib = Library(LIBRARY)
    defs = {}  # id -> element, in insertion order
    tools = []
    paper_stroke = lib.stroke_px("whiteSwatch")

    for key, name, category, more, prefix, extra in TOOLS:
        attrs = {"key": key, "name": name, "category": category, "more": "true" if more else "false",
                 "prefix": prefix}
        if category == "paper":
            color = lib.fill(extra["label"], extra["color"])
            defs["origami-paper-" + key] = make_swatch(key, name, color)
            style = line_style(paper_stroke, fill="url(#origami-paper-%s)" % key)
            style["fill-opacity"] = "1"
            attrs.update(color=color, swatch="origami-paper-" + key)
        elif category == "lines":
            stroke = lib.stroke_px(extra["label"])
            if key == "whiteLine":
                stroke["stroke"] = "#ffffff"
            style = line_style(stroke)
        elif category == "arrows":
            style = line_style({"stroke": INK, "stroke-width": fmt(ARROW_WIDTH_PX),
                                "stroke-linecap": "round", "stroke-linejoin": "round",
                                "stroke-opacity": "1"})
            marker_id, marker = make_marker(lib, extra["end"], at_start=False)
            defs[marker_id] = marker
            style["marker-end"] = "url(#%s)" % marker_id
            if extra.get("start"):
                marker_id, marker = make_marker(lib, extra["start"], at_start=True)
                defs[marker_id] = marker
                style["marker-start"] = "url(#%s)" % marker_id
        else:
            symbol, width, height = make_symbol(lib, key, name, extra["label"], defs)
            defs[symbol.get("id")] = symbol
            style = Style()
            attrs.update(symbol=symbol.get("id"), width=fmt(width), height=fmt(height))
        attrs["style"] = str(style)
        tools.append(attrs)

    nsmap = {None: inkex.NSS["svg"], "inkscape": inkex.NSS["inkscape"],
             "xlink": inkex.NSS["xlink"], "osb": OSB_NS, "origami": ORIGAMI_NS}
    root = etree.Element(inkex.addNS("svg", "svg"), nsmap=nsmap)
    root.set("version", "1.1")
    root.append(etree.Comment(" Generated by generate_origami_tools.py from origami-library.svg; "
                              "do not edit by hand. "))
    defs_el = etree.SubElement(root, inkex.addNS("defs", "svg"))
    defs_el.set("id", "origami-defs")
    for el in defs.values():
        defs_el.append(el)
    group = etree.SubElement(root, "{%s}tools" % ORIGAMI_NS)
    for attrs in tools:
        tool = etree.SubElement(group, "{%s}tool" % ORIGAMI_NS)
        for k, v in attrs.items():
            tool.set(k, v)

    etree.ElementTree(root).write(OUTPUT, pretty_print=True, xml_declaration=True, encoding="UTF-8")
    print("wrote %s (%d tools, %d defs)" % (OUTPUT, len(tools), len(defs)))


if __name__ == "__main__":
    main()
