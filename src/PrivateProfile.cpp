// SPDX-License-Identifier: GPL-3.0-only
#include "PCH.h"
#include "PrivateProfile.h"
#include "CpuStageProfiler.h"
#include "CloudComparison.h"
#include "CloudShadows.h"
#include "RuntimeAPI.h"
#include "EngineAPI.h"
#include "ResidentPageValidation.h"
#include <bcrypt.h>
#include <fstream>
#include <filesystem>
#include <nlohmann/json.hpp>

namespace FO4CS::PrivateProfile
{
#if FO4CS_ENABLE_PRIVATE_PROFILING
    namespace {
        using Json = nlohmann::json;
        struct Row {
            double seconds{}, frameMs{};
            HRESULT result{};
            bool mask{}, light{}, enabled{}, counted{};
            UINT presents{};
            CpuProfile::Counters stages{};
        };
        std::atomic<bool> active{false};
        bool initialized{}, sampling{}, cpu{}, gpu{}, cycles{}, worldReady{}, gpuReset{};
        std::filesystem::path requestPath;
        std::string lastTag, mode;
        std::wstring expectedDesktop;
        DWORD windowThread{};
        IDXGISwapChain* observedChain{};
        Microsoft::WRL::ComPtr<ID3D11Texture2D> screenshot;
        std::vector<Row> rows;
        double frequency{}, previous{}, nextPoll{}, start{}, settle{}, duration{};
        bool IsExterior() noexcept {
            __try {
                auto* player = EngineAPI::GetPlayerCharacter();
                auto* sky = EngineAPI::GetSky();
                return player && EngineAPI::IsExteriorCell(player->parentCell) &&
                    sky && EngineAPI::ReadSkyMode(sky) == EngineAPI::SkyMode::kFull;
            } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
        }
        bool DesktopMatches() {
            wchar_t own[256]{}, input[256]{}; DWORD bytes{};
            const auto desktop = GetThreadDesktop(windowThread);
            if (!desktop || !GetUserObjectInformationW(desktop, UOI_NAME, own, sizeof(own), &bytes) ||
                expectedDesktop != own) return false;
            const auto inputDesktop = OpenInputDesktop(0, FALSE, DESKTOP_READOBJECTS);
            if (!inputDesktop) return false;
            const bool read = GetUserObjectInformationW(inputDesktop, UOI_NAME, input, sizeof(input), &bytes) != FALSE;
            CloseDesktop(inputDesktop);
            return read && expectedDesktop != input;
        }
        void Initialize(IDXGISwapChain* chain) {
            initialized = true;
            wchar_t path[2048]{}, desktop[256]{};
            const auto n = GetEnvironmentVariableW(L"FO4CS_PRIVATE_PROFILE_REQUEST", path, static_cast<DWORD>(std::size(path)));
            const auto d = GetEnvironmentVariableW(L"FO4CS_PRIVATE_PROFILE_DESKTOP", desktop, static_cast<DWORD>(std::size(desktop)));
            if (!n || n >= std::size(path) || !d || d >= std::size(desktop) ||
                FO4CS::RuntimeAPI::GetSingleton().Target() != FO4CS::F4SECompat::RuntimeTarget::kLegacy) return;
            expectedDesktop = desktop;
            DXGI_SWAP_CHAIN_DESC description{};
            DWORD owner{};
            if (FAILED(chain->GetDesc(&description))) return;
            windowThread = GetWindowThreadProcessId(description.OutputWindow, &owner);
            if (!windowThread || owner != GetCurrentProcessId()) return;
            observedChain = chain;
            if (expectedDesktop.find(L"CodexCloudProfile_") != 0 || !DesktopMatches()) return;
            requestPath = std::filesystem::path(path);
            if (!requestPath.is_absolute() || !std::filesystem::is_directory(requestPath.parent_path())) return;
            LARGE_INTEGER f{}; QueryPerformanceFrequency(&f);
            frequency = static_cast<double>(f.QuadPart);
            rows.reserve(20000);
            active.store(true, std::memory_order_release);
            spdlog::default_logger()->set_pattern("[%Y-%m-%d %H:%M:%S.%e] [%t] [%L] %v");
            SPDLOG_INFO("FO4CloudShadows private profile v1 PID={}", GetCurrentProcessId());
            SPDLOG_INFO("[CloudShadows][PrivateProfile] Armed PID={} on private desktop; file control only, application cadence (not display FPS)", GetCurrentProcessId());
        }
        void PollScreenshot(IDXGISwapChain* chain) {
            // Explicit test-harness trigger only. Captures happen before any
            // measurement window. One asynchronous readback, no pipeline changes.
            using Microsoft::WRL::ComPtr;
            ComPtr<ID3D11Device> device;
            if (FAILED(chain->GetDevice(IID_PPV_ARGS(&device)))) return;
            ComPtr<ID3D11DeviceContext> context; device->GetImmediateContext(&context);
            const std::filesystem::path trigger(L"Data/screenshot_trigger");
            if (!screenshot && std::filesystem::exists(trigger)) {
                ComPtr<ID3D11Texture2D> buffer;
                if (FAILED(chain->GetBuffer(0,IID_PPV_ARGS(&buffer)))) return;
                D3D11_TEXTURE2D_DESC desc{}; buffer->GetDesc(&desc);
                if (desc.SampleDesc.Count != 1 || (desc.Format != DXGI_FORMAT_R8G8B8A8_UNORM &&
                    desc.Format != DXGI_FORMAT_R8G8B8A8_UNORM_SRGB &&
                    desc.Format != DXGI_FORMAT_B8G8R8A8_UNORM && desc.Format != DXGI_FORMAT_B8G8R8A8_UNORM_SRGB)) return;
                desc.Usage=D3D11_USAGE_STAGING; desc.BindFlags=desc.MiscFlags=0;
                desc.CPUAccessFlags=D3D11_CPU_ACCESS_READ;
                if (FAILED(device->CreateTexture2D(&desc,nullptr,&screenshot))) return;
                std::error_code triggerError;
                std::filesystem::remove(trigger,triggerError);
                // The harness may still be closing its trigger writer. Retry
                // that transient sharing violation without stopping profiling.
                if (triggerError) { screenshot.Reset(); return; }
                context->CopyResource(screenshot.Get(),buffer.Get());
                return;
            }
            if (!screenshot) return;
            D3D11_TEXTURE2D_DESC desc{}; screenshot->GetDesc(&desc);
            const bool bgra=desc.Format==DXGI_FORMAT_B8G8R8A8_UNORM || desc.Format==DXGI_FORMAT_B8G8R8A8_UNORM_SRGB;
            std::vector<char> pixels(static_cast<std::size_t>(desc.Width)*desc.Height*3);
            D3D11_MAPPED_SUBRESOURCE mapped{};
            if (FAILED(context->Map(screenshot.Get(),0,D3D11_MAP_READ,D3D11_MAP_FLAG_DO_NOT_WAIT,&mapped))) return;
            for (UINT y=0;y<desc.Height;++y) {
                const auto* source=static_cast<const unsigned char*>(mapped.pData)+static_cast<std::size_t>(y)*mapped.RowPitch;
                for (UINT x=0;x<desc.Width;++x) {
                    const auto dest=(static_cast<std::size_t>(y)*desc.Width+x)*3;
                    pixels[dest]=static_cast<char>(source[x*4+(bgra?2:0)]);
                    pixels[dest+1]=static_cast<char>(source[x*4+1]);
                    pixels[dest+2]=static_cast<char>(source[x*4+(bgra?0:2)]);
                }
            }
            context->Unmap(screenshot.Get(),0); screenshot.Reset();
            std::ofstream out("Data/screenshot_output.ppm",std::ios::binary);
            out << "P6\n" << desc.Width << ' ' << desc.Height << "\n255\n";
            out.write(pixels.data(),static_cast<std::streamsize>(pixels.size()));
        }
        void Finish() {
            sampling = false;
            CpuProfile::enabled.store(false, std::memory_order_relaxed);
            const auto gpuTimings = CloudComparison::GetTimings();
            CloudComparison::SetProfilingMeasurements(false);
            const auto base = requestPath.parent_path() / lastTag;
            std::ofstream output(base.string() + ".frames.csv", std::ios::binary);
            output << "seconds,frame_ms,present_result,mask,lighting,enabled,present_count,present_count_valid";
            for (const auto* name : CpuProfile::names)
                output << ',' << name << "_calls," << name << "_inclusive_ms," << name << "_exclusive_ms," << name << "_max_call_ms," << name << "_cycles," << name << "_exclusive_cycles";
            output << '\n';
            for (const auto& row : rows) {
                output << row.seconds << ',' << row.frameMs << ',' << row.result << ',' << row.mask << ',' << row.light << ',' << row.enabled << ',' << row.presents << ',' << row.counted;
                for (const auto& counter : row.stages)
                    output << ',' << counter.calls << ',' << counter.ticks * 1000.0 / frequency << ',' << counter.exclusiveTicks * 1000.0 / frequency << ',' << counter.maximumTicks * 1000.0 / frequency << ',' << counter.cycles << ',' << counter.exclusiveCycles;
                output << '\n';
            }
            output.close();
            Json result{
                {"tag", lastTag}, {"mode", mode}, {"cpu_instrumented",cpu}, {"thread_cycles",cycles}, {"gpu_instrumented",gpu},
                {"pid",GetCurrentProcessId()}, {"build",FO4CS_BUILD_ID}, {"rows",rows.size()}, {"settle_seconds",settle},
                {"requested_seconds",duration}, {"clock_frequency",frequency},
                {"resident_page_checks",ResidentPages::enabled.load()},
                {"start_qpc_seconds",start}, {"exterior_at_finish",IsExterior()},
                {"metric","private desktop application Present intervals, not display FPS"},
                {"gpu_frames",gpuTimings.frames}, {"gpu_dropped",gpuTimings.dropped},
                {"gpu_capture_ms",gpuTimings.captureMs}, {"gpu_projection_ms",gpuTimings.projectionMs},
                {"gpu_scope_wall_ms",gpuTimings.cpuMs}, {"gpu_draws",gpuTimings.draws},
                {"file_ok",static_cast<bool>(output)}
            };
            std::ofstream summary(base.string() + ".json",std::ios::binary);
            summary << result.dump(2);
            SPDLOG_INFO("[CloudShadows][PrivateProfile] Complete tag={} mode={} frames={} CPU={} GPU={}", lastTag,mode,rows.size(),cpu,gpu);
        }
        void Poll(double now) {
            if (now < nextPoll || sampling) return;
            nextPoll = now + 0.25;
            if (!DesktopMatches()) {
                SPDLOG_ERROR("[CloudShadows][PrivateProfile] Private output-window desktop proof failed on thread={}; controller disabled",GetCurrentThreadId());
                active.store(false, std::memory_order_release); return;
            }
            std::ifstream input(requestPath);
            if (!input) return;
            const auto request = Json::parse(input,nullptr,false);
            if (!request.is_object()) return;
            const auto tag = request.value("tag",std::string{});
            if (tag.empty() || tag == lastTag || tag.size() > 100 ||
                tag.find_first_not_of("abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789_-") != std::string::npos) return;
            const auto requestedMode = request.value("mode",std::string{"off"});
            if (requestedMode != "off" && requestedMode != "cube" && requestedMode != "sun") return;
            const double requestedDuration = request.value("seconds",20.0);
            const double requestedSettle = request.value("settle",10.0);
            if (!std::isfinite(requestedDuration) || requestedDuration < 5 || requestedDuration > 120 ||
                !std::isfinite(requestedSettle) || requestedSettle < 2 || requestedSettle > 120) return;
            lastTag=tag; mode=requestedMode; duration=requestedDuration; settle=requestedSettle;
            cpu=request.value("cpu",true); gpu=request.value("gpu",false); cycles=request.value("cycles",false);
            ResidentPages::enabled.store(request.value("resident_pages",false),std::memory_order_relaxed);
            CloudComparison::SetMethod(mode == "sun" ? CloudComparison::Method::SunMask : CloudComparison::Method::Cubemap);
            CloudShadows::g_shadowsEnabled.store(mode != "off",std::memory_order_release);
            CloudShadows::InvalidateWorldCloudCaptureForToggle();
            CloudComparison::SetProfilingMeasurements(gpu);
            rows.clear(); start=now; sampling=true; gpuReset=false;
            CpuProfile::measureCycles.store(cycles,std::memory_order_relaxed);
            CpuProfile::enabled.store(cpu,std::memory_order_relaxed);
            SPDLOG_INFO("[CloudShadows][PrivateProfile] Start tag={} mode={} CPU={} cycles={} GPU={} settle={} duration={}",tag,mode,cpu,cycles,gpu,settle,duration);
        }
    }
    bool Active() noexcept { return active.load(std::memory_order_acquire); }
    bool IsPinnedEnbPresent(std::uintptr_t address, HMODULE owner) noexcept {
        constexpr std::uintptr_t presentRva=0x3f110;
        if (!owner || address-reinterpret_cast<std::uintptr_t>(owner)!=presentRva) return false;
        static const HMODULE pinned=[]() noexcept -> HMODULE {
            try {
                wchar_t flag[8]{}, desktop[256]{}, own[256]{}, input[256]{}, request[2048]{};
                DWORD bytes{};
                if (GetEnvironmentVariableW(L"FO4CS_PRIVATE_ENB_PRESENT",flag,8)!=1 || flag[0]!=L'1' ||
                    RuntimeAPI::GetSingleton().Target()!=F4SECompat::RuntimeTarget::kLegacy) return nullptr;
                const auto d=GetEnvironmentVariableW(L"FO4CS_PRIVATE_PROFILE_DESKTOP",desktop,256);
                const auto n=GetEnvironmentVariableW(L"FO4CS_PRIVATE_PROFILE_REQUEST",request,2048);
                if (!d || d>=256 || !n || n>=2048 || std::wstring_view(desktop).find(L"CodexCloudProfile_")!=0 ||
                    !GetUserObjectInformationW(GetThreadDesktop(GetCurrentThreadId()),UOI_NAME,own,sizeof(own),&bytes) ||
                    std::wstring_view(own)!=desktop) return nullptr;
                const auto inputDesktop=OpenInputDesktop(0,FALSE,DESKTOP_READOBJECTS);
                if (!inputDesktop) return nullptr;
                const bool read=GetUserObjectInformationW(inputDesktop,UOI_NAME,input,sizeof(input),&bytes)!=FALSE;
                CloseDesktop(inputDesktop);
                if (!read || std::wstring_view(input)==desktop) return nullptr;
                const std::filesystem::path control(request);
                if (!control.is_absolute() || !std::filesystem::is_directory(control.parent_path())) return nullptr;
                const auto module=GetModuleHandleW(L"d3d11.dll");
                wchar_t modulePath[32768]{}, gamePath[32768]{};
                const auto m=GetModuleFileNameW(module,modulePath,32768);
                const auto g=GetModuleFileNameW(nullptr,gamePath,32768);
                if (!module || !m || m>=32768 || !g || g>=32768 ||
                    !std::filesystem::equivalent(std::filesystem::path(gamePath).parent_path()/L"d3d11.dll",modulePath)) return nullptr;
                std::ifstream file(modulePath,std::ios::binary|std::ios::ate);
                const auto size=file.tellg();
                if (!file || size<=0 || size>8*1024*1024) return nullptr;
                std::vector<unsigned char> data(static_cast<std::size_t>(size));
                file.seekg(0); file.read(reinterpret_cast<char*>(data.data()),static_cast<std::streamsize>(data.size()));
                if (!file) return nullptr;
                BCRYPT_ALG_HANDLE algorithm{};
                if (BCryptOpenAlgorithmProvider(&algorithm,BCRYPT_SHA256_ALGORITHM,nullptr,0)<0) return nullptr;
                std::array<unsigned char,32> digest{};
                const auto status=BCryptHash(algorithm,nullptr,0,data.data(),static_cast<ULONG>(data.size()),digest.data(),32);
                BCryptCloseAlgorithmProvider(algorithm,0);
                if (status<0) return nullptr;
                constexpr char hex[]="0123456789ABCDEF";
                std::string actual; actual.reserve(64);
                for (const auto value:digest) { actual+=hex[value>>4]; actual+=hex[value&15]; }
                if (actual!="525A6DFAC678D97EA5D49C1210D1258B258AC1B76044656F3EA7B1D2485F2B67") return nullptr;
                SPDLOG_INFO("[CloudShadows][PrivateProfile] Authenticated ENB 0.501 Present permitted for private benchmark only");
                return module;
            } catch (...) { return nullptr; }
        }();
        return pinned && owner==pinned;
    }
    void AfterPresent(IDXGISwapChain* chain, HRESULT result, bool maskValid, bool lightingApplied) noexcept {
        try {
            if (!initialized) Initialize(chain);
            if (!Active()) return;
            if (observedChain != chain) {
                DXGI_SWAP_CHAIN_DESC description{}; DWORD owner{};
                if (FAILED(chain->GetDesc(&description))) return;
                windowThread=GetWindowThreadProcessId(description.OutputWindow,&owner);
                if (owner!=GetCurrentProcessId() || !DesktopMatches()) return;
                observedChain=chain;
            }
            const double now = static_cast<double>(CpuProfile::Now()) / frequency;
            if (!worldReady && IsExterior()) {
                worldReady = true;
                SPDLOG_INFO("[CloudShadows][PrivateProfile] Exterior ready PID={} thread={}",GetCurrentProcessId(),GetCurrentThreadId());
                spdlog::default_logger_raw()->flush();
            }
            if (sampling && !gpuReset && now-start >= settle) {
                CloudComparison::ResetTimings(); gpuReset=true;
            }
            if (sampling && previous > 0 && previous - start >= settle) {
                UINT count{};
                const bool counted = SUCCEEDED(chain->GetLastPresentCount(&count));
                rows.push_back(Row{now-start,(now-previous)*1000.0,result,maskValid,lightingApplied,
                    CloudShadows::g_shadowsEnabled.load(std::memory_order_acquire),counted,count,CpuProfile::counters});
                if (now-start >= settle+duration || rows.size() >= 20000) Finish();
            }
            Poll(now);
            if (!sampling || now-start < settle-2.0) PollScreenshot(chain);
            CpuProfile::counters={};
            previous=now;
        } catch (const std::exception& error) {
            CpuProfile::enabled.store(false,std::memory_order_relaxed);
            active.store(false,std::memory_order_release);
            CloudComparison::SetProfilingMeasurements(false);
            SPDLOG_ERROR("[CloudShadows][PrivateProfile] Disabled after error: {}",error.what());
        }
    }
#else
    bool Active() noexcept { return false; }
    bool IsPinnedEnbPresent(std::uintptr_t, HMODULE) noexcept { return false; }
    void AfterPresent(IDXGISwapChain*, HRESULT, bool, bool) noexcept {}
#endif
}
