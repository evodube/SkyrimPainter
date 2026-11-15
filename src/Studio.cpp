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

#include "Studio.h"

#include "Art.h"
#include "Live.h"
#include "Overlay.h"
#include "Paint.h"
#include "PaletteSpots.h"
#include "Spectral.h"
#include "Store.h"
#include "Surface.h"
#include "Theme.h"

#define IMGUI_DEFINE_MATH_OPERATORS
#include <imgui.h>

#include <stb_image.h>  // implementation lives in Dds.cpp

namespace fs = std::filesystem;

namespace Studio
{
	namespace
	{
		using Rgb = Spectral::Rgb;
		constexpr int kMaxMixed = 24;

		struct Pigment
		{
			const char* name;
			Rgb         rgb;
		};
		// The Paints: what is on the palette when you start. Everything else comes from mixing.
		constexpr Pigment kPigments[PaletteSpots::kCount] = {
			{ "Bonemeal White", { 245, 242, 234 } },
			{ "Sun Gold", { 237, 196, 41 } },
			{ "Yellow Ochre", { 196, 150, 61 } },
			{ "Burnt Sienna", { 138, 69, 41 } },
			{ "Dragon's Blood Red", { 176, 33, 31 } },
			{ "Madder Rose", { 199, 79, 99 } },
			{ "Nightshade Violet", { 99, 56, 120 } },
			{ "Lapis Blue", { 36, 61, 161 } },
			{ "Frost Blue", { 31, 69, 110 } },
			{ "Pine Green", { 41, 110, 79 } },
			{ "Raw Umber", { 99, 79, 59 } },
			{ "Soot Black", { 28, 26, 26 } },
		};

		struct Spot
		{
			float x, y;  // 0..1 on the palette picture
			Rgb   c;
			bool  mixed;  // a puddle you put down (the paints from the tubes are not)
			std::vector<std::pair<Rgb, int>> parts{};  // a puddle: the paints mixed into it, and how many times each

			void Remix()
			{
				std::vector<std::pair<Rgb, double>> w;
				for (const auto& [col, n] : parts) {
					w.push_back({ col, static_cast<double>(n) });
				}
				if (!w.empty()) {
					c = w.size() == 1 ? w[0].first : Spectral::Mix(w);
				}
			}
			int Parts() const
			{
				int n = 0;
				for (const auto& part : parts) {
					n += part.second;
				}
				return n;
			}
		};

		struct Picture
		{
			ID3D11ShaderResourceView* srv = nullptr;
			int                       w = 0, h = 0;
			bool                      tried = false;
		};

		// ---- state (render thread)
		bool                      g_open = false;
		Items::Work               g_work;
		Paint::Painting           g_paint;
		Paint::Brush              g_brush;  // in hand
		Paint::Brush              g_rack[Paint::kBrushCount];  // the brushes put down keep the paint that was on them
		ImVec2                    g_dir{ 1.0f, 0.0f };         // smoothed stroke direction, canvas pixels (for the cursor)
		ImVec2                    g_lastCanvas{ -1e9f, -1e9f };
		// painting on the canvas in the world (the default) or in the flat close-up
		bool                      g_closeUp = false;
		std::string               g_liveDds;          // the canvas's own texture for this session
		double                    g_openedAt = 0.0;
		bool                      g_liveWarned = false;
		bool                      g_overCanvas = false;
		bool                      g_hideHud = false;  // Tab: only the canvas stays up
		bool                      g_guide = false;    // the painter's guide is open (F1)
		int                       g_placeFrames = 3;     // > 0: the panels are put in their places (a few frames, while they size themselves)
		bool                      g_canvasPlace = true;  // the close-up window is put in its place the next time it shows
		float                     g_ui = 0.85f;          // HUD size ([Studio] fHudSize)
		// the hand (Krita: pressure, opacity, smoothing) and the view (zoom, pan, mirror)
		float                     g_body = 1.0f;    // paint thickness: thin glaze .. full body
		bool                      g_rag = false;    // the rag is in hand (lifts paint off the canvas)
		Paint::Brush              g_ragHand;        // stands in for a brush while the rag is used
		Paint::Stroker            g_stroker;
		std::vector<Paint::StrokePoint> g_points;
		float                     g_zoom = 1.0f;    // 1 = the whole canvas fits
		ImVec2                    g_pan{ 0.0f, 0.0f };
		bool                      g_mirror = false;
		bool                      g_worldZoom = false;  // Z on the easel: looking 25% closer
		float                     g_pressure = 0.6f;
		std::vector<Spot>         g_spots;
		bool                      g_spotsLoaded = false;
		ID3D11Texture2D*          g_tex = nullptr;
		ID3D11ShaderResourceView* g_srv = nullptr;
		// the relief: the paint's thickness and the canvas weave as a normal map, lent to the canvas in the world
		ID3D11Texture2D*          g_reliefTex = nullptr;
		ID3D11ShaderResourceView* g_reliefSrv = nullptr;
		std::vector<std::uint8_t> g_relief;  // RGBA8, the whole canvas
		int                       g_varnish = 0;  // Paint::Varnish (chosen when finishing; kept with the painting)
		int                       g_strokes = 0;          // strokes on this painting, all sessions
		double                    g_paintSeconds = 0.0;   // time spent actually painting it, all sessions
		double                    g_lastStrokeAt = -1e9;  // time only counts while you're painting, not standing there
		std::vector<IUnknown*>    g_release;  // freed at the start of the next frame
		Picture                   g_palettePic, g_brushPic;
		char                      g_name[64]{};
		bool                      g_openFinish = false, g_openClear = false;
		std::string               g_hint;
		double                    g_hintUntil = 0.0;
		bool                      g_painting = false;  // left button held on the canvas

		void Hint(const char* a_text)
		{
			g_hint = a_text;
			g_hintUntil = ImGui::GetTime() + 3.0;
		}

		ImU32 Col(const Rgb& c, int a = 255) { return IM_COL32(c[0], c[1], c[2], a); }
		float U(float a_v) { return Theme::S(a_v) * g_ui; }  // sizes in the panels follow the HUD size

		// ---- the bare canvas: plain white primed canvas with a fine weave (as canvas_blank.dds)
		float Hash(int x, int y)
		{
			std::uint32_t h = static_cast<std::uint32_t>(x) * 374761393u + static_cast<std::uint32_t>(y) * 668265263u;
			h = (h ^ (h >> 13)) * 1274126177u;
			return static_cast<float>((h ^ (h >> 16)) & 0xFFFF) / 65535.0f;
		}

		std::vector<std::uint8_t> MakeBlank(int W, int H)
		{
			std::vector<std::uint8_t> blank(static_cast<std::size_t>(W) * H * 4);
			std::vector<float>        col(W), row(H);
			for (int x = 0; x < W; ++x) {
				col[x] = (Hash(x, 7) - 0.5f) * 3.0f;
			}
			for (int y = 0; y < H; ++y) {
				row[y] = (Hash(13, y) - 0.5f) * 3.0f;
			}
			for (int y = 0; y < H; ++y) {
				for (int x = 0; x < W; ++x) {
					const float d = std::sin(x * 2.094f) * std::sin(y * 2.094f) * 3.0f + col[x] + row[y];
					auto*       p = &blank[(static_cast<std::size_t>(y) * W + x) * 4];
					p[0] = static_cast<std::uint8_t>(std::clamp(248.0f + d, 0.0f, 255.0f));
					p[1] = static_cast<std::uint8_t>(std::clamp(247.0f + d, 0.0f, 255.0f));
					p[2] = static_cast<std::uint8_t>(std::clamp(244.0f + d, 0.0f, 255.0f));
					p[3] = 255;
				}
			}
			return blank;
		}

		// ---- textures
		void LoadPicture(Picture& a_pic, const wchar_t* a_file)
		{
			if (a_pic.tried) {
				return;
			}
			a_pic.tried = true;
			std::error_code ec;
			const auto      path = Art::ToUtf8(fs::absolute(fs::path(L"Data\\SKSE\\Plugins\\SkyrimPainter\\UI") / a_file, ec));
			int             w = 0, h = 0, n = 0;
			if (auto* px = stbi_load(path.c_str(), &w, &h, &n, 4)) {
				a_pic.srv = Overlay::MakeSRV(px, w, h);
				a_pic.w = w;
				a_pic.h = h;
				stbi_image_free(px);
			} else {
				spdlog::warn("Studio: {} missing", path);
			}
		}

		bool CreateCanvasTexture()
		{
			auto* dev = Overlay::Device();
			if (!dev) {
				return false;
			}
			D3D11_TEXTURE2D_DESC d{};
			d.Width = static_cast<UINT>(g_paint.Width());
			d.Height = static_cast<UINT>(g_paint.Height());
			d.MipLevels = 1;
			d.ArraySize = 1;
			d.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
			d.SampleDesc.Count = 1;
			d.Usage = D3D11_USAGE_DEFAULT;
			d.BindFlags = D3D11_BIND_SHADER_RESOURCE;
			D3D11_SUBRESOURCE_DATA sd{ g_paint.Composite().data(), static_cast<UINT>(g_paint.Width()) * 4, 0 };
			if (FAILED(dev->CreateTexture2D(&d, &sd, &g_tex)) || !g_tex) {
				g_tex = nullptr;
				return false;
			}
			if (FAILED(dev->CreateShaderResourceView(g_tex, nullptr, &g_srv)) || !g_srv) {
				g_tex->Release();
				g_tex = nullptr;
				return false;
			}
			// the relief (not needed to paint: without it the canvas just looks flat)
			Paint::MakeNormalMap(g_paint.Thickness(), g_paint.Width(), g_paint.Height(), static_cast<Paint::Varnish>(g_varnish), Art::ReliefStyle(), g_relief);
			D3D11_SUBRESOURCE_DATA rd{ g_relief.data(), static_cast<UINT>(g_paint.Width()) * 4, 0 };
			if (SUCCEEDED(dev->CreateTexture2D(&d, &rd, &g_reliefTex)) && g_reliefTex) {
				if (FAILED(dev->CreateShaderResourceView(g_reliefTex, nullptr, &g_reliefSrv)) || !g_reliefSrv) {
					g_reliefTex->Release();
					g_reliefTex = nullptr;
				}
			}
			return true;
		}

		void Upload()
		{
			const auto r = g_paint.TakeDirty();
			if (!g_tex || r.Empty()) {
				return;
			}
			D3D11_BOX   box{ static_cast<UINT>(r.x0), static_cast<UINT>(r.y0), 0, static_cast<UINT>(r.x1), static_cast<UINT>(r.y1), 1 };
			const auto* src = &g_paint.Composite()[(static_cast<std::size_t>(r.y0) * g_paint.Width() + r.x0) * 4];
			Overlay::Context()->UpdateSubresource(g_tex, 0, &box, src, static_cast<UINT>(g_paint.Width()) * 4, 0);
			// the relief around the change (its slopes reach one pixel further)
			if (g_reliefTex) {
				Paint::Rect rr{ std::max(0, r.x0 - 1), std::max(0, r.y0 - 1), std::min(g_paint.Width(), r.x1 + 1), std::min(g_paint.Height(), r.y1 + 1) };
				Paint::MakeNormalMap(g_paint.Thickness(), g_paint.Width(), g_paint.Height(), static_cast<Paint::Varnish>(g_varnish), Art::ReliefStyle(), g_relief, &rr);
				D3D11_BOX   rbox{ static_cast<UINT>(rr.x0), static_cast<UINT>(rr.y0), 0, static_cast<UINT>(rr.x1), static_cast<UINT>(rr.y1), 1 };
				const auto* rsrc = &g_relief[(static_cast<std::size_t>(rr.y0) * g_paint.Width() + rr.x0) * 4];
				Overlay::Context()->UpdateSubresource(g_reliefTex, 0, &rbox, rsrc, static_cast<UINT>(g_paint.Width()) * 4, 0);
			}
		}

		// the whole relief for saving, with the varnish
		std::vector<std::uint8_t> ReliefFor(int a_varnish)
		{
			std::vector<std::uint8_t> out;
			Paint::MakeNormalMap(g_paint.Thickness(), g_paint.Width(), g_paint.Height(), static_cast<Paint::Varnish>(a_varnish), Art::ReliefStyle(), out);
			return out;
		}

		// ---- the palette's paint spots (pigments + what the player mixed, kept in the INI)
		// ---- your colours: the last ones painted with, newest first
		std::vector<Rgb> g_recent;
		constexpr int    kRecent = 10;

		void SaveRecent()
		{
			std::string out;
			for (const auto& c : g_recent) {
				out += fmt::format("{}{:02X}{:02X}{:02X}", out.empty() ? "" : ";", c[0], c[1], c[2]);
			}
			::WritePrivateProfileStringA("Palette", "sRecent", out.c_str(), Overlay::IniPath().c_str());
		}

		void RememberColour(const Rgb& a_c)
		{
			if (!g_recent.empty() && g_recent.front() == a_c) {
				return;
			}
			g_recent.erase(std::remove(g_recent.begin(), g_recent.end(), a_c), g_recent.end());
			g_recent.insert(g_recent.begin(), a_c);
			if (static_cast<int>(g_recent.size()) > kRecent) {
				g_recent.resize(kRecent);
			}
			SaveRecent();
		}

		Rgb  g_prevColour{};  // the colour before the last one (X swaps back to it)
		bool g_hasPrev = false;

		// the brush takes a colour
		void LoadColour(const Rgb& a_c)
		{
			if (g_brush.type == Paint::kCharcoal) {
				return;  // charcoal is charcoal
			}
			if (g_brush.Loaded() && g_brush.color != a_c) {
				g_prevColour = g_brush.color;
				g_hasPrev = true;
			}
			g_brush.color = a_c;
			g_brush.load = 1.0f;
		}

		bool AltHeld() { return (::GetAsyncKeyState(VK_MENU) & 0x8000) != 0; }

		// a straight line: Shift + click carries on from where the last stroke ended
		ImVec2 g_lineFrom{};
		bool   g_hasLineFrom = false;

		int ColourDistance(const Rgb& a_a, const Rgb& a_b)
		{
			return std::abs(a_a[0] - a_b[0]) + std::abs(a_a[1] - a_b[1]) + std::abs(a_a[2] - a_b[2]);
		}

		void LoadSpots()
		{
			if (g_spotsLoaded) {
				return;
			}
			g_spotsLoaded = true;
			for (int i = 0; i < PaletteSpots::kCount; ++i) {
				g_spots.push_back({ PaletteSpots::kSpots[i][0], PaletteSpots::kSpots[i][1], kPigments[i].rgb, false });
			}
			char buf[2048]{};
			::GetPrivateProfileStringA("Palette", "sRecent", "", buf, sizeof buf, Overlay::IniPath().c_str());
			{
				std::stringstream rs(buf);
				for (std::string item; std::getline(rs, item, ';') && static_cast<int>(g_recent.size()) < kRecent;) {
					unsigned rgb = 0;
					if (std::sscanf(item.c_str(), "%6x", &rgb) == 1) {
						g_recent.push_back({ static_cast<std::uint8_t>(rgb >> 16), static_cast<std::uint8_t>(rgb >> 8), static_cast<std::uint8_t>(rgb) });
					}
				}
			}
			buf[0] = 0;
			::GetPrivateProfileStringA("Palette", "sMixed", "", buf, sizeof buf, Overlay::IniPath().c_str());
			std::stringstream ss(buf);
			for (std::string item; std::getline(ss, item, ';');) {
				// RRGGBBxN+RRGGBBxN+...@x,y (older: RRGGBB@x,y)
				const auto at = item.find('@');
				float      x = 0, y = 0;
				if (at == std::string::npos || std::sscanf(item.c_str() + at + 1, "%f,%f", &x, &y) != 2) {
					continue;
				}
				Spot spot{ std::clamp(x, 0.0f, 1.0f), std::clamp(y, 0.0f, 1.0f), {}, true };
				std::stringstream ps(item.substr(0, at));
				for (std::string part; std::getline(ps, part, '+');) {
					unsigned rgb = 0;
					int      n = 1;
					const int got = std::sscanf(part.c_str(), "%6xx%d", &rgb, &n);
					if (got >= 1) {
						spot.parts.push_back({ { static_cast<std::uint8_t>(rgb >> 16), static_cast<std::uint8_t>(rgb >> 8), static_cast<std::uint8_t>(rgb) }, std::clamp(got == 2 ? n : 1, 1, 99) });
					}
				}
				if (!spot.parts.empty()) {
					spot.Remix();
					g_spots.push_back(std::move(spot));
				}
			}
		}

		void SaveSpots()
		{
			std::string out;
			for (const auto& s : g_spots) {
				if (s.mixed) {
					std::string parts;
					for (const auto& [col, n] : s.parts) {
						parts += fmt::format("{}{:02X}{:02X}{:02X}x{}", parts.empty() ? "" : "+", col[0], col[1], col[2], n);
					}
					out += fmt::format("{}{}@{:.4f},{:.4f}", out.empty() ? "" : ";", parts, s.x, s.y);
				}
			}
			::WritePrivateProfileStringA("Palette", "sMixed", out.c_str(), Overlay::IniPath().c_str());
		}

		// ---- saving: the layers to the work folder, then the game side (worker thread)
		std::mutex g_saveMtx;  // one save of the work folder at a time (autosave and stepping away)

		// ---- autosave: the work folder every few minutes while painting, so a crash loses little
		constexpr double kAutosaveEvery = 180.0;  // seconds
		// The work that sets the price: each stroke counts, and the time since the last one does too, up to
		// 20 seconds (stepping back to look is painting; leaving the studio open is not).
		void CountStroke(double a_now)
		{
			++g_strokes;
			g_paintSeconds += std::clamp(a_now - g_lastStrokeAt, 0.0, 20.0);
			g_lastStrokeAt = a_now;
		}

		int              g_edits = 0;            // strokes, undos, redos... since the studio opened
		int              g_editsSaved = 0;
		double           g_autosavedAt = 0.0;

		void AutosaveTick()
		{
			const double now = ImGui::GetTime();
			if (g_painting || g_edits == g_editsSaved || now - g_autosavedAt < kAutosaveEvery) {
				return;
			}
			g_autosavedAt = now;
			g_editsSaved = g_edits;
			Store::Doc doc;
			doc.w = g_paint.Width();
			doc.h = g_paint.Height();
			for (const auto& l : g_paint.Layers()) {
				doc.layers.push_back({ l.name, l.visible, l.opacity, l.px, static_cast<int>(l.blend), l.lockAlpha });
			}
			doc.thickness = g_paint.Thickness();
			doc.varnish = g_varnish;
			doc.strokes = g_strokes;
			doc.seconds = static_cast<int>(g_paintSeconds);
			std::vector<std::uint8_t> flat = g_paint.Composite();
			std::thread([doc = std::move(doc), flat = std::move(flat), dir = g_work.dir]() mutable {
				std::lock_guard lk(g_saveMtx);
				std::string     err;
				if (Store::SaveDoc(dir, doc, flat, err)) {
					spdlog::info("Studio: autosaved");
				} else {
					spdlog::warn("Studio: autosave failed: {}", err);
				}
			}).detach();
		}

		void SaveAndThen(bool a_finish, std::string a_name)
		{
			Store::Doc doc;
			doc.w = g_paint.Width();
			doc.h = g_paint.Height();
			for (const auto& l : g_paint.Layers()) {
				doc.layers.push_back({ l.name, l.visible, l.opacity, l.px, static_cast<int>(l.blend), l.lockAlpha });
			}
			doc.thickness = g_paint.Thickness();
			doc.varnish = g_varnish;
			doc.strokes = g_strokes;
			doc.seconds = static_cast<int>(g_paintSeconds);
			std::vector<std::uint8_t> flat = g_paint.Composite();
			std::vector<std::uint8_t> relief = ReliefFor(g_varnish);
			const auto                dir = g_work.dir;
			const auto                easel = g_work.easel;
			std::thread([doc = std::move(doc), flat = std::move(flat), relief = std::move(relief), dir, easel, a_finish, name = std::move(a_name)]() mutable {
				std::string err;
				bool        saved;
				{
					std::lock_guard lk(g_saveMtx);
					saved = Store::SaveDoc(dir, doc, flat, err);
				}
				if (!saved) {
					spdlog::error("Studio: saving the painting failed: {}", err);
					Art::Notify("SkyrimPainter: could not save the painting (see SkyrimPainter.log)");
					return;
				}
				if (a_finish) {
					Items::Finish(easel, std::move(name), std::move(flat), doc.w, doc.h, std::move(relief));
				} else {
					Items::StepAway(easel, std::move(flat), doc.w, doc.h, std::move(relief));
				}
			}).detach();
		}

		void Close(bool a_saving = false)
		{
			// the canvas in the world keeps showing the paint until the saved picture takes over
			Live::Detach(a_saving);
			if (g_open) {
				Live::RestoreView();
			}
			if (g_srv) {
				g_release.push_back(g_srv);
				g_srv = nullptr;
			}
			if (g_tex) {
				g_release.push_back(g_tex);
				g_tex = nullptr;
			}
			if (g_reliefSrv) {
				g_release.push_back(g_reliefSrv);
				g_reliefSrv = nullptr;
			}
			if (g_reliefTex) {
				g_release.push_back(g_reliefTex);
				g_reliefTex = nullptr;
			}
			g_open = false;
			g_painting = false;
			g_paint = Paint::Painting{};
		}

		void StepAway()
		{
			const bool save = g_paint.Changed();
			if (save) {
				SaveAndThen(false, {});
			}
			Close(save);
		}

		// ---- drawing helpers
		void PaintBlob(ImDrawList* a_dl, const ImVec2& a_c, float a_r, const Rgb& a_col, std::uint32_t a_seed)
		{
			// a dollop of paint: a few overlapping lumps, a darker rim and a wet highlight
			auto dark = Col({ static_cast<std::uint8_t>(a_col[0] * 0.7f), static_cast<std::uint8_t>(a_col[1] * 0.7f), static_cast<std::uint8_t>(a_col[2] * 0.7f) });
			for (int k = 0; k < 4; ++k) {
				const float ang = (a_seed * 37 + k * 97) % 360 * 0.01745f;
				const float off = a_r * 0.28f;
				a_dl->AddCircleFilled(a_c + ImVec2(std::cos(ang) * off, std::sin(ang) * off), a_r * (0.72f - k * 0.04f) + 1.0f, dark, 20);
			}
			for (int k = 0; k < 4; ++k) {
				const float ang = (a_seed * 37 + k * 97) % 360 * 0.01745f;
				const float off = a_r * 0.25f;
				a_dl->AddCircleFilled(a_c + ImVec2(std::cos(ang) * off, std::sin(ang) * off), a_r * (0.7f - k * 0.04f), Col(a_col), 20);
			}
			a_dl->AddCircleFilled(a_c + ImVec2(-a_r * 0.25f, -a_r * 0.3f), a_r * 0.18f, IM_COL32(255, 255, 255, 90), 12);
		}

		void DrawBrushPic(ImDrawList* a_dl, const ImVec2& a_tip, float a_len, const Paint::Brush* a_paint, int a_alpha = 255)
		{
			// the picture's bristle tip goes to a_tip; a_len = picture width on screen
			if (!g_brushPic.srv) {
				a_dl->AddLine(a_tip, a_tip + ImVec2(-a_len * 0.7f, a_len * 0.4f), IM_COL32(60, 40, 25, a_alpha), Theme::S(4.0f));
				return;
			}
			const float  h = a_len / PaletteSpots::kBrushAspect;
			const ImVec2 origin = a_tip - ImVec2(PaletteSpots::kBrushTip[0] * a_len, PaletteSpots::kBrushTip[1] * h);
			a_dl->AddImage(reinterpret_cast<ImTextureID>(g_brushPic.srv), origin, origin + ImVec2(a_len, h), ImVec2(0, 0), ImVec2(1, 1), IM_COL32(255, 255, 255, a_alpha));
			if (a_paint && a_paint->Loaded()) {
				const float r = Theme::S(5.0f) + a_len * 0.02f;
				a_dl->AddCircleFilled(a_tip + ImVec2(-r * 0.6f, r * 0.6f), r, Col(a_paint->color, a_alpha), 16);
			}
		}

		// The brushes on the rack, drawn: a lacquered handle, a metal ferrule and the hairs, each brush its own shape.
		// Lies diagonally, handle bottom left, tip top right, centred on a_c; a_len = the whole brush.
		void DrawBrushArt(ImDrawList* a_dl, int a_type, const ImVec2& a_c, float a_len)
		{
			const ImVec2 u(0.866f, -0.5f), n(0.5f, 0.866f);  // along the brush (to the tip), across it
			const ImVec2 base = a_c - u * (a_len * 0.5f);
			auto P = [&](float t, float w) { return base + u * (t * a_len) + n * (w * a_len); };
			auto quad = [&](float t0, float w0, float t1, float w1, ImU32 c) {
				const ImVec2 q[4] = { P(t0, -w0), P(t1, -w1), P(t1, w1), P(t0, w0) };
				a_dl->AddConvexPolyFilled(q, 4, c);
			};
			struct Look
			{
				ImU32 handle, handleHi;
				float ferruleEnd;  // width of the ferrule at the hairs (flattened for flats, filberts and fans)
			};
			static const Look kLook[5] = {
				{ IM_COL32(58, 52, 48, 255), IM_COL32(150, 140, 130, 255), 0.022f },  // detail: black lacquer, a fine round ferrule
				{ IM_COL32(120, 36, 30, 255), IM_COL32(190, 90, 80, 255), 0.030f },  // round (normally the picture)
				{ IM_COL32(128, 40, 32, 255), IM_COL32(196, 96, 82, 255), 0.050f },  // filbert: oxblood
				{ IM_COL32(34, 64, 46, 255), IM_COL32(96, 140, 110, 255), 0.058f },  // flat: bottle green
				{ IM_COL32(150, 108, 60, 255), IM_COL32(214, 176, 120, 255), 0.040f }, // fan: bare wood
			};
			const Look& k = kLook[std::clamp(a_type, 0, 4)];
			// the handle: thin at the end, swelling before the ferrule
			quad(0.00f, 0.014f, 0.30f, 0.024f, k.handle);
			quad(0.30f, 0.024f, 0.56f, 0.026f, k.handle);
			a_dl->AddCircleFilled(P(0.0f, 0.0f), a_len * 0.014f, k.handle, 12);
			a_dl->AddLine(P(0.04f, -0.008f), P(0.54f, -0.014f), k.handleHi, std::max(1.0f, a_len * 0.006f));
			// the ferrule: nickel, crimped flat for the flat-bristled brushes
			const ImU32 metal = IM_COL32(176, 178, 182, 255), metalHi = IM_COL32(236, 238, 240, 255), metalLo = IM_COL32(110, 112, 118, 255);
			quad(0.56f, 0.026f, 0.70f, k.ferruleEnd, metal);
			a_dl->AddLine(P(0.57f, -0.012f), P(0.69f, -k.ferruleEnd * 0.5f), metalHi, std::max(1.0f, a_len * 0.006f));
			a_dl->AddLine(P(0.60f, -0.026f), P(0.60f, 0.026f), metalLo, 1.0f);  // the crimp
			// the hairs
			const ImU32 hog = IM_COL32(226, 212, 178, 255), hogLo = IM_COL32(170, 150, 112, 255);
			const ImU32 sable = IM_COL32(118, 74, 40, 255), sableTip = IM_COL32(52, 32, 20, 255);
			switch (a_type) {
			case 0:  // detail: a long fine point of sable
				quad(0.70f, 0.020f, 0.82f, 0.018f, sable);
				{
					const ImVec2 tri[3] = { P(0.82f, -0.018f), P(1.0f, 0.0f), P(0.82f, 0.018f) };
					a_dl->AddConvexPolyFilled(tri, 3, sable);
					const ImVec2 tip[3] = { P(0.93f, -0.007f), P(1.0f, 0.0f), P(0.93f, 0.007f) };
					a_dl->AddConvexPolyFilled(tip, 3, sableTip);
				}
				break;
			case 1:  // round: a full belly to a point
				quad(0.70f, 0.030f, 0.80f, 0.036f, sable);
				{
					const ImVec2 tri[3] = { P(0.80f, -0.036f), P(0.97f, 0.0f), P(0.80f, 0.036f) };
					a_dl->AddConvexPolyFilled(tri, 3, sable);
				}
				break;
			case 2:  // filbert: flat hog hairs with an oval end
				{
					ImVec2 pts[16];
					int    np = 0;
					pts[np++] = P(0.70f, -0.050f);
					for (int i = 0; i <= 12; ++i) {
						const float a = -1.5708f + 3.1416f * i / 12.0f;
						pts[np++] = P(0.86f + std::cos(a) * 0.10f, std::sin(a) * 0.054f);
					}
					pts[np++] = P(0.70f, 0.050f);
					a_dl->AddConvexPolyFilled(pts, np, hog);
					for (int i = -1; i <= 1; ++i) {
						a_dl->AddLine(P(0.72f, i * 0.025f), P(0.93f, i * 0.022f), hogLo, 1.0f);
					}
				}
				break;
			case 3:  // flat: a square block of hog hair
				quad(0.70f, 0.058f, 0.97f, 0.064f, hog);
				for (int i = -2; i <= 2; ++i) {
					a_dl->AddLine(P(0.72f, i * 0.024f), P(0.97f, i * 0.026f), hogLo, 1.0f);
				}
				a_dl->AddLine(P(0.97f, -0.064f), P(0.97f, 0.064f), hogLo, std::max(1.0f, a_len * 0.006f));
				break;
			default:  // fan: hairs spread in a half circle
				{
					const ImVec2 o = P(0.70f, 0.0f);
					ImVec2       pts[18];
					int          np = 0;
					pts[np++] = P(0.70f, -0.040f);
					for (int i = 0; i <= 14; ++i) {
						const float  a = -1.05f + 2.1f * i / 14.0f;  // +-60 degrees
						const ImVec2 d = u * std::cos(a) + n * std::sin(a);
						pts[np++] = o + d * (a_len * 0.26f);
					}
					pts[np++] = P(0.70f, 0.040f);
					a_dl->AddConvexPolyFilled(pts, np, hog);
					for (int i = 0; i <= 8; ++i) {
						const float  a = -1.0f + 2.0f * i / 8.0f;
						const ImVec2 d = u * std::cos(a) + n * std::sin(a);
						a_dl->AddLine(o + d * (a_len * 0.04f), o + d * (a_len * 0.25f), hogLo, 1.0f);
					}
				}
				break;
			}
		}

		// ---- panels
		Paint::Brush& Held() { return g_rag ? g_ragHand : g_brush; }

		void PutDownBrush()
		{
			if (g_brush.type >= 0) {
				g_rack[g_brush.type] = g_brush;  // put down with whatever paint is on it
			}
			g_brush = Paint::Brush{};
		}

		// pick up brush a_i (or put it down if it's the one in hand)
		void PickBrush(int a_i)
		{
			g_rag = false;
			const bool same = g_brush.type == a_i;
			PutDownBrush();
			if (!same) {
				g_brush = g_rack[a_i];
				g_brush.type = a_i;
				if (a_i == Paint::kCharcoal) {
					g_brush.color = Paint::kCharcoalColour;  // charcoal is always ready
					g_brush.load = 1.0f;
				} else if (!g_brush.Loaded()) {
					Hint(a_i == Paint::kKnife ? "Click a paint to load the knife - wiped clean, it scrapes paint off" : "Click a paint to load the brush");
				}
			}
		}

		void ToggleRag()
		{
			if (g_rag) {
				g_rag = false;
			} else {
				PutDownBrush();
				g_rag = true;
				Hint("The rag lifts paint off the canvas - press harder to take more");
			}
		}

		ImVec2 ToScreen(const Surface& a_s, float a_x, float a_y)
		{
			ImVec2 o;
			a_s.ToScreen(a_x, a_y, o.x, o.y);
			return o;
		}
		ImVec2 ToCanvas(const Surface& a_s, const ImVec2& a_p)
		{
			ImVec2 o;
			a_s.ToCanvas(a_p.x, a_p.y, o.x, o.y);
			return o;
		}

		// The brush's footprint as an outline, laid on the canvas (so in the world it is seen in
		// perspective): a circle for the round brushes, an ellipse / flat oval for the filbert and the
		// flats, turned with the stroke; the rag is a soft round wad.
		void DrawFootprint(ImDrawList* a_dl, const Surface& a_s, const ImVec2& a_c)
		{
			float rA, rL;
			if (g_rag) {
				rA = rL = 24.0f * (0.6f + 0.4f * g_pressure);
			} else if (g_brush.type >= 0) {
				const auto& bt = Paint::kBrushes[g_brush.type];
				rA = bt.radius * Paint::kSizes[std::clamp(g_brush.size, 0, Paint::kSizeCount - 1)] * (0.45f + 0.55f * g_pressure);  // across the stroke
				rL = rA / bt.aspect;                                                                                                   // along it
			} else {
				return;
			}
			const float minPx = 1.5f / std::max(0.01f, a_s.ScaleAt(a_c.x, a_c.y));
			rA = std::max(rA, minPx);
			rL = std::max(rL, minPx);
			constexpr int kN = 40;
			ImVec2        pts[kN];
			const bool    blade = !g_rag && g_brush.type == Paint::kKnife;
			for (int i = 0; i < kN; ++i) {
				const float t = 6.2831853f * i / kN;
				float       along = std::cos(t) * rL, across = std::sin(t) * rA;
				if (blade) {  // the knife's square end: the circle pushed out to a rectangle
					const float k = 1.0f / std::max(std::fabs(std::cos(t)), std::fabs(std::sin(t)));
					along *= k;
					across *= k;
				}
				pts[i] = ToScreen(a_s, a_c.x + g_dir.x * along - g_dir.y * across, a_c.y + g_dir.y * along + g_dir.x * across);
			}
			a_dl->AddPolyline(pts, kN, IM_COL32(0, 0, 0, 120), ImDrawFlags_Closed, 2.4f);
			ImU32 col = IM_COL32(255, 255, 255, 150);  // clean: it will blend
			if (g_rag) {
				col = IM_COL32(225, 214, 190, 200);
			} else if (g_brush.Loaded()) {
				col = Col(g_brush.color, 230);
			}
			a_dl->AddPolyline(pts, kN, col, ImDrawFlags_Closed, 1.3f);
		}

		// Painting on a surface (the canvas in the world, or the close-up). a_canStart: a click here may
		// start a stroke (the mouse is over this surface and nothing else); a_hovered: show the cursor.
		void PaintOn(const Surface& a_s, bool a_canStart, bool a_hovered)
		{
			const auto&  io = ImGui::GetIO();
			const float  W = static_cast<float>(g_paint.Width()), H = static_cast<float>(g_paint.Height());
			const ImVec2 m = ToCanvas(a_s, io.MousePos);
			const bool   inside = m.x >= 0.0f && m.y >= 0.0f && m.x < W && m.y < H;

			// which way the brush is travelling across the canvas (the flat brushes lie across it)
			if (g_lastCanvas.x > -1e8f) {
				const ImVec2 d = m - g_lastCanvas;
				const float  len = std::sqrt(d.x * d.x + d.y * d.y);
				if (len > 0.75f) {
					g_dir = g_dir * 0.75f + d / len * 0.25f;
					const float l2 = std::sqrt(g_dir.x * g_dir.x + g_dir.y * g_dir.y);
					g_dir = l2 > 0.01f ? g_dir / l2 : ImVec2(1.0f, 0.0f);
				}
			}
			g_lastCanvas = m;

			const double now = ImGui::GetTime();
			if (!g_painting && a_canStart && inside && ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
				if ((io.KeyCtrl || AltHeld()) && !g_rag) {
					// dip the brush into the paint on the canvas (the colour picker)
					if (g_brush.type < 0) {
						Hint("Pick up a brush first: click one below the palette");
					} else {
						LoadColour(g_paint.ColorAt(static_cast<int>(m.x), static_cast<int>(m.y)));
					}
				} else if (!g_rag && g_brush.type < 0) {
					Hint("Pick up a brush first: click one below the palette");
				} else if (io.KeyShift && g_hasLineFrom) {
					// a straight line from where the last stroke ended to here
					Paint::StrokeStyle style;
					style.body = g_body;
					style.lift = g_rag;
					if (!g_rag && g_brush.Loaded()) {
						RememberColour(g_brush.color);
					}
					g_paint.BeginStroke(Held(), g_lineFrom.x, g_lineFrom.y, g_pressure, style);
					const ImVec2 d = m - g_lineFrom;
					const int    steps = std::max(1, static_cast<int>(std::sqrt(d.x * d.x + d.y * d.y) / 2.0f));
					for (int k = 1; k <= steps; ++k) {
						const float t = static_cast<float>(k) / steps;
						g_paint.StrokeTo(Held(), g_lineFrom.x + d.x * t, g_lineFrom.y + d.y * t, g_pressure);
					}
					g_paint.EndStroke(Held());
					++g_edits;
					CountStroke(now);
					g_lineFrom = m;
				} else {
					// painting on a hidden layer would leave no mark you can see: show it again
					if (const int cur = g_paint.Current(); !g_paint.Layers()[cur].visible) {
						g_paint.SetLayerVisible(cur, true);
						static std::string msg;
						msg = fmt::format("\"{}\" was hidden - it's showing again", g_paint.Layers()[cur].name);
						Hint(msg.c_str());
					}
					Paint::StrokeStyle style;
					style.body = g_body;
					style.lift = g_rag;
					const auto& bt = Paint::kBrushes[std::max(0, g_brush.type)];
					// the string is measured on screen, so it feels the same near or far
					g_stroker.string = 0.0f;  // the brush is exactly where the mouse is
					g_stroker.taper = g_rag ? 8.0f : bt.radius * Paint::kSizes[std::clamp(g_brush.size, 0, Paint::kSizeCount - 1)] * 1.5f;
					if (!g_rag && g_brush.Loaded()) {
						RememberColour(g_brush.color);
					}
					g_stroker.Begin(m.x, m.y, now);
					g_paint.BeginStroke(Held(), m.x, m.y, 0.3f * g_pressure, style);
					g_painting = true;
				}
			}
			if (g_painting) {
				g_points.clear();
				g_stroker.Move(m.x, m.y, now, g_pressure, g_points);
				const bool up = !ImGui::IsMouseDown(ImGuiMouseButton_Left);
				if (up) {
					g_stroker.Finish(g_points);  // lift off: the stroke tapers away
				}
				for (const auto& pt : g_points) {
					g_paint.StrokeTo(Held(), pt.x, pt.y, pt.pressure);
				}
				if (up) {
					g_paint.EndStroke(Held());
					g_painting = false;
					++g_edits;
					CountStroke(now);
					g_lineFrom = m;  // Shift + click carries on from here
					g_hasLineFrom = true;
				}
			}

			// the cursor: the brush's footprint, right at the mouse
			g_overCanvas = g_painting || (a_hovered && inside && !Overlay::Looking());
			if (g_overCanvas) {
				if (io.KeyShift && g_hasLineFrom && !g_painting) {
					ImGui::GetForegroundDrawList()->AddLine(ToScreen(a_s, g_lineFrom.x, g_lineFrom.y), io.MousePos, IM_COL32(240, 228, 200, 160), 1.5f);
				}
				DrawFootprint(ImGui::GetForegroundDrawList(), a_s, m);
			}
		}

		// The canvas in the world: found on screen from its corners each frame, painted where the mouse is.
		void DrawWorldCanvas(bool a_looking)
		{
			const auto& io = ImGui::GetIO();
			float       q[4][2];
			Surface     s;
			if (g_work.canvas && Live::CanvasOnScreen(g_work.canvas, g_work.portrait, io.DisplaySize.x, io.DisplaySize.y, q)) {
				s = Surface::FromQuad(q, static_cast<float>(g_paint.Width()), static_cast<float>(g_paint.Height()));
			}
			// a window over the whole screen, behind the panels, to catch the mouse over the canvas
			ImGui::SetNextWindowPos(ImVec2(0, 0), ImGuiCond_Always);
			ImGui::SetNextWindowSize(io.DisplaySize, ImGuiCond_Always);
			ImGui::Begin("##world", nullptr,
				ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoBackground | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings |
					ImGuiWindowFlags_NoBringToFrontOnFocus | ImGuiWindowFlags_NoFocusOnAppearing | ImGuiWindowFlags_NoScrollWithMouse);
			ImGui::SetCursorScreenPos(ImVec2(0, 0));
			ImGui::InvisibleButton("##worldcanvas", io.DisplaySize, ImGuiButtonFlags_MouseButtonLeft);
			// (the press makes this button active on the same frame - hovering must still count then)
			const bool hovered = ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenBlockedByActiveItem) &&
			                     ImGui::IsWindowHovered(ImGuiHoveredFlags_AllowWhenBlockedByActiveItem);
			if (s.ok) {
				if (hovered && io.MouseWheel != 0.0f) {
					g_pressure = std::clamp(g_pressure + 0.05f * io.MouseWheel, 0.05f, 1.0f);
				}
				PaintOn(s, hovered && !a_looking, hovered);
			} else {
				if (g_painting) {  // the canvas went out of view mid-stroke
					g_paint.EndStroke(Held());
					g_painting = false;
				}
				const char*  msg = "Turn to the canvas (hold the right mouse button), or press C for a close-up";
				const ImVec2 ts = ImGui::CalcTextSize(msg);
				ImGui::GetWindowDrawList()->AddText(ImVec2((io.DisplaySize.x - ts.x) * 0.5f, io.DisplaySize.y * 0.12f), IM_COL32(240, 228, 200, 220), msg);
			}
			ImGui::End();
		}

		// The flat close-up: fitted into a_size, then zoomed / panned / mirrored. Middle button or Space +
		// left button pans, Ctrl + wheel zooms, the wheel alone sets the touch.
		void DrawCanvas(const ImVec2& a_size)
		{
			const float  W = static_cast<float>(g_paint.Width()), H = static_cast<float>(g_paint.Height());
			const ImVec2 r0 = ImGui::GetCursorScreenPos();
			const ImVec2 r1 = r0 + a_size;
			const float  fit = std::min(a_size.x / W, a_size.y / H);
			const float  scale = fit * g_zoom;
			const ImVec2 size(W * scale, H * scale);
			const ImVec2 origin = r0 + a_size * 0.5f + g_pan - size * 0.5f;
			auto*        dl = ImGui::GetWindowDrawList();
			dl->PushClipRect(r0, r1, true);
			dl->AddRectFilled(origin + ImVec2(Theme::S(5), Theme::S(6)), origin + size + ImVec2(Theme::S(5), Theme::S(6)), IM_COL32(0, 0, 0, 70), 2.0f);
			dl->AddImage(reinterpret_cast<ImTextureID>(g_srv), origin, origin + size, g_mirror ? ImVec2(1, 0) : ImVec2(0, 0), g_mirror ? ImVec2(0, 1) : ImVec2(1, 1));
			dl->AddRect(origin, origin + size, IM_COL32(60, 44, 30, 255), 0.0f, 0, Theme::S(2.0f));
			dl->PopClipRect();

			ImGui::SetCursorScreenPos(r0);
			ImGui::InvisibleButton("##canvas", a_size, ImGuiButtonFlags_MouseButtonLeft | ImGuiButtonFlags_MouseButtonMiddle);
			const bool  hovered = ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenBlockedByActiveItem);
			const bool  active = ImGui::IsItemActive();
			const auto& io = ImGui::GetIO();

			const bool panning = active && !g_painting && (ImGui::IsMouseDown(ImGuiMouseButton_Middle) || ImGui::IsKeyDown(ImGuiKey_Space));
			if (panning) {
				g_pan = g_pan + io.MouseDelta;
			}
			if (hovered && io.MouseWheel != 0.0f) {
				if (io.KeyCtrl) {  // zoom about the point under the cursor
					const float  nz = std::clamp(g_zoom * std::pow(1.15f, io.MouseWheel), 0.5f, 8.0f);
					const ImVec2 mid = r0 + a_size * 0.5f;
					const ImVec2 rel = (io.MousePos - (mid + g_pan)) * (nz / g_zoom);
					g_pan = io.MousePos - mid - rel;
					g_zoom = nz;
				} else {
					g_pressure = std::clamp(g_pressure + 0.05f * io.MouseWheel, 0.05f, 1.0f);
				}
			}
			const Surface s = Surface::FromRect(origin.x, origin.y, scale, W, g_mirror);
			PaintOn(s, hovered && !ImGui::IsKeyDown(ImGuiKey_Space) && !Overlay::Looking(), hovered && !panning);
		}

		void DrawHand(float a_w)
		{
			const float lw = U(92.0f);
			auto        row = [&](const char* a_label, const char* a_id, float* a_v, float a_min, float a_max, const char* a_fmt, const char* a_tip) {
                ImGui::AlignTextToFramePadding();
                ImGui::TextUnformatted(a_label);
                ImGui::SameLine(lw);
                float pc = *a_v * 100.0f;
                if (Theme::SliderFloat(a_id, &pc, a_min, a_max, a_fmt, a_w - lw)) {
                    *a_v = pc / 100.0f;
                }
                if (ImGui::IsItemHovered()) {
                    ImGui::SetTooltip("%s", a_tip);
                }
			};
			row("Touch", "##touch", &g_pressure, 5.0f, 100.0f, "%.0f%%",
				"Light: thin, broken strokes that only catch the canvas weave. Heavy: the bristles splay and lay the paint down thick.\nThe mouse wheel over the canvas changes it too.");
			row("Paint", "##body", &g_body, 10.0f, 100.0f, "%.0f%%",
				"How thick the paint is. Thin: a glaze you can see through, however much you go over it in one stroke. Full: covers what's under it.");
		}

		void DrawPalettePanel(float a_w)
		{
			const auto& io = ImGui::GetIO();
			auto*       dl = ImGui::GetWindowDrawList();
			const bool  click = ImGui::IsMouseClicked(ImGuiMouseButton_Left) && ImGui::IsWindowHovered(ImGuiHoveredFlags_ChildWindows);

			// ---- brushes (top row), then charcoal, the palette knife and the rag (the tools row)
			Theme::SectionLabel("1   Pick up a tool");
			constexpr int kRowBrushes = 5;  // the four brushes and the fan
			const float   cell = a_w / kRowBrushes;
			const float   rowH = U(84.0f);
			const ImVec2  b0 = ImGui::GetCursorScreenPos();
			ImGui::InvisibleButton("##brushrow", ImVec2(a_w, rowH + ImGui::GetTextLineHeight() + U(8)));
			// a tile: the tool's picture, its name under it and its key in the corner; the one in hand is lit up
			auto cellFrame = [&](const ImVec2& c0, float w, const char* label, bool inHand, const char* key) {
				const ImVec2 a = c0 + ImVec2(U(2), 0), b = c0 + ImVec2(w - U(2), rowH + ImGui::GetTextLineHeight() + U(4));
				const bool   hov = ImGui::IsMouseHoveringRect(a, b);
				if (inHand) {
					dl->AddRectFilled(a, b, IM_COL32(212, 170, 90, 70), U(7));
					dl->AddRect(a, b, Theme::Gold(), U(7), 0, U(2.0f));
				} else {
					dl->AddRectFilled(a, b, hov ? IM_COL32(255, 255, 255, 34) : IM_COL32(0, 0, 0, 22), U(7));
					if (hov) {
						dl->AddRect(a, b, Theme::GoldDim(), U(7), 0, 1.0f);
					}
				}
				const float tw = ImGui::CalcTextSize(label).x;
				dl->AddText(c0 + ImVec2((w - tw) * 0.5f, rowH), inHand ? Theme::Ink() : Theme::InkSoft(), label);
				// the key, in a little badge
				const ImVec2 ks = ImGui::CalcTextSize(key);
				const ImVec2 k0 = a + ImVec2(U(4), U(4)), k1 = k0 + ImVec2(std::max(ks.x + U(8), U(18)), ks.y + U(2));
				dl->AddRectFilled(k0, k1, inHand ? Theme::Gold() : IM_COL32(60, 44, 30, 200), U(4));
				dl->AddText(ImVec2((k0.x + k1.x - ks.x) * 0.5f, k0.y + U(1)), inHand ? IM_COL32(40, 28, 16, 255) : IM_COL32(240, 228, 200, 255), key);
				return hov;
			};
			static constexpr const char* kKey[Paint::kBrushCount] = { "1", "2", "3", "4", "5", "6", "7" };
			static constexpr const char* kShort[Paint::kBrushCount] = { "Detail", "Round", "Filbert", "Flat", "Fan", "Charcoal", "Knife" };
			for (int i = 0; i < kRowBrushes; ++i) {
				const ImVec2 c0 = b0 + ImVec2(cell * i, 0);
				const bool   hov = cellFrame(c0, cell, kShort[i], g_brush.type == i, kKey[i]);
				{
					const float len = cell * (0.8f + 0.06f * i);
					if (i == 1) {  // the round brush keeps its picture
						DrawBrushPic(dl, c0 + ImVec2(cell * 0.5f + len * 0.4f, rowH * 0.5f - len * 0.24f), len, nullptr);
					} else {
						DrawBrushArt(dl, i, c0 + ImVec2(cell * 0.5f, rowH * 0.5f + U(6)), std::min(cell * 0.92f, rowH * 1.3f));
					}
				}
				if (hov) {
					static constexpr const char* kWhat[5] = { "fine lines and small details", "an all-round brush", "soft, rounded strokes and blending",
						"broad, square strokes and blocks of colour", "grass, leaves, fur and wispy clouds" };
					ImGui::SetTooltip("%s  [key %d]\n%s\n%s", Paint::kBrushes[i].name, i + 1, kWhat[i], g_brush.type == i ? "Click: put it down" : "Click: pick it up");
					if (click) {
						PickBrush(i);
					}
				}
			}

			const float  tcell = a_w / 3.0f;
			const ImVec2 t0 = ImGui::GetCursorScreenPos();
			ImGui::InvisibleButton("##toolrow", ImVec2(a_w, rowH + ImGui::GetTextLineHeight() + U(8)));
			// charcoal: a dark stick
			{
				const ImVec2 c0 = t0;
				const bool   hov = cellFrame(c0, tcell, "Charcoal", g_brush.type == Paint::kCharcoal, "6");
				{
					const ImVec2 m = c0 + ImVec2(tcell * 0.5f, rowH * 0.5f);
					const ImVec2 d(tcell * 0.32f, -rowH * 0.22f), nrm(-d.y * 0.12f, d.x * 0.12f);
					dl->AddQuadFilled(m - d - nrm, m - d + nrm, m + d + nrm, m + d - nrm, IM_COL32(34, 32, 30, 255));
					dl->AddLine(m - d + nrm * 0.6f, m + d + nrm * 0.6f, IM_COL32(80, 76, 72, 255), 1.0f);
				}
				if (hov) {
					ImGui::SetTooltip("Charcoal  [key 6]\nSketch your drawing first. Needs no paint; dry and grainy, it doesn't mix.\nDrag a clean brush through it to smudge it.");
					if (click) {
						PickBrush(Paint::kCharcoal);
					}
				}
			}
			// the palette knife: a handle and a thin cranked blade
			{
				const ImVec2 c0 = t0 + ImVec2(tcell, 0);
				const bool   hov = cellFrame(c0, tcell, "Palette knife", g_brush.type == Paint::kKnife, "7");
				{
					const ImVec2 m = c0 + ImVec2(tcell * 0.5f, rowH * 0.5f);
					const float  sx = tcell * 0.36f, sy = rowH * 0.24f;
					dl->AddLine(m + ImVec2(-sx, sy), m + ImVec2(-sx * 0.15f, sy * 0.15f), IM_COL32(110, 70, 40, 255), U(5.0f));  // handle
					dl->AddLine(m + ImVec2(-sx * 0.15f, sy * 0.15f), m + ImVec2(sx * 0.05f, 0), IM_COL32(150, 150, 150, 255), U(1.5f));
					dl->AddTriangleFilled(m + ImVec2(sx * 0.05f, -U(2)), m + ImVec2(sx, -sy), m + ImVec2(sx * 0.05f, U(3)), IM_COL32(190, 192, 196, 255));
				}
				if (hov) {
					ImGui::SetTooltip("Palette knife  [key 7]\nLoaded with paint: flat, sharp-edged slabs of solid colour.\nClean (wiped on the rag): scrapes paint off, back to the canvas.");
					if (click) {
						PickBrush(Paint::kKnife);
					}
				}
			}
			// the rag
			{
				const ImVec2 c0 = t0 + ImVec2(tcell * 2.0f, 0);
				const bool   hov = cellFrame(c0, tcell, "Rag", g_rag, "E");
				const ImVec2 r0 = c0 + ImVec2(tcell * 0.2f, rowH * 0.2f), r1 = c0 + ImVec2(tcell * 0.8f, rowH * 0.8f);
				{
					dl->AddRectFilled(r0, r1, IM_COL32(205, 196, 176, 255), U(8));
					for (int k = 1; k < 4; ++k) {
						dl->AddLine(r0 + ImVec2(0, (r1.y - r0.y) * k / 4), r0 + ImVec2(r1.x - r0.x, (r1.y - r0.y) * k / 4 + U(4)), IM_COL32(160, 150, 130, 255), 1.0f);
					}
				}
				if (hov) {
					const bool charcoal = g_brush.type == Paint::kCharcoal;
					ImGui::SetTooltip("Rag  [key E]\n%s", charcoal                ? "Charcoal can't be wiped - put it down to take the rag"
					                        : g_brush.type >= 0      ? "Click: wipe the paint off your brush (a clean brush blends the wet paint; a clean knife scrapes)"
					                        : g_rag                  ? "Click: put the rag down"
					                                                 : "Click: take the rag - drag it over the canvas to lift paint off");
					if (click) {
						if (g_brush.type >= 0 && !charcoal) {
							g_brush.load = 0.0f;
							g_brush.color = {};
						} else if (g_brush.type < 0 || g_rag) {
							ToggleRag();
						}
					}
				}
			}

			// ---- the palette
			ImGui::Dummy(ImVec2(0, U(6)));
			Theme::SectionLabel("2   Load and mix paint");
			{
				const char* how = g_brush.type == Paint::kCharcoal ? "Charcoal needs no paint."
				                  : g_rag || g_brush.type < 0       ? "Pick up a brush or the knife first."
				                  : g_brush.Loaded()                ? "Click an empty spot to put down a puddle, then click it with another paint to mix."
				                                                    : "Click a paint to load your brush.";
				Theme::TextColoredWrapped(Theme::InkSoft(), how);
			}
			const float  ph = a_w / PaletteSpots::kAspect;
			const ImVec2 p0 = ImGui::GetCursorScreenPos();
			ImGui::InvisibleButton("##palettepic", ImVec2(a_w, ph));  // (holds the mouse, so dragging here never drags the panel)
			if (g_palettePic.srv) {
				dl->AddImage(reinterpret_cast<ImTextureID>(g_palettePic.srv), p0, p0 + ImVec2(a_w, ph));
			} else {
				dl->AddEllipseFilled(p0 + ImVec2(a_w, ph) * 0.5f, a_w * 0.48f, ph * 0.48f, IM_COL32(150, 100, 60, 255));
			}
			const float blobR = U(17.0f);
			int         hoverSpot = -1;
			for (int i = 0; i < static_cast<int>(g_spots.size()); ++i) {
				const ImVec2 c = p0 + ImVec2(g_spots[i].x * a_w, g_spots[i].y * ph);
				PaintBlob(dl, c, blobR, g_spots[i].c, static_cast<std::uint32_t>(i * 7 + 3));
				const ImVec2 d = io.MousePos - c;
				if (d.x * d.x + d.y * d.y < blobR * blobR) {
					hoverSpot = i;
				}
			}
			const bool overPalette = ImGui::IsMouseHoveringRect(p0, p0 + ImVec2(a_w, ph));
			if (hoverSpot >= 0) {
				const auto& s = g_spots[hoverSpot];
				dl->AddCircle(p0 + ImVec2(s.x * a_w, s.y * ph), blobR + 3.0f, Theme::Gold(), 0, U(2.0f));
				if (s.mixed) {
					const int n = s.Parts();
					ImGui::SetTooltip("Your puddle (%d part%s)\nClick with another colour on the brush: mix it in\nClick with a clean brush: load it\nShift + click: load it without mixing\nDrag: move it\nCtrl + click: scrape it off",
						n, n == 1 ? "" : "s");
				} else {
					ImGui::SetTooltip("%s\nClick: load the brush with it", kPigments[std::min(hoverSpot, PaletteSpots::kCount - 1)].name);
				}
			}
			// a spot was clicked (for a puddle: pressed and let go without dragging it)
			auto spotClicked = [&](int a_i, bool a_shift) {
				if (g_brush.type == Paint::kCharcoal) {
					Hint("Charcoal doesn't take paint - pick up a brush or the knife");
				} else if (g_rag) {
					Hint("Put the rag down and pick up a brush to load paint");
				} else if (g_brush.type < 0) {
					Hint("Pick up a brush first: click one above the palette (1-5)");
				} else {
					auto& spot = g_spots[a_i];
					if (spot.mixed && g_brush.Loaded() && !a_shift && ColourDistance(g_brush.color, spot.c) > 12) {
						// the paint on the brush goes into the puddle, one part more; the brush takes the new mix
						bool found = false;
						for (auto& [col, n] : spot.parts) {
							if (ColourDistance(col, g_brush.color) <= 6) {
								n = std::min(n + 1, 99);
								found = true;
								break;
							}
						}
						if (!found) {
							spot.parts.push_back({ g_brush.color, 1 });
						}
						spot.Remix();
						SaveSpots();
						LoadColour(spot.c);
						Hint(fmt::format("Mixed into the puddle ({} parts) - the brush has the new mix", spot.Parts()).c_str());
					} else {
						LoadColour(spot.c);
					}
				}
			};

			// puddles: press and drag to move one, press and let go to use it
			static int    dragSpot = -1;
			static bool   dragMoved = false;
			static ImVec2 dragFrom{};
			if (dragSpot >= static_cast<int>(g_spots.size())) {
				dragSpot = -1;
			}
			if (dragSpot >= 0) {
				if (ImGui::IsMouseDown(ImGuiMouseButton_Left)) {
					const ImVec2 d = io.MousePos - dragFrom;
					if (!dragMoved && d.x * d.x + d.y * d.y > 16.0f) {
						dragMoved = true;
					}
					if (dragMoved) {
						g_spots[dragSpot].x = std::clamp((io.MousePos.x - p0.x) / a_w, 0.03f, 0.97f);
						g_spots[dragSpot].y = std::clamp((io.MousePos.y - p0.y) / ph, 0.03f, 0.97f);
						ImGui::SetTooltip("Moving the puddle");
					}
				} else {
					if (dragMoved) {
						SaveSpots();
					} else {
						spotClicked(dragSpot, io.KeyShift);
					}
					dragSpot = -1;
				}
			}

			if (click && overPalette) {
				if (hoverSpot >= 0 && io.KeyCtrl && g_spots[hoverSpot].mixed) {
					g_spots.erase(g_spots.begin() + hoverSpot);
					SaveSpots();
				} else if (hoverSpot >= 0 && g_spots[hoverSpot].mixed) {
					dragSpot = hoverSpot;  // decided when the button comes up: a drag moves it, a click uses it
					dragMoved = false;
					dragFrom = io.MousePos;
				} else if (hoverSpot >= 0) {
					spotClicked(hoverSpot, io.KeyShift);
				} else if (g_brush.Loaded()) {
					const float x = (io.MousePos.x - p0.x) / a_w, y = (io.MousePos.y - p0.y) / ph;
					if (static_cast<int>(g_spots.size()) - PaletteSpots::kCount >= kMaxMixed) {
						Hint("The palette is full - Ctrl + click a puddle, or Clean the palette");
					} else {
						Spot spot{ x, y, g_brush.color, true };
						spot.parts.push_back({ g_brush.color, 1 });
						g_spots.push_back(std::move(spot));
						SaveSpots();
						Hint("Puddle put down - load another paint and click the puddle to mix it in");
					}
				}
			}

			// clean the palette: every puddle scraped off (the paints from the tubes stay)
			{
				int puddles = 0;
				for (const auto& sp : g_spots) {
					puddles += sp.mixed ? 1 : 0;
				}
				if (puddles > 0) {
					const char*  label = "Clean the palette";
					const ImVec2 ts = ImGui::CalcTextSize(label);
					ImGui::SetCursorScreenPos(ImVec2(p0.x + a_w - ts.x - U(14.0f), ImGui::GetCursorScreenPos().y));
					if (ImGui::SmallButton(label)) {
						ImGui::OpenPopup("##cleanpalette");
					}
					if (ImGui::IsItemHovered()) {
						ImGui::SetTooltip("Scrape every puddle off the palette");
					}
					if (ImGui::BeginPopup("##cleanpalette")) {
						ImGui::Text("Scrape all %d puddle%s off the palette?", puddles, puddles == 1 ? "" : "s");
						if (ImGui::Button("Scrape them off")) {
							g_spots.erase(std::remove_if(g_spots.begin(), g_spots.end(), [](const Spot& sp) { return sp.mixed; }), g_spots.end());
							SaveSpots();
							dragSpot = -1;
							ImGui::CloseCurrentPopup();
						}
						ImGui::SameLine();
						if (ImGui::Button("Keep them")) {
							ImGui::CloseCurrentPopup();
						}
						ImGui::EndPopup();
					}
				}
			}

			// ---- your colours: the last ones painted with
			{
				const ImU32 edge = IM_COL32(20, 16, 12, 200);
				// recent colours: the last ones painted with
				if (!g_recent.empty()) {
					ImGui::Dummy(ImVec2(0, U(2)));
					const float  rs = std::min(U(20.0f), (a_w - U(4.0f) * (kRecent - 1)) / kRecent);
					const ImVec2 r0 = ImGui::GetCursorScreenPos();
					ImGui::Dummy(ImVec2(a_w, rs));
					for (int i = 0; i < static_cast<int>(g_recent.size()); ++i) {
						const ImVec2 q0 = r0 + ImVec2((rs + U(4.0f)) * i, 0), q1 = q0 + ImVec2(rs, rs);
						dl->AddRectFilled(q0, q1, Col(g_recent[i]), U(3));
						const bool hov = ImGui::IsMouseHoveringRect(q0, q1);
						dl->AddRect(q0, q1, hov ? Theme::Gold() : edge, U(3), 0, hov ? 2.0f : 1.0f);
						if (hov) {
							ImGui::SetTooltip("A colour you painted with - click to load the brush with it");
							if (click) {
								if (g_brush.type < 0) {
									Hint("Pick up a brush first: click one above the palette (1-5)");
								} else {
									LoadColour(g_recent[i]);
								}
							}
						}
					}
				}
			}

			// ---- what's in hand, and how you hold it
			ImGui::Dummy(ImVec2(0, U(6)));
			Theme::SectionLabel("3   Paint");
			if (g_rag) {
				Theme::TextColoredWrapped(Theme::InkSoft(), "The rag in hand - drag it over the canvas to lift paint off.");
			} else if (g_brush.type < 0) {
				Theme::TextColoredWrapped(Theme::InkSoft(), "Nothing in hand yet.");
			} else {
				const ImVec2 sw = ImGui::GetCursorScreenPos();
				if (g_brush.Loaded()) {
					dl->AddRectFilled(sw, sw + ImVec2(U(22), U(22)), Col(g_brush.color), 4.0f);
				} else {
					dl->AddRect(sw, sw + ImVec2(U(22), U(22)), Theme::InkSoft(), 4.0f);
				}
				ImGui::Dummy(ImVec2(U(26), U(22)));
				ImGui::SameLine();
				static constexpr const char* kSizeNames[Paint::kSizeCount] = { "very small", "small", "", "large", "very large" };
				const char* sz = kSizeNames[std::clamp(g_brush.size, 0, Paint::kSizeCount - 1)];
				const char* state = g_brush.type == Paint::kCharcoal ? "sketching"
				                    : g_brush.Loaded()               ? "loaded"
				                    : g_brush.type == Paint::kKnife  ? "clean, scrapes"
				                                                     : "clean, blends";
				ImGui::Text("%s%s%s - %s", sz, *sz ? " " : "", Paint::kBrushes[g_brush.type].name, state);
				if (ImGui::IsItemHovered()) {
					ImGui::SetTooltip("[ and ] take a smaller or larger brush of the same kind");
				}
			}
			ImGui::Dummy(ImVec2(0, U(4)));
			DrawHand(a_w);
			ImGui::Dummy(ImVec2(0, U(6)));
			Theme::TextColoredWrapped(Theme::InkSoft(), "Hold right mouse: look around    Z: zoom    C: close-up\nCtrl+Z: undo    F1: the painter's guide");
		}

		// ---- the painter's guide: everything the studio can do, in the studio
		struct GuideLine
		{
			const char* what;  // a key or an action ("" = a plain paragraph)
			const char* text;
		};

		void GuideTab(const char* a_name, const char* a_intro, std::initializer_list<GuideLine> a_lines)
		{
			if (!ImGui::BeginTabItem(a_name)) {
				return;
			}
			ImGui::Dummy(ImVec2(0, Theme::S(4)));
			if (a_intro && *a_intro) {
				Theme::TextColoredWrapped(Theme::InkSoft(), a_intro);
				ImGui::Dummy(ImVec2(0, Theme::S(6)));
			}
			const float col = Theme::S(150.0f);
			const float wrap = ImGui::GetContentRegionAvail().x;
			for (const auto& l : a_lines) {
				if (!*l.what) {
					ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + wrap);
					ImGui::TextUnformatted(l.text);
					ImGui::PopTextWrapPos();
				} else {
					ImGui::TextColored(ImVec4(0.92f, 0.80f, 0.52f, 1.0f), "%s", l.what);
					ImGui::SameLine(col);
					ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + wrap - col);
					ImGui::TextUnformatted(l.text);
					ImGui::PopTextWrapPos();
				}
				ImGui::Dummy(ImVec2(0, Theme::S(3)));
			}
			ImGui::EndTabItem();
		}

		void DrawGuide(bool a_looking)
		{
			if (!g_guide) {
				return;
			}
			if (a_looking) {
				ImGui::PushStyleVar(ImGuiStyleVar_Alpha, 0.25f);
			}
			const ImVec2 screen = ImGui::GetIO().DisplaySize;
			ImGui::SetNextWindowPos(screen * 0.5f, ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
			ImGui::SetNextWindowSize(ImVec2(Theme::S(640.0f), Theme::S(520.0f)), ImGuiCond_Appearing);
			if (Theme::BeginWindow("##guide", nullptr, a_looking ? ImGuiWindowFlags_NoInputs : 0)) {
				ImGui::TextColored(ImVec4(0.92f, 0.85f, 0.68f, 1.0f), "The Painter's Guide");
				ImGui::SameLine(ImGui::GetWindowContentRegionMax().x - Theme::S(90.0f));
				if (Theme::Button("Close", ImVec2(Theme::S(90.0f), 0))) {
					g_guide = false;
				}
				ImGui::Separator();
				ImGui::BeginChild("##guidebody", ImVec2(0, 0), false);
				if (ImGui::BeginTabBar("##guidetabs")) {
					GuideTab("Starting", "Paint the way a painter does: from the back to the front. Block in the big shapes first, then paint the middle and finish with the details on top.",
						{
							{ "Pick up a brush", "Click one of the brushes below the palette. Click it again to put it down." },
							{ "Load it", "Click a paint on the palette: the brush takes that colour." },
							{ "Make a puddle", "With paint on the brush, click a bare spot on the palette: a puddle of that paint goes down there." },
							{ "Mix", "Load another paint and click the puddle: it mixes in like real pigment (blue and yellow make green) and the brush takes the new mix. Keep adding paints - each one is a part, so adding red twice makes it redder." },
							{ "Use a puddle", "Click it with a clean brush, or Shift + click it, to load it without mixing anything in. Drag it to move it. Ctrl + click scrapes it off; Clean the palette scrapes them all." },
							{ "Your colours", "Every colour you paint with appears in the row under the palette. Click one to load it again." },
							{ "Paint", "Drag on the canvas standing on the easel with the left mouse button - the paint goes straight onto it. Press C for a flat close-up instead." },
							{ "Clean the brush", "Click the rag." },
							{ "Varnish", "When you finish, choose Matte, Satin or Gloss: how much the finished painting shines in the light." },
							{ "Selling", "General goods merchants and innkeepers buy finished paintings. The price comes from the painting - how much of the canvas is painted, the detail and the range of colours - and from the work that went into it: the time spent painting and the number of strokes. A quick scribble fetches a few coins; a full, rich painting worked on for 45 minutes or more, a few hundred. The Finish window shows what it would fetch. A sold painting is gone for good - the merchant won't sell it back." },
							{ "Keeping paintings", "A painting lasts as long as it exists somewhere: in your pack, hung on a wall, on an easel, in a chest or lying where you dropped it. Sell it, scrape it off or destroy it and it's gone - its picture is deleted the next time you save." },
							{ "Finish", "Finish names it and takes it off the easel as a painting you can hang. Step away (Esc) leaves it on the easel for later. Your work is also saved every few minutes while you paint." },
						});
					GuideTab("Brushes", "Each brush leaves its own mark. The outline on the canvas shows where it will touch and how big it is.",
						{
							{ "Detail round", "Fine lines, edges, highlights, signatures." },
							{ "Round", "General painting and soft shapes." },
							{ "Filbert", "Rounded flat: blending, petals, leaves, skin." },
							{ "Wide flat", "Skies, washes, large areas. It lies across the way you move it." },
							{ "Fan brush", "A few bristles spread wide: flick it for grass, leaves, fur and wispy clouds." },
							{ "Charcoal", "Sketch your drawing first, the way painters do. Dry and grainy, it doesn't mix with paint and is easy to paint over; drag a clean brush through it to smudge it." },
							{ "Palette knife", "Loaded with paint, it lays down flat, sharp-edged slabs of solid colour - rocks, walls, a heavy sky. Wipe it on the rag and it scrapes paint off, back to the canvas." },
							{ "1 - 7", "1-5 the brushes, 6 charcoal, 7 the palette knife (press again to put it down)." },
							{ "[  and  ]", "A smaller or larger brush of the same kind." },
							{ "X", "Back to the colour you had before - press again to swap back." },
							{ "Straight line", "Shift + click paints a straight line from where your last stroke ended (you see it while Shift is held)." },
							{ "Never runs out", "A loaded brush keeps its paint until you wipe it on the rag. Each brush keeps its own paint when you put it down." },
							{ "Dirty brush", "Dragging through wet paint picks some of it up, so colours shift as you work, like the real thing." },
						});
					GuideTab("Your hand", "The two sliders under the palette.",
						{
							{ "Touch", "How hard you press. Light: thin, broken strokes that only catch the weave of the canvas. Heavy: the bristles splay and lay the paint down thick. The mouse wheel over the canvas changes it too." },
							{ "Paint", "How thick the paint is. Thin is a glaze you can see through, however much you go over it in one stroke. Full covers what's under it." },
							{ "Strokes", "Every stroke starts light and comes up to full pressure. A quick flick lays down less paint." },
						});
					GuideTab("Blending", "Wet paint on the canvas mixes the way pigments do: blue and yellow make green.",
						{
							{ "Wet into wet", "Paint over wet paint and the two mix on the canvas." },
							{ "Blend", "Wipe the brush on the rag, then drag the clean brush through the paint to soften and smear it." },
							{ "Lift paint", "Take the rag (click it with an empty hand, or press E) and rub the canvas to wipe paint off. Press harder to take more." },
							{ "Take a colour", "Hold Alt (or Ctrl) and click on the canvas: the brush dips into the colour there." },
						});
					GuideTab("Seeing", "You paint on the canvas on the easel, with the world around it: paint what's in front of you.",
						{
							{ "Right mouse (hold)", "Look around: the mouse turns your view instead of moving the cursor. Turn back to the canvas to paint on it." },
							{ "Tab", "Hide the panel." },
							{ "C / Close-up", "A flat close-up of the canvas in a window, for fine detail. Press C again to paint on the easel." },
							{ "Z", "Look 25% closer at the canvas on the easel; press again to look normally. In the close-up it zooms to 125% and back." },
							{ "Move the close-up", "Drag its top bar. Drag its bottom corner to make it bigger or smaller." },
							{ "Ctrl + wheel", "Zoom in and out where the cursor is." },
							{ "Middle mouse / Space", "Hold and drag to move around a zoomed-in canvas." },
							{ "M", "In the close-up: see the painting in a mirror. Painters do this to catch mistakes in the drawing." },
							{ "Step back / Ctrl+0", "The whole canvas again, the right way round." },
						});
					GuideTab("Keys", "",
						{
							{ "Left mouse", "Pick up, load, paint" },
							{ "Right mouse (hold)", "Look around" },
							{ "Mouse wheel", "Touch (pressure)" },
							{ "Alt / Ctrl + click", "Take the colour from the canvas" },
							{ "Shift + click", "Straight line from the last stroke" },
							{ "1 - 7", "Brushes, charcoal (6), palette knife (7)" },
							{ "X", "Swap to the colour before" },
							{ "Ctrl + wheel", "Zoom (close-up)" },
							{ "Middle / Space + drag", "Move the canvas (close-up)" },
							{ "[  ]", "Brush size" },
							{ "E", "Take / put down the rag" },
							{ "C", "Close-up / paint on the easel" },
							{ "Z", "Look 25% closer / back" },
							{ "M", "Mirror (close-up)" },
							{ "Ctrl+Z / Ctrl+Y", "Undo / redo (25 steps)" },
							{ "Mouse back / forward", "Undo / redo" },
							{ "Ctrl+0", "Whole canvas" },
							{ "Tab", "Hide the panel" },
							{ "F1", "This guide" },
							{ "Esc", "Close the guide, or step away from the easel" },
						});
					ImGui::EndTabBar();
				}
				ImGui::EndChild();
			}
			Theme::EndWindow();
			if (a_looking) {
				ImGui::PopStyleVar();
			}
		}

		bool ConfirmPopup(const char* a_id, const char* a_text, const char* a_yes)
		{
			bool yes = false;
			ImGui::SetNextWindowPos(ImGui::GetMainViewport()->GetCenter(), ImGuiCond_Always, ImVec2(0.5f, 0.5f));
			if (ImGui::BeginPopupModal(a_id, nullptr, ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoMove)) {
				Theme::DrawChrome();
				ImGui::Dummy(ImVec2(Theme::S(380), Theme::S(4)));
				ImGui::PushTextWrapPos(Theme::S(380));
				ImGui::TextUnformatted(a_text);
				ImGui::PopTextWrapPos();
				ImGui::Dummy(ImVec2(0, Theme::S(10)));
				if (Theme::Button(a_yes, ImVec2(Theme::S(180), 0), true)) {
					yes = true;
					ImGui::CloseCurrentPopup();
				}
				ImGui::SameLine();
				if (Theme::Button("Cancel", ImVec2(Theme::S(180), 0)) || ImGui::IsKeyPressed(ImGuiKey_Escape, false)) {
					ImGui::CloseCurrentPopup();
				}
				ImGui::EndPopup();
			}
			return yes;
		}
	}

	void Flush()
	{
		Live::Tick();
		for (auto* p : g_release) {
			p->Release();
		}
		g_release.clear();
	}

	bool Open(const Items::Work& a_work)
	{
		Close();
		LoadSpots();
		LoadPicture(g_palettePic, L"palette.png");
		LoadPicture(g_brushPic, L"brush.png");
		g_work = a_work;
		const int W = Art::Width(a_work.portrait), H = Art::Height(a_work.portrait);
		g_paint.Reset(W, H, MakeBlank(W, H));
		g_varnish = 0;
		g_strokes = 0;
		g_paintSeconds = 0.0;
		g_lastStrokeAt = -1e9;
		if (auto doc = Store::LoadDoc(a_work.dir)) {
			if (doc->w == W && doc->h == H) {
				auto& layers = g_paint.Layers();
				layers.clear();
				for (auto& l : doc->layers) {
					layers.push_back({ l.name, l.visible, l.opacity, std::move(l.px), static_cast<Paint::Blend>(l.blend), l.lockAlpha });
				}
				// carry on with the top layer that is showing (painting on a hidden one shows nothing)
				int top = static_cast<int>(layers.size()) - 1;
				while (top > 0 && !layers[top].visible) {
					--top;
				}
				g_paint.SetCurrent(top);
				g_paint.RecompositeAll();
				g_paint.SetThickness(std::move(doc->thickness));
				g_varnish = doc->varnish;
				g_strokes = std::max(0, doc->strokes);
				g_paintSeconds = std::max(0, doc->seconds);
				g_paint.MarkSaved();
				// one layer of paint now: a painting made with layers is merged (hidden ones too, nothing is lost)
				if (g_paint.Flatten()) {
					spdlog::info("Studio: merged {} layer(s) into one", doc->layers.size());
				}
				spdlog::info("Studio: continuing {} ({} layer(s))", Art::ToUtf8(a_work.dir), layers.size());
			} else {
				spdlog::warn("Studio: saved painting is {}x{}, the canvas {}x{} - starting blank", doc->w, doc->h, W, H);
			}
		}
		g_paint.TakeDirty();
		if (!CreateCanvasTexture()) {
			spdlog::error("Studio: could not create the canvas texture");
			Close();
			return false;
		}
		std::snprintf(g_name, sizeof g_name, "%s", a_work.name.c_str());
		g_placeFrames = 3;  // portrait and landscape lay out differently
		g_canvasPlace = true;
		{
			char buf[32]{};
			::GetPrivateProfileStringA("Studio", "fHudSize", "0.85", buf, sizeof buf, Overlay::IniPath().c_str());
			g_ui = std::clamp(static_cast<float>(std::atof(buf)), 0.6f, 1.2f);
		}
		g_hideHud = false;
		g_lastCanvas = ImVec2(-1e9f, -1e9f);
		g_closeUp = false;
		g_worldZoom = false;
		g_edits = g_editsSaved = 0;
		g_hasLineFrom = false;
		g_autosavedAt = ImGui::GetTime();
		g_openedAt = ImGui::GetTime();
		g_liveWarned = false;
		// paint straight onto the canvas on the easel: it gets a texture of its own for this session,
		// which the studio's picture is then lent to (Live)
		g_liveDds.clear();
		if (a_work.canvas) {
			g_liveDds = fmt::format("textures\\SkyrimPainter\\InProgress\\{}_{}.dds", Art::ToUtf8(a_work.dir.filename()), Art::NewId());
			Items::ShowLive(a_work.easel, g_liveDds, g_paint.Composite(), W, H, g_relief);
			Live::FaceCanvas(a_work.canvas, a_work.portrait);
		} else {
			g_closeUp = true;
		}
		g_open = true;
		return true;
	}

	bool Draw()
	{
		if (!g_open) {
			return false;
		}
		Upload();
		const auto&  io = ImGui::GetIO();
		const ImVec2 screen = io.DisplaySize;
		const bool   looking = Overlay::Looking();
		bool         stepAway = false;
		g_overCanvas = false;

		// the canvas on the easel shows the studio's picture as it is painted
		if (!g_liveDds.empty()) {
			Live::Attach(g_work.canvas, g_liveDds, g_tex, g_srv, g_reliefTex, g_reliefSrv);
			if (!Live::Attached() && !g_liveWarned && ImGui::GetTime() - g_openedAt > 6.0) {
				g_liveWarned = true;  // couldn't hook up: the close-up still works
				spdlog::warn("Studio: could not paint on the canvas in the world - using the close-up");
				g_closeUp = true;
				Hint("The canvas in the world couldn't be painted on - painting in the close-up");
			}
		}
		if (!g_closeUp) {
			DrawWorldCanvas(looking);
			if (ImGui::GetTime() < g_hintUntil) {  // (the close-up shows hints in its title bar)
				auto*        fg = ImGui::GetForegroundDrawList();
				const ImVec2 ts = ImGui::CalcTextSize(g_hint.c_str());
				const ImVec2 p((screen.x - ts.x) * 0.5f, screen.y * 0.06f);
				const float  pad = Theme::S(10.0f);
				fg->AddRectFilled(p - ImVec2(pad, pad * 0.6f), p + ts + ImVec2(pad, pad * 0.6f), IM_COL32(10, 9, 8, 190), Theme::S(6.0f));
				fg->AddText(p, IM_COL32(242, 222, 170, 255), g_hint.c_str());
			}
		}

		// No backdrop: the world stays in view so you can paint what's in front of you. The canvas sits on
		// the left (drag its top bar to move it, the corner to resize), the palette and the tools on the right.
		// panels keep wherever you put them; it is put in its place on opening and by Reset
		const ImGuiCond cond = g_placeFrames > 0 ? ImGuiCond_Always : ImGuiCond_FirstUseEver;
		const float     panelW = U(330.0f), margin = Theme::S(14.0f);
		if (g_closeUp) {
			const float aspect = static_cast<float>(g_paint.Width()) / static_cast<float>(g_paint.Height());
			const float bar = ImGui::GetFrameHeight() + Theme::S(8.0f);
			float       ch = screen.y * 0.70f, cw = ch * aspect;
			const float maxW = screen.x - panelW - margin * 3.0f;
			if (cw > maxW) {
				cw = maxW;
				ch = cw / aspect;
			}
			const ImGuiCond ccond = (g_placeFrames > 0 || g_canvasPlace) ? ImGuiCond_Always : ImGuiCond_FirstUseEver;
			g_canvasPlace = false;
			ImGui::SetNextWindowPos(ImVec2(margin, (screen.y - ch - bar) * 0.45f), ccond);
			ImGui::SetNextWindowSize(ImVec2(cw + Theme::S(16.0f), ch + bar + Theme::S(16.0f)), ccond);
			ImGui::SetNextWindowSizeConstraints(ImVec2(Theme::S(240.0f), Theme::S(200.0f)), screen);
			ImGui::SetNextWindowBgAlpha(looking ? 0.0f : 0.35f);
			if (ImGui::Begin("##easelcanvas", nullptr, ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse)) {
				// the top bar: grab it to move the canvas
				const std::string title = g_work.slot >= 0 ? fmt::format("\"{}\"", g_work.name) : std::string("A fresh canvas");
				ImGui::TextColored(ImVec4(0.92f, 0.85f, 0.68f, looking ? 0.4f : 1.0f), "%s", title.c_str());
				if (g_mirror || std::fabs(g_zoom - 1.0f) > 0.01f || g_pan.x != 0.0f || g_pan.y != 0.0f) {
					ImGui::SameLine();
					const std::string view = fmt::format("{}{}", g_mirror ? "  [in the mirror]" : "", std::fabs(g_zoom - 1.0f) > 0.01f ? fmt::format("  x{:.1f}", g_zoom) : std::string());
					ImGui::TextColored(ImVec4(0.75f, 0.72f, 0.65f, 1.0f), "%s", view.c_str());
					ImGui::SameLine();
					if (ImGui::SmallButton("Step back")) {
						g_zoom = 1.0f;
						g_pan = ImVec2(0.0f, 0.0f);
						g_mirror = false;
					}
					if (ImGui::IsItemHovered()) {
						ImGui::SetTooltip("See the whole canvas again, the right way round (Ctrl+0)");
					}
				}
				if (ImGui::GetTime() < g_hintUntil) {
					ImGui::SameLine();
					ImGui::TextColored(ImVec4(0.95f, 0.85f, 0.55f, 1.0f), "  %s", g_hint.c_str());
				}
				DrawCanvas(ImGui::GetContentRegionAvail());
			}
			ImGui::End();
		}

		if (!g_hideHud) {
			// While you look around (right mouse held) the panels stay where they are, faded and out of
			// the way of the mouse - they never close, so nothing moves or resets.
			const int faded = looking ? ImGuiWindowFlags_NoInputs : 0;
			if (looking) {
				ImGui::PushStyleVar(ImGuiStyleVar_Alpha, 0.25f);
			}
			// one panel on the right edge: the palette, brushes and rag, your hand, then the history, finishing and the view
			ImGui::SetNextWindowPos(ImVec2(screen.x - margin, screen.y * 0.5f), cond, ImVec2(1.0f, 0.5f));
			ImGui::SetNextWindowSize(ImVec2(panelW, 0.0f), ImGuiCond_Always);
			if (Theme::BeginWindow("##studiopanel", nullptr, faded)) {
				ImGui::SetWindowFontScale(g_ui);
				const float w = ImGui::GetContentRegionAvail().x;
				const float sp = ImGui::GetStyle().ItemSpacing.x;
				const float w2 = (w - sp) * 0.5f, w3 = (w - sp * 2.0f) / 3.0f;
				DrawPalettePanel(w);

				ImGui::Dummy(ImVec2(0, U(4)));
				ImGui::Separator();
				ImGui::Dummy(ImVec2(0, U(2)));
				ImGui::BeginDisabled(!g_paint.CanUndo());
				if (Theme::Button("Undo", ImVec2(w3, 0))) {
					g_paint.Undo();
					++g_edits;
				}
				ImGui::EndDisabled();
				ImGui::SameLine();
				ImGui::BeginDisabled(!g_paint.CanRedo());
				if (Theme::Button("Redo", ImVec2(w3, 0))) {
					g_paint.Redo();
					++g_edits;
				}
				ImGui::EndDisabled();
				ImGui::SameLine();
				if (Theme::Button("Scrape", ImVec2(w3, 0))) {
					g_openClear = true;
				}
				if (ImGui::IsItemHovered()) {
					ImGui::SetTooltip("Scrape all the paint off the canvas");
				}

				if (Theme::Button("Finish", ImVec2(w2, 0), true)) {
					g_openFinish = true;
				}
				ImGui::SameLine();
				if (Theme::Button("Step away", ImVec2(w2, 0))) {
					stepAway = true;
				}
				if (ImGui::IsItemHovered()) {
					ImGui::SetTooltip("Leave the painting on the easel and come back to it later (Esc)");
				}

				if (Theme::Button(g_guide ? "Close guide" : "Guide", ImVec2(w3, 0))) {
					g_guide = !g_guide;
				}
				if (ImGui::IsItemHovered()) {
					ImGui::SetTooltip("How everything works (F1)");
				}
				ImGui::SameLine();
				float size = g_ui * 100.0f;
				ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x);
				if (ImGui::SliderFloat("##hudsize", &size, 60.0f, 120.0f, "HUD size %.0f%%")) {
					g_ui = std::clamp(size / 100.0f, 0.6f, 1.2f);
				}
				if (ImGui::IsItemDeactivatedAfterEdit()) {
					::WritePrivateProfileStringA("Studio", "fHudSize", fmt::format("{:.2f}", g_ui).c_str(), Overlay::IniPath().c_str());
					g_placeFrames = 3;  // the panel changes size: put it back against the edge
				}
			}
			Theme::EndWindow();
			if (looking) {
				ImGui::PopStyleVar();
			}
		}
		if (g_placeFrames > 0) {
			--g_placeFrames;
		}

		DrawGuide(looking);

		// ---- popups
		if (g_openClear) {
			ImGui::OpenPopup("##clear");
			g_openClear = false;
		}
		if (ConfirmPopup("##clear", "Scrape all the paint off the canvas? (Undo can bring it back.)", "Scrape")) {
			g_paint.ClearLayer(g_paint.Current());
			++g_edits;
		}
		static int finishFocus = 0;
		if (g_openFinish) {
			ImGui::OpenPopup("##finish");
			g_openFinish = false;
			finishFocus = 3;  // the popup sizes itself on its first (hidden) frame: ask for the cursor a few frames
		}
		ImGui::SetNextWindowPos(ImGui::GetMainViewport()->GetCenter(), ImGuiCond_Always, ImVec2(0.5f, 0.5f));
		if (ImGui::BeginPopupModal("##finish", nullptr, ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoMove)) {
			Theme::DrawChrome();
			ImGui::Dummy(ImVec2(Theme::S(420), Theme::S(4)));
			ImGui::TextUnformatted("Name your painting");
			ImGui::SetNextItemWidth(Theme::S(420));
			if (finishFocus > 0) {
				--finishFocus;
				ImGui::SetKeyboardFocusHere();
			}
			const bool enter = ImGui::InputText("##name", g_name, sizeof g_name, ImGuiInputTextFlags_EnterReturnsTrue | ImGuiInputTextFlags_AutoSelectAll);
			ImGui::Dummy(ImVec2(0, Theme::S(8)));
			// the varnish: how the finished painting catches the light
			static int finishVarnish = 2;
			if (finishFocus == 2) {  // (the popup just opened)
				finishVarnish = g_varnish > 0 ? g_varnish : static_cast<int>(Paint::Varnish::Satin);
			}
			ImGui::TextUnformatted("Varnish");
			static constexpr const char* kVarnish[3] = { "Matte", "Satin", "Gloss" };
			static constexpr const char* kVarnishTip[3] = { "No shine at all - flat and soft", "A soft sheen, like most oil paintings",
				"A bright, wet-looking shine" };
			const float vw = (Theme::S(420) - ImGui::GetStyle().ItemSpacing.x * 2.0f) / 3.0f;
			for (int v = 1; v <= 3; ++v) {
				if (v > 1) {
					ImGui::SameLine();
				}
				if (Theme::Button(kVarnish[v - 1], ImVec2(vw, 0), finishVarnish == v)) {
					finishVarnish = v;
				}
				if (ImGui::IsItemHovered()) {
					ImGui::SetTooltip("%s", kVarnishTip[v - 1]);
				}
			}
			// what a merchant would pay for it
			static std::int32_t worth = 0;
			static int          worthFor = -1;
			if (finishFocus == 2 || worthFor != finishVarnish) {
				worthFor = finishVarnish;
				worth = Items::Appraise(g_paint.Composite(), g_paint.Width(), g_paint.Height(), finishVarnish, g_strokes, static_cast<int>(g_paintSeconds));
			}
			ImGui::Dummy(ImVec2(0, Theme::S(6)));
			ImGui::TextColored(ImGui::ColorConvertU32ToFloat4(Theme::Muted()), "A merchant would pay about %d gold  (%d strokes, %d min of painting)", worth, g_strokes,
				static_cast<int>(g_paintSeconds / 60.0));
			if (ImGui::IsItemHovered()) {
				ImGui::SetTooltip("The price comes from the painting - how much is painted, the detail, the colours - and from the work:\nthe time spent painting and the strokes it took. Full price takes about 45 minutes and 500 strokes.");
			}
			ImGui::Dummy(ImVec2(0, Theme::S(10)));
			bool done = enter;
			if (Theme::Button("Finish", ImVec2(Theme::S(200), 0), true)) {
				done = true;
			}
			ImGui::SameLine();
			if (Theme::Button("Keep painting", ImVec2(Theme::S(200), 0)) || ImGui::IsKeyPressed(ImGuiKey_Escape, false)) {
				ImGui::CloseCurrentPopup();
			}
			if (done) {
				ImGui::CloseCurrentPopup();
				g_varnish = finishVarnish;  // kept with the painting
				SaveAndThen(true, g_name);
				Close(true);
			}
			ImGui::EndPopup();
		}

		if (g_open && !ImGui::IsPopupOpen(nullptr, ImGuiPopupFlags_AnyPopupId) && !io.WantTextInput) {
			if (io.KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_Z)) {
				g_paint.Undo();
					++g_edits;
			} else if (io.KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_Y)) {
				g_paint.Redo();
					++g_edits;
			}
			if (ImGui::IsKeyPressed(ImGuiKey_Tab, false)) {
				g_hideHud = !g_hideHud;
			}
			if (!g_painting) {
				if (ImGui::IsKeyPressed(ImGuiKey_LeftBracket) && g_brush.type >= 0) {
					g_brush.size = std::max(0, g_brush.size - 1);
				}
				if (ImGui::IsKeyPressed(ImGuiKey_RightBracket) && g_brush.type >= 0) {
					g_brush.size = std::min(Paint::kSizeCount - 1, g_brush.size + 1);
				}
				if (ImGui::IsKeyPressed(ImGuiKey_E, false)) {
					ToggleRag();
				}
				for (int i = 0; i < Paint::kBrushCount; ++i) {  // 1-5 brushes, 6 charcoal, 7 palette knife
					if (ImGui::IsKeyPressed(static_cast<ImGuiKey>(ImGuiKey_1 + i), false)) {
						PickBrush(i);
					}
				}
				if (ImGui::IsKeyPressed(ImGuiKey_X, false) && !io.KeyCtrl && g_brush.type >= 0 && g_brush.type != Paint::kCharcoal) {
					if (g_hasPrev) {  // back to the colour before
						const Rgb now = g_brush.color;
						const bool loaded = g_brush.Loaded();
						g_brush.color = g_prevColour;
						g_brush.load = 1.0f;
						if (loaded) {
							g_prevColour = now;
						}
					}
				}
				// the mouse's side buttons: back = undo, forward = redo
				if (ImGui::IsMouseClicked(ImGuiMouseButton(3))) {
					g_paint.Undo();
					++g_edits;
				}
				if (ImGui::IsMouseClicked(ImGuiMouseButton(4))) {
					g_paint.Redo();
					++g_edits;
				}
				if (ImGui::IsKeyPressed(ImGuiKey_M, false) && g_closeUp) {
					g_mirror = !g_mirror;
				}
				if (ImGui::IsKeyPressed(ImGuiKey_Z, false) && !io.KeyCtrl) {
					if (g_closeUp) {  // the close-up: 125%, or back to the whole canvas
						const bool in = std::fabs(g_zoom - 1.25f) > 0.01f;
						g_zoom = in ? 1.25f : 1.0f;
						g_pan = ImVec2(0.0f, 0.0f);
					} else {  // on the easel: look 25% closer, or back
						g_worldZoom = !g_worldZoom;
						Live::SetZoom(g_worldZoom);
					}
				}
				if (ImGui::IsKeyPressed(ImGuiKey_C, false) && !io.KeyCtrl) {
					g_closeUp = !g_closeUp;
				}
				if (io.KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_0, false)) {
					g_zoom = 1.0f;
					g_pan = ImVec2(0.0f, 0.0f);
				}
			}
			if (ImGui::IsKeyPressed(ImGuiKey_F1, false)) {
				g_guide = !g_guide;
			}
			if (ImGui::IsKeyPressed(ImGuiKey_Escape, false)) {
				if (g_guide) {
					g_guide = false;  // Esc closes the guide first
				} else {
					stepAway = true;
				}
			}
		}

		// the cursor: over the canvas the brush's footprint takes its place; anywhere else it is the normal cursor
		if (g_open && g_overCanvas && (g_brush.type >= 0 || g_rag) && !looking) {
			ImGui::SetMouseCursor(ImGuiMouseCursor_None);
		}
		if (g_open) {
			AutosaveTick();
		}
		if (stepAway && g_open) {
			StepAway();
		}
		return g_open;
	}
}
