// SPDX-License-Identifier: GPL-3.0-only
#pragma once
#include <DirectXMath.h>
#include <algorithm>
#include <cmath>

namespace FO4CS
{
    // Shared capture/receiver transform. Positions are relative to the same
    // retained field origin used by the cubemap method. Moving the finite map
    // changes its window, not the position of the cloud field.
    struct SunMaskProjection
    {
        DirectX::XMFLOAT4 rightAndHalfWidth{};
        DirectX::XMFLOAT4 upAndHeight{};
        DirectX::XMFLOAT4 sunAndPlanetRadius{};
        DirectX::XMFLOAT4 centerAndValid{};

        static constexpr unsigned kResolution = 512;
        static constexpr float kPlanetRadius = 6371000.0f * 70.0f;

        static bool Build(DirectX::XMFLOAT3 sun,
            DirectX::XMFLOAT3 cameraRelative, float height,
            SunMaskProjection& result) noexcept
        {
            using namespace DirectX;
            result = {};
            const auto finite = [](XMFLOAT3 p) {
                return std::isfinite(p.x) && std::isfinite(p.y) && std::isfinite(p.z);
            };
            if (!finite(sun) || !finite(cameraRelative) ||
                !std::isfinite(height) || height <= 0 || sun.z <= 0)
                return false;
            XMVECTOR light = XMLoadFloat3(&sun);
            const float lengthSquared = XMVectorGetX(XMVector3LengthSq(light));
            if (!std::isfinite(lengthSquared) || lengthSquared < 1.0e-8f)
                return false;
            light = XMVector3Normalize(light);
            const XMVECTOR reference = std::abs(XMVectorGetZ(light)) > 0.99f
                ? XMVectorSet(0, 1, 0, 0) : XMVectorSet(0, 0, 1, 0);
            const XMVECTOR right = XMVector3Normalize(XMVector3Cross(reference, light));
            const XMVECTOR up = XMVector3Cross(light, right);
            const float halfWidth = (std::max)(height * 4.0f, 16384.0f);
            if (!std::isfinite(halfWidth) || !std::isfinite(height * (2.0f * kPlanetRadius + height)))
                return false;
            const float texel = 2.0f * halfWidth / kResolution;
            const XMVECTOR camera = XMLoadFloat3(&cameraRelative);
            const float cx = std::round(XMVectorGetX(XMVector3Dot(camera, right)) / texel) * texel;
            const float cy = std::round(XMVectorGetX(XMVector3Dot(camera, up)) / texel) * texel;
            if (!std::isfinite(cx) || !std::isfinite(cy)) return false;
            XMStoreFloat4(&result.rightAndHalfWidth, right);
            XMStoreFloat4(&result.upAndHeight, up);
            XMStoreFloat4(&result.sunAndPlanetRadius, light);
            XMStoreFloat4(&result.centerAndValid, right * cx + up * cy);
            result.rightAndHalfWidth.w = halfWidth;
            result.upAndHeight.w = height;
            result.sunAndPlanetRadius.w = kPlanetRadius;
            result.centerAndValid.w = 1.0f;
            return true;
        }
    };
    static_assert(sizeof(SunMaskProjection) == 64);
}
