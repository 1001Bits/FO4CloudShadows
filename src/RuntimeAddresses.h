#pragma once

#include "RuntimeAPI.h"

namespace FO4CS::RuntimeAddresses
{
    // Columns: OG 1.10.163, AE 1.11.240, VR 1.2.72, NG 1.10.984, AE 1.11.221.
    // Every version requires build revision zero.
    // RuntimeAPI proves the actual executable version before selecting a row,
    // adds the loaded image base (ASLR), and checks bounds/page permissions.
    // Zero means unsupported on that runtime, never a cross-version fallback.
    // Provenance and private-input verification: docs/RUNTIME_ADDRESSES.md.
    inline constexpr RuntimeRVA kRendererDataSymbol{
        0x0609BF80, 0x038E13A0, 0x060F3CE8, 0x03624CA0, 0x038CAB20
    };
    inline constexpr RuntimeRVA kGraphicsStateSymbol{
        0x06541EF0, 0x03D871A0, 0x065A2AB0, 0x03ACAAA0, 0x03D70920
    };
    inline constexpr RuntimeRVA kRenderTargetManagerSymbol{
        0x0384FD30, 0x02F4D750, 0x038AC010, 0x02CDBED0, 0x02F42760
    };
    inline constexpr RuntimeRVA kSkySingletonFunctionSymbol{
        0x00128CB0, 0x002F2730, 0x0012FB50, 0x0029E1E0, 0x002F2410
    };
    inline constexpr RuntimeRVA kWorldRootCameraFunctionSymbol{
        0x00D3C6B0, 0x00C37DD0, 0x00D87550, 0x00BB2100, 0x00C37A40
    };
    inline constexpr RuntimeRVA kPlayerSingletonSymbol{
        0x059D6FD0, 0x031EDE50, 0x05A38518, 0x0303ACA0, 0x031E2DD0
    };
    inline constexpr RuntimeRVA kTESSingletonSymbol{
        0x05AA4288, 0x032DD158, 0x05B042C0, 0x0303ABB0, 0x032D20C8
    };
    inline constexpr RuntimeRVA kBeginTechniqueSymbol{
        0x02891DB0, 0x02231AE0, 0x02814BE0, 0x020D4940, 0x0222A0B0
    };
    // Only VR needs the cached-render-batch SetShaders hook.
    inline constexpr RuntimeRVA kVRSetShadersSymbol{
        0, 0, 0x01D92C00, 0, 0
    };
    // The native desktop CursorMenu is not used in VR.
    inline constexpr RuntimeRVA kUIMessageQueueSingletonSymbol{
        0x058D0AC8, 0x030E8B20, 0, 0x02E665F0, 0x030DDAA0
    };
    inline constexpr RuntimeRVA kUIMessageQueueAddMessageSymbol{
        0x0204CB90, 0x01A89630, 0, 0x0196E230, 0x01A89170
    };
    inline constexpr RuntimeRVA kStringPoolGetEntrySymbol{
        0x01B43DB0, 0x0167DD20, 0, 0x01563610, 0x0167DA00
    };
    inline constexpr RuntimeRVA kWaterReflectionUpdateSymbol{
        0x007DBA50, 0x007EAD50, 0x007CA020, 0x007972D0, 0x007EAA40
    };
    // TESDataHandler* slot, read by Papyrus Game.IsPluginInstalled.
    inline constexpr RuntimeRVA kDataHandlerSymbol{
        0x058CF080, 0x030E7100, 0x05930B50, 0x02E64E68, 0x030DC080
    };
}
