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

#include "Overlay.h"

#include "Art.h"
#include "Items.h"
#include "Menus.h"
#include "Studio.h"
#include "Theme.h"
#include "Place.h"

#define IMGUI_DEFINE_MATH_OPERATORS
#include <imgui.h>
#include <imgui_impl_dx11.h>

namespace fs = std::filesystem;

namespace Overlay
{
	namespace
	{
		constexpr std::uint32_t kDIK_Escape = 0x01;
		constexpr std::uint32_t kDIK_Left = 0xCB;
		constexpr std::uint32_t kDIK_Right = 0xCD;
		constexpr std::uint32_t kDIK_Up = 0xC8;
		constexpr std::uint32_t kDIK_Down = 0xD0;

		struct Settings
		{
			float fontSize = 18.0f;
			float mouseSpeed = 1.0f;
			float uiScale = 1.0f;
		} cfg;

		DebugSwitches     dbg;
		std::string       g_iniPath;
		std::atomic<bool> g_inputDisabled{ false };

		enum class Screen
		{
			None,
			Studio,
			PaintingMenu,
			EaselMenu
		};
		Screen g_screen = Screen::None;  // render thread

		struct InputMsg
		{
			enum class Kind
			{
				MouseMove,
				MouseButton,
				Wheel,
				Key,
				Char
			} kind;
			float    x = 0.0f, y = 0.0f;
			int      button = 0;
			ImGuiKey key = ImGuiKey_None;
			bool     down = false;
		};
		std::mutex            g_inputMtx;
		std::vector<InputMsg> g_input;

		std::atomic<bool> g_open{ false };
		std::atomic<bool> g_wantOpen{ false };
		std::atomic<bool> g_studioActive{ false };  // the studio is the open screen (set on the render thread)
		std::atomic<bool> g_look{ false };          // right mouse held in the studio: the mouse turns the camera
		bool              g_imguiReady = false;
		bool              g_imguiFailed = false;
		bool              g_placementControls = false;
		std::uintptr_t    g_dispatchInput = 0;
		std::atomic<bool> g_shiftDown{ false };
		std::atomic<bool> g_ctrlDown{ false };
		std::atomic<bool> g_altDown{ false };

		IDXGISwapChain*                       g_sc = nullptr;
		ID3D11Device*                         g_dev = nullptr;
		ID3D11DeviceContext*                  g_ctx = nullptr;
		float                                 g_mouseX = 0.0f, g_mouseY = 0.0f;
		std::chrono::steady_clock::time_point g_lastFrame{};
		float                                 g_displayH = 1080.0f;
		bool                                  g_centerMouse = false;

		// True when another menu has the screen (inventory, map, console, dialogue, a mod's menu...).
		bool OtherMenuOpen()
		{
			if (auto* ui = RE::UI::GetSingleton()) {
				for (const auto& menu : ui->menuStack) {
					if (menu && (menu->PausesGame() || menu->UsesCursor() || menu->Modal())) {
						return true;
					}
				}
			}
			using UEFlag = RE::ControlMap::UEFlag;
			auto* cm = RE::ControlMap::GetSingleton();
			return cm && !cm->AreControlsEnabled(static_cast<UEFlag>(static_cast<std::uint32_t>(UEFlag::kMovement) | static_cast<std::uint32_t>(UEFlag::kLooking)));
		}

		void ApplyOpen(bool a_open)
		{
			if (g_open == a_open) {
				return;
			}
			g_open = a_open;
			g_wantOpen = a_open;
			if (!a_open) {
				g_screen = Screen::None;
				g_studioActive = false;
				g_look = false;
				Menus::Release();
			} else {
				g_centerMouse = true;
			}
			using UEFlag = RE::ControlMap::UEFlag;
			constexpr auto blocked = static_cast<UEFlag>(
				static_cast<std::uint32_t>(UEFlag::kMovement) | static_cast<std::uint32_t>(UEFlag::kLooking) |
				static_cast<std::uint32_t>(UEFlag::kActivate) | static_cast<std::uint32_t>(UEFlag::kMenu) |
				static_cast<std::uint32_t>(UEFlag::kFighting) | static_cast<std::uint32_t>(UEFlag::kJumping) |
				static_cast<std::uint32_t>(UEFlag::kSneaking) | static_cast<std::uint32_t>(UEFlag::kWheelZoom) |
				static_cast<std::uint32_t>(UEFlag::kPOVSwitch));
			// The input blocker already keeps presses away from the game while a window is open; only
			// without it are the game's controls switched off.
			if (!g_dispatchInput) {
				if (auto* cm = RE::ControlMap::GetSingleton()) {
					cm->ToggleControls(blocked, !a_open, false);
				}
			}
			if (g_imguiReady) {
				ImGui::GetIO().MouseDrawCursor = a_open;
			}
			spdlog::info("Window {}", a_open ? "opened" : "closed");
		}

		void ApplyPlacementControls(bool a_on)
		{
			if (g_placementControls == a_on) {
				return;
			}
			g_placementControls = a_on;
			if (g_dispatchInput) {
				return;
			}
			using UEFlag = RE::ControlMap::UEFlag;
			constexpr auto blocked = static_cast<UEFlag>(
				static_cast<std::uint32_t>(UEFlag::kActivate) | static_cast<std::uint32_t>(UEFlag::kMenu) |
				static_cast<std::uint32_t>(UEFlag::kFighting) | static_cast<std::uint32_t>(UEFlag::kWheelZoom));
			if (auto* cm = RE::ControlMap::GetSingleton()) {
				cm->ToggleControls(blocked, !a_on, false);
			}
		}

		// Character a key types with the current Windows keyboard layout (0 if none). Input thread.
		wchar_t KeyToChar(std::uint32_t a_dik, bool a_shift, bool a_altGr)
		{
			const UINT sc = (a_dik & 0x7F) | ((a_dik & 0x80) ? 0xE000u : 0u);
			HKL        layout = ::GetKeyboardLayout(::GetWindowThreadProcessId(::GetForegroundWindow(), nullptr));
			UINT       vk = 0;
			if (::GetKeyState(VK_NUMLOCK) & 1) {
				switch (a_dik) {
				case 0x47: vk = VK_NUMPAD7; break;
				case 0x48: vk = VK_NUMPAD8; break;
				case 0x49: vk = VK_NUMPAD9; break;
				case 0x4B: vk = VK_NUMPAD4; break;
				case 0x4C: vk = VK_NUMPAD5; break;
				case 0x4D: vk = VK_NUMPAD6; break;
				case 0x4F: vk = VK_NUMPAD1; break;
				case 0x50: vk = VK_NUMPAD2; break;
				case 0x51: vk = VK_NUMPAD3; break;
				case 0x52: vk = VK_NUMPAD0; break;
				case 0x53: vk = VK_DECIMAL; break;
				default: break;
				}
			}
			if (vk == 0) {
				vk = ::MapVirtualKeyExW(sc, MAPVK_VSC_TO_VK_EX, layout);
			}
			if (vk == 0) {
				return 0;
			}
			BYTE state[256]{};
			if (a_shift) {
				state[VK_SHIFT] = state[VK_LSHIFT] = 0x80;
			}
			if (::GetKeyState(VK_CAPITAL) & 1) {
				state[VK_CAPITAL] = 0x01;
			}
			if (::GetKeyState(VK_NUMLOCK) & 1) {
				state[VK_NUMLOCK] = 0x01;
			}
			if (a_altGr) {
				state[VK_CONTROL] = state[VK_LCONTROL] = state[VK_MENU] = state[VK_RMENU] = 0x80;
			}
			wchar_t   buf[8]{};
			const int n = ::ToUnicodeEx(vk, sc, state, buf, 8, 0x4, layout);
			return (n == 1 && buf[0] >= 0x20 && buf[0] != 0x7F) ? buf[0] : 0;
		}

		ImGuiKey KeyToImGui(std::uint32_t a_dik)
		{
			switch (a_dik) {
			case kDIK_Escape: return ImGuiKey_Escape;
			case kDIK_Left: return ImGuiKey_LeftArrow;
			case kDIK_Right: return ImGuiKey_RightArrow;
			case kDIK_Up: return ImGuiKey_UpArrow;
			case kDIK_Down: return ImGuiKey_DownArrow;
			case 0x0E: return ImGuiKey_Backspace;
			case 0xD3: return ImGuiKey_Delete;
			case 0x1C: return ImGuiKey_Enter;
			case 0x9C: return ImGuiKey_KeypadEnter;
			case 0xC7: return ImGuiKey_Home;
			case 0xCF: return ImGuiKey_End;
			case 0x0F: return ImGuiKey_Tab;
			case 0x1E: return ImGuiKey_A;
			case 0x2E: return ImGuiKey_C;
			case 0x2F: return ImGuiKey_V;
			case 0x2D: return ImGuiKey_X;
			case 0x2C: return ImGuiKey_Z;
			case 0x15: return ImGuiKey_Y;
			case 0x30: return ImGuiKey_B;  // tool shortcuts in the studio
			case 0x12: return ImGuiKey_E;
			case 0x21: return ImGuiKey_F;
			case 0x22: return ImGuiKey_G;
			case 0x1A: return ImGuiKey_LeftBracket;
			case 0x1B: return ImGuiKey_RightBracket;
			case 0x39: return ImGuiKey_Space;  // studio: hold to pan the canvas
			case 0x32: return ImGuiKey_M;      // studio: look at the painting in a mirror
			case 0x0B: return ImGuiKey_0;      // studio: Ctrl+0 fits the canvas again
			case 0x3B: return ImGuiKey_F1;     // studio: the painter's guide
			case 0x02: return ImGuiKey_1;      // studio: brushes 1-4
			case 0x03: return ImGuiKey_2;
			case 0x04: return ImGuiKey_3;
			case 0x05: return ImGuiKey_4;
			case 0x06: return ImGuiKey_5;
			case 0x07: return ImGuiKey_6;
			case 0x08: return ImGuiKey_7;
			default: return ImGuiKey_None;
			}
		}

		float g_offsetRepeatAt = 0.0f;
		float g_sizeRepeatAt = 0.0f;

		void PushInput(const InputMsg& a_m)
		{
			std::lock_guard lk(g_inputMtx);
			if (g_input.size() < 1024) {
				g_input.push_back(a_m);
			}
		}

		void HandleInput(RE::InputEvent* const* a_event)
		{
			if (!a_event) {
				return;
			}
			const bool placing = Place::IsPlacing() && !g_open;
			bool       menuBusy = false;
			bool       menuChecked = false;
			auto       busy = [&] {
				if (!menuChecked) {
					menuChecked = true;
					menuBusy = OtherMenuOpen();
				}
				return menuBusy;
			};
			for (auto* e = *a_event; e; e = e->next) {
				if (e->GetEventType() == RE::INPUT_EVENT_TYPE::kMouseMove) {
					if (g_open && !g_look) {
						auto* mm = static_cast<RE::MouseMoveEvent*>(e);
						PushInput({ InputMsg::Kind::MouseMove, static_cast<float>(mm->mouseInputX), static_cast<float>(mm->mouseInputY) });
					}
					continue;
				}
				auto* btn = e->AsButtonEvent();
				if (!btn) {
					continue;
				}
				const auto code = btn->GetIDCode();
				const bool down = btn->IsDown();
				const bool up = btn->IsUp();
				const bool keyboard = e->GetDevice() == RE::INPUT_DEVICE::kKeyboard;
				if (!down && !up) {
					// held: Up/Down keep moving the painting while hanging, Left/Right keep resizing
					if (keyboard && placing && !busy()) {
						const float held = btn->HeldDuration();
						if ((code == kDIK_Up || code == kDIK_Down) && held >= g_offsetRepeatAt) {
							Place::Nudge((code == kDIK_Up ? -1 : 1) * (g_shiftDown ? 8 : 1));
							g_offsetRepeatAt = held + (g_shiftDown ? 0.10f : 0.05f);
						}
						if ((code == kDIK_Left || code == kDIK_Right) && held >= g_sizeRepeatAt) {
							Place::Wheel(code == kDIK_Right ? 1 : -1, g_shiftDown);
							g_sizeRepeatAt = held + 0.10f;
						}
					}
					continue;
				}
				if (keyboard && (code == 0x2A || code == 0x36)) {
					g_shiftDown = down;
					if (g_open) {
						InputMsg m{ InputMsg::Kind::Key };
						m.key = ImGuiMod_Shift;
						m.down = down;
						PushInput(m);
					}
				}
				if (keyboard && (code == 0x1D || code == 0x9D)) {
					g_ctrlDown = down;
					if (g_open) {
						InputMsg m{ InputMsg::Kind::Key };
						m.key = ImGuiMod_Ctrl;
						m.down = down;
						PushInput(m);
					}
				}
				if (keyboard && (code == 0x38 || code == 0xB8)) {
					g_altDown = down;
				}
				if (placing) {
					if (!down || busy()) {
						continue;
					}
					if (e->GetDevice() == RE::INPUT_DEVICE::kMouse) {
						using MouseKey = RE::BSWin32MouseDevice::Key;
						if (code == 0) {
							Place::RequestPlace();
						} else if (code == 1) {
							Place::Cancel();
						} else if (code == MouseKey::kWheelUp || code == MouseKey::kWheelDown) {
							Place::Wheel(code == MouseKey::kWheelUp ? 1 : -1, g_shiftDown);
						}
					} else if (keyboard) {
						if (code == kDIK_Escape) {
							Place::Cancel();
						} else if (code == kDIK_Up || code == kDIK_Down) {
							Place::Nudge((code == kDIK_Up ? -1 : 1) * (g_shiftDown ? 8 : 1));
							g_offsetRepeatAt = 0.35f;
						} else if (code == kDIK_Left || code == kDIK_Right) {
							Place::Wheel(code == kDIK_Right ? 1 : -1, g_shiftDown);
							g_sizeRepeatAt = 0.35f;
						}
					}
					continue;
				}
				if (!g_open) {
					continue;
				}
				if (keyboard) {
					if (const ImGuiKey key = KeyToImGui(code); key != ImGuiKey_None) {
						InputMsg m{ InputMsg::Kind::Key };
						m.key = key;
						m.down = down;
						PushInput(m);
					}
					// the keyboard as it really is now: a modifier whose release we missed (released while
					// no window was open, Alt-Tab...) must not block typing
					const bool ctrl = (::GetAsyncKeyState(VK_CONTROL) & 0x8000) != 0;
					const bool alt = (::GetAsyncKeyState(VK_MENU) & 0x8000) != 0;
					const bool shift = (::GetAsyncKeyState(VK_SHIFT) & 0x8000) != 0;
					g_ctrlDown = ctrl;
					g_altDown = alt;
					g_shiftDown = shift;
					const bool altGr = ctrl && alt;
					if (down && (!ctrl || altGr)) {
						if (const wchar_t c = KeyToChar(code, shift, altGr); c != 0) {
							InputMsg m{ InputMsg::Kind::Char };
							m.button = static_cast<int>(c);
							PushInput(m);
						}
					}
				} else if (e->GetDevice() == RE::INPUT_DEVICE::kMouse) {
					using MouseKey = RE::BSWin32MouseDevice::Key;
					if (code == 1 && g_studioActive) {
						// in the studio the right button is "look": held, the mouse turns the camera instead of the cursor
						g_look = down;
						continue;
					}
					if (code == MouseKey::kWheelUp || code == MouseKey::kWheelDown) {
						if (down) {
							InputMsg m{ InputMsg::Kind::Wheel };
							m.y = code == MouseKey::kWheelUp ? 1.0f : -1.0f;
							PushInput(m);
						}
					} else if (code <= 4) {
						InputMsg m{ InputMsg::Kind::MouseButton };
						m.button = static_cast<int>(code);
						m.down = down;
						PushInput(m);
					}
				}
			}
		}

		void SafeHandleInput(RE::InputEvent* const* a_event)
		{
			__try {
				HandleInput(a_event);
			} __except (SehFilter(GetExceptionInformation(), "HandleInput")) {
				g_inputDisabled = true;
			}
		}

		// While a window is open, other mods' hotkeys and the game must not see the keys pressed in it.
		// We sit in front of the call that hands each frame's input to every listener (see InstallInputBlocker).
		using DispatchInput = void(RE::BSTEventSource<RE::InputEvent*>*, RE::InputEvent* const*);
		bool g_inBlockerPass = false;

		// While hanging, the player still walks and looks around, but clicks, the wheel, Esc and the
		// activate / attack / menu keys are ours.
		bool PlacementSwallows(RE::InputEvent* a_e)
		{
			auto* b = a_e ? a_e->AsButtonEvent() : nullptr;
			if (!b || b->IsUp()) {
				return false;
			}
			const auto code = b->GetIDCode();
			if (b->GetDevice() == RE::INPUT_DEVICE::kMouse) {
				using MouseKey = RE::BSWin32MouseDevice::Key;
				return code <= 2 || code == MouseKey::kWheelUp || code == MouseKey::kWheelDown;
			}
			if (b->GetDevice() == RE::INPUT_DEVICE::kKeyboard && code == kDIK_Escape) {
				return true;
			}
			auto* ue = RE::UserEvents::GetSingleton();
			if (!ue) {
				return false;
			}
			const auto& ev = b->QUserEvent();
			return ev == ue->activate || ev == ue->leftAttack || ev == ue->rightAttack || ev == ue->readyWeapon || ev == ue->shout ||
			       ev == ue->tweenMenu || ev == ue->journal || ev == ue->favorites || ev == ue->wait || ev == ue->quickInventory ||
			       ev == ue->quickMagic || ev == ue->quickStats || ev == ue->quickMap || ev == ue->zoomIn || ev == ue->zoomOut;
		}

		void Relink(RE::InputEvent* (&a_head)[1], const std::vector<RE::InputEvent*>& a_list)
		{
			a_head[0] = nullptr;
			RE::InputEvent* last = nullptr;
			for (auto* e : a_list) {
				if (last) {
					last->next = e;
				} else {
					a_head[0] = e;
				}
				last = e;
			}
			if (last) {
				last->next = nullptr;
			}
		}

		void DispatchInputHook(RE::BSTEventSource<RE::InputEvent*>* a_source, RE::InputEvent* const* a_events)
		{
			const auto original = reinterpret_cast<DispatchInput*>(g_dispatchInput);
			if (!g_open && Place::IsPlacing() && a_events && *a_events && !g_inputDisabled && !OtherMenuOpen()) {
				SafeHandleInput(a_events);
				static RE::InputEvent*       placing[1];
				std::vector<RE::InputEvent*> pass;
				for (auto* e = *a_events; e; e = e->next) {
					if (!PlacementSwallows(e)) {
						pass.push_back(e);
					}
				}
				Relink(placing, pass);
				g_inBlockerPass = true;
				original(a_source, placing);
				g_inBlockerPass = false;
				return;
			}
			if (g_open && a_events && *a_events && !g_inputDisabled) {
				SafeHandleInput(a_events);
				// releases still go on, so nothing stays held after the window closes
				static RE::InputEvent*       kept[1];
				std::vector<RE::InputEvent*> releases;
				const bool look = g_look && g_studioActive;  // looking around in the studio: the camera gets the mouse
				for (auto* e = *a_events; e; e = e->next) {
					if (auto* b = e->AsButtonEvent(); b && b->IsUp()) {
						releases.push_back(e);
					} else if (look && (e->GetEventType() == RE::INPUT_EVENT_TYPE::kMouseMove || e->GetEventType() == RE::INPUT_EVENT_TYPE::kThumbstick)) {
						releases.push_back(e);
					}
				}
				Relink(kept, releases);
				g_inBlockerPass = true;
				original(a_source, kept);
				g_inBlockerPass = false;
				return;
			}
			original(a_source, a_events);
		}

		class InputSink final : public RE::BSTEventSink<RE::InputEvent*>
		{
		public:
			RE::BSEventNotifyControl ProcessEvent(RE::InputEvent* const* a_event, RE::BSTEventSource<RE::InputEvent*>*) override
			{
				if (!g_inputDisabled && !g_inBlockerPass) {
					SafeHandleInput(a_event);
				}
				return RE::BSEventNotifyControl::kContinue;
			}
		};

		void DrainInput()
		{
			// ImGui's modifiers follow the real keyboard too, or a stuck Ctrl makes text boxes ignore typing
			if (g_open) {
				auto& io = ImGui::GetIO();
				const bool ctrl = (::GetAsyncKeyState(VK_CONTROL) & 0x8000) != 0;
				const bool shift = (::GetAsyncKeyState(VK_SHIFT) & 0x8000) != 0;
				if (io.KeyCtrl != ctrl) {
					io.AddKeyEvent(ImGuiMod_Ctrl, ctrl);
				}
				if (io.KeyShift != shift) {
					io.AddKeyEvent(ImGuiMod_Shift, shift);
				}
				g_ctrlDown = ctrl;
				g_shiftDown = shift;
			}
			std::vector<InputMsg> msgs;
			{
				std::lock_guard lk(g_inputMtx);
				msgs.swap(g_input);
			}
			auto& io = ImGui::GetIO();
			for (const auto& m : msgs) {
				switch (m.kind) {
				case InputMsg::Kind::MouseMove:
					g_mouseX = std::clamp(g_mouseX + m.x * cfg.mouseSpeed, 0.0f, io.DisplaySize.x - 1.0f);
					g_mouseY = std::clamp(g_mouseY + m.y * cfg.mouseSpeed, 0.0f, io.DisplaySize.y - 1.0f);
					io.AddMousePosEvent(g_mouseX, g_mouseY);
					break;
				case InputMsg::Kind::MouseButton:
					io.AddMouseButtonEvent(m.button, m.down);
					break;
				case InputMsg::Kind::Wheel:
					io.AddMouseWheelEvent(0.0f, m.y);
					break;
				case InputMsg::Kind::Key:
					io.AddKeyEvent(m.key, m.down);
					break;
				case InputMsg::Kind::Char:
					io.AddInputCharacterUTF16(static_cast<ImWchar16>(m.button));
					break;
				}
			}
		}

		void ApplyTheme()
		{
			char windir[MAX_PATH]{};
			::GetWindowsDirectoryA(windir, MAX_PATH);
			const std::string fonts = std::string(windir) + "\\Fonts\\";
			const std::string regular = fonts + "georgia.ttf", bold = fonts + "georgiab.ttf", italic = fonts + "georgiai.ttf";
			const float       scale = (std::max)(1.0f, g_displayH / 1080.0f) * cfg.uiScale;
			if (g_imguiReady) {
				ImGui_ImplDX11_InvalidateDeviceObjects();
			}
			static bool lookChosen = false;
			if (!lookChosen) {
				lookChosen = true;
				for (int i = 0; i < Theme::StyleCount(); ++i) {
					if (std::strcmp(Theme::StyleName(i), "Modern") == 0) {
						Theme::SetStyle(i);
					}
				}
				Theme::SetTextureFactory([](int a_w, int a_h, const unsigned char* a_px) -> ImTextureID {
					return reinterpret_cast<ImTextureID>(MakeSRV(a_px, a_w, a_h));
				});
			}
			std::error_code   fec;
			const std::string own = Art::ToUtf8(fs::absolute(fs::path(L"Data\\SKSE\\Plugins\\SkyrimPainter\\Fonts"), fec)) + "\\";
			const std::string display = own + "Cinzel-SemiBold.ttf", uiReg = own + "Inter-Regular.ttf", uiSemi = own + "Inter-SemiBold.ttf",
							  uiItal = own + "Inter-Italic.ttf";
			Theme::FontFiles files{ regular.c_str(), bold.c_str(), italic.c_str() };
			files.display = display.c_str();
			files.uiRegular = uiReg.c_str();
			files.uiSemiBold = uiSemi.c_str();
			files.uiItalic = uiItal.c_str();
			Theme::Setup(scale, cfg.fontSize, files);
			ImGui::GetIO().MouseDrawCursor = g_open;
			spdlog::info("UI: scale {:.2f}, font {}", scale, cfg.fontSize);
		}

		bool InitImGui()
		{
			IMGUI_CHECKVERSION();
			ImGui::CreateContext();
			auto& io = ImGui::GetIO();
			io.IniFilename = nullptr;
			io.LogFilename = nullptr;
			io.ConfigFlags |= ImGuiConfigFlags_NoMouseCursorChange;
			ApplyTheme();
			if (!ImGui_ImplDX11_Init(g_dev, g_ctx)) {
				return false;
			}
			io.MouseDrawCursor = g_open;
			return true;
		}

		// Which screen to open, if any (render thread, nothing open).
		void PollRequests()
		{
			if (g_open || g_wantOpen || Place::IsPlacing()) {
				return;
			}
			if (auto easel = Items::TakeStudioRequest()) {
				if (Studio::Open(*easel)) {
					g_screen = Screen::Studio;
					g_wantOpen = true;
				}
				return;
			}
			if (auto menu = Place::TakeActivated()) {
				Menus::OpenPainting(*menu);
				g_screen = Screen::PaintingMenu;
				g_wantOpen = true;
				return;
			}
			if (auto easel = Items::TakeEaselMenuRequest()) {
				Menus::OpenEasel(*easel);
				g_screen = Screen::EaselMenu;
				g_wantOpen = true;
			}
		}
	}

	void LoadSettings()
	{
		std::error_code ec;
		g_iniPath = fs::absolute(fs::path(L"Data\\SKSE\\Plugins\\SkyrimPainter.ini"), ec).string();
		const char* ini = g_iniPath.c_str();
		char        buf[64]{};
		::GetPrivateProfileStringA("General", "fFontSize", "18", buf, sizeof buf, ini);
		cfg.fontSize = std::clamp(static_cast<float>(std::atof(buf)), 8.0f, 64.0f);
		::GetPrivateProfileStringA("General", "fUIScale", "1.0", buf, sizeof buf, ini);
		cfg.uiScale = std::clamp(static_cast<float>(std::atof(buf)), 0.6f, 2.0f);
		::GetPrivateProfileStringA("General", "fMouseSpeed", "1.0", buf, sizeof buf, ini);
		cfg.mouseSpeed = std::clamp(static_cast<float>(std::atof(buf)), 0.1f, 10.0f);
		dbg.hookPresent = ::GetPrivateProfileIntA("Debug", "bHookPresent", 1, ini) != 0;
		dbg.inputSink = ::GetPrivateProfileIntA("Debug", "bInputSink", 1, ini) != 0;
		dbg.blockInput = ::GetPrivateProfileIntA("Debug", "bBlockInput", 1, ini) != 0;
		spdlog::info("INI {} ({}): font {} scale {} mouse {}", g_iniPath, fs::exists(g_iniPath, ec) ? "found" : "not found, defaults",
			cfg.fontSize, cfg.uiScale, cfg.mouseSpeed);
	}

	const DebugSwitches& GetDebugSwitches() { return dbg; }
	std::string          IniPath() { return g_iniPath; }
	ID3D11Device*        Device() { return g_dev; }
	ID3D11DeviceContext* Context() { return g_ctx; }

	ID3D11ShaderResourceView* MakeSRV(const std::uint8_t* a_rgba, int a_w, int a_h)
	{
		if (!g_dev) {
			return nullptr;
		}
		D3D11_TEXTURE2D_DESC d{};
		d.Width = static_cast<UINT>(a_w);
		d.Height = static_cast<UINT>(a_h);
		d.MipLevels = 1;
		d.ArraySize = 1;
		d.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
		d.SampleDesc.Count = 1;
		d.Usage = D3D11_USAGE_DEFAULT;
		d.BindFlags = D3D11_BIND_SHADER_RESOURCE;
		D3D11_SUBRESOURCE_DATA sd{ a_rgba, static_cast<UINT>(a_w) * 4, 0 };
		ID3D11Texture2D*       tex = nullptr;
		if (FAILED(g_dev->CreateTexture2D(&d, &sd, &tex)) || !tex) {
			return nullptr;
		}
		ID3D11ShaderResourceView* srv = nullptr;
		g_dev->CreateShaderResourceView(tex, nullptr, &srv);
		tex->Release();
		return srv;
	}

	int SehFilter(EXCEPTION_POINTERS* a_info, const char* a_where)
	{
		const auto* rec = a_info ? a_info->ExceptionRecord : nullptr;
		const auto  addr = rec ? reinterpret_cast<std::uintptr_t>(rec->ExceptionAddress) : 0;
		HMODULE     mod = nullptr;
		wchar_t     modPath[MAX_PATH]{};
		if (addr && ::GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
						reinterpret_cast<LPCWSTR>(addr), &mod)) {
			::GetModuleFileNameW(mod, modPath, MAX_PATH);
		}
		const auto modName = Art::ToUtf8(fs::path(modPath).filename());
		const auto offset = mod ? addr - reinterpret_cast<std::uintptr_t>(mod) : addr;
		spdlog::critical("CRASH CAUGHT in {}: exception 0x{:08X} at {}+0x{:X} - this part is now switched off so the game keeps running",
			a_where, rec ? static_cast<std::uint32_t>(rec->ExceptionCode) : 0u, modName.empty() ? "?" : modName, offset);
		return EXCEPTION_EXECUTE_HANDLER;
	}

	bool Looking()
	{
		return g_look && g_studioActive;
	}

	void PrepareInputBlocker()
	{
		if (dbg.inputSink && dbg.blockInput) {
			SKSE::AllocTrampoline(14);
		}
	}

	void InstallInputBlocker()
	{
		if (!dbg.inputSink || !dbg.blockInput) {
			spdlog::info("Input blocker off");
			return;
		}
		// BSInputDeviceManager::PollInputDevices -> the call that sends the input events to all listeners.
		REL::Relocation<std::uintptr_t> target{ REL::RelocationID(67315, 68617), REL::VariantOffset(0x7B, 0x7B, 0x81) };
		const auto*                     code = reinterpret_cast<const std::uint8_t*>(target.address());
		if (*code != 0xE8) {
			spdlog::warn("Input blocker: unexpected code at {:X} ({:02X}) - other mods' hotkeys can still fire in the studio", target.address(), *code);
			return;
		}
		g_dispatchInput = SKSE::GetTrampoline().write_call<5>(target.address(), &DispatchInputHook);
		spdlog::info("Input blocker installed");
	}

	bool InstallInputSink()
	{
		static InputSink sink;
		auto*            mgr = RE::BSInputDeviceManager::GetSingleton();
		if (!mgr) {
			return false;
		}
		mgr->AddEventSink<RE::InputEvent*>(&sink);
		return true;
	}

	void OnPresent(IDXGISwapChain* a_sc)
	{
		if (!g_sc) {
			DXGI_SWAP_CHAIN_DESC desc{};
			if (FAILED(a_sc->GetDesc(&desc)) || !desc.OutputWindow) {
				return;
			}
			if (FAILED(a_sc->GetDevice(__uuidof(ID3D11Device), reinterpret_cast<void**>(&g_dev))) || !g_dev) {
				return;
			}
			g_dev->GetImmediateContext(&g_ctx);
			g_sc = a_sc;
			g_displayH = static_cast<float>(desc.BufferDesc.Height);
			spdlog::info("Attached to the game's swap chain ({}x{})", desc.BufferDesc.Width, desc.BufferDesc.Height);
		}
		if (a_sc != g_sc) {
			return;
		}

		Studio::Flush();
		Menus::Flush();
		ApplyPlacementControls(Place::IsPlacing());
		Place::Tick();
		PollRequests();
		if (g_wantOpen != g_open) {
			ApplyOpen(g_wantOpen);
		}
		Place::Status placing;
		const bool    hud = !g_open && Place::GetStatus(placing);
		if ((!g_open && !hud) || g_imguiFailed) {
			if (!g_open) {
				std::lock_guard lk(g_inputMtx);
				g_input.clear();
			}
			return;
		}
		if (!g_imguiReady) {
			if (!InitImGui()) {
				g_imguiFailed = true;
				spdlog::error("ImGui failed to initialise");
				ApplyOpen(false);
				return;
			}
			g_imguiReady = true;
		}

		ID3D11Texture2D* back = nullptr;
		if (FAILED(a_sc->GetBuffer(0, __uuidof(ID3D11Texture2D), reinterpret_cast<void**>(&back))) || !back) {
			return;
		}
		D3D11_TEXTURE2D_DESC bd{};
		back->GetDesc(&bd);
		auto& io = ImGui::GetIO();
		io.DisplaySize = ImVec2(static_cast<float>(bd.Width), static_cast<float>(bd.Height));
		const auto now = std::chrono::steady_clock::now();
		const float dt = g_lastFrame.time_since_epoch().count() ? std::chrono::duration<float>(now - g_lastFrame).count() : 1.0f / 60.0f;
		g_lastFrame = now;
		io.DeltaTime = std::clamp(dt, 1.0f / 1000.0f, 0.5f);
		if (g_centerMouse) {
			g_centerMouse = false;
			g_mouseX = io.DisplaySize.x * 0.5f;
			g_mouseY = io.DisplaySize.y * 0.5f;
			io.AddMousePosEvent(g_mouseX, g_mouseY);
		}
		if (hud) {
			std::lock_guard lk(g_inputMtx);
			g_input.clear();
		} else {
			DrainInput();
		}
		g_studioActive = g_open && g_screen == Screen::Studio;
		if (!g_studioActive) {
			g_look = false;
		}
		if (g_open) {
			io.MouseDrawCursor = !g_look;
		}

		ImGui_ImplDX11_NewFrame();
		ImGui::NewFrame();
		if (hud) {
			io.MouseDrawCursor = false;
			Menus::DrawPlaceHud(placing);
		} else {
			bool keep = true;
			switch (g_screen) {
			case Screen::Studio:
				keep = Studio::Draw();
				break;
			case Screen::PaintingMenu:
				keep = Menus::DrawPainting();
				break;
			case Screen::EaselMenu:
				keep = Menus::DrawEasel();
				break;
			default:
				keep = false;
				break;
			}
			if (!keep) {
				g_wantOpen = false;
			}
		}
		ImGui::Render();
		ID3D11RenderTargetView* rtv = nullptr;
		g_dev->CreateRenderTargetView(back, nullptr, &rtv);
		back->Release();
		if (rtv) {
			g_ctx->OMSetRenderTargets(1, &rtv, nullptr);
			ImGui_ImplDX11_RenderDrawData(ImGui::GetDrawData());
			rtv->Release();
		}
	}
}
