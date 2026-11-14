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

// A painting on disk: a folder with one PNG per layer (layer0.png is the back), layers.txt
// (name, visible, opacity, blend, lock alpha per layer) and flat.png (everything together, what the DDS is made from).
// Paintings live in Documents\My Games\Skyrim Special Edition\SkyrimPainter.
namespace Store
{
	struct LayerFile
	{
		std::string               name;
		bool                      visible = true;
		float                     opacity = 1.0f;
		std::vector<std::uint8_t> px;  // RGBA, straight alpha
		int                       blend = 0;  // Paint::Blend
		bool                      lockAlpha = false;
	};

	struct Doc
	{
		int                       w = 0, h = 0;
		std::vector<LayerFile>    layers;
		std::vector<std::uint8_t> thickness;  // w x h, how thick the paint is (thickness.png; empty = flat)
		int                       varnish = 0;  // Paint::Varnish (meta.txt)
		int                       strokes = 0;  // the work in it (meta.txt; -1 = from before this was counted)
		int                       seconds = 0;  // time spent actually painting it
	};

	std::filesystem::path PaintingsFolder();  // finished paintings: Paintings\<id>
	std::filesystem::path WorkFolder();       // on an easel: InProgress\<id>

	bool                SaveDoc(const std::filesystem::path& a_dir, const Doc& a_doc, const std::vector<std::uint8_t>& a_flat, std::string& a_error);
	std::optional<Doc>  LoadDoc(const std::filesystem::path& a_dir);
	bool                LoadFlat(const std::filesystem::path& a_dir, std::vector<std::uint8_t>& a_px, int& a_w, int& a_h);
	// the paint's thickness and the varnish only (for making the relief texture)
	bool                LoadThickness(const std::filesystem::path& a_dir, std::vector<std::uint8_t>& a_px, int& a_w, int& a_h);
	int                 LoadVarnish(const std::filesystem::path& a_dir);
	void                LoadEffort(const std::filesystem::path& a_dir, int& a_strokes, int& a_seconds);  // -1, -1 if never counted
	bool                HasDoc(const std::filesystem::path& a_dir);
	// Replaces a_to with a copy of a_from.
	bool                CopyDoc(const std::filesystem::path& a_from, const std::filesystem::path& a_to);
	void                RemoveDoc(const std::filesystem::path& a_dir);
	// Turns every layer and the flat picture a quarter turn.
	bool                RotateDoc(const std::filesystem::path& a_dir, bool a_clockwise);
}
