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

#include "Spectral.h"

#include <cstdint>
#include <deque>
#include <string>
#include <vector>

// The paint engine: layers composited back to front over the canvas, and brushes that behave like
// real ones. The backbone follows Krita's: bristle tips that leave streaks, pressure that splays them,
// opacity (how thick the paint is) kept apart from flow (paint per dab), wet paint that mixes
// (spectrally) with what is under it, a clean brush that smears (the colour smudge engine), a rag that
// lifts paint (the eraser), the canvas weave catching a light touch (the texture option), and a stroke
// helper that steadies the hand and tapers the ends (smoothing / stabiliser, fade and speed sensors).
// No game or UI code, so it can be tested on its own.
namespace Paint
{
	using Rgb = Spectral::Rgb;

	enum class Kind : int
	{
		Bristle = 0,  // the brushes: streaky bristle marks, wet paint mixes
		Fan = 1,      // a few bristles fanned wide apart: broken, feathery marks (grass, leaves, clouds)
		Charcoal = 2, // dry, grainy, always "loaded" with charcoal; no thickness, doesn't mix
		Knife = 3,    // palette knife: loaded, a flat hard-edged slab of thick paint; clean, it scrapes paint off
	};
	struct BrushType
	{
		const char* name;
		float       radius;     // canvas pixels at full pressure
		float       aspect;     // tip width across the stroke / along it (flat brushes > 1)
		float       flow;       // paint laid down per dab
		float       unused;     // (was: how fast it ran out of paint)
		Kind        kind = Kind::Bristle;
	};
	inline constexpr int       kBrushCount = 7;   // 0-3 brushes, 4 fan, 5 charcoal, 6 palette knife
	inline constexpr int       kFan = 4, kCharcoal = 5, kKnife = 6;
	inline constexpr Rgb       kCharcoalColour{ 38, 36, 34 };
	extern const BrushType     kBrushes[kBrushCount];

	struct Brush
	{
		int   type = -1;     // -1 = no brush in hand
		Rgb   color{};       // paint on the brush
		float load = 0.0f;   // 0 = clean, 1 = paint on it (never runs out; the rag wipes it)
		int   size = 2;      // which of the kSizes this brush is (a bigger or smaller brush of the same kind)
		bool  Loaded() const { return type >= 0 && load > 0.5f; }
	};
	inline constexpr int   kSizeCount = 5;
	inline constexpr float kSizes[kSizeCount] = { 0.5f, 0.7f, 1.0f, 1.4f, 2.0f };

	// How a stroke goes down.
	struct StrokeStyle
	{
		float body = 1.0f;   // paint thickness: 0.1 = a thin glaze .. 1 = full body (caps how much one stroke covers)
		bool  lift = false;  // the rag: takes paint off the current layer instead of putting it on
	};

	// Layer blending: paint sits on top, or glazes (multiplies) over what is under it.
	enum class Blend : int
	{
		Normal = 0,
		Glaze = 1,
	};

	struct Layer
	{
		std::string               name;
		bool                      visible = true;
		float                     opacity = 1.0f;
		std::vector<std::uint8_t> px;  // RGBA, straight alpha
		Blend                     blend = Blend::Normal;
		bool                      lockAlpha = false;  // only paint over paint that is already on this layer
	};

	struct Rect
	{
		int x0 = 0, y0 = 0, x1 = 0, y1 = 0;  // [x0, x1) x [y0, y1)
		bool Empty() const { return x1 <= x0 || y1 <= y0; }
		void Add(int a_x0, int a_y0, int a_x1, int a_y1);
	};

	inline constexpr int kMaxLayers = 8;

	class Painting
	{
	public:
		// a_blank: the bare canvas (RGBA, a_w x a_h). Starts with one empty layer.
		void Reset(int a_w, int a_h, const std::vector<std::uint8_t>& a_blank);
		int  Width() const { return m_w; }
		int  Height() const { return m_h; }

		// ---- layers (painted back to front: layer 0 is at the back)
		std::vector<Layer>&       Layers() { return m_layers; }
		const std::vector<Layer>& Layers() const { return m_layers; }
		int                       Current() const { return m_current; }
		void                      SetCurrent(int a_i);
		bool                      AddLayer(const std::string& a_name);  // above the current one
		void                      DeleteLayer(int a_i);
		void                      MoveLayer(int a_i, int a_delta);
		void                      SetLayerVisible(int a_i, bool a_on);
		void                      SetLayerOpacity(int a_i, float a_opacity);
		void                      ClearLayer(int a_i);
		// One layer of paint: everything merged into it, bottom to top (hidden layers too, so nothing
		// painted is lost). True if there was anything to merge.
		bool                      Flatten();
		void                      SetLayerBlend(int a_i, Blend a_blend);
		void                      SetLayerLockAlpha(int a_i, bool a_on);

		// ---- painting
		// A loaded brush paints, a clean one smears the wet paint, a_style.lift lifts it (no brush needed).
		void BeginStroke(Brush& a_brush, float a_x, float a_y, float a_pressure, const StrokeStyle& a_style = {});
		void StrokeTo(Brush& a_brush, float a_x, float a_y, float a_pressure);
		void EndStroke(Brush& a_brush);
		bool Stroking() const { return m_stroking; }
		// the visible colour at a pixel (for the colour picker / the paint under the brush)
		Rgb  ColorAt(int a_x, int a_y) const;

		// ---- undo (strokes and clears; per layer)
		bool CanUndo() const { return !m_undo.empty(); }
		bool CanRedo() const { return !m_redo.empty(); }
		void Undo();
		void Redo();

		// ---- what is seen: the canvas with every visible layer on it
		const std::vector<std::uint8_t>& Composite() const { return m_composite; }
		Rect                             TakeDirty();  // part of Composite() changed since the last call
		void                             RecompositeAll();
		bool                             Changed() const { return m_changed; }
		void                             MarkSaved() { m_changed = false; }

		// Rotates everything a quarter turn (clockwise or not): width and height swap.
		void Rotate(bool a_clockwise, const std::vector<std::uint8_t>& a_newBlank);

		// ---- how thick the paint is (impasto): 0 = bare canvas .. 255 = thick ridges. Heavy strokes and
		// thick paint build it up along the bristle marks; smearing drags it, the rag wipes it.
		const std::vector<std::uint8_t>& Thickness() const { return m_height; }
		void                             SetThickness(std::vector<std::uint8_t> a_height);  // (wrong size = bare)

	private:
		struct UndoStep
		{
			int                       layer;
			std::vector<std::uint8_t> px;
			std::vector<std::uint8_t> height;
		};

		void Dab(Brush& a_brush, float a_x, float a_y, float a_pressure, float a_dirX, float a_dirY);
		void MakeGrain();
		void Recomposite(const Rect& a_r);
		void PushUndo(int a_layer);

		int                       m_w = 0, m_h = 0;
		std::vector<std::uint8_t> m_blank;
		std::vector<std::uint8_t> m_grain;  // the canvas weave: 255 = a raised thread, 0 = the hollows between
		std::vector<std::uint8_t> m_cover;  // how much the current stroke has covered each pixel (as opacity)
		std::vector<std::uint8_t> m_wet;    // how wet the paint is: fresh from a stroke, drying with each stroke after
		std::vector<std::uint8_t> m_height; // how thick the paint is
		std::vector<std::uint8_t> m_composite;
		std::vector<Layer>        m_layers;
		int                       m_current = 0;
		Rect                      m_dirty;
		bool                      m_changed = false;
		std::deque<UndoStep>      m_undo, m_redo;

		bool  m_stroking = false;
		float m_lastX = 0, m_lastY = 0, m_dirX = 1, m_dirY = 0;
		float m_carry = 0;  // distance left over before the next dab
		int   m_dabs = 0;
		bool  m_moved = false;
		float m_firstPressure = 1.0f;
		float m_prevDabX = 0, m_prevDabY = 0;  // where the last dab went (smearing drags from there)
		bool  m_hasPrevDab = false;
		StrokeStyle m_style;
	};

	// The hand on the brush, between the mouse and Painting::StrokeTo. Like Krita's stabiliser: the
	// brush hangs on a string behind the cursor (steadier lines, the longer the string), and when the
	// button comes up the brush runs out the rest of the string, lifting off - so strokes taper at
	// the end as they do at the start. A fast flick lays down less paint (the speed sensor).
	struct StrokePoint
	{
		float x, y, pressure;
	};

	class Stroker
	{
	public:
		float string = 6.0f;   // canvas px the brush trails the cursor by (set from the "steady hand")
		float taper = 12.0f;   // canvas px the stroke takes to come up to full pressure

		void Begin(float a_x, float a_y, double a_time);
		void Move(float a_x, float a_y, double a_time, float a_pressure, std::vector<StrokePoint>& a_out);
		void Finish(std::vector<StrokePoint>& a_out);  // the button came up: run out the string, tapering
		float BrushX() const { return m_bx; }
		float BrushY() const { return m_by; }

	private:
		float  m_bx = 0, m_by = 0, m_cx = 0, m_cy = 0;
		float  m_travel = 0, m_speed = 0, m_pressure = 0;
		double m_time = 0;
	};

	// Rotates RGBA pixels a quarter turn; a_w / a_h are the source size.
	std::vector<std::uint8_t> Rotate90(const std::vector<std::uint8_t>& a_px, int a_w, int a_h, bool a_clockwise);
	std::vector<std::uint8_t> Rotate90Gray(const std::vector<std::uint8_t>& a_px, int a_w, int a_h, bool a_clockwise);

	// ---- the canvas's relief and shine for the game: a tangent-space normal map
	enum class Varnish : int
	{
		None = 0,   // not varnished: only the oil paint itself has a slight sheen
		Matte = 1,  // flat, no shine at all
		Satin = 2,
		Gloss = 3,
	};
	struct NormalStyle
	{
		bool swapXY = true;    // the game reads the mesh's tangent / bitangent the other way round from ours
		bool flipY = false;    // (INI: if the relief looks lit from the wrong side)
	};
	// RGBA8 normal map of the whole canvas (a_w x a_h) from the paint's thickness and the canvas weave;
	// alpha = specular. a_rect limits the work to a part (a_out is still the whole canvas).
	void MakeNormalMap(const std::vector<std::uint8_t>& a_height, int a_w, int a_h, Varnish a_varnish, const NormalStyle& a_style,
		std::vector<std::uint8_t>& a_out, const Rect* a_rect = nullptr);
}
