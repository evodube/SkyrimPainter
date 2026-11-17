#include "VR.h"

#include "Live.h"
#include "Overlay.h"

// Skyrim VR. Every position here is either OpenVR's (metres, the tracking space: the panel and the pointer)
// or Skyrim's (game units, the world: the canvas and the brush tip) - never mixed.

#ifdef SKYRIMPAINTER_VR

namespace VR
{
	namespace
	{
		constexpr float kUnitsPerMetre = 70.0f;  // Skyrim: 1 unit = 1.428 cm

		struct Settings
		{
			// the panel above the palette, relative to the palette hand's controller (metres, degrees)
			float panelWidth = 0.34f;
			float panelX = 0.0f, panelY = 0.09f, panelZ = -0.06f;
			float panelTilt = -35.0f;
			// menus: how far in front of you
			float floatDistance = 0.7f;
			float floatWidth = 0.55f;
			// the brush: its tip ahead of the brush hand (Skyrim units, in the hand node's space) ...
			float tipX = 0.0f, tipY = 9.5f, tipZ = 0.0f;
			// ... and the same distance in metres along the controller, for dipping it into the panel's paints
			float tipMetres = 0.135f;
			// the brush and palette models in the hands
			float brushYaw = 90.0f;  // the brush model lies along X: turn it to point along the hand's Y
			float brushForward = 2.0f, brushDown = -0.5f;
			float paletteX = 0.0f, paletteY = 4.0f, paletteZ = 2.0f, palettePitch = -35.0f;
			bool  showModels = true;
			bool  leftHanded = false;  // the brush in the left hand
			float haptics = 1.0f;
		} g_cfg;

		// ---- the panel
		vr::VROverlayHandle_t   g_overlay = vr::k_ulOverlayHandleInvalid;
		bool                    g_overlayFailed = false;
		ID3D11Texture2D*        g_tex = nullptr;
		ID3D11RenderTargetView* g_rtv = nullptr;
		PanelMode               g_mode = PanelMode::Hidden;
		constexpr int           kPanelW = 640, kPanelH = 520;

		// ---- the controllers
		std::mutex  g_btnMtx;
		std::uint32_t g_pressed = 0;  // Button bits pressed since last asked
		bool        g_trigger = false;  // the brush hand's trigger is held
		float       g_stickX = 0.0f, g_stickY = 0.0f;

		// ---- the keyboard
		bool g_keyboardShown = false;

		vr::IVROverlay* Ov()
		{
			return RE::BSOpenVR::GetCleanIVROverlay();
		}

		vr::IVRSystem* Sys()
		{
			return RE::BSOpenVR::GetIVRSystem();
		}

		vr::TrackedDeviceIndex_t HandIndex(bool a_brushHand)
		{
			auto* sys = Sys();
			if (!sys) {
				return vr::k_unTrackedDeviceIndexInvalid;
			}
			const bool right = a_brushHand != g_cfg.leftHanded;
			return sys->GetTrackedDeviceIndexForControllerRole(right ? vr::TrackedControllerRole_RightHand : vr::TrackedControllerRole_LeftHand);
		}

		bool Pose(vr::TrackedDeviceIndex_t a_index, vr::HmdMatrix34_t& a_out)
		{
			auto* comp = RE::BSOpenVR::GetIVRCompositor();
			if (!comp || a_index >= vr::k_unMaxTrackedDeviceCount) {
				return false;
			}
			vr::TrackedDevicePose_t poses[vr::k_unMaxTrackedDeviceCount]{};
			if (comp->GetLastPoses(poses, vr::k_unMaxTrackedDeviceCount, nullptr, 0) != vr::VRCompositorError_None) {
				return false;
			}
			if (!poses[a_index].bPoseIsValid) {
				return false;
			}
			a_out = poses[a_index].mDeviceToAbsoluteTracking;
			return true;
		}

		vr::ETrackingUniverseOrigin Universe()
		{
			auto* comp = RE::BSOpenVR::GetIVRCompositor();
			return comp ? comp->GetTrackingSpace() : vr::TrackingUniverseStanding;
		}

		bool EnsureOverlay()
		{
			if (g_overlay != vr::k_ulOverlayHandleInvalid) {
				return true;
			}
			if (g_overlayFailed) {
				return false;
			}
			auto* ov = Ov();
			if (!ov) {
				g_overlayFailed = true;
				spdlog::error("VR: no IVROverlay - the panel can't be shown in the headset");
				return false;
			}
			const auto err = ov->CreateOverlay("skyrimpainter.panel", "SkyrimPainter", &g_overlay);
			if (err != vr::VROverlayError_None) {
				g_overlayFailed = true;
				g_overlay = vr::k_ulOverlayHandleInvalid;
				spdlog::error("VR: CreateOverlay failed ({})", static_cast<int>(err));
				return false;
			}
			ov->SetOverlayFlag(g_overlay, vr::VROverlayFlags_SortWithNonSceneOverlays, true);
			spdlog::info("VR: panel overlay created");
			return true;
		}

		// a rotation about X (degrees) and a translation, as an OpenVR matrix
		vr::HmdMatrix34_t Tilted(float a_x, float a_y, float a_z, float a_tiltDeg)
		{
			const float       a = a_tiltDeg * 0.0174532925f, c = std::cos(a), s = std::sin(a);
			vr::HmdMatrix34_t m{};
			m.m[0][0] = 1.0f;
			m.m[1][1] = c;
			m.m[1][2] = -s;
			m.m[2][1] = s;
			m.m[2][2] = c;
			m.m[0][3] = a_x;
			m.m[1][3] = a_y;
			m.m[2][3] = a_z;
			return m;
		}

		// in front of where the headset looks, level, facing it
		void PlaceInFront()
		{
			vr::HmdMatrix34_t hmd{};
			if (!Pose(vr::k_unTrackedDeviceIndex_Hmd, hmd)) {
				return;
			}
			float fx = -hmd.m[0][2], fz = -hmd.m[2][2];  // the headset looks along its -Z; level it
			const float len = std::sqrt(fx * fx + fz * fz);
			if (len < 1e-3f) {
				fx = 0.0f;
				fz = -1.0f;
			} else {
				fx /= len;
				fz /= len;
			}
			// the overlay faces its +Z: point that back at the viewer
			const float       zx = -fx, zz = -fz;
			const float       xx = zz, xz = -zx;  // right = up x z
			vr::HmdMatrix34_t m{};
			m.m[0][0] = xx;
			m.m[2][0] = xz;
			m.m[1][1] = 1.0f;
			m.m[0][2] = zx;
			m.m[2][2] = zz;
			m.m[0][3] = hmd.m[0][3] + fx * g_cfg.floatDistance;
			m.m[1][3] = hmd.m[1][3] - 0.12f;
			m.m[2][3] = hmd.m[2][3] + fz * g_cfg.floatDistance;
			Ov()->SetOverlayTransformAbsolute(g_overlay, Universe(), &m);
		}

		float IniF(const char* a_key, float a_def)
		{
			char buf[32]{};
			::GetPrivateProfileStringA("VR", a_key, "", buf, sizeof buf, Overlay::IniPath().c_str());
			return buf[0] ? static_cast<float>(std::atof(buf)) : a_def;
		}

		// ---- the models in the hands
		RE::NiPointer<RE::NiNode> g_brushModel, g_paletteModel;

		RE::NiNode* WandNode(bool a_brushHand)
		{
			auto* pc = RE::PlayerCharacter::GetSingleton();
			auto* nodes = pc ? pc->GetVRNodeData() : nullptr;
			if (!nodes) {
				return nullptr;
			}
			const bool right = a_brushHand != g_cfg.leftHanded;
			return (right ? nodes->RightWandNode : nodes->LeftWandNode).get();
		}

		RE::NiPointer<RE::NiNode> LoadModel(const char* a_path)
		{
			RE::NiPointer<RE::NiNode>                 model;
			const RE::BSModelDB::DBTraits::ArgsType   args{};
			if (RE::BSModelDB::Demand(a_path, model, args) != RE::BSResource::ErrorCode::kNone || !model) {
				spdlog::warn("VR: couldn't load {}", a_path);
				return {};
			}
			auto* copy = model->Clone();
			auto* node = copy ? copy->AsNode() : nullptr;
			if (!node) {
				return {};
			}
			node->collisionObject.reset();  // just a picture in the hand: nothing to bump into
			return RE::NiPointer<RE::NiNode>(node);
		}

		RE::NiMatrix3 Rot(float a_pitchDeg, float a_yawDeg)
		{
			RE::NiMatrix3 m;
			m.SetEulerAnglesXYZ(a_pitchDeg * 0.0174532925f, 0.0f, a_yawDeg * 0.0174532925f);
			return m;
		}

		void Detach(RE::NiPointer<RE::NiNode>& a_model)
		{
			if (a_model && a_model->parent) {
				a_model->parent->DetachChild2(a_model.get());
			}
			a_model.reset();
		}
	}

	void LoadSettings()
	{
		g_cfg.panelWidth = IniF("fPanelWidth", g_cfg.panelWidth);
		g_cfg.panelX = IniF("fPanelX", g_cfg.panelX);
		g_cfg.panelY = IniF("fPanelY", g_cfg.panelY);
		g_cfg.panelZ = IniF("fPanelZ", g_cfg.panelZ);
		g_cfg.panelTilt = IniF("fPanelTilt", g_cfg.panelTilt);
		g_cfg.floatDistance = IniF("fMenuDistance", g_cfg.floatDistance);
		g_cfg.floatWidth = IniF("fMenuWidth", g_cfg.floatWidth);
		g_cfg.tipX = IniF("fBrushTipX", g_cfg.tipX);
		g_cfg.tipY = IniF("fBrushTipY", g_cfg.tipY);
		g_cfg.tipZ = IniF("fBrushTipZ", g_cfg.tipZ);
		g_cfg.tipMetres = std::sqrt(g_cfg.tipX * g_cfg.tipX + g_cfg.tipY * g_cfg.tipY + g_cfg.tipZ * g_cfg.tipZ) / kUnitsPerMetre;
		g_cfg.brushYaw = IniF("fBrushYaw", g_cfg.brushYaw);
		g_cfg.brushForward = IniF("fBrushForward", g_cfg.brushForward);
		g_cfg.brushDown = IniF("fBrushDown", g_cfg.brushDown);
		g_cfg.paletteX = IniF("fPaletteX", g_cfg.paletteX);
		g_cfg.paletteY = IniF("fPaletteY", g_cfg.paletteY);
		g_cfg.paletteZ = IniF("fPaletteZ", g_cfg.paletteZ);
		g_cfg.palettePitch = IniF("fPalettePitch", g_cfg.palettePitch);
		g_cfg.showModels = IniF("bShowModels", 1.0f) != 0.0f;
		g_cfg.leftHanded = IniF("bLeftHanded", 0.0f) != 0.0f;
		g_cfg.haptics = std::clamp(IniF("fHaptics", g_cfg.haptics), 0.0f, 2.0f);
		spdlog::info("VR: settings - panel {:.2f} m, brush tip ({:.1f}, {:.1f}, {:.1f}), {}", g_cfg.panelWidth, g_cfg.tipX, g_cfg.tipY, g_cfg.tipZ,
			g_cfg.leftHanded ? "left-handed" : "right-handed");
	}

	// ------------------------------------------------------------------ the panel
	ImVec2 PanelSize() { return ImVec2(static_cast<float>(kPanelW), static_cast<float>(kPanelH)); }

	ID3D11RenderTargetView* PanelTarget(ID3D11Device* a_dev)
	{
		if (g_rtv || !a_dev) {
			return g_rtv;
		}
		D3D11_TEXTURE2D_DESC d{};
		d.Width = kPanelW;
		d.Height = kPanelH;
		d.MipLevels = 1;
		d.ArraySize = 1;
		d.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
		d.SampleDesc.Count = 1;
		d.Usage = D3D11_USAGE_DEFAULT;
		d.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
		d.MiscFlags = D3D11_RESOURCE_MISC_SHARED;  // the VR compositor opens it from its own process
		if (FAILED(a_dev->CreateTexture2D(&d, nullptr, &g_tex)) || !g_tex) {
			spdlog::error("VR: couldn't create the panel texture");
			return nullptr;
		}
		a_dev->CreateRenderTargetView(g_tex, nullptr, &g_rtv);
		return g_rtv;
	}

	void ShowPanel(PanelMode a_mode, ID3D11DeviceContext* a_ctx)
	{
		if (a_mode == PanelMode::Hidden) {
			if (g_mode != PanelMode::Hidden && g_overlay != vr::k_ulOverlayHandleInvalid) {
				Ov()->HideOverlay(g_overlay);
				if (g_keyboardShown) {
					Ov()->HideKeyboard();
					g_keyboardShown = false;
				}
			}
			g_mode = a_mode;
			return;
		}
		if (!g_tex || !EnsureOverlay()) {
			return;
		}
		auto* ov = Ov();
		if (a_ctx) {
			a_ctx->Flush();  // the compositor reads the texture from another process
		}
		vr::Texture_t tex{ g_tex, vr::TextureType_DirectX, vr::ColorSpace_Auto };
		ov->SetOverlayTexture(g_overlay, &tex);
		if (a_mode == PanelMode::Palette) {
			const auto hand = HandIndex(false);
			if (hand != vr::k_unTrackedDeviceIndexInvalid) {
				const auto m = Tilted(g_cfg.panelX * (g_cfg.leftHanded ? -1.0f : 1.0f), g_cfg.panelY, g_cfg.panelZ, g_cfg.panelTilt);
				ov->SetOverlayTransformTrackedDeviceRelative(g_overlay, hand, &m);
			}
			if (g_mode != PanelMode::Palette) {
				ov->SetOverlayWidthInMeters(g_overlay, g_cfg.panelWidth);
			}
		} else if (g_mode != PanelMode::Floating) {
			ov->SetOverlayWidthInMeters(g_overlay, g_cfg.floatWidth);
			PlaceInFront();
		}
		if (g_mode == PanelMode::Hidden) {
			ov->ShowOverlay(g_overlay);
		}
		g_mode = a_mode;
	}

	void FeedMouse(ImGuiIO& a_io)
	{
		bool  hit = false;
		bool  dipped = false;
		float x = -FLT_MAX, y = -FLT_MAX;
		vr::HmdMatrix34_t pose{};
		if (g_overlay != vr::k_ulOverlayHandleInvalid && g_mode != PanelMode::Hidden && Pose(HandIndex(true), pose)) {
			vr::VROverlayIntersectionParams_t params{};
			params.vSource = { pose.m[0][3], pose.m[1][3], pose.m[2][3] };
			params.vDirection = { -pose.m[0][2], -pose.m[1][2], -pose.m[2][2] };  // the controller points along its -Z
			params.eOrigin = Universe();
			vr::VROverlayIntersectionResults_t res{};
			if (Ov()->ComputeOverlayIntersection(g_overlay, &params, &res)) {
				hit = true;
				x = res.vUVs.v[0] * kPanelW;
				y = (1.0f - res.vUVs.v[1]) * kPanelH;
				// the brush's tip has reached the panel: dip it (a click where it touches)
				dipped = res.fDistance <= g_cfg.tipMetres + 0.01f;
			}
		}
		a_io.AddMousePosEvent(x, y);
		a_io.MouseDrawCursor = hit;
		static bool wasDown = false;
		const bool  down = hit && (g_trigger || dipped);
		if (down != wasDown) {
			a_io.AddMouseButtonEvent(ImGuiMouseButton_Left, down);
			wasDown = down;
			if (down && dipped) {
				Buzz(0.4f);
			}
		}
		if (hit && std::fabs(g_stickY) > 0.25f) {
			a_io.AddMouseWheelEvent(0.0f, g_stickY * 0.25f);  // the stick scrolls the panel
		}
	}

	void FeedKeyboard(ImGuiIO& a_io)
	{
		if (g_overlay == vr::k_ulOverlayHandleInvalid || g_mode == PanelMode::Hidden) {
			return;
		}
		auto* ov = Ov();
		if (a_io.WantTextInput && !g_keyboardShown) {
			g_keyboardShown = ov->ShowKeyboardForOverlay(g_overlay, vr::k_EGamepadTextInputModeNormal, vr::k_EGamepadTextInputLineModeSingleLine, "SkyrimPainter", 64, "",
								  false, 0) == vr::VROverlayError_None;
		} else if (!a_io.WantTextInput && g_keyboardShown) {
			ov->HideKeyboard();
			g_keyboardShown = false;
		}
		vr::VREvent_t ev{};
		while (ov->PollNextOverlayEvent(g_overlay, &ev, sizeof ev)) {
			if (ev.eventType == vr::VREvent_KeyboardDone) {
				char text[256]{};
				ov->GetKeyboardText(text, sizeof text);
				// replace what's in the box with what was typed
				a_io.AddKeyEvent(ImGuiMod_Ctrl, true);
				a_io.AddKeyEvent(ImGuiKey_A, true);
				a_io.AddKeyEvent(ImGuiKey_A, false);
				a_io.AddKeyEvent(ImGuiMod_Ctrl, false);
				a_io.AddKeyEvent(ImGuiKey_Backspace, true);
				a_io.AddKeyEvent(ImGuiKey_Backspace, false);
				a_io.AddInputCharactersUTF8(text);
				g_keyboardShown = false;
			} else if (ev.eventType == vr::VREvent_KeyboardClosed) {
				g_keyboardShown = false;
			}
		}
	}

	// ------------------------------------------------------------------ the buttons
	bool IsVRDevice(int a_device)
	{
		return a_device >= static_cast<int>(RE::INPUT_DEVICE::kVivePrimary) && a_device <= static_cast<int>(RE::INPUT_DEVICE::kWMRSecondary);
	}

	namespace
	{
		// Vive / Oculus / WMR: the primary device is the main (brush) hand
		bool BrushHand(int a_device)
		{
			using D = RE::INPUT_DEVICE;
			return a_device == static_cast<int>(D::kVivePrimary) || a_device == static_cast<int>(D::kOculusPrimary) || a_device == static_cast<int>(D::kWMRPrimary);
		}

		void Press(Button a_b)
		{
			g_pressed |= 1u << static_cast<int>(a_b);
		}
	}

	void OnButton(int a_device, std::uint32_t a_id, bool a_down)
	{
		using K = RE::BSOpenVRControllerDevice::Keys;
		std::lock_guard lk(g_btnMtx);
		const bool brush = BrushHand(a_device);
		if (brush && a_id == K::kTrigger) {
			g_trigger = a_down;
			if (a_down) {
				Press(Button::Place);
			}
		}
		if (!a_down) {
			return;
		}
		if (brush && a_id == K::kXA) {
			Press(Button::Undo);
		} else if (brush && a_id == K::kBY) {
			Press(Button::Redo);
			Press(Button::Cancel);
		} else if (!brush && a_id == K::kXA) {
			Press(Button::Rag);
		} else if (!brush && a_id == K::kBY) {
			Press(Button::HidePanel);
		}
	}

	void OnStick(int a_device, float a_x, float a_y)
	{
		if (BrushHand(a_device)) {
			g_stickX = a_x;
			g_stickY = a_y;
		}
	}

	bool Pressed(Button a_b)
	{
		std::lock_guard lk(g_btnMtx);
		const std::uint32_t bit = 1u << static_cast<int>(a_b);
		const bool          was = (g_pressed & bit) != 0;
		g_pressed &= ~bit;
		return was;
	}

	float StickX() { return g_stickX; }
	float StickY() { return g_stickY; }

	// ------------------------------------------------------------------ the brush
	Contact BrushOnCanvas(std::uint32_t a_canvas, bool a_portrait)
	{
		Contact      out;
		RE::NiPoint3 c[4];
		auto*        wand = WandNode(true);
		if (!wand || !Live::CanvasCorners(a_canvas, a_portrait, c)) {
			return out;
		}
		const RE::NiTransform& w = wand->world;
		const RE::NiPoint3     tip = w.translate + w.rotate * (RE::NiPoint3(g_cfg.tipX, g_cfg.tipY, g_cfg.tipZ) * w.scale);

		const RE::NiPoint3 e1 = c[1] - c[0], e2 = c[3] - c[0];
		RE::NiPoint3       n = e1.Cross(e2);
		const float        nl = n.Length();
		if (nl < 1e-4f) {
			return out;
		}
		n = n / nl;
		// the painted side faces the painter: the side the headset is on
		if (auto* cam = RE::PlayerCamera::GetSingleton(); cam && cam->cameraRoot) {
			if ((cam->cameraRoot->world.translate - c[0]).Dot(n) < 0.0f) {
				n = -n;
			}
		}
		const float        d = (tip - c[0]).Dot(n);  // in front of the paint: > 0
		const RE::NiPoint3 p = tip - n * d - c[0];
		out.u = p.Dot(e1) / e1.SqrLength();
		out.v = p.Dot(e2) / e2.SqrLength();
		const bool on = out.u >= -0.02f && out.u <= 1.02f && out.v >= -0.02f && out.v <= 1.02f;  // (the face isn't quite square)
		constexpr float kContact = 0.5f;   // within ~7 mm of the surface counts as touching
		constexpr float kSquash = 2.5f;    // pressed ~3.5 cm in: full pressure
		constexpr float kThrough = 12.0f;  // further than this, the hand has gone through the canvas
		out.hovering = on && d < 6.0f && d > -kThrough;
		out.touching = on && d < kContact && d > -kThrough;
		out.pressure = std::clamp((kContact - d) / kSquash, 0.08f, 1.0f);
		return out;
	}

	void HoldTools(bool a_hold)
	{
		SKSE::GetTaskInterface()->AddTask([a_hold] {
			if (!a_hold || !g_cfg.showModels) {
				Detach(g_brushModel);
				Detach(g_paletteModel);
				return;
			}
			if (!g_brushModel) {
				if (auto* hand = WandNode(true); hand && (g_brushModel = LoadModel("SkyrimPainter\\Item_Paint.nif"))) {
					g_brushModel->local.rotate = Rot(0.0f, g_cfg.brushYaw * (g_cfg.leftHanded ? -1.0f : 1.0f));
					g_brushModel->local.translate = RE::NiPoint3(0.0f, g_cfg.brushForward, g_cfg.brushDown);
					hand->AttachChild(g_brushModel.get(), true);
					RE::NiUpdateData ud{};
					g_brushModel->Update(ud);
				}
			}
			if (!g_paletteModel) {
				if (auto* hand = WandNode(false); hand && (g_paletteModel = LoadModel("SkyrimPainter\\Item_Palette.nif"))) {
					g_paletteModel->local.rotate = Rot(g_cfg.palettePitch, 0.0f);
					g_paletteModel->local.translate = RE::NiPoint3(g_cfg.paletteX * (g_cfg.leftHanded ? -1.0f : 1.0f), g_cfg.paletteY, g_cfg.paletteZ);
					hand->AttachChild(g_paletteModel.get(), true);
					RE::NiUpdateData ud{};
					g_paletteModel->Update(ud);
				}
			}
		});
	}

	void Buzz(float a_strength)
	{
		if (a_strength <= 0.0f || g_cfg.haptics <= 0.0f) {
			return;
		}
		if (auto* vr = RE::BSOpenVR::GetSingleton()) {
			// (units of 4 ms)
			vr->TriggerHapticPulse(!g_cfg.leftHanded, std::clamp(a_strength * g_cfg.haptics, 0.0f, 2.0f));
		}
	}
}

#else  // the SE / AE build: nothing in VR

namespace VR
{
	void                    LoadSettings() {}
	ImVec2                  PanelSize() { return ImVec2(0, 0); }
	ID3D11RenderTargetView* PanelTarget(ID3D11Device*) { return nullptr; }
	void                    ShowPanel(PanelMode, ID3D11DeviceContext*) {}
	void                    FeedMouse(ImGuiIO&) {}
	void                    FeedKeyboard(ImGuiIO&) {}
	void                    OnButton(int, std::uint32_t, bool) {}
	void                    OnStick(int, float, float) {}
	bool                    IsVRDevice(int) { return false; }
	bool                    Pressed(Button) { return false; }
	float                   StickX() { return 0.0f; }
	float                   StickY() { return 0.0f; }
	Contact                 BrushOnCanvas(std::uint32_t, bool) { return {}; }
	void                    HoldTools(bool) {}
	void                    Buzz(float) {}
}

#endif
