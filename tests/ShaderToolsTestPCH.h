#pragma once

// The production target supplies this alias from src/PCH.h.  ShaderTools tests
// intentionally avoid CommonLibF4, so provide only the logging dependency the
// standalone DXBC patcher needs.
#include <spdlog/spdlog.h>

namespace logger = spdlog;
