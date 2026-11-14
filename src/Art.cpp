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

#include "Art.h"

#include "Dds.h"
#include "Overlay.h"
#include "Paint.h"
#include "Store.h"

namespace fs = std::filesystem;

namespace Art
{
	namespace
	{
		constexpr const char* kCanvasShape = "SPCanvas";
		constexpr const char* kFlatNormal = "textures\\SkyrimPainter\\flat_n.dds";

		enum class Conv
		{
			Running,
			Ready,
			Failed
		};

		std::mutex                                g_mtx;  // guards the two maps
		std::unordered_map<RE::FormID, std::string> g_shown;        // reference -> texture it should show
		std::unordered_map<std::string, Conv>       g_conversions;  // texture -> state

		// main thread only: references waiting for their 3D / texture (ticks left)
		std::unordered_map<RE::FormID, int> g_pending;

		void QueueApply(RE::FormID a_id, int a_ticks = 600)
		{
			auto& left = g_pending[a_id];
			left = (std::max)(left, a_ticks);
		}

		std::string CurrentTexture(RE::TESObjectREFR* a_ref)
		{
			auto* root = a_ref ? a_ref->Get3D() : nullptr;
			auto* obj = root ? root->GetObjectByName(RE::BSFixedString(kCanvasShape)) : nullptr;
			auto* geom = obj ? obj->AsGeometry() : nullptr;
			auto* shader = geom ? netimmerse_cast<RE::BSLightingShaderProperty*>(geom->GetGeometryRuntimeData().shaderProperty.get()) : nullptr;
			auto* material = shader ? static_cast<RE::BSLightingShaderMaterialBase*>(shader->material) : nullptr;
			if (!material || !material->textureSet) {
				return {};
			}
			const char* p = material->textureSet->GetTexturePath(RE::BSTextureSet::Texture::kDiffuse);
			return p ? p : "";
		}

		// true when done (applied, or nothing to apply), false to try again later
		bool ApplyToRef(RE::FormID a_id)
		{
			std::string dds;
			{
				std::lock_guard lk(g_mtx);
				const auto      it = g_shown.find(a_id);
				if (it == g_shown.end()) {
					return true;
				}
				const auto c = g_conversions.find(it->second);
				if (c != g_conversions.end() && c->second == Conv::Failed) {
					return true;
				}
				if (c == g_conversions.end() || c->second != Conv::Ready) {
					return false;  // still converting
				}
				dds = it->second;
			}
			auto* ref = RE::TESForm::LookupByID<RE::TESObjectREFR>(a_id);
			if (!ref || ref->IsDisabled()) {
				return true;
			}
			if (!ref->Get3D()) {
				return false;
			}
			if (ApplyNow(ref, dds)) {
				spdlog::info("{:08X}: canvas shows {}", a_id, dds);
			}
			return true;
		}

		void ProcessPending()
		{
			for (auto it = g_pending.begin(); it != g_pending.end();) {
				if (ApplyToRef(it->first) || --it->second <= 0) {
					it = g_pending.erase(it);
				} else {
					++it;
				}
			}
		}

		void QueueAllUsing(const std::string& a_dds)
		{
			std::vector<RE::FormID> ids;
			{
				std::lock_guard lk(g_mtx);
				for (const auto& [id, d] : g_shown) {
					if (d == a_dds) {
						ids.push_back(id);
					}
				}
			}
			for (const auto id : ids) {
				QueueApply(id);
			}
		}

		// The game rebuilds a reference's 3D from the mesh when it moves or reloads, dropping our texture.
		// A slice of the references is checked each call; loaded ones are also handled by the events.
		void Watchdog()
		{
			constexpr std::size_t                           kPerCall = 32;
			static std::size_t                              cursor = 0;
			std::vector<std::pair<RE::FormID, std::string>> refs;
			{
				std::lock_guard lk(g_mtx);
				const std::size_t n = g_shown.size();
				if (n == 0) {
					return;
				}
				cursor %= n;
				auto it = g_shown.begin();
				std::advance(it, cursor);
				for (std::size_t k = 0; k < (std::min)(kPerCall, n); ++k) {
					const auto c = g_conversions.find(it->second);
					if (c != g_conversions.end() && c->second == Conv::Ready) {
						refs.emplace_back(it->first, it->second);
					}
					if (++it == g_shown.end()) {
						it = g_shown.begin();
					}
				}
				cursor = (cursor + (std::min)(kPerCall, n)) % n;
			}
			for (const auto& [id, dds] : refs) {
				auto* ref = RE::TESForm::LookupByID<RE::TESObjectREFR>(id);
				if (!ref || ref->IsDisabled() || !ref->Get3D()) {
					continue;
				}
				if (_stricmp(CurrentTexture(ref).c_str(), dds.c_str()) != 0 && ApplyNow(ref, dds)) {
					spdlog::info("{:08X}: canvas restored after its 3D was rebuilt", id);
				}
			}
		}

		bool Shown(RE::FormID a_id)
		{
			std::lock_guard lk(g_mtx);
			return g_shown.contains(a_id);
		}

		class EventSink final :
			public RE::BSTEventSink<RE::TESCellAttachDetachEvent>,
			public RE::BSTEventSink<RE::TESObjectLoadedEvent>
		{
		public:
			RE::BSEventNotifyControl ProcessEvent(const RE::TESObjectLoadedEvent* a_event, RE::BSTEventSource<RE::TESObjectLoadedEvent>*) override
			{
				if (a_event && a_event->loaded && Shown(a_event->formID)) {
					const auto id = a_event->formID;
					SKSE::GetTaskInterface()->AddTask([id] { QueueApply(id, 120); });
				}
				return RE::BSEventNotifyControl::kContinue;
			}

			RE::BSEventNotifyControl ProcessEvent(const RE::TESCellAttachDetachEvent* a_event, RE::BSTEventSource<RE::TESCellAttachDetachEvent>*) override
			{
				if (a_event && a_event->attached && a_event->reference && Shown(a_event->reference->GetFormID())) {
					const auto id = a_event->reference->GetFormID();
					SKSE::GetTaskInterface()->AddTask([id] { QueueApply(id, 300); });
				}
				return RE::BSEventNotifyControl::kContinue;
			}
		};
		EventSink g_sink;
	}

	std::string ToUtf8(const fs::path& a_path)
	{
		const auto s = a_path.u8string();
		return std::string(s.begin(), s.end());
	}

	fs::path FromUtf8(const std::string& a_text)
	{
		return fs::path(std::u8string(a_text.begin(), a_text.end()));
	}

	fs::path DataPath(const std::string& a_gamePath)
	{
		std::error_code ec;
		return fs::absolute(fs::path(L"Data") / FromUtf8(a_gamePath), ec);
	}

	fs::path UserFolder()
	{
		PWSTR    docs = nullptr;
		fs::path out;
		if (SUCCEEDED(::SHGetKnownFolderPath(FOLDERID_Documents, 0, nullptr, &docs)) && docs) {
			out = fs::path(docs) / L"My Games" / L"Skyrim Special Edition" / L"SkyrimPainter";
			::CoTaskMemFree(docs);
		} else {
			std::error_code ec;
			out = fs::absolute(fs::path(L"Data\\SKSE\\Plugins\\SkyrimPainter"), ec);
		}
		return out;
	}

	std::string NewId()
	{
		static std::atomic<std::uint32_t> counter{ 0 };
		SYSTEMTIME st{};
		::GetLocalTime(&st);
		const auto salt = (::GetTickCount() ^ (++counter * 2654435761u)) & 0xFFFF;
		return fmt::format("{:04}{:02}{:02}_{:02}{:02}{:02}_{:04x}", st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond, salt);
	}

	void Notify(const std::string& a_msg)
	{
		spdlog::info("Notice: {}", a_msg);
		SKSE::GetTaskInterface()->AddTask([a_msg] { RE::SendHUDMessage::ShowHUDMessage(a_msg.c_str()); });
	}

	bool ApplyNow(RE::TESObjectREFR* a_ref, const std::string& a_dds)
	{
		auto* root = a_ref ? a_ref->Get3D() : nullptr;
		if (!root) {
			return false;
		}
		auto* obj = root->GetObjectByName(RE::BSFixedString(kCanvasShape));
		auto* geom = obj ? obj->AsGeometry() : nullptr;
		if (!geom) {
			spdlog::warn("{:08X}: no SPCanvas shape in its 3D", a_ref->GetFormID());
			return false;
		}
		auto* shader = netimmerse_cast<RE::BSLightingShaderProperty*>(geom->GetGeometryRuntimeData().shaderProperty.get());
		if (!shader || !shader->material) {
			spdlog::warn("{:08X}: SPCanvas has no lighting shader", a_ref->GetFormID());
			return false;
		}
		auto* material = static_cast<RE::BSLightingShaderMaterialBase*>(shader->material);
		auto* textures = RE::BSShaderTextureSet::Create();
		if (!textures) {
			return false;
		}
		textures->SetTexturePath(RE::BSTextureSet::Texture::kDiffuse, a_dds.c_str());
		// the painting's own relief (impasto, canvas weave, varnish) when it has one
		std::error_code ec;
		const auto      normal = NormalPath(a_dds);
		textures->SetTexturePath(RE::BSTextureSet::Texture::kNormal, fs::exists(DataPath(normal), ec) ? normal.c_str() : kFlatNormal);
		// The game's own pattern for texture-set swaps: copy the material, give the copy the new
		// textures, hand it to the shader (which keeps its own cached copy), free ours.
		auto* copy = static_cast<RE::BSLightingShaderMaterialBase*>(material->Create());
		if (!copy) {
			return false;
		}
		copy->CopyMembers(material);
		copy->ClearTextures();
		copy->OnLoadTextureSet(0, textures);
		shader->SetMaterial(copy, true);
		shader->SetupGeometry(geom);
		shader->FinishSetupGeometry(geom);
		copy->~BSLightingShaderMaterialBase();
		RE::free(copy);
		return true;
	}

	void Show(RE::FormID a_ref, const std::string& a_dds)
	{
		{
			std::lock_guard lk(g_mtx);
			g_shown[a_ref] = a_dds;
			std::error_code ec;
			if (!g_conversions.contains(a_dds) && fs::exists(DataPath(a_dds), ec)) {
				g_conversions[a_dds] = Conv::Ready;
			}
		}
		QueueApply(a_ref);
	}

	void Forget(RE::FormID a_ref)
	{
		std::lock_guard lk(g_mtx);
		g_shown.erase(a_ref);
	}

	void ForgetAll()
	{
		std::lock_guard lk(g_mtx);
		g_shown.clear();
	}

	bool IsReady(const std::string& a_dds)
	{
		std::lock_guard lk(g_mtx);
		const auto      it = g_conversions.find(a_dds);
		return it != g_conversions.end() && it->second == Conv::Ready;
	}

	void EnsureFromPng(const std::string& a_dds, const fs::path& a_png, int a_w, int a_h)
	{
		std::error_code ec;
		{
			std::lock_guard lk(g_mtx);
			if (fs::exists(DataPath(a_dds), ec)) {
				g_conversions[a_dds] = Conv::Ready;
				return;
			}
			if (const auto it = g_conversions.find(a_dds); it != g_conversions.end() && it->second == Conv::Running) {
				return;
			}
			g_conversions[a_dds] = Conv::Running;
		}
		std::thread([a_dds, a_png, a_w, a_h] {
			std::string err;
			bool        ok = false;
			try {
				// the relief, from the painting's saved thickness, if it has one
				const auto      dir = a_png.parent_path();
				std::error_code nec;
				if (!fs::exists(DataPath(NormalPath(a_dds)), nec)) {
					std::vector<std::uint8_t> th;
					int                       tw = 0, thh = 0;
					if (Store::LoadThickness(dir, th, tw, thh)) {
						std::vector<std::uint8_t> nm;
						Paint::MakeNormalMap(th, tw, thh, static_cast<Paint::Varnish>(Store::LoadVarnish(dir)), ReliefStyle(), nm);
						std::string nerr;
						Dds::ConvertNormalMap(nm.data(), tw, thh, a_w, a_h, DataPath(NormalPath(a_dds)), nerr);
					}
				}
				ok = Dds::ConvertPicture(a_png, a_w, a_h, DataPath(a_dds), err);
			} catch (const std::exception& e) {
				err = e.what();
			}
			{
				std::lock_guard lk(g_mtx);
				g_conversions[a_dds] = ok ? Conv::Ready : Conv::Failed;
			}
			if (ok) {
				spdlog::info("Converted {} -> {}", ToUtf8(a_png), a_dds);
				SKSE::GetTaskInterface()->AddTask([a_dds] { QueueAllUsing(a_dds); });
			} else {
				spdlog::error("Could not convert {}: {}", ToUtf8(a_png), err);
			}
		}).detach();
	}

	bool ConvertNow(const std::uint8_t* a_rgba, int a_srcW, int a_srcH, const std::string& a_dds, int a_w, int a_h)
	{
		{
			std::lock_guard lk(g_mtx);
			g_conversions[a_dds] = Conv::Running;
		}
		std::string err;
		bool        ok = false;
		try {
			ok = Dds::ConvertPixels(a_rgba, a_srcW, a_srcH, a_w, a_h, DataPath(a_dds), err);
		} catch (const std::exception& e) {
			err = e.what();
		}
		{
			std::lock_guard lk(g_mtx);
			g_conversions[a_dds] = ok ? Conv::Ready : Conv::Failed;
		}
		if (!ok) {
			spdlog::error("Could not write {}: {}", a_dds, err);
		}
		return ok;
	}

	std::string NormalPath(const std::string& a_dds)
	{
		if (a_dds.size() > 4 && _stricmp(a_dds.c_str() + a_dds.size() - 4, ".dds") == 0) {
			return a_dds.substr(0, a_dds.size() - 4) + "_n.dds";
		}
		return a_dds + "_n.dds";
	}

	void ConvertAndShow(std::vector<std::uint8_t> a_rgba, int a_srcW, int a_srcH, const std::string& a_dds, int a_w, int a_h, RE::FormID a_ref,
		std::vector<std::uint8_t> a_normal)
	{
		std::thread([px = std::move(a_rgba), nm = std::move(a_normal), a_srcW, a_srcH, a_dds, a_w, a_h, a_ref] {
			// the relief first: the canvas picks it up when the colour texture is put on
			if (nm.size() == static_cast<std::size_t>(a_srcW) * a_srcH * 4) {
				std::string err;
				if (!Dds::ConvertNormalMap(nm.data(), a_srcW, a_srcH, a_w, a_h, DataPath(NormalPath(a_dds)), err)) {
					spdlog::warn("Could not write {}: {}", NormalPath(a_dds), err);
				}
			}
			if (!ConvertNow(px.data(), a_srcW, a_srcH, a_dds, a_w, a_h)) {
				return;
			}
			SKSE::GetTaskInterface()->AddTask([a_dds, a_ref] {
				if (a_ref) {
					Show(a_ref, a_dds);
				}
				QueueAllUsing(a_dds);
			});
		}).detach();
	}

	void Tick()
	{
		ProcessPending();
		static int ticks = 0;
		if (++ticks % 30 == 0) {
			Watchdog();
		}
	}

	void RegisterEvents()
	{
		if (auto* events = RE::ScriptEventSourceHolder::GetSingleton()) {
			events->AddEventSink<RE::TESObjectLoadedEvent>(&g_sink);
			events->AddEventSink<RE::TESCellAttachDetachEvent>(&g_sink);
		}
	}

	void DeleteTexture(const std::string& a_dds)
	{
		if (a_dds.empty()) {
			return;
		}
		{
			std::lock_guard lk(g_mtx);
			for (const auto& [id, d] : g_shown) {
				if (_stricmp(d.c_str(), a_dds.c_str()) == 0) {
					return;  // still on something
				}
			}
			g_conversions.erase(a_dds);
		}
		std::error_code ec;
		fs::remove(DataPath(a_dds), ec);
		fs::remove(DataPath(NormalPath(a_dds)), ec);
	}

	Paint::NormalStyle ReliefStyle()
	{
		static const Paint::NormalStyle style = [] {
			Paint::NormalStyle s;
			const auto         ini = Overlay::IniPath();
			s.swapXY = ::GetPrivateProfileIntA("Studio", "bReliefSwapXY", 1, ini.c_str()) != 0;
			s.flipY = ::GetPrivateProfileIntA("Studio", "bReliefFlipY", 0, ini.c_str()) != 0;
			return s;
		}();
		return style;
	}
}
