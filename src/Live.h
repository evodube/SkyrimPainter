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

#include <cstdint>
#include <string>

struct ID3D11Texture2D;
struct ID3D11ShaderResourceView;

// Painting on the canvas that stands on the easel, in the world (render thread unless noted).
namespace Live
{
	// Where the canvas's painted face is on screen, in pixels: top-left, top-right, bottom-right,
	// bottom-left of the picture. False if the canvas isn't there or isn't in front of the camera.
	bool CanvasOnScreen(std::uint32_t a_canvas, bool a_portrait, float a_screenW, float a_screenH, float a_out[4][2]);

	// The canvas shows the studio's own texture, so every stroke is seen on it as it is painted.
	// a_dds is the session's own texture on that canvas (Items::ShowLive): only a texture of that name
	// is ever touched. Call every frame while painting; it hooks up as soon as the game has loaded it.
	// a_reliefTex / a_reliefSrv: the painting's relief (normal map), lent to the canvas's normal map the same way.
	void Attach(std::uint32_t a_canvas, const std::string& a_dds, ID3D11Texture2D* a_tex, ID3D11ShaderResourceView* a_srv,
		ID3D11Texture2D* a_reliefTex = nullptr, ID3D11ShaderResourceView* a_reliefSrv = nullptr);
	bool Attached();
	// Stop. a_wait: the painting was saved and the canvas is about to get its new texture - keep showing
	// ours until it has (no flash of the old picture in between).
	void Detach(bool a_wait);
	void Tick();  // every frame: finishes a waiting Detach

	// Main thread work, queued: first person, turned to face the canvas; and back again.
	void FaceCanvas(std::uint32_t a_canvas, bool a_portrait);
	void RestoreView();  // (also takes off the zoom)
	// Look 25% closer (a narrower field of view), or back. Main thread work, queued.
	void SetZoom(bool a_on);
}
