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

#include "Live.h"

#include "Art.h"
#include "CanvasGeom.h"

#include <chrono>
#include <cmath>
#include <optional>

namespace Live
{
	namespace
	{
		constexpr const char* kCanvasShape = "SPCanvas";

		// The game's texture we lent our picture to, and what it had before.
		struct Patch
		{
			std::uint32_t                         canvas = 0;
			RE::NiPointer<RE::NiSourceTexture>    tex;  // kept alive while it shows ours
			REX::W32::ID3D11Resource*             oldRes = nullptr;
			REX::W32::ID3D11ShaderResourceView*   oldSrv = nullptr;
			ID3D11Texture2D*                      ourTex = nullptr;  // one reference each, held by us
			ID3D11ShaderResourceView*             ourSrv = nullptr;
		};
		std::optional<Patch>                  g_patch;   // the colour texture
		std::optional<Patch>                  g_relief;  // the relief (normal map): ridges of paint seen as they are painted
		bool                                  g_detaching = false;
		std::chrono::steady_clock::time_point g_detachBy;
		bool                                  g_wasThirdPerson = false;
		bool                                  g_viewChanged = false;
		float                                 g_fovBefore = 0.0f;  // > 0: zoomed in, this was the field of view

		RE::TESObjectREFR* Canvas(std::uint32_t a_id)
		{
			auto* ref = a_id ? RE::TESForm::LookupByID<RE::TESObjectREFR>(a_id) : nullptr;
			return ref && !ref->IsDisabled() && !ref->IsDeleted() ? ref : nullptr;
		}

		RE::BSGeometry* CanvasShape(RE::TESObjectREFR* a_ref)
		{
			auto* root = a_ref ? a_ref->Get3D() : nullptr;
			auto* obj = root ? root->GetObjectByName(RE::BSFixedString(kCanvasShape)) : nullptr;
			return obj ? obj->AsGeometry() : nullptr;
		}

		RE::BSLightingShaderMaterialBase* Material(RE::BSGeometry* a_geom)
		{
			auto* shader = a_geom ? netimmerse_cast<RE::BSLightingShaderProperty*>(a_geom->GetGeometryRuntimeData().shaderProperty.get()) : nullptr;
			return shader && shader->material ? static_cast<RE::BSLightingShaderMaterialBase*>(shader->material) : nullptr;
		}

		bool SamePath(const char* a_a, const std::string& a_b)
		{
			if (!a_a) {
				return false;
			}
			// the game may keep it with or without "textures\" in front
			std::string a = a_a, b = a_b;
			auto        strip = [](std::string& s) {
                std::replace(s.begin(), s.end(), '/', '\\');
                if (s.size() > 9 && _strnicmp(s.c_str(), "textures\\", 9) == 0) {
                    s.erase(0, 9);
                }
			};
			strip(a);
			strip(b);
			return _stricmp(a.c_str(), b.c_str()) == 0;
		}

		void RestoreOne(std::optional<Patch>& a_patch)
		{
			if (!a_patch) {
				return;
			}
			auto& p = *a_patch;
			if (p.tex && p.tex->rendererTexture) {
				auto* rt = p.tex->rendererTexture;
				if (rt->resourceView == reinterpret_cast<REX::W32::ID3D11ShaderResourceView*>(p.ourSrv)) {
					rt->resourceView = p.oldSrv;
					rt->texture = p.oldRes;
				}
			}
			if (p.ourSrv) {
				p.ourSrv->Release();
			}
			if (p.ourTex) {
				p.ourTex->Release();
			}
			a_patch.reset();
		}

		void Restore()
		{
			if (g_patch) {
				spdlog::info("Live: canvas {:08X} shows its own texture again", g_patch->canvas);
			}
			RestoreOne(g_patch);
			RestoreOne(g_relief);
			g_detaching = false;
		}

		// Lends our texture to the game's a_tex (one of the canvas's textures, whose path is a_path),
		// only if a_path is the session's own a_want. Keeps it lent across frames.
		void Lend(std::optional<Patch>& a_patch, std::uint32_t a_canvas, RE::NiSourceTexture* a_tex, const char* a_path, const std::string& a_want,
			ID3D11Texture2D* a_ourTex, ID3D11ShaderResourceView* a_ourSrv)
		{
			if (a_patch) {
				// still ours? The game rebuilds the canvas's 3D now and then (it may load the texture anew)
				if (a_patch->canvas == a_canvas && a_tex == a_patch->tex.get() && a_tex && a_tex->rendererTexture &&
					a_tex->rendererTexture->resourceView == reinterpret_cast<REX::W32::ID3D11ShaderResourceView*>(a_patch->ourSrv)) {
					return;
				}
				RestoreOne(a_patch);
			}
			if (!a_ourTex || !a_ourSrv || !a_tex || !a_tex->rendererTexture || !a_tex->rendererTexture->resourceView) {
				return;  // not loaded yet
			}
			if (!SamePath(a_path, a_want)) {
				return;  // the canvas isn't showing the session's texture (yet): never touch any other
			}
			Patch p;
			p.canvas = a_canvas;
			p.tex.reset(a_tex);
			auto* rt = a_tex->rendererTexture;
			p.oldRes = rt->texture;
			p.oldSrv = rt->resourceView;
			a_ourTex->AddRef();
			a_ourSrv->AddRef();
			p.ourTex = a_ourTex;
			p.ourSrv = a_ourSrv;
			rt->texture = reinterpret_cast<REX::W32::ID3D11Resource*>(a_ourTex);
			rt->resourceView = reinterpret_cast<REX::W32::ID3D11ShaderResourceView*>(a_ourSrv);
			a_patch = std::move(p);
		}

		RE::NiPoint3 Corner(const RE::NiTransform& a_world, bool a_portrait, int a_i)
		{
			const auto& c = a_portrait ? CanvasGeom::kPortrait[a_i] : CanvasGeom::kLandscape[a_i];
			return a_world * RE::NiPoint3(c[0], c[1], c[2]);
		}
	}

	bool CanvasOnScreen(std::uint32_t a_canvas, bool a_portrait, float a_screenW, float a_screenH, float a_out[4][2])
	{
		// the game's own view matrix and viewport (as TrueHUD uses them). The call's result is ignored on
		// purpose: it is false for points off the edge of the screen, and up close a canvas corner often is.
		static const REL::Relocation<float(*)[4]>       worldToCam{ RELOCATION_ID(519579, 406126) };
		static const REL::Relocation<RE::NiRect<float>*> viewPort{ RELOCATION_ID(519618, 406160) };
		auto* geom = CanvasShape(Canvas(a_canvas));
		if (!geom) {
			return false;
		}
		const RE::NiTransform world = geom->world;
		for (int i = 0; i < 4; ++i) {
			float x = 0.0f, y = 0.0f, depth = 0.0f;
			RE::NiCamera::WorldPtToScreenPt3(worldToCam.get(), *viewPort.get(), Corner(world, a_portrait, i), x, y, depth, 1e-5f);
			if (depth < 0.0f) {
				return false;  // behind the camera
			}
			a_out[i][0] = x * a_screenW;
			a_out[i][1] = (1.0f - y) * a_screenH;
		}
		static bool logged = false;
		if (!logged) {
			logged = true;
			spdlog::info("Live: canvas on screen at ({:.0f},{:.0f}) ({:.0f},{:.0f}) ({:.0f},{:.0f}) ({:.0f},{:.0f})", a_out[0][0], a_out[0][1], a_out[1][0], a_out[1][1], a_out[2][0],
				a_out[2][1], a_out[3][0], a_out[3][1]);
		}
		return true;
	}

	void Attach(std::uint32_t a_canvas, const std::string& a_dds, ID3D11Texture2D* a_tex, ID3D11ShaderResourceView* a_srv,
		ID3D11Texture2D* a_reliefTex, ID3D11ShaderResourceView* a_reliefSrv)
	{
		if (!a_tex || !a_srv || a_dds.empty()) {
			return;
		}
		if (g_detaching) {
			Restore();
		}
		auto* material = Material(CanvasShape(Canvas(a_canvas)));
		if (!material || !material->textureSet) {
			RestoreOne(g_patch);
			RestoreOne(g_relief);
			return;
		}
		const bool had = g_patch.has_value();
		Lend(g_patch, a_canvas, material->diffuseTexture.get(), material->textureSet->GetTexturePath(RE::BSTextureSet::Texture::kDiffuse), a_dds, a_tex, a_srv);
		if (g_patch) {
			Lend(g_relief, a_canvas, material->normalTexture.get(), material->textureSet->GetTexturePath(RE::BSTextureSet::Texture::kNormal),
				Art::NormalPath(a_dds), a_reliefTex, a_reliefSrv);
		}
		if (!had && g_patch) {
			spdlog::info("Live: painting straight onto canvas {:08X}{}", a_canvas, g_relief ? " (with its relief)" : "");
		}
	}

	bool Attached() { return g_patch.has_value() && !g_detaching; }

	void Detach(bool a_wait)
	{
		if (!g_patch) {
			return;
		}
		if (!a_wait) {
			Restore();
			return;
		}
		g_detaching = true;
		g_detachBy = std::chrono::steady_clock::now() + std::chrono::seconds(20);
	}

	void Tick()
	{
		if (!g_patch || !g_detaching) {
			return;
		}
		auto* geom = CanvasShape(Canvas(g_patch->canvas));
		auto* material = Material(geom);
		auto* diffuse = material ? material->diffuseTexture.get() : nullptr;
		// the canvas has moved on to its new texture (or is gone): ours can go
		if (!geom || diffuse != g_patch->tex.get() || std::chrono::steady_clock::now() > g_detachBy) {
			Restore();
		}
	}

	void FaceCanvas(std::uint32_t a_canvas, bool a_portrait)
	{
		SKSE::GetTaskInterface()->AddTask([a_canvas, a_portrait] {
			auto* player = RE::PlayerCharacter::GetSingleton();
			auto* cam = RE::PlayerCamera::GetSingleton();
			auto* geom = CanvasShape(Canvas(a_canvas));
			if (!player || !cam || !geom) {
				return;
			}
			g_wasThirdPerson = !cam->IsInFirstPerson();
			g_viewChanged = true;
			if (g_wasThirdPerson) {
				cam->ForceFirstPerson();
			}
			// look at the middle of the canvas
			RE::NiPoint3 centre;
			for (int i = 0; i < 4; ++i) {
				centre += Corner(geom->world, a_portrait, i);
			}
			centre *= 0.25f;
			const RE::NiPoint3 eye = player->GetLookingAtLocation();
			const RE::NiPoint3 d = centre - eye;
			const float        flat = std::sqrt(d.x * d.x + d.y * d.y);
			if (flat < 1.0f) {
				return;
			}
			player->SetHeading(std::atan2(d.x, d.y));
			player->SetLooking(std::clamp(-std::atan2(d.z, flat), -1.2f, 1.2f));
		});
	}

	namespace
	{
		// Runs a console command the way the console does (the "fov" command sets the field of view on every
		// game version, without relying on where the camera keeps it).
		void RunConsole(const std::string& a_cmd)
		{
			auto* factory = RE::IFormFactory::GetConcreteFormFactoryByType<RE::Script>();
			auto* script = factory ? factory->Create() : nullptr;
			if (!script) {
				spdlog::warn("Zoom: can't run \"{}\"", a_cmd);
				return;
			}
			script->SetCommand(a_cmd);
			script->CompileAndRun(nullptr);
			delete script;
		}

		// The field of view now: the camera's, if it looks like one, else the INI's, else the game's default.
		float CurrentFov()
		{
			// (where the camera keeps it is only known up to 1.6; on 1.7 the INI's value is used)
			auto* cam = REL::Module::get().version() < SKSE::RUNTIME_SSE_1_7_99 ? RE::PlayerCamera::GetSingleton() : nullptr;
			if (cam) {
				const float f = cam->GetRuntimeData2().worldFOV;
				if (f >= 20.0f && f <= 150.0f) {
					return f;
				}
			}
			if (auto* ini = RE::INISettingCollection::GetSingleton()) {
				if (auto* set = ini->GetSetting("fDefaultWorldFOV:Display"); set && set->GetFloat() >= 20.0f && set->GetFloat() <= 150.0f) {
					return set->GetFloat();
				}
			}
			return 80.0f;
		}
	}

	void SetZoom(bool a_on)
	{
		SKSE::GetTaskInterface()->AddTask([a_on] {
			if (a_on && g_fovBefore <= 0.0f) {
				// 25% closer: everything in view looks 1.25 times bigger
				g_fovBefore = CurrentFov();
				const float half = g_fovBefore * 0.5f * 0.0174532925f;
				const float fov = 2.0f * std::atan(std::tan(half) / 1.25f) * 57.2957795f;
				RunConsole(fmt::format("fov {:.2f}", fov));
				spdlog::info("Zoom in: field of view {:.1f} -> {:.1f}", g_fovBefore, fov);
			} else if (!a_on && g_fovBefore > 0.0f) {
				RunConsole(fmt::format("fov {:.2f}", g_fovBefore));
				spdlog::info("Zoom out: field of view back to {:.1f}", g_fovBefore);
				g_fovBefore = 0.0f;
			}
		});
	}

	void RestoreView()
	{
		SetZoom(false);
		SKSE::GetTaskInterface()->AddTask([] {
			if (!g_viewChanged) {
				return;
			}
			g_viewChanged = false;
			if (g_wasThirdPerson) {
				if (auto* cam = RE::PlayerCamera::GetSingleton()) {
					cam->ForceThirdPerson();
				}
			}
		});
	}
}
