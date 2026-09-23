// SPDX-License-Identifier: GPL-3.0-only
#pragma once

#include <d3d11.h>
#include <wrl/client.h>
#include <array>

namespace FO4CS
{
    // Read-only, mip-zero view of the six native cube faces. Call only while
    // the menu preview is visible; no readback or background update is needed.
    class CloudCubePreview
    {
    public:
        ID3D11ShaderResourceView* Update(ID3D11DeviceContext* context,
            ID3D11ShaderResourceView* committedCube) noexcept;
        void Reset() noexcept;
        UINT FaceSize() const noexcept { return faceSize_; }

    private:
        template<class T> using Ptr = Microsoft::WRL::ComPtr<T>;
        struct Source
        {
            Ptr<ID3D11Texture2D> texture;
            Ptr<ID3D11ShaderResourceView> arrayView;
        };
        Ptr<ID3D11Device> device_;
        Ptr<ID3D11ComputeShader> shader_;
        Ptr<ID3D11Texture2D> atlas_;
        Ptr<ID3D11ShaderResourceView> atlasView_;
        Ptr<ID3D11UnorderedAccessView> atlasOutput_;
        std::array<Source, 2> sources_{};
        UINT faceSize_{}, nextSource_{};
        bool compileAttempted_{};
    };
}
