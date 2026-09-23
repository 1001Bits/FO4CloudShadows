#pragma once

#include "BuildFeatures.h"

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <memory>
#include <string>
#include <string_view>
#include <type_traits>

#include <DirectXMath.h>
#include <d3d11.h>
#include <d3d11_1.h>
#include <d3dcompiler.h>
#include <dxgi.h>
#include <wrl/client.h>

#include <detours.h>

#include <nlohmann/json.hpp>
#include <spdlog/spdlog.h>
#include <spdlog/sinks/basic_file_sink.h>

#include "EngineAPI.h"
#include "F4SECompat.h"

namespace logger = spdlog;

namespace fs = std::filesystem;
using namespace DirectX;
using Microsoft::WRL::ComPtr;

#define DLLEXPORT __declspec(dllexport)

#ifndef FO4CLOUDSHADOWS_VERSION_STR
#define FO4CLOUDSHADOWS_VERSION_STR "1.0.0"
#endif
#ifndef FO4CS_RELEASE_VERSION_STR
#define FO4CS_RELEASE_VERSION_STR "1.0"
#endif
