// SPDX-License-Identifier: GPL-3.0-only
#include "PCH.h"
#include "MenuFrameworkPage.h"
#include "McmSettings.h"
#include "RuntimeAPI.h"

#include <mutex>
#include <type_traits>

namespace FO4CS::MenuFrameworkPage
{
    namespace
    {
        // Consumer ABI of DCCStudios/F4SEMenuFramework (the same exports the
        // Realistic Reflections page uses): a section registration, its event
        // registration and the cimgui functions. Every widget draws in the
        // host's ImGui context; no ImGui objects cross the DLL boundary.
        enum class FrameworkEvent : int { None = 0, Open = 1, Close = 2, BeforeRender = 3, AfterRender = 4 };
        using EventCallback = void(__stdcall*)(FrameworkEvent);
        using RenderCallback = void(__stdcall*)();

        struct Api
        {
            void (*addSection)(const char*, RenderCallback){};
            std::int64_t (*registerEvent)(EventCallback){};
            void (*text)(const char*, const char*){};
            void (*wrapped)(const char*, ...){};
            void (*separator)(){};
            bool (*checkbox)(const char*, bool*){};
            bool (*sliderFloat)(const char*, float*, float, float, const char*, int){};
            bool (*button)(const char*){};
            bool (*hovered)(int){};
            void (*tooltip)(const char*, ...){};

            bool Load(HMODULE module) noexcept
            {
                const auto bind = [module](auto& function, const char* name) {
                    function = reinterpret_cast<std::remove_reference_t<decltype(function)>>(
                        GetProcAddress(module, name));
                    return function != nullptr;
                };
                // RegisterEvent is optional: without it the page still works
                // and saves through Apply.
                (void)bind(registerEvent, "RegisterEvent");
                return bind(addSection, "AddSectionItem") &&
                    bind(text, "igTextUnformatted") && bind(wrapped, "igTextWrapped") &&
                    bind(separator, "igSeparator") && bind(checkbox, "igCheckbox") &&
                    bind(sliderFloat, "igSliderFloat") &&
                    bind(button, "igSmallButton") && bind(hovered, "igIsItemHovered") &&
                    bind(tooltip, "igSetTooltip");
            }
        };

        Api ui;
        // The framework renders and raises Open/Close on different threads.
        std::mutex mutex;
        McmSettingsFile::Values original, edited;
        bool loaded{}, saveFailed{};
        // A failed read is latched until the page is opened again, instead of
        // re-reading the INI (and logging) on every rendered frame.
        bool readFailed{};
        // Ticket of the newest queued write; the page reports its failure.
        std::uint64_t saveTicket{};

        void Tooltip(const char* help)
        {
            if (ui.hovered(0))
                ui.tooltip("%s", help);
        }

        // Caller holds the mutex. Only the file is written; the render
        // thread's MCM poll applies it within two seconds, as for MCM itself.
        bool Save()
        {
            if (!loaded || edited == original) {
                saveFailed = false;
                return true;
            }
            // Queued on the settings writer; never blocks the framework thread.
            const auto ticket = McmSettings::WriteSaved(edited);
            if (ticket == 0) {
                saveFailed = true;
                return false;
            }
            saveTicket = ticket;
            original = edited;
            saveFailed = false;
            SPDLOG_INFO("[CloudShadows][MenuFramework] Settings saved: enabled={} opacity={:.2f} "
                "developmentMenu={}",
                edited.enabled, edited.opacity, edited.hotkeys);
            return true;
        }

        void __stdcall Render()
        {
            std::lock_guard lock(mutex);
            if (!loaded && !readFailed) {
                if (McmSettings::ReadSaved(edited)) {
                    original = edited;
                    loaded = true;
                } else {
                    readFailed = true;
                }
            }
            if (!loaded) {
                ui.text("Cloud Shadows settings are unavailable; see FO4CloudShadows.log. "
                    "Reopen this page to retry.", nullptr);
                return;
            }
            if (saveTicket != 0 && McmSettings::SaveFailed(saveTicket)) {
                // The queued write failed: keep the edit pending for a retry.
                saveTicket = 0;
                original = {};
                saveFailed = true;
            }
            ui.checkbox("Enable cloud shadows", &edited.enabled);
            Tooltip("Shadows from the moving clouds dim direct sunlight outdoors. Default: enabled.");
            // Ctrl+click text entry is not clamped by ImGui; Validate keeps the
            // value finite and in range before it can be saved.
            if (ui.sliderFloat("Shadow opacity", &edited.opacity, 0.0f, 4.0f, "%.2f", 0))
                edited = McmSettingsFile::Validate(edited);
            Tooltip("Controls how strongly clouds dim direct sunlight. Default: 2.0.");
            ui.separator();
            ui.text("Development Menu", nullptr);
            ui.checkbox("Development Menu##hotkeys", &edited.hotkeys);
            Tooltip("Off by default. When on: F11 opens the Cloud Shadows development menu, F10 toggles "
                "cloud shadows for comparison, F8 cycles the sky-alignment preview and F7 hides it.");
            ui.separator();
            ui.wrapped("%s", "Changes are saved when you close this menu or press Apply, and take effect "
                "within two seconds. MCM shows them after restarting Fallout.");
            if (ui.button("Apply"))
                Save();
            if (saveFailed)
                ui.wrapped("%s", "Settings could not be saved. Check that Data/MCM/Settings is writable, "
                    "then press Apply to retry.");
        }

        void __stdcall OnEvent(FrameworkEvent event)
        {
            if (event != FrameworkEvent::Open && event != FrameworkEvent::Close)
                return;
            std::lock_guard lock(mutex);
            if (event == FrameworkEvent::Open) {
                // Re-read on open so MCM or F11 changes made meanwhile show,
                // unless an unsaved edit is waiting for a retry.
                readFailed = false;
                if (!saveFailed)
                    loaded = false;
            } else if (Save()) {
                loaded = false;
            }
        }
    }

    void Install() noexcept
    {
        static bool attempted = false;
        if (attempted)
            return;
        attempted = true;
        // F4SE Menu Framework is a flat-game host; VR keeps MCM and F11.
        if (RuntimeAPI::GetSingleton().Target() == F4SECompat::RuntimeTarget::kVR)
            return;
        const auto framework = GetModuleHandleW(L"F4SEMenuFramework.dll");
        if (!framework)
            return;
        if (!McmSettings::Available()) {
            SPDLOG_WARN("[CloudShadows][MenuFramework] Settings page not registered: "
                "the MCM settings file is unavailable");
            return;
        }
        Api candidate;
        if (!candidate.Load(framework)) {
            SPDLOG_WARN("[CloudShadows][MenuFramework] Settings page not registered: "
                "required widget API missing");
            return;
        }
        ui = candidate;
        ui.addSection("Cloud Shadows/Settings", Render);
        if (!ui.registerEvent) {
            SPDLOG_WARN("[CloudShadows][MenuFramework] Settings page registered without open/close "
                "events (RegisterEvent missing); changes are saved by Apply");
            return;
        }
        const auto listener = ui.registerEvent(OnEvent);
        if (listener <= 0) {
            SPDLOG_WARN("[CloudShadows][MenuFramework] Settings page registered, but open/close events "
                "were rejected; changes are saved only by Apply");
            return;
        }
        SPDLOG_INFO("[CloudShadows][MenuFramework] Settings page registered (event listener {})", listener);
    }
}
