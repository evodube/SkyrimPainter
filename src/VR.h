#pragma once

#include <imgui.h>

// Skyrim VR: the studio's panel shown in the headset, the controllers as the mouse, and the brush in your
// hand touching the canvas. In the SE / AE build everything here does nothing (kOn is false).
namespace VR
{
#ifdef SKYRIMPAINTER_VR
	inline constexpr bool kOn = true;
#else
	inline constexpr bool kOn = false;
#endif

	void LoadSettings();  // [VR] in SkyrimPainter.ini

	// ---- the panel: ImGui draws into its texture, the headset shows it
	enum class PanelMode
	{
		Hidden,
		Floating,  // menus: in front of you, where you were looking when it opened
		Palette,   // the studio: above the palette in your off hand
	};
	ImVec2                  PanelSize();                            // the panel in pixels (ImGui's display size)
	ID3D11RenderTargetView* PanelTarget(ID3D11Device* a_dev);       // created on first use
	void                    ShowPanel(PanelMode a_mode, ID3D11DeviceContext* a_ctx);  // after drawing into it
	void                    FeedMouse(ImGuiIO& a_io);              // the brush hand points (and the brush tip dips) at the panel
	void                    FeedKeyboard(ImGuiIO& a_io);           // the headset's keyboard, while a text box wants it

	// ---- the controllers' buttons (from the input sink)
	void OnButton(int a_device, std::uint32_t a_id, bool a_down);
	void OnStick(int a_device, float a_x, float a_y);
	bool IsVRDevice(int a_device);
	enum class Button
	{
		Undo,       // A on the brush hand
		Redo,       // B on the brush hand
		Rag,        // X on the palette hand
		HidePanel,  // Y on the palette hand
		Place,      // the brush hand's trigger, while placing an easel or a painting
		Cancel,     // B, while placing
	};
	bool  Pressed(Button a_b);  // pressed since last asked
	void  ClearPresses();       // forget presses from before (opening the studio)
	float StickX();             // the brush hand's stick, while placing: turn / size
	float StickY();             // ...and scrolling the panel

	// ---- the brush in your hand
	struct Contact
	{
		bool  hovering = false;  // the tip is close to the canvas, in front of it
		bool  touching = false;  // the tip is on (or pressed into) the canvas
		float u = 0.0f, v = 0.0f;  // where, 0..1 across and down the painted face
		float pressure = 0.0f;   // how hard: 0..1
	};
	Contact BrushOnCanvas(std::uint32_t a_canvas, bool a_portrait);
	void    HoldTools(bool a_hold);  // the brush in the brush hand, the palette in the other
	void    Buzz(float a_strength);  // the brush hand's haptics, 0..1
}
