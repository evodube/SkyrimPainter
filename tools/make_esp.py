#!/usr/bin/env python3
"""Generates SkyrimPainter.esp (ESL-flagged, master Skyrim.esm) and src/Forms.h.

Records (local form IDs; the plugin finds them with TESDataHandler::LookupForm(id, "SkyrimPainter.esp")):
  MISC  0x800 Blank Canvas, 0x802 Painter's Palette, 0x803 Paints
  BOOK  0x801 Easel          - "reading" it (clicking it in the inventory) starts placing it
  BOOK  0x900 + i            - painting slot i (50): reading it starts hanging that painting
  ACTI  0x810 easel standing in the world (empty)
        0x811 / 0x812 canvas on an easel, landscape / portrait (placed with the easel's position and angle)
        0x820 / 0x821 painting on a wall, landscape / portrait
  COBJ  0x830.. recipes at the tanning rack (see RECIPES)

Run tools/convert_models.py first (it writes tools/bounds.json).
"""
import json, math, os, struct

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
PLUGIN = "SkyrimPainter.esp"
OWN = 0x01000000          # one master (Skyrim.esm) -> our own records are 01xxxxxx in the file
# Skyrim.esm items used in the recipes (IDs checked against UESP)
FIREWOOD = 0x0006F993
LEATHER_STRIPS = 0x000800E4
LINEN_WRAP = 0x00034CD6
CHARCOAL = 0x00033760         # the charcoal stick (tools)
BONE_MEAL = 0x00034CDD        # white
RED_MOUNTAIN_FLOWER = 0x00077E1D
BLUE_MOUNTAIN_FLOWER = 0x00077E1C
DRAGONS_TONGUE = 0x000889A2   # yellow
TANNING_RACK = 0x0007866A  # CraftingTanningRack keyword (the plugin double-checks it at runtime)
HIGH_POLY_NOTE = 0x00097788  # STAT shown by the book menu (a note) - the plugin closes it straight away
ITM_BOOK_UP = 0x000C7A54     # SNDR pick-up sound
VENDOR_ITEM_CLUTTER = 0x000914E9

ID_CANVAS, ID_EASEL, ID_PALETTE, ID_PAINTS = 0x800, 0x801, 0x802, 0x803
ID_EASEL_STAND, ID_EASEL_CANVAS_L, ID_EASEL_CANVAS_P = 0x810, 0x811, 0x812
ID_WALL_L, ID_WALL_P = 0x820, 0x821
ID_RECIPE_FIRST = 0x830
ID_PAINTING_FIRST, PAINTING_SLOTS = 0x900, 50

LICENSE_HEADER = """// SkyrimPainter - paint your own pictures in Skyrim Special Edition
// Copyright (C) 2026 evodube
//
// This program is free software: you can redistribute it and/or modify it under the terms of the
// GNU General Public License as published by the Free Software Foundation, either version 3 of the
// License, or (at your option) any later version.
//
// This program is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY; without
// even the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU
// General Public License for more details.
//
// You should have received a copy of the GNU General Public License along with this program
// (LICENSE.txt). If not, see <https://www.gnu.org/licenses/>.
"""


def sub(tag, data): return tag.encode() + struct.pack("<H", len(data)) + data
def zstr(s): return s.encode("cp1252") + b"\0"
def u8(v): return struct.pack("<B", v)
def u16(v): return struct.pack("<H", v)
def u32(v): return struct.pack("<I", v)
def i32(v): return struct.pack("<i", v)
def f32(v): return struct.pack("<f", v)
def record(tag, formid, payload, flags=0):
    return tag.encode() + struct.pack("<IIIIHH", len(payload), flags, formid, 0, 44, 0) + payload
def group(label, payload):
    return b"GRUP" + struct.pack("<I4sIIHH", 24 + len(payload), label.encode(), 0, 0, 0, 0) + payload


def main():
    data = json.load(open(os.path.join(ROOT, "tools", "bounds.json")))
    bounds = data["bounds"]

    def obnd(stem):
        b = bounds[stem]
        return sub("OBND", struct.pack("<6h", *[int(math.floor(v)) for v in b["min"]], *[int(math.ceil(v)) for v in b["max"]]))

    def model(stem): return sub("MODL", zstr(f"SkyrimPainter\\{stem}.nif"))

    def book(fid, edid, name, stem, desc, value, weight):
        return record("BOOK", OWN | fid,
                      sub("EDID", zstr(edid)) + obnd(stem) + sub("FULL", zstr(name)) + model(stem) +
                      sub("DESC", zstr(desc)) + sub("YNAM", u32(ITM_BOOK_UP)) +
                      sub("KSIZ", u32(1)) + sub("KWDA", u32(VENDOR_ITEM_CLUTTER)) +
                      # flags, type (0 book; the note art keeps the book menu small), teaches nothing, value, weight
                      sub("DATA", u8(0) + u8(0) + u16(0) + i32(-1) + u32(value) + f32(weight)) +
                      sub("INAM", u32(HIGH_POLY_NOTE)) + sub("CNAM", zstr("")))

    count = 0
    acti = b""
    for fid, edid, name, stem, verb in [
        (ID_EASEL_STAND, "SP_EaselStand", "Easel", "Easel", "Use"),
        (ID_EASEL_CANVAS_L, "SP_EaselCanvasLandscape", "Easel", "EaselCanvas_Landscape", "Use"),
        (ID_EASEL_CANVAS_P, "SP_EaselCanvasPortrait", "Easel", "EaselCanvas_Portrait", "Use"),
        (ID_WALL_L, "SP_WallCanvasLandscape", "Painting", "WallCanvas_Landscape", "Examine"),
        (ID_WALL_P, "SP_WallCanvasPortrait", "Painting", "WallCanvas_Portrait", "Examine"),
    ]:
        acti += record("ACTI", OWN | fid, sub("EDID", zstr(edid)) + obnd(stem) + sub("FULL", zstr(name)) + model(stem) + sub("RNAM", zstr(verb)))
        count += 1

    books = book(ID_EASEL, "SP_Easel", "Easel", "Item_Easel", "A folding easel. Use it to set it up.", 15, 5.0)
    count += 1
    for i in range(PAINTING_SLOTS):
        books += book(ID_PAINTING_FIRST + i, f"SP_Painting{i + 1:02}", "Painting", "Item_Canvas",
                      "One of your paintings. Use it to hang it on a wall.", 50, 2.0)
        count += 1

    misc = b""
    items = [(ID_CANVAS, "SP_Canvas", "Blank Canvas", "Item_Canvas", 8, 1.0),
             (ID_PALETTE, "SP_Palette", "Painter's Palette", "Item_Palette", 10, 0.5),
             (ID_PAINTS, "SP_Paints", "Paints", "Item_Paint", 20, 0.5)]
    for fid, edid, name, stem, value, weight in items:
        misc += record("MISC", OWN | fid, sub("EDID", zstr(edid)) + obnd(stem) + sub("FULL", zstr(name)) + model(stem) +
                       sub("YNAM", u32(ITM_BOOK_UP)) + sub("DATA", struct.pack("<If", value, weight)))
        count += 1

    # at the tanning rack: (made item, editor name, [(item, count), ...])
    RECIPES = [
        (ID_CANVAS, "Canvas", [(LINEN_WRAP, 2), (FIREWOOD, 1)]),                       # cloth stretched on a frame
        (ID_EASEL, "Easel", [(FIREWOOD, 3), (LEATHER_STRIPS, 2)]),                     # wood, lashed together
        (ID_PALETTE, "Palette", [(FIREWOOD, 1)]),                                      # a board of wood
        (ID_PAINTS, "Paints", [(CHARCOAL, 1), (BONE_MEAL, 1), (RED_MOUNTAIN_FLOWER, 1),  # black, white, red,
                               (BLUE_MOUNTAIN_FLOWER, 1), (DRAGONS_TONGUE, 1)]),        # blue, yellow pigments
    ]
    cobj = b""
    for i, (fid, edid, parts) in enumerate(RECIPES):
        cnto = b"".join(sub("CNTO", u32(item) + i32(n)) for item, n in parts)
        cobj += record("COBJ", OWN | (ID_RECIPE_FIRST + i),
                       sub("EDID", zstr("SP_Recipe" + edid)) + sub("COCT", u32(len(parts))) + cnto +
                       sub("CNAM", u32(OWN | fid)) + sub("BNAM", u32(TANNING_RACK)) + sub("NAM1", u16(1)))
        count += 1

    groups = [("ACTI", acti), ("BOOK", books), ("MISC", misc), ("COBJ", cobj)]
    next_id = ID_PAINTING_FIRST + PAINTING_SLOTS
    tes4 = (sub("HEDR", struct.pack("<fiI", 1.70, count + len(groups), next_id)) + sub("CNAM", zstr("evodube")) +
            sub("SNAM", zstr("SkyrimPainter - paint on a canvas at your own easel (needs SkyrimPainter.dll)")) +
            sub("MAST", zstr("Skyrim.esm")) + sub("DATA", b"\0" * 8))
    esp = record("TES4", 0, tes4, flags=0x200) + b"".join(group(l, p) for l, p in groups)
    open(os.path.join(ROOT, PLUGIN), "wb").write(esp)

    c = data["canvas"]
    L = [LICENSE_HEADER, "#pragma once", "// Generated by tools/make_esp.py - keep in sync with SkyrimPainter.esp.", "",
         "namespace Forms", "{",
         f'\tinline constexpr std::string_view kPlugin = "{PLUGIN}"sv;',
         f"\tinline constexpr std::uint32_t kCanvas = 0x{ID_CANVAS:X};",
         f"\tinline constexpr std::uint32_t kEasel = 0x{ID_EASEL:X};          // BOOK",
         f"\tinline constexpr std::uint32_t kPalette = 0x{ID_PALETTE:X};",
         f"\tinline constexpr std::uint32_t kPaints = 0x{ID_PAINTS:X};",
         f"\tinline constexpr std::uint32_t kEaselStand = 0x{ID_EASEL_STAND:X};",
         f"\tinline constexpr std::uint32_t kEaselCanvas[2] = {{ 0x{ID_EASEL_CANVAS_L:X}, 0x{ID_EASEL_CANVAS_P:X} }};  // landscape, portrait",
         f"\tinline constexpr std::uint32_t kWallCanvas[2] = {{ 0x{ID_WALL_L:X}, 0x{ID_WALL_P:X} }};",
         f"\tinline constexpr std::uint32_t kPaintingFirst = 0x{ID_PAINTING_FIRST:X};  // BOOK, one per slot",
         f"\tinline constexpr int           kPaintingSlots = {PAINTING_SLOTS};",
         f"\tinline constexpr std::uint32_t kRecipeFirst = 0x{ID_RECIPE_FIRST:X};",
         "\tinline constexpr int           kRecipeCount = 4;",
         f"\tinline constexpr RE::FormID    kTanningRackKeyword = 0x{TANNING_RACK:08X};",
         "",
         "\t// canvas size in game units (the painting is 1024 x 768, or 768 x 1024 standing up)",
         f"\tinline constexpr float kCanvasW = {c['canvasW']:.1f}f;",
         f"\tinline constexpr float kCanvasH = {c['canvasH']:.1f}f;",
         f"\tinline constexpr float kCanvasCentreUp = {c['centreUp']:.1f}f;  // height of the canvas's middle on the easel (landscape)",
         "}", ""]
    open(os.path.join(ROOT, "src", "Forms.h"), "w").write("\n".join(L))
    print(f"wrote {PLUGIN} ({len(esp)} bytes, {count} records) and src/Forms.h")


if __name__ == "__main__":
    main()
