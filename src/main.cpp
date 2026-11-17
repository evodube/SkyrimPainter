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
#include "Items.h"
#include "Overlay.h"
#include "Place.h"

namespace
{
	void SetupLog()
	{
		auto path = SKSE::log::log_directory();
		if (!path) {
			return;
		}
		{
			const auto      dir = *path;
			std::error_code ec;
			std::filesystem::remove(dir / "SkyrimPainter.1.log", ec);
			std::filesystem::rename(dir / "SkyrimPainter.log", dir / "SkyrimPainter.1.log", ec);
		}
		*path /= "SkyrimPainter.log";
		auto sink = std::make_shared<spdlog::sinks::basic_file_sink_mt>(path->string(), true);
		auto log = std::make_shared<spdlog::logger>("global log", std::move(sink));
		log->set_level(spdlog::level::info);
		log->flush_on(spdlog::level::info);
		spdlog::set_default_logger(std::move(log));
		spdlog::set_pattern("[%H:%M:%S.%e] [%l] %v");
	}

	// ------------------------------------------------------------------ Present hook (from Hearthframe)
	using PresentFn = HRESULT(STDMETHODCALLTYPE*)(IDXGISwapChain*, UINT, UINT);
	PresentFn         g_originalPresent = nullptr;
	std::atomic<bool> g_presentDisabled{ false };

	void SafeOnPresent(IDXGISwapChain* a_sc)
	{
		__try {
			Overlay::OnPresent(a_sc);
		} __except (Overlay::SehFilter(GetExceptionInformation(), "OnPresent")) {
			g_presentDisabled = true;
		}
	}

	std::atomic<std::uint64_t> g_presents{ 0 };
	thread_local int           t_inPresent = 0;  // Present1 may call Present (or the other way round): draw once

	HRESULT STDMETHODCALLTYPE HookedPresent(IDXGISwapChain* a_sc, UINT a_sync, UINT a_flags)
	{
		++g_presents;
		if (!g_presentDisabled && t_inPresent++ == 0) {
			SafeOnPresent(a_sc);
		}
		const HRESULT hr = g_originalPresent(a_sc, a_sync, a_flags);
		--t_inPresent;
		return hr;
	}

	using Present1Fn = HRESULT(STDMETHODCALLTYPE*)(IDXGISwapChain1*, UINT, UINT, const DXGI_PRESENT_PARAMETERS*);
	Present1Fn g_originalPresent1 = nullptr;

	HRESULT STDMETHODCALLTYPE HookedPresent1(IDXGISwapChain1* a_sc, UINT a_sync, UINT a_flags, const DXGI_PRESENT_PARAMETERS* a_params)
	{
		++g_presents;
		if (!g_presentDisabled && t_inPresent++ == 0) {
			SafeOnPresent(a_sc);
		}
		const HRESULT hr = g_originalPresent1(a_sc, a_sync, a_flags, a_params);
		--t_inPresent;
		return hr;
	}

	std::string ModuleOf(const void* a_addr)
	{
		HMODULE mod = nullptr;
		char    name[MAX_PATH]{};
		if (::GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, static_cast<LPCSTR>(a_addr), &mod) && mod &&
			::GetModuleFileNameA(mod, name, MAX_PATH)) {
			const char* slash = std::strrchr(name, '\\');
			return slash ? slash + 1 : name;
		}
		return "?";
	}

	bool PatchSlot(void** a_vtable, int a_slot, void* a_hook, void** a_original)
	{
		if (a_vtable[a_slot] == a_hook) {
			return true;
		}
		DWORD old = 0;
		if (!::VirtualProtect(&a_vtable[a_slot], sizeof(void*), PAGE_EXECUTE_READWRITE, &old)) {
			spdlog::error("VirtualProtect failed ({})", ::GetLastError());
			return false;
		}
		*a_original = a_vtable[a_slot];
		a_vtable[a_slot] = a_hook;
		::VirtualProtect(&a_vtable[a_slot], sizeof(void*), old, &old);
		return true;
	}

	IDXGISwapChain* g_hookedSc = nullptr;

	bool PatchPresentSlot(IDXGISwapChain* a_sc)
	{
		auto** vtable = *reinterpret_cast<void***>(a_sc);
		spdlog::info("Swap chain {:X}: Present is in {}", reinterpret_cast<std::uintptr_t>(a_sc), ModuleOf(vtable[8]));
		if (!PatchSlot(vtable, 8, reinterpret_cast<void*>(&HookedPresent), reinterpret_cast<void**>(&g_originalPresent))) {
			return false;
		}
		// Present1 too: some swap chain wrappers present through it
		IDXGISwapChain1* sc1 = nullptr;
		if (SUCCEEDED(a_sc->QueryInterface(__uuidof(IDXGISwapChain1), reinterpret_cast<void**>(&sc1))) && sc1) {
			auto** vt1 = *reinterpret_cast<void***>(sc1);
			PatchSlot(vt1, 22, reinterpret_cast<void*>(&HookedPresent1), reinterpret_cast<void**>(&g_originalPresent1));
			sc1->Release();
		}
		g_hookedSc = a_sc;
		return true;
	}

	IDXGISwapChain* GetGameSwapChain()
	{
		auto* renderer = RE::BSGraphics::Renderer::GetSingleton();
		return renderer ? reinterpret_cast<IDXGISwapChain*>(renderer->GetRuntimeData().renderWindows[0].swapChain) : nullptr;
	}

	bool InstallPresentHook()
	{
		IDXGISwapChain* sc = nullptr;
		__try {
			sc = GetGameSwapChain();
		} __except (Overlay::SehFilter(GetExceptionInformation(), "GetGameSwapChain")) {
			sc = nullptr;
		}
		// No dummy swap chain fallback: an extra swap chain breaks ENB Frame Generation.
		return sc && PatchPresentSlot(sc);
	}

	void OnMessage(SKSE::MessagingInterface::Message* a_msg)
	{
		switch (a_msg->type) {
		case SKSE::MessagingInterface::kDataLoaded:
			{
				const auto& dbg = Overlay::GetDebugSwitches();
				if (dbg.hookPresent) {
					spdlog::info(InstallPresentHook() ? "Present hook installed" : "Could not install the Present hook - the studio will not appear");
					// Another mod may swap the game's swap chain for its own later (frame generation, upscalers):
					// if frames stop coming through the hook, hook whatever the game presents with now.
					std::thread([] {
						int tries = 0;
						for (std::uint64_t last = g_presents; tries < 20;) {
							std::this_thread::sleep_for(std::chrono::seconds(3));
							const std::uint64_t now = g_presents;
							if (now != last) {
								last = now;
								continue;
							}
							++tries;
							SKSE::GetTaskInterface()->AddTask([] {
								IDXGISwapChain* sc = nullptr;
								__try {
									sc = GetGameSwapChain();
								} __except (Overlay::SehFilter(GetExceptionInformation(), "GetGameSwapChain")) {
									sc = nullptr;
								}
								if (sc && (sc != g_hookedSc || (*reinterpret_cast<void***>(sc))[8] != reinterpret_cast<void*>(&HookedPresent))) {
									spdlog::info("No frames through the Present hook - hooking the game's swap chain again");
									PatchPresentSlot(sc);
								}
							});
						}
					}).detach();
				}
				if (dbg.inputSink) {
					spdlog::info(Overlay::InstallInputSink() ? "Input sink registered" : "Could not register for input events");
				}
				if (!Items::Init()) {
					spdlog::error("Some item records are missing - is SkyrimPainter.esp enabled?");
				}
				if (!Place::Init()) {
					spdlog::error("The wall painting records are missing - is SkyrimPainter.esp enabled?");
				}
				Art::RegisterEvents();
				spdlog::info("Setup finished");
				break;
			}
		case SKSE::MessagingInterface::kPostLoadGame:
		case SKSE::MessagingInterface::kNewGame:
			{
				static bool blockerDone = false;
				if (!blockerDone) {
					blockerDone = true;
					Overlay::InstallInputBlocker();  // after every plugin that hooks input at the main menu
				}
				Place::Cancel(false);
				Items::OnGameLoaded();
				Place::OnGameLoaded();
				break;
			}
		default:
			break;
		}
	}
}

SKSEPluginLoad(const SKSE::LoadInterface* a_skse)
{
	SKSE::Init(a_skse);
	SetupLog();
	const auto ver = REL::Module::get().version();
	spdlog::info("SkyrimPainter {} loaded - game {} ({}), SKSE {}", SKSE::PluginDeclaration::GetSingleton()->GetVersion().string("."sv),
		ver.string("."sv), REL::Module::IsAE() ? "AE" : "SE", REL::Version::unpack(a_skse->SKSEVersion()).string("."sv));
	Overlay::LoadSettings();
	Overlay::PrepareInputBlocker();
	Items::InstallSerialization();
	if (!SKSE::GetMessagingInterface()->RegisterListener(OnMessage)) {
		spdlog::error("Could not register with the SKSE messaging interface");
		return false;
	}
	return true;
}
