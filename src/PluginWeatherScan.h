// SPDX-License-Identifier: GPL-3.0-only
#pragma once

#include "McmSettingsFile.h"

#include <array>
#include <cstdint>
#include <cstring>
#include <istream>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace FO4CS::PluginWeatherScan
{
    // What one plugin file says about weathers. Form IDs are as stored in the
    // file: the top byte indexes `masters`, or is the file itself when it
    // equals masters.size().
    struct Weather
    {
        std::uint32_t id{};
        std::string editorId;
        // DATA classification flags: 1 pleasant, 2 cloudy, 4 rainy, 8 snow
        // (the game's radstorms use snow).
        std::uint8_t kind{};
    };
    // A climate (CLMT WLST) or region (REGN RDWT) and the weathers it lists.
    // Every such record is kept, even without a list: an override that drops
    // the list leaves the climate or region without those weathers.
    struct WeatherList
    {
        std::uint32_t id{};
        bool region{};
        std::vector<std::uint32_t> weathers;
    };
    struct Result
    {
        std::vector<std::string> masters;
        std::vector<Weather> weathers;
        std::vector<WeatherList> lists;
        std::uint32_t compressedSkipped{};
    };

    namespace detail
    {
        inline constexpr std::uint32_t kCompressed = 0x00040000;
        inline constexpr std::uint32_t kMaxRecord = 64u << 20;

        struct Header
        {
            std::array<char, 4> type{};
            std::uint32_t size{};   // record data size, or the whole group's size
            std::array<char, 4> label{};  // flags of a record, label of a group
            std::uint32_t formID{};
        };

        [[nodiscard]] inline bool ReadHeader(std::istream& input, Header& header)
        {
            std::array<char, 24> raw{};
            if (!input.read(raw.data(), raw.size()))
                return false;
            std::memcpy(header.type.data(), raw.data(), 4);
            std::memcpy(&header.size, raw.data() + 4, 4);
            std::memcpy(header.label.data(), raw.data() + 8, 4);
            std::memcpy(&header.formID, raw.data() + 12, 4);
            return true;
        }

        [[nodiscard]] inline bool Is(const std::array<char, 4>& value, std::string_view tag)
        {
            return std::string_view(value.data(), 4) == tag;
        }

        // Calls `visit(type, data)` for each subrecord, honouring XXXX sizes.
        template <class Visit>
        void Subrecords(std::string_view data, Visit&& visit)
        {
            std::uint32_t extended = 0;
            while (data.size() >= 6) {
                const std::string_view type = data.substr(0, 4);
                std::uint16_t shortSize{};
                std::memcpy(&shortSize, data.data() + 4, 2);
                const std::uint32_t size = extended ? extended : shortSize;
                extended = 0;
                data.remove_prefix(6);
                if (size > data.size())
                    return;
                const auto payload = data.substr(0, size);
                if (type == "XXXX" && size == 4)
                    std::memcpy(&extended, payload.data(), 4);
                else
                    visit(type, payload);
                data.remove_prefix(size);
            }
        }

        inline void ReadWeatherList(std::string_view entries, std::vector<std::uint32_t>& out)
        {
            // CLMT WLST and REGN RDWT: { weather, chance, global } per entry.
            for (; entries.size() >= 12; entries.remove_prefix(12)) {
                std::uint32_t weather{};
                std::memcpy(&weather, entries.data(), 4);
                if (weather)
                    out.push_back(weather);
            }
        }
    }

    // Reads the TES4 header and only the WTHR, CLMT and REGN top-level groups,
    // seeking past everything else. Compressed records are skipped (the plugin
    // links no zlib); a truncated or malformed file ends the scan early.
    [[nodiscard]] inline std::optional<Result> Scan(std::istream& input)
    {
        using namespace detail;
        Result result;
        Header header;
        if (!ReadHeader(input, header) || !Is(header.type, "TES4") || header.size > kMaxRecord)
            return std::nullopt;
        std::string data(header.size, '\0');
        if (!input.read(data.data(), header.size))
            return std::nullopt;
        Subrecords(data, [&](std::string_view type, std::string_view payload) {
            if (type == "MAST")
                result.masters.emplace_back(payload.data(), strnlen(payload.data(), payload.size()));
        });

        for (std::streamoff groupStart = input.tellg(); ReadHeader(input, header);
             groupStart = input.tellg()) {
            if (!Is(header.type, "GRUP") || header.size < 24)
                break;
            const std::streamoff groupEnd = groupStart + header.size;
            const bool weather = Is(header.label, "WTHR");
            const bool climate = Is(header.label, "CLMT");
            const bool region = Is(header.label, "REGN");
            if (weather || climate || region) {
                Header record;
                while (input.tellg() < groupEnd && ReadHeader(input, record)) {
                    if (Is(record.type, "GRUP")) {
                        input.seekg(static_cast<std::streamoff>(record.size) - 24, std::ios::cur);
                        continue;
                    }
                    std::uint32_t flags{};
                    std::memcpy(&flags, record.label.data(), 4);
                    if ((flags & kCompressed) || record.size > kMaxRecord) {
                        result.compressedSkipped += (flags & kCompressed) ? 1 : 0;
                        input.seekg(record.size, std::ios::cur);
                        continue;
                    }
                    data.assign(record.size, '\0');
                    if (!input.read(data.data(), record.size))
                        return result;
                    std::string editorId;
                    std::uint8_t kind = 0;
                    std::vector<std::uint32_t> listed;
                    Subrecords(data, [&](std::string_view type, std::string_view payload) {
                        if (weather && type == "EDID")
                            editorId.assign(payload.data(), strnlen(payload.data(), payload.size()));
                        else if (weather && type == "DATA" && payload.size() > 11)
                            kind = static_cast<std::uint8_t>(payload[11] & 0x0F);
                        else if ((climate && type == "WLST") || (region && type == "RDWT"))
                            ReadWeatherList(payload, listed);
                    });
                    if (weather)
                        result.weathers.push_back({ record.formID, std::move(editorId), kind });
                    else
                        result.lists.push_back({ record.formID, region, std::move(listed) });
                }
            }
            input.clear();
            input.seekg(groupEnd);
            if (!input)
                break;
        }
        return result;
    }

    // Canonical settings key of a form ID stored in `fileName` (see
    // McmSettingsFile::WeatherOpacities).
    [[nodiscard]] inline std::optional<std::string> KeyFor(const Result& scan,
        std::string_view fileName, std::uint32_t id)
    {
        const auto index = id >> 24;
        const std::string_view owner = index < scan.masters.size()
            ? std::string_view(scan.masters[index]) : fileName;
        std::array<char, 8> hex{};
        std::snprintf(hex.data(), hex.size(), "%06X", id & 0xFFFFFFu);
        return McmSettingsFile::CanonicalWeatherKey(std::string(owner) + "|" + hex.data());
    }
}
