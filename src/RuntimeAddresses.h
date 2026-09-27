#pragma once

#include "RuntimeAPI.h"

namespace FO4CS::RuntimeAddresses
{
    // Reviewed RVAs for exactly OG 1.10.163.0, AE 1.11.240.0 and VR 1.2.72.0.
    // RuntimeAPI proves the actual executable version before selecting a row,
    // adds the loaded image base (ASLR), and checks bounds/page permissions.
    // Zero means unsupported on that runtime, never a cross-version fallback.
    // Provenance and private-input verification: docs/RUNTIME_ADDRESSES.md.
    inline constexpr RuntimeRVA kRendererDataSymbol{
        0x0609BF80, 0x038E13A0, 0x060F3CE8
    };
    inline constexpr RuntimeRVA kGraphicsStateSymbol{
        0x06541EF0, 0x03D871A0, 0x065A2AB0
    };
    inline constexpr RuntimeRVA kRenderTargetManagerSymbol{
        0x0384FD30, 0x02F4D750, 0x038AC010
    };
    inline constexpr RuntimeRVA kSkySingletonFunctionSymbol{
        0x00128CB0, 0x002F2730, 0x0012FB50
    };
    inline constexpr RuntimeRVA kWorldRootCameraFunctionSymbol{
        0x00D3C6B0, 0x00C37DD0, 0x00D87550
    };
    inline constexpr RuntimeRVA kPlayerSingletonSymbol{
        0x059D6FD0, 0x031EDE50, 0x05A38518
    };
    inline constexpr RuntimeRVA kTESSingletonSymbol{
        0x05AA4288, 0x032DD158, 0x05B042C0
    };
    inline constexpr RuntimeRVA kBeginTechniqueSymbol{
        0x02891DB0, 0x02231AE0, 0x02814BE0
    };
    // Only VR needs the cached-render-batch SetShaders hook.
    inline constexpr RuntimeRVA kVRSetShadersSymbol{
        0, 0, 0x01D92C00
    };
    // The native desktop CursorMenu is not used in VR.
    inline constexpr RuntimeRVA kUIMessageQueueSingletonSymbol{
        0x058D0AC8, 0x030E8B20, 0
    };
    inline constexpr RuntimeRVA kUIMessageQueueAddMessageSymbol{
        0x0204CB90, 0x01A89630, 0
    };
    inline constexpr RuntimeRVA kStringPoolGetEntrySymbol{
        0x01B43DB0, 0x0167DD20, 0
    };
    inline constexpr RuntimeRVA kWaterReflectionUpdateSymbol{
        0x007DBA50, 0x007EAD50, 0x007CA020
    };
}
