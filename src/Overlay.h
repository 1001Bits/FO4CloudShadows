#pragma once

#include <d3d11.h>
#include <dxgi.h>

// F11-toggled Open Shaders FO4 menu for the standalone Cloud Shadows
// plugin. Closing with F11 or X saves settings automatically. Release controls
// match the optional MCM page. Developer builds alone
// expose capture previews and diagnostics. Draw() is called from the Present hook, just
// before the swap-chain flip, so ImGui composites on the finished frame.
namespace Overlay
{
	// A compatible host claims menu ownership during F4SE plugin loading.  The
	// claim lasts for the process lifetime; it is not the current F11 visibility.
	// A late claim is consumed by Draw() on the render thread so backend teardown
	// never happens from the loader thread.
	void SetExternalHostActive(bool active) noexcept;
	bool IsExternalHostActive() noexcept;

	// Called from the Present thunk (render thread) with the live swap chain.
	// Lazily initialises the ImGui DX11/Win32 backends on first call; a no-op
	// (beyond the foreground-only F11 edge check) while the overlay is hidden.
	// The standalone backend owns cursor confinement only while its window is
	// visible and Fallout is foreground; the game's prior clip is restored on
	// every normal close/teardown transition.
	void Draw(IDXGISwapChain* swapChain);

	// Tears down all ImGui/D3D objects tied to the current renderer device so a
	// recreated device can initialize a fresh backend on the next Present.
	void Shutdown() noexcept;

	// Current visibility (F11 toggles it inside Draw()).
	bool IsVisible();
}
