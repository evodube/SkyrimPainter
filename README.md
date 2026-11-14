# SkyrimPainter

An SKSE plugin for **Skyrim Special Edition / Anniversary Edition** that lets you actually paint.
Set up an easel, put a canvas on it, paint with real-feeling brushes, mix your own colours on the palette, then hang
the finished painting on any wall.

Inspired by *Painting in Skyrim* (Iceburg and Blargargy), which let you craft ready-made paintings at an easel.
Here you paint the picture yourself. No assets or code from that mod are used.

---

## Requirements

| Requirement | Notes |
|---|---|
| Skyrim SE / AE | 1.5.97 and 1.6.x (Steam and GOG), one DLL. No VR. |
| [SKSE64](https://skse.silverlock.org/) | The build for your game version |
| [Address Library for SKSE Plugins](https://www.nexusmods.com/skyrimspecialedition/mods/32444) | SE file for 1.5.97, AE file for 1.6.x |

`SkyrimPainter.esp` is ESL-flagged and only needs `Skyrim.esm`.

---

## How to play

### 1. Craft your kit (tanning rack)
| Item | Cost (test values) |
|---|---|
| Easel | 3 Firewood, 2 Leather Strips |
| Blank Canvas | 2 Linen Wrap, 1 Firewood |
| Painter's Palette | 1 Firewood |
| Paints | 1 each: Charcoal, Bone Meal, Red Mountain Flower, Blue Mountain Flower, Dragon's Tongue |

### 2. Set up the easel
**Use the Easel in your inventory**: the inventory closes and a preview of the easel follows your aim.
Mouse wheel or Left/Right turns it (Shift: finer), left-click sets it up, right-click or Esc cancels.

### 3. Put a canvas on
**Activate the easel**. Its menu lets you:
- put on a **blank canvas, landscape or portrait** (needs a Blank Canvas)
- put one of your **finished paintings** back on to work on it more
- **turn the canvas** between landscape and portrait (the painting turns with it)
- **paint** (needs the Painter's Palette and the Paints)
- take the canvas off, scrape a painting off for good (frees its slot), or fold the easel back up

### 4. Paint
The studio shows the canvas with your **palette** on the right, paint already on it, and four **brushes** (detail round,
round, filbert, wide flat) with a **rag**. Everything is done with the **right mouse button**:
- right-click a **brush** to pick it up
- right-click a **paint** to load the brush; right-click a second paint and it **mixes on the brush** like real paint
  (blue and yellow make green - Spectral.js)
- right-click the **bare palette** to put your mix down there (it stays on your palette)
- **hold right-click on the canvas and move** to paint
- right-click the **rag** to wipe the brush clean

Painting feels like paint: the bristles leave streaks, the brush runs out of paint and goes dry (load it again),
wet paint on the layer mixes with the paint you drag through it, and **pressure** (slider, or the mouse wheel while
painting) changes how wide and how thick the stroke is.

**Layers**: the painting is built back to front - start with the background layer, add layers for what's in front.
Each layer can be shown/hidden, moved, made see-through or scraped. Undo / Redo: Ctrl+Z / Ctrl+Y.

- **Step away** (Esc) leaves the painting on the easel - you see it on the canvas in the world - to carry on later.
- **Finish painting**: name it. It is saved as a DDS and becomes a **Painting** in your pack; the easel is free again.

### 5. Hang it
**Use the Painting in your inventory** and aim at a wall: wheel or Left/Right sizes it, Up/Down moves it towards or
away from the wall, left-click hangs it. **Activate a hung painting** to resize, move or take it down.

To change a finished painting, take it down, put it on an easel and paint again: finishing it **replaces its DDS**
(and the copies on the walls). You can have **50 paintings** at once.

### Where your paintings go
Each painting is kept (layers and a flat PNG) in `Documents\My Games\Skyrim Special Edition\SkyrimPainter\Paintings`,
work in progress in `...\SkyrimPainter\InProgress`. The game textures are in `Data\textures\SkyrimPainter`.

---

## Settings
`Data\SKSE\Plugins\SkyrimPainter.ini`: text size, interface size, cursor speed. Mixed colours are kept there too.
The log is `Documents\My Games\Skyrim Special Edition\SKSE\SkyrimPainter.log`.

---

## Building
GitHub Actions builds on every push to `main` (Windows, Visual Studio 2022, vcpkg). The DLL and the full mod folder
come out as the `SkyrimPainter` artifact and on the `dist` branch.

The meshes, textures and the plugin file are made by scripts (Python 3 + numpy + Pillow):

```
python tools/convert_models.py ...  # easel, canvas, brush, palette meshes + UI pictures (see the script for the steps)
python tools/textures.py            # the blank canvas
python tools/make_esp.py            # SkyrimPainter.esp + src/Forms.h
python tools/gen_spectral.py spectral.js   # src/SpectralData.h from Spectral.js
```

---

## License
GPL-3.0 (see `LICENSE.txt`). Built on the plumbing of [Hearthframe](https://github.com/evodube/Hearthframe)
(same author, same license).

Third-party parts: CommonLibSSE-NG (GPL-3.0 with exceptions), Dear ImGui (MIT), stb (MIT / public domain),
Spectral.js colour mixing by Ronald van Wijnen (MIT, ported to C++ in `src/Spectral.cpp`),
Cinzel and Inter fonts (SIL Open Font License 1.1).

Art: the easel is "Easel / Easel with painting" by Every 3D and the paint brush is "CC0 Paint Brush 3" by plaggy
(both CC BY 4.0); the palette is "Round Artist Palette" by LUMI-Studio (CGTrader royalty free, paint cleaned off).
They are converted with `tools/convert_models.py` (details in `CREDITS.txt`); the canvas is the one from the
easel model.
