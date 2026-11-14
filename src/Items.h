// SkyrimPainter - paint your own pictures in Skyrim Special Edition
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

#pragma once

// The painter's things and the game side of painting:
//   Easel (book): use it in the inventory -> place it with a preview
//   Activate the easel -> menu: put a Blank Canvas on (landscape / portrait), put a painting back on to
//     edit it, turn the canvas, paint, take the canvas off, pick the easel up
//   Finish a painting -> it becomes one of 50 Painting items (each its own DDS)
//   Use a Painting in the inventory -> hang it on a wall
namespace Items
{
	bool Init();          // kDataLoaded: forms, recipe station, events
	void OnGameLoaded();  // puts the textures back on easels, renames the painting items
	void InstallSerialization();

	// ---- painting slots (50)
	struct SlotView
	{
		std::string name;
		std::string           dds;  // game path of its texture
		bool                  portrait = false;
		std::filesystem::path dir;  // its folder (flat.png is the picture)
	};
	bool GetSlot(int a_slot, SlotView& a_out);  // false if the slot is unused. Any thread.
	void EnsureSlotTexture(int a_slot);          // re-makes the DDS from the saved picture if it's missing
	void TakePaintingItem(int a_slot);           // main thread
	void GivePaintingItem(int a_slot);           // main thread
	void RenamePainting(int a_slot, std::string a_name);
	// What a merchant pays: the painting itself (RGBA) and the work in it (-1 = not counted)
	std::int32_t Appraise(const std::vector<std::uint8_t>& a_flat, int a_w, int a_h, int a_varnish, int a_strokes, int a_seconds);  // any thread: the slot and its item take the new name

	// ---- the easel
	RE::TESObjectACTI* EaselStandForm();
	void               EaselPlaced(RE::TESObjectREFR* a_stand);  // main thread (Place)

	struct EaselMenuInfo
	{
		RE::ObjectRefHandle easel;
		bool                hasCanvas = false;
		bool                portrait = false;
		int                 slot = -1;     // editing a finished painting
		std::string         name;
		bool                painted = false;
		bool                haveCanvasItem = false, havePalette = false, havePaints = false;
		int                 freeSlots = 0;
		std::vector<std::pair<int, std::string>> paintingsInPack;  // slot, name
	};
	std::optional<EaselMenuInfo> TakeEaselMenuRequest();  // any thread

	// Easel menu actions (any thread; the game work runs on the main thread)
	void SetCanvas(RE::ObjectRefHandle a_easel, bool a_portrait);
	void EditPainting(RE::ObjectRefHandle a_easel, int a_slot);
	void TurnCanvas(RE::ObjectRefHandle a_easel);
	void TakeCanvasOff(RE::ObjectRefHandle a_easel);
	void ScrapePainting(RE::ObjectRefHandle a_easel);  // editing a painting: it is scraped off, its slot freed
	void PickUpEasel(RE::ObjectRefHandle a_easel);
	void Paint(RE::ObjectRefHandle a_easel);

	// ---- the studio
	struct Work
	{
		RE::ObjectRefHandle   easel;
		std::filesystem::path dir;  // where the layers are kept while on the easel
		bool                  portrait = false;
		int                   slot = -1;
		std::string           name;
		RE::FormID            canvas = 0;  // the canvas reference on the easel (painting on it in the world)
	};
	std::optional<Work> TakeStudioRequest();  // any thread
	// The studio paints on the canvas in the world: the canvas gets a texture of its own for the session
	// (a_dds, a new name), made from a_flat; the studio then draws straight into it. Any thread.
	void ShowLive(RE::ObjectRefHandle a_easel, std::string a_dds, std::vector<std::uint8_t> a_flat, int a_w, int a_h, std::vector<std::uint8_t> a_normal);
	// After the studio saved the layers to Work::dir (worker thread):
	// a_normal: the painting's relief and shine (RGBA8, same size as a_flat; empty = flat canvas)
	void StepAway(RE::ObjectRefHandle a_easel, std::vector<std::uint8_t> a_flat, int a_w, int a_h, std::vector<std::uint8_t> a_normal);
	void Finish(RE::ObjectRefHandle a_easel, std::string a_name, std::vector<std::uint8_t> a_flat, int a_w, int a_h, std::vector<std::uint8_t> a_normal);

	void WriteString(SKSE::SerializationInterface* a_intfc, const std::string& a_s);
	bool ReadString(SKSE::SerializationInterface* a_intfc, std::string& a_s);
}
