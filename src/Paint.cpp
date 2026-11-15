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

#include "Paint.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>

namespace Paint
{
	const BrushType kBrushes[kBrushCount] = {
		{ "Detail round", 3.5f, 1.0f, 0.75f, 0.6f },
		{ "Round", 10.0f, 1.0f, 0.65f, 1.0f },
		{ "Filbert", 19.0f, 1.6f, 0.60f, 1.1f },
		{ "Wide flat", 34.0f, 3.2f, 0.55f, 1.2f },
		{ "Fan brush", 26.0f, 2.6f, 0.55f, 1.0f, Kind::Fan },
		{ "Charcoal", 4.5f, 1.0f, 0.5f, 1.0f, Kind::Charcoal },
		{ "Palette knife", 16.0f, 3.5f, 0.95f, 1.0f, Kind::Knife },
	};

	namespace
	{
		constexpr int kUndoDepth = 25;
		constexpr int kTip = 64;  // tip mask resolution

		std::uint32_t Hash(std::uint32_t x)
		{
			x ^= x >> 16;
			x *= 0x7feb352dU;
			x ^= x >> 15;
			x *= 0x846ca68bU;
			x ^= x >> 16;
			return x;
		}
		float Rand(std::uint32_t a_seed) { return (Hash(a_seed) & 0xFFFFFF) / float(0xFFFFFF); }

		float Smooth(float e0, float e1, float x)
		{
			const float t = std::clamp((x - e0) / (e1 - e0), 0.0f, 1.0f);
			return t * t * (3.0f - 2.0f * t);
		}

		// Bristle tips: a soft-edged ellipse made of bristles. Across the stroke (u) the bristles leave
		// gaps and differ in strength, which is what draws the streaks; along it (v) they are continuous.
		struct Tip
		{
			std::array<float, kTip * kTip> mask{};
		};

		const Tip& TipFor(int a_type)
		{
			static std::array<Tip, kBrushCount> tips;
			static bool                         made = false;
			if (!made) {
				made = true;
				for (int b = 0; b < kBrushCount; ++b) {
					auto&      m = tips[b].mask;
					const Kind kind = kBrushes[b].kind;
					if (kind == Kind::Knife || kind == Kind::Charcoal) {
						for (int y = 0; y < kTip; ++y) {
							for (int x = 0; x < kTip; ++x) {
								const float u = (x + 0.5f) / kTip * 2.0f - 1.0f, v = (y + 0.5f) / kTip * 2.0f - 1.0f;
								float       val;
								if (kind == Kind::Knife) {  // the blade: a hard-edged slab, very slightly ragged at its ends
									val = (1.0f - Smooth(0.9f, 1.0f, std::fabs(u))) * (1.0f - Smooth(0.75f, 1.0f, std::fabs(v)));
									val *= 0.92f + 0.08f * Rand(b * 7919 + y * 131 + x);
								} else {  // charcoal: a soft round stick end, dusty and grainy
									const float r = std::sqrt(u * u + v * v);
									val = (1.0f - Smooth(0.45f, 1.0f, r)) * (0.55f + 0.45f * Rand(b * 7919 + y * 131 + x));
								}
								m[y * kTip + x] = std::clamp(val, 0.0f, 1.0f);
							}
						}
						continue;
					}
					if (kind == Kind::Fan) {
						// a few springy bristles, unevenly spread and of different lengths: broken, feathery marks
						constexpr int kN = 13;
						float         pos[kN], len[kN], str[kN], wid[kN];
						for (int k = 0; k < kN; ++k) {
							pos[k] = -0.92f + 1.84f * (k + 0.5f) / kN + (Rand(b * 401 + k * 17 + 1) - 0.5f) * 0.12f;
							len[k] = 0.35f + 0.65f * Rand(b * 401 + k * 17 + 2);
							str[k] = 0.35f + 0.65f * Rand(b * 401 + k * 17 + 3);
							wid[k] = 0.02f + 0.03f * Rand(b * 401 + k * 17 + 4);
						}
						for (int y = 0; y < kTip; ++y) {
							for (int x = 0; x < kTip; ++x) {
								const float u = (x + 0.5f) / kTip * 2.0f - 1.0f, v = (y + 0.5f) / kTip * 2.0f - 1.0f;
								float       val = 0.0f;
								for (int k = 0; k < kN; ++k) {
									const float d = std::fabs(u - pos[k]) / wid[k];
									if (d < 1.0f && std::fabs(v) < len[k]) {
										val = std::max(val, str[k] * (1.0f - d * d) * (1.0f - Smooth(len[k] * 0.6f, len[k], std::fabs(v))));
									}
								}
								m[y * kTip + x] = std::clamp(val * (0.85f + 0.15f * Rand(b * 7919 + y * 131 + x)), 0.0f, 1.0f);
							}
						}
						continue;
					}
					// the others: many bristles packed close
					const int bristles = 10 + b * 14;
					std::vector<float> across(kTip, 0.0f);
					for (int k = 0; k < bristles; ++k) {
						const float c = Rand(b * 977 + k * 31 + 1) * 2.0f - 1.0f;   // position across the tip
						const float w = kind == Kind::Fan ? 0.012f + Rand(b * 977 + k * 31 + 2) * 0.02f : 0.6f / bristles + Rand(b * 977 + k * 31 + 2) * 1.2f / bristles;
						const float s = 0.55f + 0.45f * Rand(b * 977 + k * 31 + 3);
						for (int i = 0; i < kTip; ++i) {
							const float u = (i + 0.5f) / kTip * 2.0f - 1.0f;
							const float d = std::fabs(u - c) / (w * 2.0f);
							if (d < 1.0f) {
								across[i] = std::max(across[i], s * (1.0f - d * d));
							}
						}
					}
					for (int y = 0; y < kTip; ++y) {
						for (int x = 0; x < kTip; ++x) {
							const float u = (x + 0.5f) / kTip * 2.0f - 1.0f, v = (y + 0.5f) / kTip * 2.0f - 1.0f;
							const float r = std::sqrt(u * u + v * v);
							const float edge = 1.0f - Smooth(0.7f, 1.0f, r);
							// a little grain along the bristles too
							const float grain = 0.85f + 0.15f * Rand(b * 7919 + y * 131 + x);
							m[y * kTip + x] = std::clamp(edge * (0.5f + 0.5f * across[x]) * grain, 0.0f, 1.0f);
						}
					}
				}
			}
			return tips[std::clamp(a_type, 0, kBrushCount - 1)];
		}

		float SampleTip(const Tip& a_tip, float a_u, float a_v)  // u, v in -1..1
		{
			const float fx = (a_u + 1.0f) * 0.5f * kTip - 0.5f, fy = (a_v + 1.0f) * 0.5f * kTip - 0.5f;
			const int   x0 = static_cast<int>(std::floor(fx)), y0 = static_cast<int>(std::floor(fy));
			const float tx = fx - x0, ty = fy - y0;
			auto        at = [&](int x, int y) {
                if (x < 0 || y < 0 || x >= kTip || y >= kTip) {
                    return 0.0f;
                }
                return a_tip.mask[y * kTip + x];
			};
			const float a = at(x0, y0), b = at(x0 + 1, y0), c = at(x0, y0 + 1), d = at(x0 + 1, y0 + 1);
			return (a + (b - a) * tx) + ((c + (d - c) * tx) - (a + (b - a) * tx)) * ty;
		}
	}

	void Rect::Add(int a_x0, int a_y0, int a_x1, int a_y1)
	{
		if (Empty()) {
			*this = { a_x0, a_y0, a_x1, a_y1 };
			return;
		}
		x0 = std::min(x0, a_x0);
		y0 = std::min(y0, a_y0);
		x1 = std::max(x1, a_x1);
		y1 = std::max(y1, a_y1);
	}

	void Painting::Reset(int a_w, int a_h, const std::vector<std::uint8_t>& a_blank)
	{
		m_w = a_w;
		m_h = a_h;
		m_blank = a_blank;
		m_layers.clear();
		m_layers.push_back({ "Underpainting", true, 1.0f, std::vector<std::uint8_t>(static_cast<std::size_t>(a_w) * a_h * 4, 0) });
		MakeGrain();
		m_wet.clear();  // a painting you come back to has dried
		m_height.assign(static_cast<std::size_t>(a_w) * a_h, 0);
		m_current = 0;
		m_undo.clear();
		m_redo.clear();
		m_changed = false;
		m_stroking = false;
		RecompositeAll();
	}

	void Painting::MakeGrain()
	{
		// the bare canvas is white with the weave pressed into it: brighter = a raised thread
		const std::size_t n = static_cast<std::size_t>(m_w) * m_h;
		m_grain.assign(n, 255);
		if (m_blank.size() < n * 4) {
			return;
		}
		for (std::size_t i = 0; i < n; ++i) {
			const float l = (m_blank[i * 4] + m_blank[i * 4 + 1] + m_blank[i * 4 + 2]) / 3.0f;
			m_grain[i] = static_cast<std::uint8_t>(std::clamp((l - 238.0f) / 14.0f, 0.0f, 1.0f) * 255.0f);
		}
	}

	bool Painting::Flatten()
	{
		if (m_layers.empty()) {
			return false;
		}
		if (m_layers.size() == 1 && m_layers[0].visible && m_layers[0].opacity >= 0.999f && m_layers[0].blend == Blend::Normal && !m_layers[0].lockAlpha) {
			return false;
		}
		const std::size_t         n = static_cast<std::size_t>(m_w) * m_h;
		std::vector<std::uint8_t> out(n * 4, 0);
		for (const auto& l : m_layers) {
			const float op = std::clamp(l.opacity, 0.0f, 1.0f);
			for (std::size_t i = 0; i < n; ++i) {
				const float a = l.px[i * 4 + 3] / 255.0f * op;
				if (a <= 0.0f) {
					continue;
				}
				const float da = out[i * 4 + 3] / 255.0f;
				const float oa = a + da * (1.0f - a);
				for (int c = 0; c < 3; ++c) {
					const float v = (l.px[i * 4 + c] * a + out[i * 4 + c] * da * (1.0f - a)) / oa;
					out[i * 4 + c] = static_cast<std::uint8_t>(std::clamp(v + 0.5f, 0.0f, 255.0f));
				}
				out[i * 4 + 3] = static_cast<std::uint8_t>(std::clamp(oa * 255.0f + 0.5f, 0.0f, 255.0f));
			}
		}
		m_layers.clear();
		m_layers.push_back({ "Paint", true, 1.0f, std::move(out) });
		m_current = 0;
		m_undo.clear();
		m_redo.clear();
		m_wet.clear();
		m_changed = true;
		RecompositeAll();
		return true;
	}

	void Painting::SetLayerBlend(int a_i, Blend a_blend)
	{
		if (a_i >= 0 && a_i < static_cast<int>(m_layers.size()) && m_layers[a_i].blend != a_blend) {
			m_layers[a_i].blend = a_blend;
			m_changed = true;
			RecompositeAll();
		}
	}

	void Painting::SetLayerLockAlpha(int a_i, bool a_on)
	{
		if (a_i >= 0 && a_i < static_cast<int>(m_layers.size())) {
			m_layers[a_i].lockAlpha = a_on;
			m_changed = true;
		}
	}

	void Painting::SetCurrent(int a_i) { m_current = std::clamp(a_i, 0, static_cast<int>(m_layers.size()) - 1); }

	bool Painting::AddLayer(const std::string& a_name)
	{
		if (static_cast<int>(m_layers.size()) >= kMaxLayers) {
			return false;
		}
		m_layers.insert(m_layers.begin() + m_current + 1, Layer{ a_name, true, 1.0f, std::vector<std::uint8_t>(static_cast<std::size_t>(m_w) * m_h * 4, 0) });
		++m_current;
		m_changed = true;
		return true;
	}

	void Painting::DeleteLayer(int a_i)
	{
		if (m_layers.size() <= 1 || a_i < 0 || a_i >= static_cast<int>(m_layers.size())) {
			return;
		}
		m_layers.erase(m_layers.begin() + a_i);
		m_current = std::clamp(m_current - (a_i <= m_current ? 1 : 0), 0, static_cast<int>(m_layers.size()) - 1);
		m_undo.clear();  // undo steps point at layers by position
		m_redo.clear();
		m_changed = true;
		RecompositeAll();
	}

	void Painting::MoveLayer(int a_i, int a_delta)
	{
		const int j = a_i + a_delta;
		if (a_i < 0 || j < 0 || a_i >= static_cast<int>(m_layers.size()) || j >= static_cast<int>(m_layers.size())) {
			return;
		}
		std::swap(m_layers[a_i], m_layers[j]);
		if (m_current == a_i) {
			m_current = j;
		} else if (m_current == j) {
			m_current = a_i;
		}
		m_undo.clear();
		m_redo.clear();
		m_changed = true;
		RecompositeAll();
	}

	void Painting::SetLayerVisible(int a_i, bool a_on)
	{
		if (a_i >= 0 && a_i < static_cast<int>(m_layers.size()) && m_layers[a_i].visible != a_on) {
			m_layers[a_i].visible = a_on;
			RecompositeAll();
		}
	}

	void Painting::SetLayerOpacity(int a_i, float a_opacity)
	{
		if (a_i >= 0 && a_i < static_cast<int>(m_layers.size())) {
			m_layers[a_i].opacity = std::clamp(a_opacity, 0.0f, 1.0f);
			m_changed = true;
			RecompositeAll();
		}
	}

	void Painting::ClearLayer(int a_i)
	{
		if (a_i < 0 || a_i >= static_cast<int>(m_layers.size())) {
			return;
		}
		PushUndo(a_i);
		std::fill(m_layers[a_i].px.begin(), m_layers[a_i].px.end(), 0);
		std::fill(m_height.begin(), m_height.end(), 0);
		m_changed = true;
		RecompositeAll();
	}

	void Painting::PushUndo(int a_layer)
	{
		m_undo.push_back({ a_layer, m_layers[a_layer].px, m_height });
		while (static_cast<int>(m_undo.size()) > kUndoDepth) {
			m_undo.pop_front();
		}
		m_redo.clear();
	}

	void Painting::Undo()
	{
		if (m_undo.empty()) {
			return;
		}
		auto step = std::move(m_undo.back());
		m_undo.pop_back();
		if (step.layer >= static_cast<int>(m_layers.size())) {
			return;
		}
		m_redo.push_back({ step.layer, std::move(m_layers[step.layer].px), std::move(m_height) });
		m_layers[step.layer].px = std::move(step.px);
		m_height = std::move(step.height);
		m_changed = true;
		RecompositeAll();
	}

	void Painting::Redo()
	{
		if (m_redo.empty()) {
			return;
		}
		auto step = std::move(m_redo.back());
		m_redo.pop_back();
		if (step.layer >= static_cast<int>(m_layers.size())) {
			return;
		}
		m_undo.push_back({ step.layer, std::move(m_layers[step.layer].px), std::move(m_height) });
		m_layers[step.layer].px = std::move(step.px);
		m_height = std::move(step.height);
		m_changed = true;
		RecompositeAll();
	}

	Rgb Painting::ColorAt(int a_x, int a_y) const
	{
		a_x = std::clamp(a_x, 0, m_w - 1);
		a_y = std::clamp(a_y, 0, m_h - 1);
		const auto* p = &m_composite[(static_cast<std::size_t>(a_y) * m_w + a_x) * 4];
		return { p[0], p[1], p[2] };
	}

	void Painting::RecompositeAll()
	{
		m_composite.resize(static_cast<std::size_t>(m_w) * m_h * 4);
		Recomposite({ 0, 0, m_w, m_h });
	}

	void Painting::Recomposite(const Rect& a_r)
	{
		const int x0 = std::max(0, a_r.x0), y0 = std::max(0, a_r.y0), x1 = std::min(m_w, a_r.x1), y1 = std::min(m_h, a_r.y1);
		for (int y = y0; y < y1; ++y) {
			for (int x = x0; x < x1; ++x) {
				const std::size_t i = (static_cast<std::size_t>(y) * m_w + x) * 4;
				float             r = m_blank[i], g = m_blank[i + 1], b = m_blank[i + 2];
				for (const auto& l : m_layers) {  // back to front
					if (!l.visible) {
						continue;
					}
					const float a = l.px[i + 3] / 255.0f * l.opacity;
					if (a <= 0.0f) {
						continue;
					}
					if (l.blend == Blend::Glaze) {  // a transparent coat: darkens and tints what is under it
						r += (r * l.px[i] / 255.0f - r) * a;
						g += (g * l.px[i + 1] / 255.0f - g) * a;
						b += (b * l.px[i + 2] / 255.0f - b) * a;
					} else {
						r += (l.px[i] - r) * a;
						g += (l.px[i + 1] - g) * a;
						b += (l.px[i + 2] - b) * a;
					}
				}
				m_composite[i] = static_cast<std::uint8_t>(r + 0.5f);
				m_composite[i + 1] = static_cast<std::uint8_t>(g + 0.5f);
				m_composite[i + 2] = static_cast<std::uint8_t>(b + 0.5f);
				m_composite[i + 3] = 255;
			}
		}
		m_dirty.Add(x0, y0, x1, y1);
	}

	Rect Painting::TakeDirty()
	{
		const Rect r = m_dirty;
		m_dirty = {};
		return r;
	}

	void Painting::BeginStroke(Brush& a_brush, float a_x, float a_y, float a_pressure, const StrokeStyle& a_style)
	{
		if (m_layers.empty() || (!a_style.lift && a_brush.type < 0)) {
			return;
		}
		PushUndo(m_current);
		m_style = a_style;
		m_style.body = std::clamp(m_style.body, 0.05f, 1.0f);
		m_cover.assign(static_cast<std::size_t>(m_w) * m_h, 0);
		// paint dries: what earlier strokes put down gets drier with every new stroke
		if (m_wet.size() != m_cover.size()) {
			m_wet.assign(m_cover.size(), 0);
		} else {
			for (auto& w : m_wet) {
				w = static_cast<std::uint8_t>(w * 0.55f);
			}
		}
		m_hasPrevDab = false;
		m_stroking = true;
		m_lastX = a_x;
		m_lastY = a_y;
		m_carry = 0.0f;
		m_dabs = 0;
		m_moved = false;
		m_firstPressure = a_pressure;
	}

	void Painting::StrokeTo(Brush& a_brush, float a_x, float a_y, float a_pressure)
	{
		if (!m_stroking) {
			return;
		}
		const float dx = a_x - m_lastX, dy = a_y - m_lastY;
		const float dist = std::sqrt(dx * dx + dy * dy);
		if (dist < 0.01f) {
			return;
		}
		// the tip turns with the stroke (smoothed, so a shaky hand doesn't spin it)
		const float nx = dx / dist, ny = dy / dist;
		if (!m_moved) {  // first movement: the tip faces this way from the start, and the stroke begins here
			m_moved = true;
			m_dirX = nx;
			m_dirY = ny;
			Dab(a_brush, m_lastX, m_lastY, std::min(m_firstPressure, a_pressure), m_dirX, m_dirY);
		}
		m_dirX = m_dirX * 0.7f + nx * 0.3f;
		m_dirY = m_dirY * 0.7f + ny * 0.3f;
		const float dl = std::sqrt(m_dirX * m_dirX + m_dirY * m_dirY);
		if (dl > 1e-4f) {
			m_dirX /= dl;
			m_dirY /= dl;
		}
		const auto& bt = kBrushes[std::clamp(a_brush.type, 0, kBrushCount - 1)];
		const float radius = m_style.lift ? 24.0f : bt.radius * kSizes[std::clamp(a_brush.size, 0, kSizeCount - 1)] * (0.45f + 0.55f * a_pressure);
		const float along = m_style.lift ? radius : radius / bt.aspect;  // a flat brush is short along the stroke
		const float step = std::max(0.6f, along * (a_brush.Loaded() || m_style.lift ? 0.2f : 0.1f));  // smearing wants close dabs
		float       t = step - m_carry;
		while (t <= dist) {
			Dab(a_brush, m_lastX + nx * t, m_lastY + ny * t, a_pressure, m_dirX, m_dirY);
			t += step;
		}
		m_carry = dist - (t - step);
		m_lastX = a_x;
		m_lastY = a_y;
	}

	void Painting::EndStroke(Brush& a_brush)
	{
		if (m_stroking && !m_moved) {
			Dab(a_brush, m_lastX, m_lastY, m_firstPressure, m_dirX, m_dirY);  // a click without moving: one dab
		}
		// what this stroke painted is wet now
		if (m_stroking && !m_style.lift && m_wet.size() == m_cover.size()) {
			for (std::size_t i = 0; i < m_wet.size(); ++i) {
				m_wet[i] = std::max(m_wet[i], m_cover[i]);
			}
		}
		m_stroking = false;
	}

	void Painting::Dab(Brush& a_brush, float a_x, float a_y, float a_pressure, float a_dirX, float a_dirY)
	{
		const bool  lift = m_style.lift;
		if (!lift && a_brush.type < 0) {
			return;
		}
		const auto& bt = kBrushes[std::clamp(a_brush.type, 0, kBrushCount - 1)];
		const Kind  kind = lift ? Kind::Bristle : bt.kind;
		const bool  scrape = !lift && kind == Kind::Knife && !a_brush.Loaded();  // a clean knife scrapes paint off
		const bool  smear = !lift && !scrape && !a_brush.Loaded() && kind != Kind::Charcoal;
		const float p = std::clamp(a_pressure, 0.05f, 1.0f);
		// pressing harder splays the bristles; the rag is a soft wad of cloth
		const float rAcross = lift ? 24.0f * (0.6f + 0.4f * p) : bt.radius * kSizes[std::clamp(a_brush.size, 0, kSizeCount - 1)] * (0.45f + 0.55f * p);
		const float rAlong = lift ? rAcross : rAcross / bt.aspect;
		const float reach = std::max(rAcross, rAlong) + 1.0f;
		const int   x0 = std::max(0, static_cast<int>(std::floor(a_x - reach)));
		const int   y0 = std::max(0, static_cast<int>(std::floor(a_y - reach)));
		const int   x1 = std::min(m_w - 1, static_cast<int>(std::ceil(a_x + reach)));
		const int   y1 = std::min(m_h - 1, static_cast<int>(std::ceil(a_y + reach)));
		const float fromX = m_hasPrevDab ? m_prevDabX - a_x : 0.0f, fromY = m_hasPrevDab ? m_prevDabY - a_y : 0.0f;
		m_prevDabX = a_x;
		m_prevDabY = a_y;
		m_hasPrevDab = true;
		if (x0 > x1 || y0 > y1) {
			return;
		}
		auto&      layer = m_layers[m_current].px;
		const bool lockAlpha = m_layers[m_current].lockAlpha && !lift;
		if (m_height.size() != static_cast<std::size_t>(m_w) * m_h) {
			m_height.assign(static_cast<std::size_t>(m_w) * m_h, 0);
		}
		const Tip& tip = TipFor(lift ? 1 : a_brush.type);
		const float ca = a_dirX, sa = a_dirY;  // stroke direction; u runs across it

		// the tip's strength at a pixel, with the canvas weave: a light touch only catches the raised threads
		float tooth = (1.0f - Smooth(0.1f, 0.8f, p)) * (smear ? 0.3f : 0.7f);
		if (kind == Kind::Charcoal) {
			tooth = 0.85f - 0.25f * p;  // charcoal always catches the weave: grainy, more so with a light hand
		} else if (kind == Kind::Knife) {
			tooth = 0.05f;  // the blade rides over the weave: paint sits on top
		}
		auto        tipAt = [&](int x, int y) {
            const float px = x + 0.5f - a_x, py = y + 0.5f - a_y;
            const float along = px * ca + py * sa;
            const float across = -px * sa + py * ca;
            const float u = across / rAcross, v = along / rAlong;
            if (kind == Kind::Knife ? (std::fabs(u) >= 1.0f || std::fabs(v) >= 1.0f) : u * u + v * v >= 1.0f) {
                return 0.0f;  // (the blade is square-ended)
            }
            float m;
            if (lift) {  // cloth: soft, uneven
                const float r = std::sqrt(u * u + v * v);
                m = (1.0f - Smooth(0.35f, 1.0f, r)) * (0.65f + 0.35f * Rand(static_cast<std::uint32_t>(y * 7919 + x * 31 + m_dabs)));
            } else {
                m = SampleTip(tip, u, v);
            }
            if (tooth > 0.0f) {
                const float g = m_grain[static_cast<std::size_t>(y) * m_w + x] / 255.0f;
                m *= 1.0f - tooth * (1.0f - g);
            }
            if (kind == Kind::Charcoal) {
                // the surface's tooth: charcoal only catches on its high points, more of them with a heavier hand
                const std::uint32_t hsh = static_cast<std::uint32_t>(x) * 73856093u ^ static_cast<std::uint32_t>(y) * 19349663u;
                const float         t = Rand(hsh) * 0.6f + Rand(hsh / 7u + static_cast<std::uint32_t>(x / 3 + y / 3) * 83492791u) * 0.4f;
                m *= std::clamp((t - (0.6f - 0.55f * p)) * 3.0f, 0.0f, 1.0f);
            }
            return m;
		};

		if (lift || scrape) {
			// ---- the rag: wipes paint off this layer, more with a heavier hand; a clean palette knife
			// scrapes it off cleanly, back to the canvas
			const float strength = scrape ? 0.55f + 0.4f * p : 0.25f + 0.5f * p;
			for (int y = y0; y <= y1; ++y) {
				for (int x = x0; x <= x1; ++x) {
					const float m = tipAt(x, y);
					if (m <= 0.002f) {
						continue;
					}
					const std::size_t i = (static_cast<std::size_t>(y) * m_w + x) * 4;
					const float keep = 1.0f - std::clamp(m * strength, 0.0f, 1.0f);
					layer[i + 3] = static_cast<std::uint8_t>(layer[i + 3] * keep + 0.5f);
					auto& hh = m_height[static_cast<std::size_t>(y) * m_w + x];
					hh = static_cast<std::uint8_t>(hh * keep + 0.5f);  // the rag takes the thickness off too
				}
			}
			++m_dabs;
		} else if (smear) {
			// ---- a clean brush: drags the wet paint along (Krita's smearing). Each pixel takes some of
			// what was where the brush came from; the paint thins as it is dragged out.
			const int   w = x1 - x0 + 1, h = y1 - y0 + 1;
			const float strength = 0.55f + 0.35f * p;
			std::vector<float> src(static_cast<std::size_t>(w) * h * 4);
			std::vector<float> srcH(static_cast<std::size_t>(w) * h);
			for (int y = y0; y <= y1; ++y) {
				for (int x = x0; x <= x1; ++x) {
					const int         sx = std::clamp(static_cast<int>(std::lround(x + fromX)), 0, m_w - 1);
					const int         sy = std::clamp(static_cast<int>(std::lround(y + fromY)), 0, m_h - 1);
					const std::size_t si = (static_cast<std::size_t>(sy) * m_w + sx) * 4;
					float*            d = &src[(static_cast<std::size_t>(y - y0) * w + (x - x0)) * 4];
					const float       a = layer[si + 3] / 255.0f;
					srcH[static_cast<std::size_t>(y - y0) * w + (x - x0)] = m_height[static_cast<std::size_t>(sy) * m_w + sx];
					d[0] = layer[si] * a;  // premultiplied, so bare canvas pulls the paint thin
					d[1] = layer[si + 1] * a;
					d[2] = layer[si + 2] * a;
					d[3] = a;
				}
			}
			for (int y = y0; y <= y1; ++y) {
				for (int x = x0; x <= x1; ++x) {
					const float k = std::clamp(tipAt(x, y) * strength, 0.0f, 1.0f);
					if (k <= 0.002f) {
						continue;
					}
					const std::size_t i = (static_cast<std::size_t>(y) * m_w + x) * 4;
					const float*      sp = &src[(static_cast<std::size_t>(y - y0) * w + (x - x0)) * 4];
					const float       da = layer[i + 3] / 255.0f;
					const float       na = lockAlpha ? da : da + (sp[3] - da) * k;
					if (na <= 0.002f) {
						layer[i + 3] = 0;
						continue;
					}
					for (int c = 0; c < 3; ++c) {
						const float pre = layer[i + c] * da + (sp[c] - layer[i + c] * da) * k;
						layer[i + c] = static_cast<std::uint8_t>(std::clamp(pre / std::max(na, 1e-4f) + 0.5f, 0.0f, 255.0f));
					}
					layer[i + 3] = static_cast<std::uint8_t>(std::clamp(na * 255.0f + 0.5f, 0.0f, 255.0f));
					// the thickness is dragged along too, a little flattened by the brush
					auto&       hh = m_height[static_cast<std::size_t>(y) * m_w + x];
					const float sh = srcH[static_cast<std::size_t>(y - y0) * w + (x - x0)];
					hh = static_cast<std::uint8_t>(std::clamp((hh + (sh - hh) * k) * (1.0f - 0.04f * k) + 0.5f, 0.0f, 255.0f));
				}
			}
		} else {
			// ---- paint. The paint under the brush is what was there before the stroke began (the undo
			// snapshot); only paint that is still wet (from the last couple of strokes) mixes in - older
			// paint has dried and new paint covers it.
			const auto& before = m_undo.empty() ? layer : m_undo.back().px;
			const bool  haveWet = m_wet.size() == static_cast<std::size_t>(m_w) * m_h;
			float       under[3]{}, wet = 0.0f, wsum = 0.0f;
			for (int k = 0; k < 5; ++k) {
				const int sx = std::clamp(static_cast<int>(a_x + (k == 1 ? rAcross * 0.5f : k == 2 ? -rAcross * 0.5f : 0.0f)), 0, m_w - 1);
				const int sy = std::clamp(static_cast<int>(a_y + (k == 3 ? rAcross * 0.5f : k == 4 ? -rAcross * 0.5f : 0.0f)), 0, m_h - 1);
				const std::size_t i = (static_cast<std::size_t>(sy) * m_w + sx) * 4;
				const float       a = before[i + 3] / 255.0f;
				for (int c = 0; c < 3; ++c) {
					under[c] += before[i + c] * a;
				}
				wsum += a;
				wet += haveWet ? m_wet[static_cast<std::size_t>(sy) * m_w + sx] / 255.0f : 0.0f;
			}
			wet /= 5.0f;
			Rgb deposit = a_brush.color;
			// charcoal is dry: it never mixes with the paint; a knife lays its paint on cleaner than a brush
			const float mixK = kind == Kind::Charcoal ? 0.0f : kind == Kind::Knife ? 0.35f : 1.0f;
			wet *= mixK;
			if (wet > 0.15f && wsum > 0.0f) {
				const Rgb u{ static_cast<std::uint8_t>(under[0] / wsum), static_cast<std::uint8_t>(under[1] / wsum), static_cast<std::uint8_t>(under[2] / wsum) };
				if ((m_dabs & 3) == 0) {
					// the brush picks up some of the wet paint it drags through
					a_brush.color = Spectral::Mix(a_brush.color, u, 0.08f * wet);
				}
				// a heavy hand pushes its own paint through; a light one blends with the wet paint
				deposit = Spectral::Mix(a_brush.color, u, 0.5f * wet * (1.0f - 0.5f * p));
			}
			++m_dabs;

			// flow: paint per dab; body: how far one stroke can cover (a glaze never hides what's under it)
			const float flow = bt.flow * (0.25f + 0.75f * p);
			const float cap = m_style.body * 255.0f;
			// impasto: thick paint and a heavy hand leave paint standing up off the canvas
			float build = 0.0f;  // no thick paint (was 70 * body^2 * (0.2 + 0.8p))
			if (kind == Kind::Charcoal) {
				build = 0.0f;  // dust, not paint
			} else if (kind == Kind::Knife) {
				build = 0.0f;  // (no thick paint)
			}
			const float dr = deposit[0], dg = deposit[1], db = deposit[2];
			for (int y = y0; y <= y1; ++y) {
				for (int x = x0; x <= x1; ++x) {
					float a = std::clamp(tipAt(x, y) * flow, 0.0f, 1.0f);
					if (a <= 0.002f) {
						continue;
					}
					const std::size_t pi = static_cast<std::size_t>(y) * m_w + x;
					// coverage builds like layers of paint (1 - (1-a1)(1-a2)...), capped by the paint's body
					const float cov = m_cover[pi] / 255.0f, capA = cap / 255.0f;
					if (cov >= capA - 0.002f) {
						continue;
					}
					a = std::min(a, (capA - cov) / std::max(1e-3f, 1.0f - cov));
					if (a <= 0.002f) {
						continue;
					}
					const float covNew = std::min(1.0f, cov + a * (1.0f - cov));
					m_cover[pi] = static_cast<std::uint8_t>(covNew * 255.0f + 0.5f);
					const float built = (covNew - cov) * build;  // a stroke adds at most `build` of thickness
					const std::size_t i = pi * 4;
					const float       da = layer[i + 3] / 255.0f;
					if (lockAlpha && da <= 0.0f) {
						continue;
					}
					const float oa = lockAlpha ? da : a + da * (1.0f - a);
					const float k = lockAlpha ? a : a / oa;  // share of the new paint in the pixel's colour
					layer[i] = static_cast<std::uint8_t>(layer[i] + (dr - layer[i]) * k + 0.5f);
					layer[i + 1] = static_cast<std::uint8_t>(layer[i + 1] + (dg - layer[i + 1]) * k + 0.5f);
					layer[i + 2] = static_cast<std::uint8_t>(layer[i + 2] + (db - layer[i + 2]) * k + 0.5f);
					layer[i + 3] = static_cast<std::uint8_t>(std::clamp(oa * 255.0f + 0.5f, 0.0f, 255.0f));
					auto& hh = m_height[pi];
					hh = static_cast<std::uint8_t>(std::min(255.0f, hh + built + 0.5f));
				}
			}
		}
		m_changed = true;
		Recomposite({ x0, y0, x1 + 1, y1 + 1 });
	}

	// ---- the hand on the brush
	void Stroker::Begin(float a_x, float a_y, double a_time)
	{
		m_bx = m_cx = a_x;
		m_by = m_cy = a_y;
		m_travel = 0.0f;
		m_speed = 0.0f;
		m_pressure = 0.0f;
		m_time = a_time;
	}

	void Stroker::Move(float a_x, float a_y, double a_time, float a_pressure, std::vector<StrokePoint>& a_out)
	{
		const float dt = static_cast<float>(std::max(1e-3, a_time - m_time));
		const float cdx = a_x - m_cx, cdy = a_y - m_cy;
		m_speed = m_speed * 0.8f + std::sqrt(cdx * cdx + cdy * cdy) / dt * 0.2f;
		m_cx = a_x;
		m_cy = a_y;
		m_time = a_time;
		// the brush is pulled along on the string
		const float dx = m_cx - m_bx, dy = m_cy - m_by;
		const float d = std::sqrt(dx * dx + dy * dy);
		if (d <= string) {
			return;
		}
		const float pull = (d - string) / d;
		m_bx += dx * pull;
		m_by += dy * pull;
		m_travel += d - string;
		// fade in at the start, lighter when flicked fast; the pressure itself is smoothed a little
		const float fade = 0.3f + 0.7f * Smooth(0.0f, std::max(4.0f, taper), m_travel);
		const float speed = 1.0f - 0.35f * Smooth(500.0f, 3000.0f, m_speed);
		const float target = a_pressure * fade * speed;
		m_pressure = m_pressure <= 0.0f ? target : m_pressure + (target - m_pressure) * 0.35f;
		a_out.push_back({ m_bx, m_by, m_pressure });
	}

	void Stroker::Finish(std::vector<StrokePoint>& a_out)
	{
		// lift off: the brush runs out the rest of the string as the pressure comes off
		const float dx = m_cx - m_bx, dy = m_cy - m_by;
		const float d = std::sqrt(dx * dx + dy * dy);
		if (d < 0.5f || m_pressure <= 0.0f) {
			return;
		}
		const int steps = std::clamp(static_cast<int>(d / 2.0f), 2, 24);
		for (int i = 1; i <= steps; ++i) {
			const float t = static_cast<float>(i) / steps;
			a_out.push_back({ m_bx + dx * t, m_by + dy * t, m_pressure * (1.0f - 0.8f * t) });
		}
		m_bx = m_cx;
		m_by = m_cy;
	}

	void Painting::Rotate(bool a_clockwise, const std::vector<std::uint8_t>& a_newBlank)
	{
		for (auto& l : m_layers) {
			l.px = Rotate90(l.px, m_w, m_h, a_clockwise);
		}
		if (m_height.size() == static_cast<std::size_t>(m_w) * m_h) {
			m_height = Rotate90Gray(m_height, m_w, m_h, a_clockwise);
		}
		std::swap(m_w, m_h);
		m_blank = a_newBlank;
		MakeGrain();
		m_wet.clear();
		m_undo.clear();
		m_redo.clear();
		m_changed = true;
		RecompositeAll();
	}

	void Painting::SetThickness(std::vector<std::uint8_t> a_height)
	{
		if (a_height.size() == static_cast<std::size_t>(m_w) * m_h) {
			m_height = std::move(a_height);
		} else {
			m_height.assign(static_cast<std::size_t>(m_w) * m_h, 0);
		}
	}

	std::vector<std::uint8_t> Rotate90Gray(const std::vector<std::uint8_t>& a_px, int a_w, int a_h, bool a_clockwise)
	{
		std::vector<std::uint8_t> out(a_px.size());
		for (int y = 0; y < a_h; ++y) {
			for (int x = 0; x < a_w; ++x) {
				const int nx = a_clockwise ? a_h - 1 - y : y;
				const int ny = a_clockwise ? x : a_w - 1 - x;
				out[static_cast<std::size_t>(ny) * a_h + nx] = a_px[static_cast<std::size_t>(y) * a_w + x];
			}
		}
		return out;
	}

	void MakeNormalMap(const std::vector<std::uint8_t>& a_height, int a_w, int a_h, Varnish a_varnish, const NormalStyle& a_style,
		std::vector<std::uint8_t>& a_out, const Rect* a_rect)
	{
		const std::size_t n = static_cast<std::size_t>(a_w) * a_h;
		if (a_out.size() != n * 4) {
			a_out.assign(n * 4, 0);
			a_rect = nullptr;  // the whole thing
		}
		const bool haveH = false;  // no thick paint: the surface is the canvas weave (the varnish still shines)
		(void)n;
		// the surface: the canvas weave (threads every 3 px, as on the blank canvas) where the paint is
		// thin, plus the paint's own thickness
		auto surface = [&](int x, int y) {
			x = std::clamp(x, 0, a_w - 1);
			y = std::clamp(y, 0, a_h - 1);
			const float paint = haveH ? a_height[static_cast<std::size_t>(y) * a_w + x] : 0.0f;
			const float weave = std::sin(x * 2.094f) * std::sin(y * 2.094f) * 5.0f * (1.0f - std::min(1.0f, paint / 50.0f));
			return paint + weave;
		};
		constexpr float kSlope = 0.035f;  // how steep the relief looks
		int             shine0 = 0;       // specular everywhere (the varnish) ...
		float           shinePaint = 0.3f;  // ... and from the paint itself, by thickness
		switch (a_varnish) {
		case Varnish::Matte:
			shine0 = 0;
			shinePaint = 0.0f;
			break;
		case Varnish::Satin:
			shine0 = 100;
			shinePaint = 0.25f;
			break;
		case Varnish::Gloss:
			shine0 = 200;
			shinePaint = 0.2f;
			break;
		default:
			break;
		}
		const int x0 = a_rect ? std::max(0, a_rect->x0) : 0, x1 = a_rect ? std::min(a_w, a_rect->x1) : a_w;
		const int y0 = a_rect ? std::max(0, a_rect->y0) : 0, y1 = a_rect ? std::min(a_h, a_rect->y1) : a_h;
		for (int y = y0; y < y1; ++y) {
			for (int x = x0; x < x1; ++x) {
				const float dx = (surface(x + 1, y) - surface(x - 1, y)) * 0.5f * kSlope;
				const float dy = (surface(x, y + 1) - surface(x, y - 1)) * 0.5f * kSlope;
				// the normal in (u right, v down, out of the canvas)
				float       nx = -dx, ny = -dy;
				const float nz = 1.0f;
				if (a_style.flipY) {
					ny = -ny;
				}
				if (a_style.swapXY) {
					std::swap(nx, ny);
				}
				const float l = 1.0f / std::sqrt(nx * nx + ny * ny + nz * nz);
				std::uint8_t* o = &a_out[(static_cast<std::size_t>(y) * a_w + x) * 4];
				o[0] = static_cast<std::uint8_t>(std::clamp((nx * l * 0.5f + 0.5f) * 255.0f + 0.5f, 0.0f, 255.0f));
				o[1] = static_cast<std::uint8_t>(std::clamp((ny * l * 0.5f + 0.5f) * 255.0f + 0.5f, 0.0f, 255.0f));
				o[2] = static_cast<std::uint8_t>(std::clamp((nz * l * 0.5f + 0.5f) * 255.0f + 0.5f, 0.0f, 255.0f));
				const float paint = haveH ? a_height[static_cast<std::size_t>(y) * a_w + x] : 0.0f;
				o[3] = static_cast<std::uint8_t>(std::clamp(shine0 + paint * shinePaint, 0.0f, 255.0f));
			}
		}
	}

	std::vector<std::uint8_t> Rotate90(const std::vector<std::uint8_t>& a_px, int a_w, int a_h, bool a_clockwise)
	{
		std::vector<std::uint8_t> out(a_px.size());
		for (int y = 0; y < a_h; ++y) {
			for (int x = 0; x < a_w; ++x) {
				// clockwise: (x, y) -> (h - 1 - y, x) in a picture a_h wide
				const int nx = a_clockwise ? a_h - 1 - y : y;
				const int ny = a_clockwise ? x : a_w - 1 - x;
				std::memcpy(&out[(static_cast<std::size_t>(ny) * a_h + nx) * 4], &a_px[(static_cast<std::size_t>(y) * a_w + x) * 4], 4);
			}
		}
		return out;
	}
}
