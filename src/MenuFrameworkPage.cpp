// SPDX-License-Identifier: GPL-3.0-only
#include "PCH.h"
#include "MenuFrameworkPage.h"
#include "McmSettings.h"
#include "RuntimeAPI.h"
#include "WeatherOpacity.h"

#include <algorithm>
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
            // Optional layout helpers for the weather list.
            void (*sameLine)(float, float){};
            void (*disabled)(const char*, ...){};

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
                (void)bind(sameLine, "igSameLine");
                (void)bind(disabled, "igTextDisabled");
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

        void Note(const char* text)
        {
            if (ui.disabled)
                ui.disabled("%s", text);
            else
                ui.wrapped("%s", text);
        }

        // Caller holds the mutex. Unpaired weathers show the opacity of the
        // vanilla slider they follow (or Shadow opacity) and gain a pairing
        // when moved; Reset removes it again.
        void WeatherRow(const WeatherOpacity::Entry& weather, bool current)
        {
            const auto found = edited.weathers.find(weather.key);
            const bool paired = found != edited.weathers.end();
            const auto follows = WeatherOpacity::SliderFor(weather.key);
            std::string followLabel;
            if (!follows.empty() && follows != weather.key)
                for (const auto& vanilla : McmSettingsFile::kVanillaWeathers)
                    if (vanilla.key == follows)
                        followLabel = std::string(vanilla.label);
            const auto inherited = followLabel.empty() ? edited.weathers.end() : edited.weathers.find(follows);
            const bool inherits = inherited != edited.weathers.end();
            float value = paired ? found->second : inherits ? inherited->second : edited.opacity;
            const auto label = weather.label + (current ? "  (current)" : "") + "##w" + weather.key;
            const std::string format = paired ? "%.2f" : inherits ? "%.2f (" + followLabel + ")" : "%.2f (Shadow opacity)";
            if (ui.sliderFloat(label.c_str(), &value, 0.0f, 4.0f, format.c_str(), 0)) {
                edited.weathers[weather.key] = value;
                edited = McmSettingsFile::Validate(edited);
            }
            const auto help = weather.plugin + " " + weather.key.substr(weather.key.find('|') + 1) +
                (paired ? std::string{} : !followLabel.empty()
                    ? "\nUntil you move this slider, the weather follows the " + followLabel + " slider."
                    : std::string("\nUntil you move this slider, the weather uses Shadow opacity."));
            Tooltip(help.c_str());
            if (paired) {
                if (ui.sameLine)
                    ui.sameLine(0.0f, -1.0f);
                if (ui.button(("Reset##r" + weather.key).c_str()))
                    edited.weathers.erase(weather.key);
                const auto reset = !followLabel.empty() ? "Follow the " + followLabel + " slider again."
                    : std::string("Use Shadow opacity for this weather again.");
                Tooltip(reset.c_str());
            }
        }

        // Caller holds the mutex.
        void WeatherSection()
        {
            ui.checkbox("Weather specific cloud opacity", &edited.weatherOpacity);
            Tooltip("Each weather can use its own shadow opacity. The opacity fades gradually when "
                "the weather changes. A weather you have not changed follows the vanilla weather "
                "slider its name or type matches, else Shadow opacity. Off by default.");
            if (!edited.weatherOpacity)
                return;
            const auto current = WeatherOpacity::CurrentWeather();
            const auto catalog = WeatherOpacity::Catalog();
            if (!catalog) {
                Note("Listing the weathers of your load order...");
                return;
            }
            const bool currentListed = current.key.empty() || std::any_of(catalog->begin(),
                catalog->end(), [&](const auto& entry) { return entry.key == current.key; });
            if (!currentListed)
                WeatherRow(current, true);
            for (const auto& entry : *catalog)
                WeatherRow(entry, entry.key == current.key);
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
                "developmentMenu={} weatherOpacity={} weatherPairings={}",
                edited.enabled, edited.opacity, edited.hotkeys,
                edited.weatherOpacity, edited.weathers.size());
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
            Tooltip("Controls how strongly clouds dim direct sunlight. Default: 1.0.");
            ui.separator();
            WeatherSection();
            ui.separator();
            ui.checkbox("Development menu##hotkeys", &edited.hotkeys);
            Tooltip("When enabled, close this menu and use:\n"
                "F11 - Open/close the development menu and start an FPS test.\n"
                "F10 - Toggle cloud shadows for a manual FPS comparison.\n"
                "The menu shows ON/OFF averages and can run an automatic comparison. Stand still outdoors.\n"
                "Desktop only. Off by default.");
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
