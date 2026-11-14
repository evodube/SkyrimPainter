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

#include "Menus.h"

#include "Overlay.h"
#include "Store.h"
#include "Theme.h"

#define IMGUI_DEFINE_MATH_OPERATORS
#include <imgui.h>

namespace Menus
{
	namespace
	{
		Items::EaselMenuInfo g_easel;
		bool                 g_easelOpen = false;
		bool                 g_pickPainting = false;  // showing the list of paintings to put on
		int                  g_confirm = 0;           // 1 take off (painted), 2 scrape painting

		Place::MenuInfo g_painting;
		int             g_sizePct = 100;
		bool            g_paintingOpen = false;
		bool            g_renaming = false;
		char            g_renameBuf[64]{};
		int             g_renameFocus = 0;  // frames left to put the cursor in the name box

		// ---- small pictures of the paintings (made from their flat.png, one per frame at most)
		struct Thumb
		{
			std::string               key;  // the painting's texture path: a new one when it is repainted
			ID3D11ShaderResourceView* srv = nullptr;
			int                       w = 0, h = 0;
		};
		std::map<int, Thumb> g_thumbs;
		int                  g_thumbFrame = -1;

		void ReleaseThumbs()
		{
			for (auto& [slot, t] : g_thumbs) {
				if (t.srv) {
					t.srv->Release();
				}
			}
			g_thumbs.clear();
		}

		const Thumb* GetThumb(int a_slot)
		{
			Items::SlotView v;
			if (!Items::GetSlot(a_slot, v)) {
				return nullptr;
			}
			auto it = g_thumbs.find(a_slot);
			if (it != g_thumbs.end() && it->second.key == v.dds) {
				return it->second.srv ? &it->second : nullptr;
			}
			if (g_thumbFrame == ImGui::GetFrameCount()) {
				return nullptr;  // one picture loaded per frame
			}
			g_thumbFrame = ImGui::GetFrameCount();
			std::vector<std::uint8_t> px;
			int                       w = 0, h = 0;
			Thumb                     t;
			t.key = v.dds;
			if (Store::LoadFlat(v.dir, px, w, h) && w > 0 && h > 0) {
				// shrink to 160 px across, averaging the pixels each one covers
				const int tw = std::min(160, w), th = std::max(1, h * tw / w);
				std::vector<std::uint8_t> small(static_cast<std::size_t>(tw) * th * 4);
				for (int y = 0; y < th; ++y) {
					for (int x = 0; x < tw; ++x) {
						const int x0 = x * w / tw, x1 = std::max(x0 + 1, (x + 1) * w / tw);
						const int y0 = y * h / th, y1 = std::max(y0 + 1, (y + 1) * h / th);
						unsigned  sum[4]{}, n = 0;
						for (int yy = y0; yy < y1; ++yy) {
							for (int xx = x0; xx < x1; ++xx) {
								const auto* q = &px[(static_cast<std::size_t>(yy) * w + xx) * 4];
								for (int c = 0; c < 4; ++c) {
									sum[c] += q[c];
								}
								++n;
							}
						}
						for (int c = 0; c < 4; ++c) {
							small[(static_cast<std::size_t>(y) * tw + x) * 4 + c] = static_cast<std::uint8_t>(sum[c] / std::max(1u, n));
						}
					}
				}
				t.srv = Overlay::MakeSRV(small.data(), tw, th);
				t.w = tw;
				t.h = th;
			}
			if (it != g_thumbs.end() && it->second.srv) {
				it->second.srv->Release();
			}
			g_thumbs[a_slot] = t;
			return t.srv ? &g_thumbs[a_slot] : nullptr;
		}

		// the picture, a_h tall (an empty frame while it loads)
		void ThumbImage(int a_slot, float a_h)
		{
			const Thumb* t = GetThumb(a_slot);
			const float  aspect = t ? static_cast<float>(t->w) / static_cast<float>(t->h) : 1.333f;
			const ImVec2 size(a_h * aspect, a_h);
			const ImVec2 p = ImGui::GetCursorScreenPos();
			if (t) {
				ImGui::Image(reinterpret_cast<ImTextureID>(t->srv), size);
			} else {
				ImGui::Dummy(size);
			}
			ImGui::GetWindowDrawList()->AddRect(p, p + size, IM_COL32(60, 44, 30, 255), 0.0f, 0, 1.5f);
		}

		bool BeginSmall(const char* a_id, const char* a_title, const char* a_subtitle, float a_w)
		{
			const ImVec2 screen = ImGui::GetIO().DisplaySize;
			ImGui::SetNextWindowPos(screen * 0.5f, ImGuiCond_Always, ImVec2(0.5f, 0.5f));
			ImGui::SetNextWindowSize(ImVec2(Theme::S(a_w), 0.0f), ImGuiCond_Always);
			const bool open = Theme::BeginWindow(a_id, nullptr, ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_AlwaysAutoResize);
			return open && !Theme::Header(a_title, a_subtitle, nullptr, 0, true);
		}

		bool Wide(const char* a_label, bool a_enabled = true, const char* a_why = nullptr, bool a_primary = false)
		{
			ImGui::BeginDisabled(!a_enabled);
			const bool clicked = Theme::Button(a_label, ImVec2(ImGui::GetContentRegionAvail().x, 0.0f), a_primary);
			ImGui::EndDisabled();
			if (!a_enabled && a_why && ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) {
				ImGui::SetTooltip("%s", a_why);
			}
			return clicked && a_enabled;
		}
	}

	void OpenEasel(const Items::EaselMenuInfo& a_info)
	{
		g_easel = a_info;
		g_easelOpen = true;
		g_pickPainting = false;
		g_confirm = 0;
	}

	bool DrawEasel()
	{
		if (!g_easelOpen) {
			return false;
		}
		const auto& e = g_easel;
		std::string sub = !e.hasCanvas ? std::string("No canvas on it") :
		                  e.slot >= 0  ? fmt::format("\"{}\" ({})", e.name, e.portrait ? "portrait" : "landscape") :
		                                 fmt::format("{} canvas{}", e.portrait ? "Portrait" : "Landscape", e.painted ? ", painting in progress" : ", blank");
		bool close = false;
		if (!BeginSmall("##easelmenu", "Easel", sub.c_str(), 460.0f)) {
			close = true;
		} else if (g_confirm) {
			ImGui::PushTextWrapPos(0.0f);
			ImGui::TextUnformatted(g_confirm == 1 ? (e.slot >= 0 ? "Take the painting off? Changes since you last finished it are lost." :
			                                                       "Take the canvas off? The paint on it is scraped off.") :
			                                        "Scrape this painting off for good? It frees one of your 50 painting slots and leaves a blank canvas.");
			ImGui::PopTextWrapPos();
			ImGui::Dummy(ImVec2(0, Theme::S(8)));
			if (Wide(g_confirm == 1 ? "Take it off" : "Scrape it off", true, nullptr, true)) {
				if (g_confirm == 1) {
					Items::TakeCanvasOff(e.easel);
				} else {
					Items::ScrapePainting(e.easel);
				}
				close = true;
			}
			if (Wide("Back")) {
				g_confirm = 0;
			}
		} else if (g_pickPainting) {
			Theme::SectionLabel("Put a painting on to work on it");
			const float th = Theme::S(64.0f);
			for (const auto& [slot, name] : e.paintingsInPack) {
				ImGui::PushID(slot);
				ThumbImage(slot, th);
				ImGui::SameLine();
				if (Theme::Button(name.c_str(), ImVec2(ImGui::GetContentRegionAvail().x, th))) {
					Items::EditPainting(e.easel, slot);
					close = true;
				}
				ImGui::PopID();
			}
			if (Wide("Back")) {
				g_pickPainting = false;
			}
		} else if (!e.hasCanvas) {
			const char* noCanvas = "You need a Blank Canvas (tanning rack: 2 Linen Wrap, 1 Firewood)";
			if (Wide("Put on a blank canvas - landscape", e.haveCanvasItem, noCanvas, true)) {
				Items::SetCanvas(e.easel, false);
				close = true;
			}
			if (Wide("Put on a blank canvas - portrait", e.haveCanvasItem, noCanvas)) {
				Items::SetCanvas(e.easel, true);
				close = true;
			}
			if (Wide("Put on one of your paintings...", !e.paintingsInPack.empty(), "You have no finished paintings with you (take one down from the wall first)")) {
				g_pickPainting = true;
			}
			if (Wide("Fold up the easel")) {
				Items::PickUpEasel(e.easel);
				close = true;
			}
		} else {
			const bool tools = e.havePalette && e.havePaints;
			if (Wide("Paint", tools, "You need a Painter's Palette and Paints", true)) {
				Items::Paint(e.easel);
				close = true;
			}
			if (Wide(e.portrait ? "Turn the canvas to landscape" : "Turn the canvas to portrait")) {
				Items::TurnCanvas(e.easel);
				close = true;
			}
			if (Wide(e.slot >= 0 ? "Take the painting off" : "Take the canvas off")) {
				if (e.painted && e.slot < 0) {
					g_confirm = 1;
				} else {
					Items::TakeCanvasOff(e.easel);
					close = true;
				}
			}
			if (e.slot >= 0 && Wide("Scrape the painting off (frees its slot)")) {
				g_confirm = 2;
			}
			if (!e.painted && e.freeSlots == 0) {
				Theme::TextColoredWrapped(Theme::Warning(), "All 50 painting slots are used: scrape an old painting off to finish a new one.");
			}
		}
		if (!close && g_easelOpen && Wide("Close")) {
			close = true;
		}
		if (ImGui::IsKeyPressed(ImGuiKey_Escape, false)) {
			close = true;
		}
		Theme::EndWindow();
		if (close) {
			g_easelOpen = false;
			ReleaseThumbs();
		}
		return g_easelOpen;
	}

	void OpenPainting(const Place::MenuInfo& a_info)
	{
		g_painting = a_info;
		g_sizePct = static_cast<int>(std::lround(a_info.scale * 100.0f));
		g_renaming = false;
		g_paintingOpen = true;
	}

	bool DrawPainting()
	{
		if (!g_paintingOpen) {
			return false;
		}
		Items::SlotView v;
		Items::GetSlot(g_painting.slot, v);
		bool close = false;
		if (!BeginSmall("##paintingmenu", "Painting", v.name.c_str(), 440.0f)) {
			close = true;
		} else {
			{
				const float  th = Theme::S(150.0f);
				const Thumb* t = GetThumb(g_painting.slot);
				const float  tw = t ? th * t->w / static_cast<float>(t->h) : th * 1.333f;
				ImGui::SetCursorPosX(ImGui::GetCursorPosX() + (ImGui::GetContentRegionAvail().x - tw) * 0.5f);
				ThumbImage(g_painting.slot, th);
				ImGui::Dummy(ImVec2(0, Theme::S(4)));
			}
			if (g_renaming) {
				ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x);
				if (g_renameFocus > 0) {
					--g_renameFocus;
					ImGui::SetKeyboardFocusHere();
				}
				const bool enter = ImGui::InputText("##rename", g_renameBuf, sizeof g_renameBuf, ImGuiInputTextFlags_EnterReturnsTrue | ImGuiInputTextFlags_AutoSelectAll);
				const float half = (ImGui::GetContentRegionAvail().x - ImGui::GetStyle().ItemSpacing.x) * 0.5f;
				if (Theme::Button("Rename", ImVec2(half, 0), true) || enter) {
					Items::RenamePainting(g_painting.slot, g_renameBuf);
					g_renaming = false;
				}
				ImGui::SameLine();
				if (Theme::Button("Cancel", ImVec2(half, 0))) {
					g_renaming = false;
				}
				ImGui::Dummy(ImVec2(0, Theme::S(6)));
			}
			if (Theme::SliderInt("Size", &g_sizePct, 30, 300, "%d%%", ImGui::GetContentRegionAvail().x)) {
				Place::SetScale(g_painting.ref, g_sizePct / 100.0f);
			}
			ImGui::Dummy(ImVec2(0, Theme::S(6)));
			if (!g_renaming && Wide("Rename")) {
				std::snprintf(g_renameBuf, sizeof g_renameBuf, "%s", v.name.c_str());
				g_renaming = true;
				g_renameFocus = 3;
			}
			if (Wide("Move")) {
				Place::Move(g_painting.ref);
				close = true;
			}
			if (Wide("Take down")) {
				Place::TakeDown(g_painting.ref);
				close = true;
			}
			if (Wide("Close", true, nullptr, true)) {
				close = true;
			}
		}
		if (ImGui::IsKeyPressed(ImGuiKey_Escape, false)) {
			if (g_renaming) {
				g_renaming = false;
			} else {
				close = true;
			}
		}
		Theme::EndWindow();
		if (close) {
			g_paintingOpen = false;
			g_renaming = false;
			ReleaseThumbs();
		}
		return g_paintingOpen;
	}

	void DrawPlaceHud(const Place::Status& a_s)
	{
		const ImVec2 screen = ImGui::GetIO().DisplaySize;
		std::string  line1, line2;
		if (a_s.mode == Place::Mode::Easel) {
			line1 = fmt::format("Setting up the easel  \xC2\xB7  turned {:+d}\xC2\xB0", a_s.turnDeg);
			line2 = "Click: set it up   \xC2\xB7   Wheel or Left/Right: turn (Shift: finer)   \xC2\xB7   Right-click / Esc: cancel";
		} else {
			line1 = fmt::format("Hanging \"{}\"  \xC2\xB7  Size {:.0f}%  \xC2\xB7  Distance {:+.2f}", a_s.name, a_s.scale * 100.0f, a_s.distance);
			line2 = "Click: hang   \xC2\xB7   Wheel or Left/Right: size   \xC2\xB7   Up/Down: distance from the wall   \xC2\xB7   Right-click / Esc: cancel";
		}
		ImGui::SetNextWindowPos(ImVec2(screen.x * 0.5f, screen.y - Theme::S(40.0f)), ImGuiCond_Always, ImVec2(0.5f, 1.0f));
		ImGui::SetNextWindowBgAlpha(0.7f);
		ImGui::Begin("##placehud", nullptr,
			ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoInputs | ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoFocusOnAppearing |
				ImGuiWindowFlags_NoNav | ImGuiWindowFlags_NoSavedSettings);
		ImGui::TextUnformatted(line1.c_str());
		ImGui::TextDisabled("%s", line2.c_str());
		ImGui::End();
	}

	void Release()
	{
		g_easelOpen = false;
		g_paintingOpen = false;
		g_renaming = false;
		ReleaseThumbs();
	}

	void Flush() {}
}
