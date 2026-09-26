#include "PCH.h"
#include "AcceptanceRunner.h"
#include "Overlay.h"
#include "McmSettings.h"
#include "EngineAPI.h"
#include "CloudComparison.h"
#include "CloudCubePreview.h"
#include "CloudShadows.h"
#include "GodraysIntegration.h"
#include "RuntimeAPI.h"
#include "Utf8Path.h"

#include <imgui.h>
#include <imgui_impl_dx11.h>
#include <imgui_impl_win32.h>

#include <array>
#include <cfloat>
#include <mutex>
#include <TlHelp32.h>
#include <vector>

// imgui_impl_win32.h intentionally keeps this declaration in a #if 0 block so
// including it does not pull Windows types into every consumer.
extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(
	HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam);

namespace Overlay
{
	namespace
	{
		[[nodiscard]] const char* RuntimeLabel() noexcept
		{
			using FO4CS::F4SECompat::RuntimeTarget;
			switch (FO4CS::RuntimeAPI::GetSingleton().Target()) {
			case RuntimeTarget::kLegacy:
				return "Fallout 4 OG 1.10.163";
			case RuntimeTarget::kAE:
				return "Fallout 4 AE 1.11.240";
			case RuntimeTarget::kVR:
				return "Fallout 4 VR 1.2.72";
			default:
				return "Unsupported Fallout 4 runtime";
			}
		}

		std::atomic<bool> g_externalHostActive{ false };
		std::atomic<bool> g_inited{ false };
		bool g_failed = false;
		// A transient window-hook refusal retries after this tick instead of
		// disabling the standalone menu for the rest of the session.
		ULONGLONG g_initRetryAfter = 0;
		std::atomic<bool> g_visible{ false };
		// Set when the menu is hidden off the render thread (window close) or
		// by a late host claim; the next render-thread Draw/Shutdown saves.
		std::atomic<bool> g_pendingSave{ false };
#if FO4CS_ENABLE_DEVELOPER_TOOLS
		bool g_showCapturePreview = false;
#endif
		bool g_win32BackendInited = false;
		bool g_dx11BackendInited = false;
		bool g_wndProcInstalled = false;
		bool g_windowRetired = false;
		HWND g_retiredHwnd = nullptr;
		std::atomic<bool> g_menuOwnsMouseCapture{ false };
		std::atomic<HWND> g_hwnd{ nullptr };
		std::atomic<WNDPROC> g_originalWndProc{ nullptr };
		// Protected by g_imguiLifetimeMutex. This is deliberately distinct from
		// g_hwnd so a destroyed/recreated swap-chain window cannot inherit an old
		// process-global "installed" bit.
		HWND g_wndProcHwnd = nullptr;
		ID3D11Device* g_dev = nullptr;
		ID3D11DeviceContext* g_ctx = nullptr;
		FO4CS::CloudCubePreview g_cubePreview;
		ImFont* g_titleFont = nullptr;
		ImFont* g_subtextFont = nullptr;

		// Serializes the complete ImGui frame/backend lifetime against the game's
		// window thread. It is recursive because Win32 calls made while drawing or
		// releasing capture can synchronously re-enter MenuWndProc on the same
		// thread.
		std::recursive_mutex g_imguiLifetimeMutex;
		bool IsFalloutForeground() noexcept
		{
			const HWND hwnd = g_hwnd.load(std::memory_order_acquire);
			if (!hwnd || !IsWindow(hwnd))
				return false;
			const HWND foreground = GetForegroundWindow();
			return foreground == hwnd ||
				(foreground && GetAncestor(foreground, GA_ROOT) == hwnd);
		}

		void ReleaseOwnedMouseCapture() noexcept
		{
			if (!g_menuOwnsMouseCapture.exchange(false, std::memory_order_acq_rel))
				return;
			const HWND hwnd = g_hwnd.load(std::memory_order_acquire);
			if (!hwnd)
				return;
			if (GetCapture() == hwnd) {
				ReleaseCapture();
			} else if (IsWindow(hwnd)) {
				// GetCapture is thread-local. Shutdown can run on a render thread
				// while the ImGui capture belongs to the window thread; cancel it
				// asynchronously rather than blocking cross-thread teardown.
				PostMessageW(hwnd, WM_CANCELMODE, 0, 0);
			}
		}

		// ImGui's stock DX11 shader displays an R8 SRV as (R, 0, 0, 1), which is
		// why the old previews were red. Dynamic Reflections avoids that by
		// presenting an RGBA debug target. These are UI-only RGBA mirrors of the
		// two single-channel runtime textures; the actual half/full-float shadow
		// resource is not changed by the menu.
#if FO4CS_ENABLE_DEVELOPER_TOOLS
		struct GrayPreview
		{
			ComPtr<ID3D11Texture2D> texture;
			ComPtr<ID3D11ShaderResourceView> srv;
			ComPtr<ID3D11UnorderedAccessView> uav;
			uint32_t width = 0;
			uint32_t height = 0;
		};

		ComPtr<ID3D11ComputeShader> g_grayPreviewCS;
		GrayPreview g_screenMaskPreview;
#endif

		class ScopedBackendShaderState
		{
		public:
			explicit ScopedBackendShaderState(ID3D11DeviceContext* context) :
				context_(context)
			{
				CaptureShader(&ID3D11DeviceContext::HSGetShader, hs_, hsClasses_, hsClassCount_);
				CaptureShader(&ID3D11DeviceContext::DSGetShader, ds_, dsClasses_, dsClassCount_);
				CaptureShader(&ID3D11DeviceContext::CSGetShader, cs_, csClasses_, csClassCount_);

				if (SUCCEEDED(context_->QueryInterface(
						__uuidof(ID3D11DeviceContext1),
						reinterpret_cast<void**>(context1_.GetAddressOf()))) &&
					context1_) {
					context1_->VSGetConstantBuffers1(
						0, 1, vsConstantBuffer_.GetAddressOf(),
						&vsFirstConstant_, &vsConstantCount_);
					hasConstantBufferRange_ = true;
				} else {
					context_->VSGetConstantBuffers(
						0, 1, vsConstantBuffer_.GetAddressOf());
				}
			}

			~ScopedBackendShaderState()
			{
				RestoreShader(&ID3D11DeviceContext::HSSetShader, hs_, hsClasses_, hsClassCount_);
				RestoreShader(&ID3D11DeviceContext::DSSetShader, ds_, dsClasses_, dsClassCount_);
				RestoreShader(&ID3D11DeviceContext::CSSetShader, cs_, csClasses_, csClassCount_);
				ID3D11Buffer* buffer = vsConstantBuffer_.Get();
				if (hasConstantBufferRange_ && context1_) {
					context1_->VSSetConstantBuffers1(
						0, 1, &buffer, &vsFirstConstant_, &vsConstantCount_);
				} else {
					context_->VSSetConstantBuffers(0, 1, &buffer);
				}
			}

			ScopedBackendShaderState(const ScopedBackendShaderState&) = delete;
			ScopedBackendShaderState& operator=(const ScopedBackendShaderState&) = delete;

		private:
			static constexpr UINT kMaximumClasses = D3D11_SHADER_MAX_INTERFACES;
			using Classes = std::array<ComPtr<ID3D11ClassInstance>, kMaximumClasses>;

			template <class Shader>
			using GetShaderMethod = void (STDMETHODCALLTYPE ID3D11DeviceContext::*)(
				Shader**, ID3D11ClassInstance**, UINT*);

			template <class Shader>
			using SetShaderMethod = void (STDMETHODCALLTYPE ID3D11DeviceContext::*)(
				Shader*, ID3D11ClassInstance* const*, UINT);

			template <class Shader>
			void CaptureShader(
				GetShaderMethod<Shader> getter,
				ComPtr<Shader>& shader,
				Classes& classes,
				UINT& classCount)
			{
				std::array<ID3D11ClassInstance*, kMaximumClasses> raw{};
				classCount = kMaximumClasses;
				(context_->*getter)(shader.GetAddressOf(), raw.data(), &classCount);
				classCount = (std::min)(classCount, kMaximumClasses);
				for (UINT i = 0; i < classCount; ++i)
					classes[i].Attach(raw[i]);
			}

			template <class Shader>
			void RestoreShader(
				SetShaderMethod<Shader> setter,
				const ComPtr<Shader>& shader,
				const Classes& classes,
				UINT classCount)
			{
				std::array<ID3D11ClassInstance*, kMaximumClasses> raw{};
				for (UINT i = 0; i < classCount; ++i)
					raw[i] = classes[i].Get();
				(context_->*setter)(shader.Get(), raw.data(), classCount);
			}

			ID3D11DeviceContext* context_;
			ComPtr<ID3D11HullShader> hs_;
			ComPtr<ID3D11DomainShader> ds_;
			ComPtr<ID3D11ComputeShader> cs_;
			Classes hsClasses_;
			Classes dsClasses_;
			Classes csClasses_;
			UINT hsClassCount_{ 0 };
			UINT dsClassCount_{ 0 };
			UINT csClassCount_{ 0 };
			ComPtr<ID3D11Buffer> vsConstantBuffer_;
			ComPtr<ID3D11DeviceContext1> context1_;
			UINT vsFirstConstant_{ 0 };
			UINT vsConstantCount_{ 0 };
			bool hasConstantBufferRange_{ false };
		};

		bool IsMouseMessage(UINT msg)
		{
			switch (msg) {
			case WM_MOUSEMOVE:
			case WM_LBUTTONDOWN:
			case WM_LBUTTONDBLCLK:
			case WM_LBUTTONUP:
			case WM_RBUTTONDOWN:
			case WM_RBUTTONDBLCLK:
			case WM_RBUTTONUP:
			case WM_MBUTTONDOWN:
			case WM_MBUTTONDBLCLK:
			case WM_MBUTTONUP:
			case WM_XBUTTONDOWN:
			case WM_XBUTTONDBLCLK:
			case WM_XBUTTONUP:
			case WM_MOUSEWHEEL:
			case WM_MOUSEHWHEEL:
				return true;
			default:
				return false;
			}
		}

		bool IsRawMouseInput(LPARAM lParam) noexcept
		{
			RAWINPUTHEADER header{};
			UINT size = sizeof(header);
			return GetRawInputData(reinterpret_cast<HRAWINPUT>(lParam), RID_HEADER,
				       &header, &size, sizeof(RAWINPUTHEADER)) == sizeof(header) &&
				header.dwType == RIM_TYPEMOUSE;
		}

		bool IsMouseButtonDownMessage(UINT msg) noexcept
		{
			switch (msg) {
			case WM_LBUTTONDOWN:
			case WM_LBUTTONDBLCLK:
			case WM_RBUTTONDOWN:
			case WM_RBUTTONDBLCLK:
			case WM_MBUTTONDOWN:
			case WM_MBUTTONDBLCLK:
			case WM_XBUTTONDOWN:
			case WM_XBUTTONDBLCLK:
				return true;
			default:
				return false;
			}
		}

		bool IsKeyboardMessage(UINT msg)
		{
			switch (msg) {
			case WM_KEYDOWN:
			case WM_KEYUP:
			case WM_SYSKEYDOWN:
			case WM_SYSKEYUP:
			case WM_CHAR:
				return true;
			default:
				return false;
			}
		}

		LRESULT CALLBACK MenuWndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam)
		{
			// Capture the forwarding chain before any cursor or ImGui lock can block.
			// A device/window recreation may install a different chain while this old
			// window callback is waiting.
			const WNDPROC originalWndProc =
				g_originalWndProc.load(std::memory_order_acquire);
			const bool visibleAtEntry =
				g_visible.load(std::memory_order_acquire);
			// ImGui has no input ownership while the standalone menu is hidden.
			// Forward ordinary game input immediately so the window thread cannot
			// block behind render-thread ImGui work. WM_NCDESTROY is the sole hidden
			// exception because it must retire our subclass state under the lifetime
			// mutex before Windows discards the HWND.
			if (!visibleAtEntry && msg != WM_NCDESTROY) {
				return originalWndProc
					? CallWindowProcW(originalWndProc, hwnd, msg, wParam, lParam)
					: DefWindowProcW(hwnd, msg, wParam, lParam);
			}
			const bool focusLost = visibleAtEntry &&
				(msg == WM_KILLFOCUS ||
				 (msg == WM_ACTIVATEAPP && wParam == FALSE));
			const bool focusGained = visibleAtEntry &&
				(msg == WM_SETFOCUS ||
				 (msg == WM_ACTIVATEAPP && wParam != FALSE));
			const bool windowClosing = visibleAtEntry &&
				(msg == WM_CLOSE || msg == WM_DESTROY || msg == WM_NCDESTROY);
			(void)focusGained;
			if (windowClosing) {
				ReleaseOwnedMouseCapture();
				FO4CS::EngineAPI::ShowGameCursorMenu(false);
				g_visible.store(false, std::memory_order_release);
				// Saving is render-thread work; the next Draw or Shutdown
				// persists the edits made before the window closed.
				g_pendingSave.store(true, std::memory_order_release);
			}
			if (focusLost)
				ReleaseOwnedMouseCapture();

			bool consume = false;
			{
				std::scoped_lock imguiLock(g_imguiLifetimeMutex);
				if (visibleAtEntry &&
					g_inited.load(std::memory_order_acquire) &&
					ImGui::GetCurrentContext()) {
					if (windowClosing)
						ImGui::GetIO().MouseDrawCursor = false;

					const HWND captureBefore = GetCapture();
					const bool handled =
						ImGui_ImplWin32_WndProcHandler(hwnd, msg, wParam, lParam) != 0;
					if (visibleAtEntry && IsMouseButtonDownMessage(msg) &&
						captureBefore == nullptr && GetCapture() == hwnd) {
						g_menuOwnsMouseCapture.store(true, std::memory_order_release);
					}
					if (g_menuOwnsMouseCapture.load(std::memory_order_acquire) &&
						GetCapture() != hwnd) {
						g_menuOwnsMouseCapture.store(false, std::memory_order_release);
					}
					if (visibleAtEntry && !windowClosing) {
						const ImGuiIO& io = ImGui::GetIO();
						if (handled || IsMouseMessage(msg) ||
							(msg == WM_INPUT && IsRawMouseInput(lParam)) ||
							(io.WantCaptureKeyboard && IsKeyboardMessage(msg))) {
							consume = true;
						}
					}
				}
				if (msg == WM_NCDESTROY && g_wndProcInstalled &&
					g_wndProcHwnd == hwnd) {
					// Windows removes the procedure chain with the destroyed HWND. Mark
					// this exact ownership dead even when the menu was hidden so a new
					// swap-chain OutputWindow can install its own hook.
					g_wndProcInstalled = false;
					g_wndProcHwnd = nullptr;
					g_windowRetired = true;
					g_retiredHwnd = hwnd;
					HWND expected = hwnd;
					(void)g_hwnd.compare_exchange_strong(
						expected, nullptr, std::memory_order_acq_rel);
				}
			}

			// Consumed raw input is withheld from the game but still passed to
			// DefWindowProc, which releases the system's raw-input buffer.
			if (consume && msg == WM_INPUT)
				return DefWindowProcW(hwnd, msg, wParam, lParam);
			const LRESULT result = consume ? 1 :
				(originalWndProc
					? CallWindowProcW(originalWndProc, hwnd, msg, wParam, lParam)
					: DefWindowProcW(hwnd, msg, wParam, lParam));

			return result;
		}

		bool InstallWndProcHook()
		{
			const HWND hwnd = g_hwnd.load(std::memory_order_acquire);
			if (!hwnd)
				return false;
			if (g_wndProcInstalled) {
				if (g_wndProcHwnd == hwnd)
					return true;
				if (g_wndProcHwnd && IsWindow(g_wndProcHwnd)) {
					SPDLOG_ERROR(
						"[CloudShadows][Menu] refusing to hook a new HWND while the "
						"previous window hook is still live");
					return false;
				}
				g_wndProcInstalled = false;
				g_wndProcHwnd = nullptr;
			}

			// Publish the observed chain before installing our callback so even a
			// callback scheduled immediately by another process thread can forward.
			SetLastError(ERROR_SUCCESS);
			const auto observed = GetWindowLongPtrW(hwnd, GWLP_WNDPROC);
			if (!observed && GetLastError() != ERROR_SUCCESS) {
				SPDLOG_ERROR(
					"[CloudShadows][Menu] failed to inspect Win32 input hook (error={})",
					GetLastError());
				return false;
			}
			if (reinterpret_cast<WNDPROC>(observed) == &MenuWndProc) {
				// A destruction/recreation transition can report the existing hook
				// before Windows finishes retiring the old HWND. Do not install our
				// procedure on top of itself.
				g_wndProcHwnd = hwnd;
				g_wndProcInstalled = true;
				return true;
			}
			g_originalWndProc.store(
				reinterpret_cast<WNDPROC>(observed), std::memory_order_release);

			SetLastError(ERROR_SUCCESS);
			auto previous = SetWindowLongPtrW(
				hwnd, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(&MenuWndProc));
			if (!previous && GetLastError() != ERROR_SUCCESS) {
				SPDLOG_ERROR("[CloudShadows][Menu] failed to install Win32 input hook (error={})", GetLastError());
				return false;
			}
			g_originalWndProc.store(
				reinterpret_cast<WNDPROC>(previous), std::memory_order_release);
			g_wndProcHwnd = hwnd;
			g_wndProcInstalled = true;
			return true;
		}

		void SetVisible(bool visible)
		{
			const bool wasVisible = g_visible.load(std::memory_order_acquire);
			{
				std::scoped_lock imguiLock(g_imguiLifetimeMutex);
				if (visible) {
					g_visible.store(true, std::memory_order_release);
					if (g_inited.load(std::memory_order_acquire) &&
						ImGui::GetCurrentContext()) {
						ImGui::GetIO().MouseDrawCursor = true;
					}
				} else if (g_inited.load(std::memory_order_acquire) &&
					ImGui::GetCurrentContext()) {
					ImGui::GetIO().MouseDrawCursor = false;
				}
			}

			if (visible && !wasVisible) {
				// Vanilla mouse behaviour: no ClipCursor/SetCursorPos hooks. Ask the
				// engine for its own cursor menu, as the Pip-Boy and dialogue do, so
				// the game itself shows the pointer, stops mouse-look recentring and
				// keeps its native window confinement while the menu is open.
				if (!FO4CS::EngineAPI::ShowGameCursorMenu(true)) {
					SPDLOG_WARN("[CloudShadows][Menu] engine cursor menu unavailable on this "
						"runtime; the pointer keeps vanilla mouse-look behaviour while the menu is open");
				}
			} else if (!visible && wasVisible) {
				FO4CS::EngineAPI::ShowGameCursorMenu(false);
				ReleaseOwnedMouseCapture();
				g_visible.store(false, std::memory_order_release);
				// F11 and the X button both close here on the render thread.
				// Persist once per close, after releasing the game's input.
				try {
					CloudShadows::SaveSettings();
				} catch (const std::exception& error) {
					SPDLOG_ERROR("[CloudShadows][Menu] Could not save settings on close: {}", error.what());
				}
			}
		}

		void ApplyStyle()
		{
			// Match Dynamic Reflections' shipped Default Dark theme. The standalone
			// plugin uses ImGui 1.90 without docking, so the two docking-only palette
			// entries from that theme are intentionally omitted below.
			ImGui::StyleColorsDark();
			auto& style = ImGui::GetStyle();
			style.WindowBorderSize = 2.0f;
			style.ChildBorderSize = 0.0f;
			style.FrameBorderSize = 0.0f;
			style.WindowPadding = ImVec2(8.0f, 8.0f);
			style.WindowRounding = 12.0f;
			style.IndentSpacing = 8.0f;
			style.FramePadding = ImVec2(4.0f, 4.0f);
			style.CellPadding = ImVec2(8.0f, 2.0f);
			style.ItemSpacing = ImVec2(4.0f, 8.0f);
			style.ItemInnerSpacing = ImVec2(2.0f, 4.0f);
			style.FrameRounding = 6.0f;
			style.TabRounding = 4.0f;
			style.ScrollbarRounding = 12.0f;
			style.ScrollbarSize = 10.0f;
			style.GrabRounding = 12.0f;
			style.GrabMinSize = 8.0f;
			style.ButtonTextAlign = ImVec2(0.5f, 0.5f);
			style.SelectableTextAlign = ImVec2(0.0f, 0.0f);
			style.SeparatorTextAlign = ImVec2(0.32f, 0.5f);
			style.SeparatorTextPadding = ImVec2(20.0f, 3.0f);
			style.SeparatorTextBorderSize = 3.0f;

			auto& colors = style.Colors;
			colors[ImGuiCol_Text] = ImVec4(1.0f, 1.0f, 1.0f, 1.0f);
			colors[ImGuiCol_TextDisabled] = ImVec4(1.0f, 1.0f, 1.0f, 0.3f);
			colors[ImGuiCol_WindowBg] = ImVec4(0.03f, 0.03f, 0.03f, 0.39216f);
			colors[ImGuiCol_ChildBg] = ImVec4(0.0f, 0.0f, 0.0f, 0.0f);
			colors[ImGuiCol_PopupBg] = ImVec4(0.05f, 0.05f, 0.10f, 0.85f);
			colors[ImGuiCol_Border] = ImVec4(0.5f, 0.5f, 0.5f, 0.8f);
			colors[ImGuiCol_BorderShadow] = ImVec4(0.0f, 0.0f, 0.0f, 0.0f);
			colors[ImGuiCol_FrameBg] = ImVec4(0.4f, 0.4f, 0.4f, 0.7f);
			colors[ImGuiCol_FrameBgHovered] = ImVec4(0.26f, 0.26f, 0.26f, 0.4f);
			colors[ImGuiCol_FrameBgActive] = ImVec4(0.4f, 0.4f, 0.4f, 0.45f);
			colors[ImGuiCol_TitleBg] = ImVec4(0.0f, 0.0f, 0.0f, 0.83f);
			colors[ImGuiCol_TitleBgActive] = ImVec4(0.0f, 0.0f, 0.0f, 0.87f);
			colors[ImGuiCol_TitleBgCollapsed] = ImVec4(0.2f, 0.2f, 0.3f, 0.9f);
			colors[ImGuiCol_MenuBarBg] = ImVec4(0.02f, 0.02f, 0.03f, 0.9f);
			colors[ImGuiCol_ScrollbarBg] = ImVec4(0.2f, 0.22f, 0.27f, 0.0f);
			colors[ImGuiCol_ScrollbarGrab] = ImVec4(0.28f, 0.28f, 0.28f, 0.3f);
			colors[ImGuiCol_ScrollbarGrabHovered] = ImVec4(0.42f, 0.42f, 0.42f, 0.5f);
			colors[ImGuiCol_ScrollbarGrabActive] = ImVec4(0.56f, 0.56f, 0.56f, 0.8f);
			colors[ImGuiCol_CheckMark] = colors[ImGuiCol_Text];
			colors[ImGuiCol_SliderGrab] = colors[ImGuiCol_FrameBg];
			colors[ImGuiCol_SliderGrabActive] = colors[ImGuiCol_FrameBg];
			colors[ImGuiCol_Button] = ImVec4(0.26f, 0.59f, 0.98f, 0.39f);
			colors[ImGuiCol_ButtonHovered] = ImVec4(0.26f, 0.59f, 0.98f, 0.2f);
			colors[ImGuiCol_ButtonActive] = ImVec4(0.26f, 0.59f, 0.98f, 0.59f);
			colors[ImGuiCol_Header] = ImVec4(0.06f, 0.53f, 0.98f, 0.39f);
			colors[ImGuiCol_HeaderHovered] = ImVec4(0.26f, 0.59f, 0.98f, 0.2f);
			colors[ImGuiCol_HeaderActive] = ImVec4(0.26f, 0.59f, 0.98f, 0.59f);
			colors[ImGuiCol_Separator] = ImVec4(0.5f, 0.5f, 0.5f, 0.6f);
			colors[ImGuiCol_SeparatorHovered] = ImVec4(0.7f, 0.6f, 0.6f, 1.0f);
			colors[ImGuiCol_SeparatorActive] = ImVec4(0.9f, 0.7f, 0.7f, 1.0f);
			colors[ImGuiCol_ResizeGrip] = ImVec4(0.6f, 0.6f, 0.6f, 0.8f);
			colors[ImGuiCol_ResizeGripHovered] = ImVec4(0.6f, 0.6f, 0.6f, 0.1f);
			colors[ImGuiCol_ResizeGripActive] = colors[ImGuiCol_ResizeGripHovered];
			colors[ImGuiCol_Tab] = ImVec4(0.26f, 0.59f, 0.98f, 0.31f);
			colors[ImGuiCol_TabHovered] = ImVec4(0.26f, 0.59f, 0.98f, 0.8f);
			colors[ImGuiCol_TabActive] = ImVec4(0.26f, 0.59f, 0.98f, 1.0f);
			colors[ImGuiCol_TabUnfocused] = ImVec4(0.15f, 0.15f, 0.15f, 0.97f);
			colors[ImGuiCol_TabUnfocusedActive] = ImVec4(0.26f, 0.59f, 0.98f, 1.0f);
			colors[ImGuiCol_PlotLines] = ImVec4(1.0f, 1.0f, 1.0f, 1.0f);
			colors[ImGuiCol_PlotLinesHovered] = ImVec4(0.9f, 0.7f, 0.0f, 1.0f);
			colors[ImGuiCol_PlotHistogram] = ImVec4(0.9f, 0.7f, 0.0f, 1.0f);
			colors[ImGuiCol_PlotHistogramHovered] = ImVec4(0.9f, 0.7f, 0.0f, 1.0f);
			colors[ImGuiCol_TableHeaderBg] = ImVec4(0.26f, 0.59f, 0.98f, 0.4f);
			colors[ImGuiCol_TableBorderStrong] = ImVec4(0.26f, 0.26f, 0.26f, 1.0f);
			colors[ImGuiCol_TableBorderLight] = ImVec4(0.19f, 0.19f, 0.19f, 1.0f);
			colors[ImGuiCol_TableRowBg] = ImVec4(0.0f, 0.0f, 0.0f, 0.0f);
			colors[ImGuiCol_TableRowBgAlt] = ImVec4(1.0f, 1.0f, 1.0f, 0.06f);
			colors[ImGuiCol_TextSelectedBg] = ImVec4(0.26f, 0.59f, 0.98f, 0.35f);
			colors[ImGuiCol_DragDropTarget] = ImVec4(0.8f, 0.5f, 0.5f, 1.0f);
			colors[ImGuiCol_NavHighlight] = ImVec4(0.26f, 0.59f, 0.98f, 1.0f);
			colors[ImGuiCol_NavWindowingHighlight] = ImVec4(0.3f, 0.3f, 0.3f, 0.56f);
			colors[ImGuiCol_NavWindowingDimBg] = ImVec4(0.2f, 0.2f, 0.2f, 0.35f);
			colors[ImGuiCol_ModalWindowDimBg] = ImVec4(0.2f, 0.2f, 0.2f, 0.35f);
		}

		void LoadStandaloneFonts(ImGuiIO& io)
		{
			// Jost is shipped under this standalone plugin's own runtime path;
			// no mutually-exclusive Community Shaders plugin is a font dependency.
			constexpr std::array<const char*, 1> regularPaths = {
				"Data\\Interface\\FO4CloudShadows\\Fonts\\Jost\\Jost-Regular.ttf"
			};
			constexpr std::array<const char*, 1> lightPaths = {
				"Data\\Interface\\FO4CloudShadows\\Fonts\\Jost\\Jost-Light.ttf"
			};
			auto loadFont = [&](const char* relativePath, float size) -> ImFont* {
				fs::path resolvedPath;
				if (!CloudShadows::ResolveGameRelativePath(
						fs::path(relativePath), resolvedPath)) {
					return nullptr;
				}
				// ImGui opens font files from UTF-8 paths on Windows; the ANSI
				// conversion failed (or threw) for non-ANSI install folders.
				const std::string utf8Path = FO4CS::Utf8Path(resolvedPath);
				if (utf8Path.empty())
					return nullptr;
				return io.Fonts->AddFontFromFileTTF(utf8Path.c_str(), size);
			};

			for (const char* path : regularPaths) {
				if (ImFont* font = loadFont(path, 27.0f)) {
					io.FontDefault = font;
					break;
				}
			}
			if (!io.FontDefault)
				io.FontDefault = io.Fonts->AddFontDefault();
			for (const char* path : lightPaths) {
				if (ImFont* font = loadFont(path, 35.1f)) {
					g_titleFont = font;
					break;
				}
			}
			for (const char* path : regularPaths) {
				if (ImFont* font = loadFont(path, 24.3f)) {
					g_subtextFont = font;
					break;
				}
			}
			if (!g_titleFont)
				g_titleFont = io.FontDefault;
			if (!g_subtextFont)
				g_subtextFont = io.FontDefault;
		}

#if FO4CS_ENABLE_DEVELOPER_TOOLS
		bool EnsureGrayPreviewShader()
		{
			if (g_grayPreviewCS)
				return true;

			static constexpr char source[] = R"hlsl(
Texture2D<float> Source : register(t0);
RWTexture2D<float4> Preview : register(u0);

[numthreads(8, 8, 1)]
void main(uint3 id : SV_DispatchThreadID)
{
	uint width, height;
	Source.GetDimensions(width, height);
	if (id.x >= width || id.y >= height)
		return;
	float value = saturate(Source.Load(int3(id.xy, 0)));
	Preview[id.xy] = float4(value, value, value, 1.0);
}
)hlsl";

			ComPtr<ID3DBlob> bytecode;
			ComPtr<ID3DBlob> errors;
			const HRESULT compileHr = D3DCompile(
				source,
				sizeof(source) - 1,
				"CloudShadowGrayPreviewCS",
				nullptr,
				nullptr,
				"main",
				"cs_5_0",
				D3DCOMPILE_OPTIMIZATION_LEVEL3,
				0,
				&bytecode,
				&errors);
			if (FAILED(compileHr)) {
				SPDLOG_ERROR(
					"[CloudShadows][Menu] grayscale preview shader compile failed: {}",
					errors ? static_cast<const char*>(errors->GetBufferPointer()) : "unknown error");
				return false;
			}

			const HRESULT createHr = g_dev->CreateComputeShader(
				bytecode->GetBufferPointer(), bytecode->GetBufferSize(), nullptr, &g_grayPreviewCS);
			if (FAILED(createHr)) {
				SPDLOG_ERROR("[CloudShadows][Menu] grayscale preview shader creation failed: 0x{:08X}", static_cast<uint32_t>(createHr));
				return false;
			}
			return true;
		}

		ID3D11ShaderResourceView* UpdateGrayPreview(
			ID3D11ShaderResourceView* source,
			GrayPreview& preview)
		{
			if (!source || !g_dev || !g_ctx || !EnsureGrayPreviewShader())
				return nullptr;

			ComPtr<ID3D11Resource> resource;
			source->GetResource(&resource);
			ComPtr<ID3D11Texture2D> sourceTexture;
			if (!resource || FAILED(resource.As(&sourceTexture)))
				return nullptr;

			D3D11_TEXTURE2D_DESC sourceDesc{};
			sourceTexture->GetDesc(&sourceDesc);
			if (!sourceDesc.Width || !sourceDesc.Height || sourceDesc.ArraySize != 1 || sourceDesc.SampleDesc.Count != 1)
				return nullptr;

			if (!preview.texture || preview.width != sourceDesc.Width || preview.height != sourceDesc.Height) {
				GrayPreview replacement;
				D3D11_TEXTURE2D_DESC desc{};
				desc.Width = sourceDesc.Width;
				desc.Height = sourceDesc.Height;
				desc.MipLevels = 1;
				desc.ArraySize = 1;
				desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
				desc.SampleDesc.Count = 1;
				desc.Usage = D3D11_USAGE_DEFAULT;
				desc.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS;
				if (FAILED(g_dev->CreateTexture2D(&desc, nullptr, &replacement.texture)) ||
					FAILED(g_dev->CreateShaderResourceView(replacement.texture.Get(), nullptr, &replacement.srv)) ||
					FAILED(g_dev->CreateUnorderedAccessView(replacement.texture.Get(), nullptr, &replacement.uav))) {
					SPDLOG_ERROR("[CloudShadows][Menu] failed to create {}x{} RGBA preview", desc.Width, desc.Height);
					return nullptr;
				}
				replacement.width = desc.Width;
				replacement.height = desc.Height;
				preview = std::move(replacement);
			}

			// Preserve the compute slots touched by this UI conversion. This runs at
			// Present, after the game's frame, but state hygiene keeps it compatible
			// with other render plugins in the same Present chain.
			ID3D11ComputeShader* savedShader = nullptr;
			std::array<ID3D11ClassInstance*, D3D11_SHADER_MAX_INTERFACES> rawClasses{};
			std::array<ComPtr<ID3D11ClassInstance>, D3D11_SHADER_MAX_INTERFACES> savedClasses;
			UINT savedClassCount = static_cast<UINT>(rawClasses.size());
			ID3D11ShaderResourceView* savedSRV = nullptr;
			ID3D11UnorderedAccessView* savedUAV = nullptr;
			ComPtr<ID3D11Predicate> savedPredicate;
			BOOL savedPredicateValue = FALSE;
			g_ctx->CSGetShader(&savedShader, rawClasses.data(), &savedClassCount);
			savedClassCount = (std::min)(
				savedClassCount, static_cast<UINT>(savedClasses.size()));
			for (UINT i = 0; i < savedClassCount; ++i)
				savedClasses[i].Attach(rawClasses[i]);
			g_ctx->CSGetShaderResources(0, 1, &savedSRV);
			g_ctx->CSGetUnorderedAccessViews(0, 1, &savedUAV);
			g_ctx->GetPredication(savedPredicate.GetAddressOf(), &savedPredicateValue);
			g_ctx->SetPredication(nullptr, FALSE);

			ID3D11ShaderResourceView* input = source;
			ID3D11UnorderedAccessView* output = preview.uav.Get();
			g_ctx->CSSetShader(g_grayPreviewCS.Get(), nullptr, 0);
			g_ctx->CSSetShaderResources(0, 1, &input);
			g_ctx->CSSetUnorderedAccessViews(0, 1, &output, nullptr);
			g_ctx->Dispatch((preview.width + 7) / 8, (preview.height + 7) / 8, 1);

			ID3D11ShaderResourceView* nullSRV = nullptr;
			ID3D11UnorderedAccessView* nullUAV = nullptr;
			g_ctx->CSSetShaderResources(0, 1, &nullSRV);
			g_ctx->CSSetUnorderedAccessViews(0, 1, &nullUAV, nullptr);
			rawClasses = {};
			for (UINT i = 0; i < savedClassCount; ++i)
				rawClasses[i] = savedClasses[i].Get();
			g_ctx->CSSetShader(savedShader, rawClasses.data(), savedClassCount);
			g_ctx->CSSetShaderResources(0, 1, &savedSRV);
			g_ctx->CSSetUnorderedAccessViews(0, 1, &savedUAV, nullptr);
			g_ctx->SetPredication(savedPredicate.Get(), savedPredicateValue);

			if (savedShader)
				savedShader->Release();
			if (savedSRV)
				savedSRV->Release();
			if (savedUAV)
				savedUAV->Release();

			return preview.srv.Get();
		}

#endif
		bool EnsureInit(IDXGISwapChain* sc)
		{
			std::scoped_lock imguiLock(g_imguiLifetimeMutex);
			if (g_inited.load(std::memory_order_acquire))
				return true;
			if (g_failed || GetTickCount64() < g_initRetryAfter)
				return false;

			g_dev = CloudShadows::g_capturedDevice;
			g_ctx = CloudShadows::g_capturedContext;
			if (!g_dev || !g_ctx || !sc)
				return false;

			DXGI_SWAP_CHAIN_DESC scd{};
			if (FAILED(sc->GetDesc(&scd)) || !scd.OutputWindow ||
				!IsWindow(scd.OutputWindow)) {
				// Window recreation can briefly leave the retiring swap chain
				// without a usable HWND. This is transient: a later Present on the
				// replacement chain must be allowed to initialize. Logged once
				// per episode, not on every Present while it lasts.
				static bool loggedMissingWindow = false;
				if (!std::exchange(loggedMissingWindow, true)) {
					SPDLOG_WARN(
						"[CloudShadows][Menu] swap chain has no live output window; "
						"deferring standalone menu initialization");
				}
				return false;
			}
			if (g_windowRetired && scd.OutputWindow == g_retiredHwnd)
				return false;
			g_hwnd.store(scd.OutputWindow, std::memory_order_release);

			IMGUI_CHECKVERSION();
			ImGui::CreateContext();
			ImGuiIO& io = ImGui::GetIO();
			io.IniFilename = nullptr;
			io.ConfigFlags |= ImGuiConfigFlags_NoMouseCursorChange;
			io.MouseDrawCursor = false;
			LoadStandaloneFonts(io);
			ApplyStyle();

			if (!ImGui_ImplWin32_Init(scd.OutputWindow)) {
				ImGui::DestroyContext();
				g_failed = true;
				SPDLOG_ERROR("[CloudShadows][Menu] ImGui Win32 initialization failed");
				return false;
			}
			g_win32BackendInited = true;
			if (!ImGui_ImplDX11_Init(g_dev, g_ctx)) {
				ImGui_ImplWin32_Shutdown();
				g_win32BackendInited = false;
				ImGui::DestroyContext();
				g_failed = true;
				SPDLOG_ERROR("[CloudShadows][Menu] ImGui DX11 initialization failed");
				return false;
			}
			g_dx11BackendInited = true;
			if (!InstallWndProcHook()) {
				ImGui_ImplDX11_Shutdown();
				ImGui_ImplWin32_Shutdown();
				g_dx11BackendInited = false;
				g_win32BackendInited = false;
				ImGui::DestroyContext();
				// A window hook can be refused transiently (window recreation in
				// progress); retry later rather than disabling the menu for good.
				g_initRetryAfter = GetTickCount64() + 5000;
				SPDLOG_WARN("[CloudShadows][Menu] input hook unavailable; retrying in 5 s");
				return false;
			}

			g_inited.store(true, std::memory_order_release);
			SPDLOG_INFO("[CloudShadows][Menu] feature menu ready (hwnd={}) — F11 toggles it", (void*)scd.OutputWindow);
			return true;
		}

		void DrawCubemapPreview()
		{
			if (!ImGui::CollapsingHeader("Cubemap preview")) return;
			ImGui::TextWrapped("Live cloud opacity: black = clear sky, white = cloud. "
				"These are the six captured faces before shadow strength is applied.");
			int mode = static_cast<int>(FO4CS::CloudComparison::GetPreview());
			const char* modes[]{ "Off", "Magenta over visible sky", "Raw opacity across screen" };
			if (ImGui::Combo("Sky alignment (F8)", &mode, modes, IM_ARRAYSIZE(modes)))
				FO4CS::CloudComparison::SetPreview(static_cast<FO4CS::CloudComparison::Preview>(mode));
			ImGui::TextWrapped("F8 cycles the sky views; F7 clears the preview.");
			if (!CloudShadows::g_shadowsEnabled.load(std::memory_order_relaxed)) {
				ImGui::TextWrapped("Enable Cloud Shadows to view the capture.");
				return;
			}
			auto* cube = CloudShadows::GetCommittedWorldCloudTiles();
			if (!cube) {
				ImGui::TextWrapped("Waiting for a cloud capture. Load an exterior with Cloud Shadows enabled.");
				return;
			}
			auto* atlas = g_cubePreview.Update(g_ctx, cube);
			if (!atlas) {
				ImGui::TextWrapped("Cubemap preview unavailable for this capture.");
				return;
			}
			ImGui::TextDisabled("%u x %u per face | latest completed capture",
				g_cubePreview.FaceSize(), g_cubePreview.FaceSize());
			if (ImGui::BeginTable("Cloud cube faces", 3, ImGuiTableFlags_SizingStretchSame)) {
				const char* labels[]{ "+X", "-X", "+Y", "-Y", "+Z (up)", "-Z (down)" };
				const float faceSize = static_cast<float>(g_cubePreview.FaceSize());
				const float inset = 0.5f / faceSize;
				for (unsigned face = 0; face < 6; ++face) {
					ImGui::TableNextColumn();
					ImGui::TextUnformatted(labels[face]);
					const float size = std::clamp(ImGui::GetContentRegionAvail().x, 1.0f, faceSize);
					const float x = static_cast<float>(face % 3), y = static_cast<float>(face / 3);
					ImGui::Image(reinterpret_cast<ImTextureID>(atlas), ImVec2(size, size),
						ImVec2((x + inset) / 3, (y + inset) / 2),
						ImVec2((x + 1 - inset) / 3, (y + 1 - inset) / 2));
				}
				ImGui::EndTable();
			}
		}

		void DrawSettings()
		{
			using namespace CloudShadows;

			bool enabled = g_shadowsEnabled.load(std::memory_order_relaxed);
			if (ImGui::Checkbox("Enable Cloud Shadows", &enabled)) {
				g_shadowsEnabled.store(enabled, std::memory_order_relaxed);
				InvalidateWorldCloudCaptureForToggle();
			}
			ImGui::SameLine();
			ImGui::TextDisabled("(F10 quick toggle)");

#if FO4CS_ENABLE_DEVELOPER_TOOLS
            const auto godrays = FO4CS::GodraysIntegration::GetDiagnostics();
            bool godrayOcclusion = godrays.cloudOcclusionEnabled;
            ImGui::BeginDisabled(!godrays.nativeConsumerSupported);
            if (ImGui::Checkbox("Cloud shadows in godrays", &godrayOcclusion))
                FO4CS::GodraysIntegration::SetCloudOcclusionEnabled(godrayOcclusion);
            ImGui::EndDisabled();
            if (!godrays.nativeConsumerSupported) {
                ImGui::TextDisabled("Native godray cloud occlusion is unavailable in this runtime.");
            } else {
                ImGui::TextWrapped(
                    "Dim sunbeams where clouds block sunlight. Turning this on "
                    "also enables Fallout's godrays. Turning it off restores vanilla sunbeams.");
                const auto nativeSettings = FO4CS::GodraysIntegration::GetDiagnostics();
                if (godrayOcclusion && nativeSettings.nativeSettingsError)
                    ImGui::TextWrapped(
                        "Could not enable Fallout's godrays. Check the log, then "
                        "close this menu to retry saving.");
                else if (godrayOcclusion && nativeSettings.nativeSettingsRestartRequired)
                    ImGui::TextWrapped(
                        "Fallout's godrays are enabled for the next launch. "
                        "Close this menu, then restart the game.");
                if (godrayOcclusion && !enabled)
                    ImGui::TextDisabled("Paused while Cloud Shadows is disabled.");
                else if (godrayOcclusion && godrays.renderVolumeCalls == 0 &&
                    !nativeSettings.nativeSettingsRestartRequired && !nativeSettings.nativeSettingsError)
                    ImGui::TextWrapped(
                        "No godray rendering detected yet. Load a daylight exterior "
                        "to test this option.");
            }

#endif
			ImGui::SliderFloat("Shadow opacity", &g_settings.Opacity, 0.0f, 4.0f, "%.2f",
				ImGuiSliderFlags_AlwaysClamp);
            ImGui::TextWrapped("Controls how strongly clouds dim direct sunlight. Default: 2.0.");

#if FO4CS_ENABLE_DEVELOPER_TOOLS
            ImGui::TextWrapped("F10 toggles shadows and starts timing. "
                "Keep the camera fixed and close this menu for 13 seconds. F8 previews the cubemap; "
                "preview frames are excluded from timing. F7 stops measurement and hides the HUD. "
                "Test selections reset at the next launch.");
            if (FO4CS::CloudComparison::HudVisible() && ImGui::Button("Stop comparison HUD (F7)"))
                FO4CS::CloudComparison::StopMeasurements();

#endif

			// Cloud height is no longer user-adjustable (removed 10 Sep 2026); the
			// saved fCloudHeight value is still honoured for compatibility.
			DrawCubemapPreview();

#if FO4CS_ENABLE_DEVELOPER_TOOLS
			if (ImGui::CollapsingHeader("Advanced / debug")) {
				bool isolateSingleCloud =
					g_singleCloudIsolationEnabled.load(std::memory_order_acquire);
				if (ImGui::Checkbox(
						"Isolate cloud in capture analysis", &isolateSingleCloud)) {
					if (isolateSingleCloud) {
						if (!LockSingleCloudToCurrentView())
							isolateSingleCloud = false;
					} else {
						g_singleCloudIsolationEnabled.store(
							false, std::memory_order_release);
					}
				}
				if (isolateSingleCloud) {
					float isolationRadius = std::clamp(
						g_singleCloudRadiusDegrees.load(
							std::memory_order_relaxed),
						2.0f, 30.0f);
					if (ImGui::SliderFloat(
							"Isolation radius", &isolationRadius,
							2.0f, 30.0f, "%.1f deg")) {
						g_singleCloudRadiusDegrees.store(
							isolationRadius, std::memory_order_relaxed);
					}
					if (ImGui::Button("Re-lock current view"))
						LockSingleCloudToCurrentView();
					ImGui::TextWrapped(
						"Look at a cloud before locking, then select Capture analysis. "
						"Its right panel shows coverage only inside this sky patch. "
						"Normal shadows still include all visible clouds.");
				}
				ImGui::TextDisabled(
					"Physical mode traces one exact receiver-to-sun ray per pixel.");
				int debug = std::clamp(static_cast<int>(g_settings.DebugMode), 0, 4);
				const char* debugModes[] = {
					"Off", "World checkerboard", "Raw depth", "Transmittance",
					"Capture analysis"
				};
				if (ImGui::Combo("Debug view", &debug, debugModes, IM_ARRAYSIZE(debugModes)))
					g_settings.DebugMode = static_cast<float>(debug);
			}

#endif
			ImGui::Separator();
            ImGui::TextWrapped("Changes apply immediately and save automatically when this menu closes.");
			if (ImGui::Button("Restore Defaults")) {
				const bool wasEnabled =
					g_shadowsEnabled.load(std::memory_order_relaxed);
				g_settings = Settings{};
				FO4CS::GodraysIntegration::SetCloudOcclusionEnabled(false);
				g_shadowsEnabled.store(true, std::memory_order_relaxed);
				if (!wasEnabled)
					InvalidateWorldCloudCaptureForToggle();
				g_singleCloudIsolationEnabled.store(
					false, std::memory_order_release);
				g_singleCloudRadiusDegrees.store(
					12.0f, std::memory_order_relaxed);
			}
		}

#if FO4CS_ENABLE_DEVELOPER_TOOLS
		void DrawDiagnostics()
		{
			using namespace CloudShadows;

			const bool ready = g_worldCloudReady.load(std::memory_order_acquire);
			const uint32_t layers = g_worldCloudActiveLayers.load(std::memory_order_acquire);
			const uint32_t skyTechnique = g_lastSkyTechnique.load(std::memory_order_relaxed);
			const char* skyTechniqueName = "none";
			switch (skyTechnique) {
			case 5:
				skyTechniqueName = "Clouds";
				break;
			case 6:
				skyTechniqueName = "CloudsLerp";
				break;
			case 7:
				skyTechniqueName = "CloudsFade";
				break;
			default:
				break;
			}
			const bool maskValid =
				g_lastCompletedShadowMaskValid.load(std::memory_order_acquire);
			const float anchorX = g_projectionAnchorX.load(std::memory_order_relaxed);
			const float anchorY = g_projectionAnchorY.load(std::memory_order_relaxed);
			const float anchorZ = g_projectionAnchorZ.load(std::memory_order_relaxed);
			const auto godrays = FO4CS::GodraysIntegration::GetDiagnostics();
            const bool passiveCaptureProven = ready && layers != 0 &&
                g_worldCloudCommittedEpoch.load(std::memory_order_acquire) != 0;
			const bool surfacePathProven = ready && layers != 0 && maskValid &&
				g_worldCloudCommittedEpoch.load(std::memory_order_acquire) != 0;
			const bool nativeGodrayPathProven =
				godrays.cloudOcclusionEnabled &&
                godrays.nativeConsumerSupported &&
				godrays.renderVolumeExportResolved &&
				godrays.renderVolumeHookInstalled &&
				(godrays.authenticatedDirectionalVariants & 0x3u) == 0x3u &&
				godrays.renderVolumeCalls != 0 &&
				godrays.submittedDrawsSinceEnable != 0;
			const bool flatRuntime = FO4CS::RuntimeAPI::GetSingleton().Target() !=
				FO4CS::F4SECompat::RuntimeTarget::kVR;
			const bool liveParityProven = flatRuntime && passiveCaptureProven &&
				surfacePathProven && nativeGodrayPathProven;
			const char* liveProofText = !flatRuntime
				? (passiveCaptureProven && surfacePathProven
					? "SURFACE PASS; VR GODRAYS UNSUPPORTED"
					: "awaiting VR surface evidence")
				: !godrays.cloudOcclusionEnabled
                    ? (surfacePathProven ? "SURFACE PASS; GODRAYS OFF" : "awaiting surface evidence")
                    : (liveParityProven ? "PASS" : "awaiting evidence");

			ImGui::TextUnformatted("Live capture health");
			ImGui::Separator();
			ImGui::Text("Live runtime path proof : %s", liveProofText);
            ImGui::Text("Cloud opacity / mask dispatch / godray draws: %s / %s / %s",
				passiveCaptureProven ? "PASS" : "WAIT",
				surfacePathProven ? "SUBMITTED" : "WAIT",
				flatRuntime
					? (!godrays.cloudOcclusionEnabled ? "OFF" : nativeGodrayPathProven ? "PASS" : "WAIT")
					: "N/A (no native VR consumer)");
            if (!passiveCaptureProven) {
                ImGui::TextColored(ImVec4(1.0f, 0.45f, 0.35f, 1.0f),
                    "Waiting for a complete cloud opacity capture.");
                ImGui::TextWrapped(
                    "Load an exterior with the sky visible. Capture follows the "
                    "game's visible clouds, including overlapping surfaces. "
                    "Rejected captures are reported in the log.");
                ImGui::Separator();
            }
			ImGui::Text("World-cloud resources : %s", ready ? "READY" : "not created");
			ImGui::Text("Committed cloud draws : %u", layers);
			ImGui::Text("Last screen-mask dispatch: %s", maskValid ? "COMPLETE" : "unavailable (neutral fallback)");
			ImGui::TextWrapped("A completed dispatch can still contain no shadows. Use the GPU evidence below to check actual attenuation.");
			ImGui::Text("Sky Begin calls       : %u", g_skyDrawsSeen.load(std::memory_order_relaxed));
			ImGui::Text("Last Sky technique    : %u (%s)", skyTechnique, skyTechniqueName);
            ImGui::Text("Private opacity draws (total): %llu",
                static_cast<unsigned long long>(g_geometryCaptureDraws.load(std::memory_order_relaxed)));
			ImGui::Text("Capture epoch          : %llu -> %llu",
				static_cast<unsigned long long>(g_worldCloudPendingEpoch.load(std::memory_order_relaxed)),
				static_cast<unsigned long long>(g_worldCloudCommittedEpoch.load(std::memory_order_relaxed)));
			ImGui::Text("Render camera          : (%.0f, %.0f, %.0f)", anchorX, anchorY, anchorZ);
            ImGui::Text("Capture frames / published / rejected: %llu / %llu / %llu",
                static_cast<unsigned long long>(g_geometryCaptureAttempts.load(std::memory_order_relaxed)),
                static_cast<unsigned long long>(g_geometryCapturePublished.load(std::memory_order_relaxed)),
                static_cast<unsigned long long>(g_geometryCaptureRejected.load(std::memory_order_relaxed)));
            ImGui::TextUnformatted("Capture: 256px cube, live surface blending, no UV-map cache");

			const char* godrayCapability = !godrays.nativeConsumerSupported
				? "unsupported by native runtime"
				: !godrays.cloudOcclusionEnabled
                    ? "OFF (vanilla sunbeams)"
                : !g_shadowsEnabled.load(std::memory_order_acquire)
                    ? "paused (Cloud Shadows disabled)"
				: !godrays.renderVolumeHookInstalled
					? "native export not hooked; using vanilla"
					: godrays.submittedDrawsSinceEnable != 0
						? "cloud-occluded draws submitted"
						: "hook installed; waiting for supported draws";
			ImGui::Text("Godray cloud occlusion : %s", godrayCapability);
            ImGui::Text("Godray lookup / hook: %s / %s",
                godrays.renderVolumeExportResolved ? "resolved" : "unavailable",
                godrays.renderVolumeHookInstalled ? "installed" : "unavailable");
            ImGui::Text("Godray draws since enabling: %llu",
                static_cast<unsigned long long>(godrays.submittedDrawsSinceEnable));
			ImGui::Text(
				"Godray PS / variants / calls / draws: %u / 0x%X / %llu / %llu",
				godrays.authenticatedShaderObjects,
				godrays.authenticatedDirectionalVariants,
				static_cast<unsigned long long>(godrays.renderVolumeCalls),
				static_cast<unsigned long long>(
					godrays.submittedCloudOcclusionDraws));
			ImGui::Text("Godray patch failures / last epoch: %u / %llu",
				godrays.patchFailures,
				static_cast<unsigned long long>(
					godrays.lastSubmittedCloudEpoch));

			ImGui::Separator();
			ImGui::TextUnformatted("Physical cloud-field telemetry");
			const bool telemetryValid =
				g_cloudTelemetryValid.load(std::memory_order_acquire);
			if (telemetryValid) {
				const float rayOpacity =
					g_receiverSunRayCloudOpacity.load(std::memory_order_relaxed);
				const float localMinimum =
					g_localCloudOpacityMinimum.load(std::memory_order_relaxed);
				const float localMean =
					g_localCloudOpacityMean.load(std::memory_order_relaxed);
				const float localMaximum =
					g_localCloudOpacityMaximum.load(std::memory_order_relaxed);
				const float shadowedFraction =
					g_localCloudShadowedFraction.load(std::memory_order_relaxed);
				const uint32_t sampleCount =
					g_localCloudTelemetrySampleCount.load(std::memory_order_relaxed);
				ImGui::Text("Camera -> sun cloud opacity : %.1f%%", rayOpacity * 100.0f);
				ImGui::Text(
					"Horizontal disk R=%.0f units (~%.1f m), camera Z",
					kCloudTelemetryRadiusWorldUnits,
					kCloudTelemetryRadiusWorldUnits * kWorldUnitMetres);
				ImGui::Text("Disk min / mean / max opacity: %.1f%% / %.1f%% / %.1f%%",
					localMinimum * 100.0f, localMean * 100.0f,
					localMaximum * 100.0f);
				ImGui::Text("Disk samples > %.0f%% opacity : %.1f%% (%u samples)",
					kCloudTelemetryOpacityThreshold * 100.0f,
					shadowedFraction * 100.0f, sampleCount);
				ImGui::Text("Telemetry capture epoch     : %llu",
					static_cast<unsigned long long>(
						g_cloudTelemetryEpoch.load(std::memory_order_relaxed)));
				if (ImGui::IsItemHovered()) {
					ImGui::BeginTooltip();
					ImGui::TextWrapped(
						"The disk samples the captured cloud field on a horizontal plane at camera height. It is not terrain-area coverage.");
					ImGui::EndTooltip();
				}
			} else {
				ImGui::TextDisabled(
					"<invalid: no committed field, above-horizon sun, or completed GPU sample>");
			}

			ImGui::Separator();
			ImGui::TextUnformatted("Automated acceptance test");
			const auto acceptance = AcceptanceRunner::GetStatusSnapshot();
			const bool acceptanceRunning =
				acceptance.status == AcceptanceRunner::Status::kRunning;
			if (acceptanceRunning)
				ImGui::BeginDisabled();
			static bool acceptanceStartRejected = false;
			if (ImGui::Button("Run automated cloud-shadow test"))
				acceptanceStartRejected =
					!AcceptanceRunner::StartLocalRequest();
			if (acceptanceRunning)
				ImGui::EndDisabled();
			if (acceptanceStartRejected && !acceptanceRunning)
				ImGui::TextDisabled("Could not queue test: another request is active.");

			const ImVec4 acceptanceColour =
				acceptance.status == AcceptanceRunner::Status::kPass
				? ImVec4(0.35f, 0.90f, 0.45f, 1.0f)
				: (acceptance.status == AcceptanceRunner::Status::kFail
					? ImVec4(1.0f, 0.35f, 0.30f, 1.0f)
					: (acceptance.status ==
							AcceptanceRunner::Status::kInconclusiveWeather
						? ImVec4(1.0f, 0.78f, 0.25f, 1.0f)
						: ImGui::GetStyleColorVec4(ImGuiCol_Text)));
			ImGui::TextColored(
				acceptanceColour, "%s", acceptance.statusText.data());
			if (acceptanceRunning && acceptance.timeoutMilliseconds != 0) {
				const float progress = std::clamp(
					static_cast<float>(acceptance.elapsedMilliseconds) /
						static_cast<float>(acceptance.timeoutMilliseconds),
					0.0f, 1.0f);
				ImGui::ProgressBar(progress, ImVec2(-1.0f, 0.0f));
			}
			ImGui::TextWrapped(
				"Use a daylight exterior with the sun above the horizon and broken "
				"clouds (both blue sky and visible cloud). Close F11 and keep normal "
				"ground/building receivers visible. The runner never moves the player, "
				"changes weather, or launches the game.");
			ImGui::TextDisabled(
				"PASS is structural evidence, not visual silhouette certification. "
				"External results use Data/F4SE/Plugins/"
				"FO4CloudShadows.acceptance.result.<RequestId>.json");

			ImGui::Separator();
			ImGui::Checkbox("Preview final shadow mask", &g_showCapturePreview);
			if (ImGui::IsItemHovered()) {
				ImGui::BeginTooltip();
				ImGui::TextWrapped(
					"White is fully lit. Darker values are the direct-sun transmittance beneath the committed clouds.");
				ImGui::EndTooltip();
			}
			if (!g_showCapturePreview)
				return;

			const float size = std::clamp(ImGui::GetContentRegionAvail().x, 160.0f, 512.0f);
			ImGui::TextUnformatted("Final screen-space shadow factor");
			if (g_cloudShadowSRV) {
				if (ID3D11ShaderResourceView* preview = UpdateGrayPreview(g_cloudShadowSRV, g_screenMaskPreview))
					ImGui::Image(reinterpret_cast<ImTextureID>(preview), ImVec2(size, size * 9.0f / 16.0f));
			} else {
				ImGui::TextDisabled("<no shadow texture>");
			}
		}
#endif

		void BuildWindow()
		{
			const ImGuiViewport* viewport = ImGui::GetMainViewport();
			ImGui::SetNextWindowPos(
				viewport->GetCenter(),
				ImGuiCond_FirstUseEver,
				ImVec2(0.5f, 0.5f));
			ImGui::SetNextWindowSize(
				ImVec2(viewport->WorkSize.x * 0.8f, viewport->WorkSize.y * 0.8f),
				ImGuiCond_FirstUseEver);
			ImGui::SetNextWindowSizeConstraints(ImVec2(640.0f, 460.0f), ImVec2(1400.0f, 1000.0f));

			bool open = g_visible.load(std::memory_order_acquire);
			const ImGuiWindowFlags flags =
				ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoTitleBar;
			if (ImGui::Begin("Open Shaders FO4", &open, flags)) {
				// Dynamic Reflections' undocked CS header: large Jost title on the
				// left, global actions on the right, then the heavy separator.
				if (ImGui::BeginTable("##HeaderLayout", 2, ImGuiTableFlags_SizingStretchProp)) {
					ImGui::TableSetupColumn("Title", ImGuiTableColumnFlags_WidthStretch);
					ImGui::TableSetupColumn("Buttons", ImGuiTableColumnFlags_WidthFixed);
					ImGui::TableNextColumn();
					ImGui::SetCursorPosX(ImGui::GetCursorPosX() + 14.0f);
					if (g_titleFont)
						ImGui::PushFont(g_titleFont);
					ImGui::SetWindowFontScale(1.7f);
					ImGui::TextUnformatted("Open Shaders FO4");
					ImGui::SetWindowFontScale(1.0f);
					if (g_titleFont)
						ImGui::PopFont();

					ImGui::TableNextColumn();
					if (ImGui::Button("X"))
						open = false;
					ImGui::EndTable();
				}
				ImGui::Separator();
				ImGui::Spacing();

				const float footerHeight = ImGui::GetFrameHeightWithSpacing() + ImGui::GetStyle().ItemSpacing.y * 2.0f;
				ImGui::BeginChild("Menus Table", ImVec2(0.0f, -footerHeight));
				if (ImGui::BeginTable(
						"##MenusTableColumns",
						2,
						ImGuiTableFlags_SizingStretchProp | ImGuiTableFlags_Resizable)) {
					ImGui::TableSetupColumn("##ListOfMenus", ImGuiTableColumnFlags_WidthStretch, 2.0f);
					ImGui::TableSetupColumn("##MenuConfig", ImGuiTableColumnFlags_WidthStretch, 8.0f);
					ImGui::TableNextColumn();

					ImGui::PushStyleVar(ImGuiStyleVar_FrameBorderSize, 0.0f);
					ImGui::PushStyleColor(ImGuiCol_FrameBg, ImVec4());
					if (ImGui::BeginListBox("##MenusList", ImVec2(-FLT_MIN, -FLT_MIN))) {
						ImGui::SeparatorText("Features");
						if (ImGui::TreeNodeEx(
								"Sky (1)",
								ImGuiTreeNodeFlags_DefaultOpen | ImGuiTreeNodeFlags_SpanAvailWidth)) {
							ImGui::Selectable(
								" Cloud Shadows ",
								true,
								ImGuiSelectableFlags_SpanAllColumns);
							ImGui::TreePop();
						}
						ImGui::EndListBox();
					}
					ImGui::PopStyleColor();
					ImGui::PopStyleVar();

					ImGui::TableNextColumn();
					if (ImGui::BeginChild("##FeatureConfigFrame", ImVec2(0.0f, 0.0f), true)) {
						if (g_titleFont)
							ImGui::PushFont(g_titleFont);
						ImGui::SetWindowFontScale(1.5f);
						ImGui::TextUnformatted("Cloud Shadows");
						ImGui::SetWindowFontScale(1.0f);
						if (g_titleFont)
							ImGui::PopFont();
						if (g_subtextFont)
							ImGui::PushFont(g_subtextFont);
						ImGui::TextColored(
							ImVec4(1.0f, 1.0f, 1.0f, 0.7f),
							"Projects the visible Fallout 4 clouds as moving exterior sunlight shadows.");
						if (g_subtextFont)
							ImGui::PopFont();
						ImGui::Separator();
						DrawSettings();

#if FO4CS_ENABLE_DEVELOPER_TOOLS
						if (ImGui::TreeNodeEx("Diagnostics", ImGuiTreeNodeFlags_None)) {
							DrawDiagnostics();
							ImGui::TreePop();
						}
#endif
					}
					ImGui::EndChild();
					ImGui::EndTable();
				}
				ImGui::EndChild();

				ImGui::Spacing();
				ImGui::Separator();
				ImGui::TextDisabled("%s", RuntimeLabel());
				ImGui::SameLine(ImGui::GetWindowWidth() - 245.0f);
				ImGui::TextDisabled("Cloud Shadows " FO4CS_RELEASE_VERSION_STR "  |  F11 closes");
				ImGui::TextDisabled("Build " FO4CS_BUILD_ID);
			}
			ImGui::End();

			if (!open)
				SetVisible(false);
		}
	}

	void SetExternalHostActive(bool active) noexcept
	{
		g_externalHostActive.store(active, std::memory_order_release);
	}

	bool IsExternalHostActive() noexcept
	{
		return g_externalHostActive.load(std::memory_order_acquire);
	}

	bool IsVisible()
	{
		return !IsExternalHostActive() &&
			g_visible.load(std::memory_order_acquire);
	}

	namespace
	{
		void FlushPendingSave() noexcept
		{
			if (!g_pendingSave.exchange(false, std::memory_order_acq_rel))
				return;
			try {
				CloudShadows::SaveSettings();
			} catch (...) {
				SPDLOG_ERROR("[CloudShadows][Menu] Could not save settings after the menu closed");
			}
		}
	}

	void CloseForDisabledHotkeys() noexcept
	{
		std::scoped_lock imguiLock(g_imguiLifetimeMutex);
		if (g_visible.load(std::memory_order_acquire)) {
			SetVisible(false);
			SPDLOG_INFO("[CloudShadows][Menu] hidden (Development Menu turned off)");
		}
	}

	void Shutdown() noexcept
	{
		std::scoped_lock imguiLock(g_imguiLifetimeMutex);
		SetVisible(false);
		FlushPendingSave();
		// Cover teardown reached after a focus/window transition that already
		// cleared visibility but still has transient capture state.
		FO4CS::EngineAPI::ShowGameCursorMenu(false);
		ReleaseOwnedMouseCapture();
		g_inited.store(false, std::memory_order_release);
		if (ImGui::GetCurrentContext()) {
			ImGui::GetIO().MouseDrawCursor = false;
			if (g_dx11BackendInited)
				ImGui_ImplDX11_Shutdown();
			if (g_win32BackendInited)
				ImGui_ImplWin32_Shutdown();
			ImGui::DestroyContext();
		}
		g_dx11BackendInited = false;
		g_win32BackendInited = false;

		const HWND hookedHwnd = g_wndProcHwnd;
		const WNDPROC originalWndProc =
			g_originalWndProc.load(std::memory_order_acquire);
		if (g_wndProcInstalled && hookedHwnd && originalWndProc &&
			IsWindow(hookedHwnd)) {
			const auto current = reinterpret_cast<WNDPROC>(
				GetWindowLongPtrW(hookedHwnd, GWLP_WNDPROC));
			if (current == &MenuWndProc) {
				SetLastError(ERROR_SUCCESS);
				const auto result = SetWindowLongPtrW(
					hookedHwnd, GWLP_WNDPROC,
					reinterpret_cast<LONG_PTR>(originalWndProc));
				if (result || GetLastError() == ERROR_SUCCESS) {
					g_wndProcInstalled = false;
					g_wndProcHwnd = nullptr;
					// Retain the forwarding target until the next installation
					// overwrites it. A callback that entered before this unhook may be
					// waiting on g_imguiLifetimeMutex and must still reach the game's
					// original procedure after teardown completes.
				}
			}
		} else if (g_wndProcInstalled &&
			(!hookedHwnd || !IsWindow(hookedHwnd))) {
			g_wndProcInstalled = false;
			g_wndProcHwnd = nullptr;
		}

#if FO4CS_ENABLE_DEVELOPER_TOOLS
		g_grayPreviewCS.Reset();
		g_screenMaskPreview = {};
#endif
		g_cubePreview.Reset();
		g_titleFont = nullptr;
		g_subtextFont = nullptr;
		g_dev = nullptr;
		g_ctx = nullptr;
		if (!g_wndProcInstalled)
			g_hwnd.store(nullptr, std::memory_order_release);
		g_failed = false;
	}

	void Draw(IDXGISwapChain* sc)
	{
		std::scoped_lock imguiLock(g_imguiLifetimeMutex);
		// A compatible host renders the Cloud Shadows page with its own ImGui
		// context.  Return before EnsureInit so this module never creates a second
		// Win32/DX11 backend, WndProc, cursor owner, or F11 consumer.  If a host
		// claimed ownership late, teardown is deliberately performed here on the
		// render thread.
		if (IsExternalHostActive()) {
			if (g_visible.load(std::memory_order_acquire)) {
				if (g_inited.load(std::memory_order_acquire) && ImGui::GetCurrentContext())
					ImGui::GetIO().MouseDrawCursor = false;
				FO4CS::EngineAPI::ShowGameCursorMenu(false);
				ReleaseOwnedMouseCapture();
				g_visible.store(false, std::memory_order_release);
				g_pendingSave.store(true, std::memory_order_release);
			}
			if (g_inited.load(std::memory_order_acquire) ||
				g_visible.load(std::memory_order_acquire)) {
				Shutdown();
			}
			FlushPendingSave();
			return;
		}
		FlushPendingSave();

		// With the Development Menu off and no HUD or preview showing there is
		// nothing to draw or poll: never create ImGui, hook the window or read
		// the swap chain for every player who never uses the menu.
		if (!CloudShadows::g_hotkeysEnabled.load(std::memory_order_relaxed) &&
			!g_visible.load(std::memory_order_acquire) &&
			!FO4CS::CloudComparison::HudVisible() &&
			FO4CS::CloudComparison::GetPreview() == FO4CS::CloudComparison::Preview::Off)
			return;

		// Present can reach us below RenderDoc's swap-chain wrapper, while the
		// captured renderer device/context and preview SRVs are above it. Use
		// the engine-facing output interface with that same device. Mixing a
		// native back buffer with a wrapped CreateRenderTargetView crashed F11.
		ComPtr<IDXGISwapChain> outputChain = sc;
		DXGI_SWAP_CHAIN_DESC presented{};
		if (!sc || FAILED(sc->GetDesc(&presented)))
			return;
		if (const auto* renderer = CloudShadows::GetRendererData();
			renderer && renderer->device == CloudShadows::g_capturedDevice &&
			renderer->context == CloudShadows::g_capturedContext &&
			renderer->renderWindow[0].swapChain) {
			auto* candidate = renderer->renderWindow[0].swapChain;
			DXGI_SWAP_CHAIN_DESC candidateDescription{};
			if (SUCCEEDED(candidate->GetDesc(&candidateDescription)) &&
				candidateDescription.OutputWindow == presented.OutputWindow)
				outputChain = candidate;
		}
		ComPtr<ID3D11Device> outputDevice;
		if (FAILED(outputChain->GetDevice(IID_PPV_ARGS(&outputDevice))) ||
			outputDevice.Get() != CloudShadows::g_capturedDevice) {
			static bool mismatchLogged = false;
			if (!mismatchLogged) {
				SPDLOG_WARN("[CloudShadows][Menu] output/device interface mismatch; deferring menu draw");
				mismatchLogged = true;
			}
			return;
		}
		sc = outputChain.Get();

		DXGI_SWAP_CHAIN_DESC desc{};
		const bool hasLiveOutput =
			sc && SUCCEEDED(sc->GetDesc(&desc)) && desc.OutputWindow &&
			IsWindow(desc.OutputWindow);
		const bool isRetiredOutput =
			g_windowRetired &&
			(!hasLiveOutput || desc.OutputWindow == g_retiredHwnd);

		if (g_inited.load(std::memory_order_acquire)) {
			const HWND initializedHwnd =
				g_hwnd.load(std::memory_order_acquire);
			const bool outputChanged =
				!hasLiveOutput ||
				desc.OutputWindow != initializedHwnd || !IsWindow(initializedHwnd);
			if (isRetiredOutput || outputChanged) {
				SPDLOG_INFO(
					"[CloudShadows][Menu] output window retired or replaced; "
					"recreating standalone ImGui backends");
				Shutdown();
			}
		}

		if (g_windowRetired) {
			// Do not recreate against the exact HWND whose NCDESTROY callback is
			// still unwinding. Retirement intentionally survives Shutdown; only a
			// distinct live swap-chain window is allowed to clear it.
			if (!hasLiveOutput || desc.OutputWindow == g_retiredHwnd)
				return;
			g_windowRetired = false;
			g_retiredHwnd = nullptr;
		}

		if (!EnsureInit(sc))
			return;

		static bool s_f11Was = false;
		const bool hotkeys = CloudShadows::g_hotkeysEnabled.load(std::memory_order_relaxed);
		if (!hotkeys && g_visible.load(std::memory_order_acquire)) {
			SetVisible(false);
			SPDLOG_INFO("[CloudShadows][Menu] hidden (hotkeys disabled in settings)");
		}
		const bool f11Down = hotkeys && (GetAsyncKeyState(VK_F11) & 0x8000) != 0;
		if (f11Down && !s_f11Was && IsFalloutForeground()) {
			const bool show = !g_visible.load(std::memory_order_acquire);
			SetVisible(show);
			SPDLOG_INFO("[CloudShadows][Menu] {}", show ? "shown" : "hidden");
		}
		s_f11Was = f11Down;

		const bool menuVisible = g_visible.load(std::memory_order_acquire);
		if (!menuVisible && !FO4CS::CloudComparison::HudVisible())
			return;

		ImGui_ImplDX11_NewFrame();
		ImGui_ImplWin32_NewFrame();
		ImGui::NewFrame();
        const auto skyMode = FO4CS::CloudComparison::GetPreview();
        if (skyMode != FO4CS::CloudComparison::Preview::Off) {
            if (auto* skyImage = FO4CS::CloudComparison::PreviewImage())
                ImGui::GetBackgroundDrawList()->AddImage(
                    reinterpret_cast<ImTextureID>(skyImage), ImVec2(0, 0), ImGui::GetIO().DisplaySize);
        }
        if (FO4CS::CloudComparison::HudVisible()) {
            ImGui::SetNextWindowPos(ImVec2(16, 16), ImGuiCond_Always);
            ImGui::SetNextWindowBgAlpha(0.8f);
            constexpr auto flags = ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_AlwaysAutoResize |
                ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoInputs | ImGuiWindowFlags_NoFocusOnAppearing;
            if (ImGui::Begin("Cloud comparison status", nullptr, flags)) {
                ImGui::Text("Cloud Shadows: %s",
                    CloudShadows::g_shadowsEnabled.load(std::memory_order_relaxed) ? "ON" : "OFF");
                if (skyMode != FO4CS::CloudComparison::Preview::Off) {
                    ImGui::TextColored(ImVec4(1, 0.6f, 1, 1), "SKY ALIGNMENT PREVIEW - timing paused");
                    if (!FO4CS::CloudComparison::PreviewImage())
                        ImGui::TextUnformatted("Waiting for a current sky capture and camera. Enable shadows with F10.");
                    else if (skyMode == FO4CS::CloudComparison::Preview::SkyOverlay)
                        ImGui::TextUnformatted("Magenta shows captured clouds. Rotate to compare edges. F8: next view | F7: off.");
                    else
                        ImGui::TextUnformatted("Raw opacity: white = cloud, black = clear sky. F8 returns to the game.");
                } else
                {
                    ImGui::TextUnformatted(FO4CS::CloudComparison::Status().c_str());
#if FO4CS_ENABLE_DEVELOPER_TOOLS
                    const auto gpu = FO4CS::CloudComparison::GetTimings();
                    if (gpu.frames)
                        ImGui::Text("GPU capture %.3f ms | projection %.3f ms | %.0f capture draws/frame",
                            gpu.captureMs, gpu.projectionMs, gpu.draws);
                    ImGui::TextDisabled("F10: on/off + timing | F8: sky alignment | F7: stop | F11: settings");
#else
                    ImGui::TextDisabled("F10: toggle | F8: sky preview | F7: hide | F11: settings");
#endif
                }
            }
            ImGui::End();
        }
		if (menuVisible) BuildWindow();
		ImGui::Render();

		ID3D11Texture2D* backBuffer = nullptr;
		if (SUCCEEDED(sc->GetBuffer(
				0, __uuidof(ID3D11Texture2D), reinterpret_cast<void**>(&backBuffer))) &&
			backBuffer) {
			ID3D11RenderTargetView* rtv = nullptr;
			if (SUCCEEDED(g_dev->CreateRenderTargetView(backBuffer, nullptr, &rtv)) && rtv) {
				std::array<ComPtr<ID3D11RenderTargetView>, D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT>
					savedRenderTargets;
				std::array<ID3D11RenderTargetView*, D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT>
					rawRenderTargets{};
				ComPtr<ID3D11DepthStencilView> savedDepthStencil;
				g_ctx->OMGetRenderTargets(
					static_cast<UINT>(rawRenderTargets.size()), rawRenderTargets.data(),
					savedDepthStencil.GetAddressOf());
				UINT savedRenderTargetCount = 0;
				for (UINT i = 0; i < rawRenderTargets.size(); ++i) {
					savedRenderTargets[i].Attach(rawRenderTargets[i]);
					if (rawRenderTargets[i])
						savedRenderTargetCount = i + 1;
				}

				std::array<ComPtr<ID3D11UnorderedAccessView>, D3D11_PS_CS_UAV_REGISTER_COUNT>
					savedUAVs;
				std::array<ID3D11UnorderedAccessView*, D3D11_PS_CS_UAV_REGISTER_COUNT>
					rawUAVs{};
				if (savedRenderTargetCount < rawUAVs.size()) {
					g_ctx->OMGetRenderTargetsAndUnorderedAccessViews(
						0, nullptr, nullptr, savedRenderTargetCount,
						static_cast<UINT>(rawUAVs.size()) - savedRenderTargetCount,
						rawUAVs.data() + savedRenderTargetCount);
					for (UINT i = savedRenderTargetCount; i < rawUAVs.size(); ++i)
						savedUAVs[i].Attach(rawUAVs[i]);
				}
				ComPtr<ID3D11Predicate> savedPredicate;
				BOOL savedPredicateValue = FALSE;
				g_ctx->GetPredication(
					savedPredicate.GetAddressOf(), &savedPredicateValue);
				g_ctx->SetPredication(nullptr, FALSE);

				g_ctx->OMSetRenderTargetsAndUnorderedAccessViews(
					1, &rtv, nullptr, 1,
					D3D11_KEEP_UNORDERED_ACCESS_VIEWS, nullptr, nullptr);
				{
					// ImGui 1.90's stock DX11 backend clears HS/DS/CS without
					// restoring them and restores VS b0 through the legacy API,
					// which loses D3D11.1 subranges. Repair those exact states.
					ScopedBackendShaderState savedBackendShaderState(g_ctx);
					ImGui_ImplDX11_RenderDrawData(ImGui::GetDrawData());
				}

				for (UINT i = 0; i < rawRenderTargets.size(); ++i)
					rawRenderTargets[i] = savedRenderTargets[i].Get();
				for (UINT i = savedRenderTargetCount; i < rawUAVs.size(); ++i)
					rawUAVs[i] = savedUAVs[i].Get();
				std::array<UINT, D3D11_PS_CS_UAV_REGISTER_COUNT> keepCounters{};
				keepCounters.fill(UINT(-1));
				g_ctx->OMSetRenderTargetsAndUnorderedAccessViews(
					savedRenderTargetCount,
					savedRenderTargetCount ? rawRenderTargets.data() : nullptr,
					savedDepthStencil.Get(), savedRenderTargetCount,
					static_cast<UINT>(rawUAVs.size()) - savedRenderTargetCount,
					savedRenderTargetCount < rawUAVs.size()
						? rawUAVs.data() + savedRenderTargetCount
						: nullptr,
					savedRenderTargetCount < keepCounters.size()
						? keepCounters.data() + savedRenderTargetCount
						: nullptr);
				g_ctx->SetPredication(
					savedPredicate.Get(), savedPredicateValue);
				rtv->Release();
			}
			backBuffer->Release();
		}

	}
}
