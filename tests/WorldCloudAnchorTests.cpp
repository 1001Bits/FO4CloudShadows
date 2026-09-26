#include "WorldCloudAnchor.h"
#include <iostream>
#include <limits>
#include <stdexcept>

void Require(bool value, const char* reason)
{
    if (!value) throw std::runtime_error(reason);
}

int main()
{
    using Anchor = FO4CS::WorldCloudAnchor;
    using Result = Anchor::Confirmation;
    try {
        Anchor anchor;
        Require(!anchor.IsConfirmed(), "loading/provisional captures cannot supply a committed projection origin");
        Require(anchor.ConfirmMainView({0,0,std::numeric_limits<float>::quiet_NaN()}) ==
            Result::kRejected && !anchor.IsConfirmed(), "invalid camera cannot establish the origin");
        const Anchor::Point gameplay{-24040,-29150,1336};
        Require(anchor.ConfirmMainView(gameplay) == Result::kEstablished &&
            anchor.IsConfirmed() && anchor.Position() == gameplay,
            "first validated main lighting view establishes the actual gameplay origin");
        for (int step = 1; step < 10000; ++step) {
            const float displacement = static_cast<float>(step);
            Require(anchor.ConfirmMainView({gameplay[0] + displacement, gameplay[1] - displacement,
                gameplay[2] + displacement * 0.1f}) == Result::kUnchanged &&
                anchor.Position() == gameplay,
                "walking, climbing and later views must not move the world field");
        }
        Require(anchor.ConfirmMainView({0,0,std::numeric_limits<float>::infinity()}) ==
            Result::kRejected && anchor.Position() == gameplay,
            "bad later view cannot poison the retained anchor");
        // Toggling capture/publication does not reset this world-owned object.
        Require(anchor.ConfirmMainView({5000,9000,100}) == Result::kUnchanged,
            "a later ON after moving cannot silently reanchor the cloud pattern");
        anchor.Reset();
        Require(!anchor.IsConfirmed(), "world/load/device transition withdraws the old anchor");
        Require(anchor.ConfirmMainView({100,200,300}) == Result::kEstablished &&
            anchor.Position() == Anchor::Point{100,200,300},
            "a new world can establish its own validated anchor");
        // Travel re-anchoring: a cylinder of 1.5 cloud heights horizontally
        // and half a cloud height vertically around the origin.
        const Anchor::Point origin{ 1000, -2000, 300 };
        constexpr float height = 10000;
        Require(!FO4CS::NeedsReanchor(origin, origin, height),
            "standing at the origin never re-anchors");
        Require(!FO4CS::NeedsReanchor(origin, { 1000 + 14999, -2000, 300 }, height) &&
            FO4CS::NeedsReanchor(origin, { 1000 + 15001, -2000, 300 }, height),
            "horizontal travel re-anchors just beyond 1.5 cloud heights");
        Require(FO4CS::NeedsReanchor(origin, { 1000 + 10700, -2000 + 10700, 300 }, height),
            "diagonal travel uses the true horizontal distance");
        Require(!FO4CS::NeedsReanchor(origin, { 1000, -2000, 300 + 4999 }, height) &&
            FO4CS::NeedsReanchor(origin, { 1000, -2000, 300 + 5001 }, height) &&
            FO4CS::NeedsReanchor(origin, { 1000, -2000, 300 - 5001 }, height),
            "climbing or descending half a cloud height re-anchors");
        Require(!FO4CS::NeedsReanchor(origin, { 1e9f, 0, 0 }, 0.0f) &&
            !FO4CS::NeedsReanchor(origin,
                { std::numeric_limits<float>::quiet_NaN(), 0, 0 }, height),
            "invalid inputs never move the field");
        Require(FO4CS::CrossfadeWeight(0, 4000) == 1.0f &&
            FO4CS::CrossfadeWeight(4000, 4000) == 0.0f &&
            FO4CS::CrossfadeWeight(9000, 4000) == 0.0f &&
            FO4CS::CrossfadeWeight(10, 0) == 0.0f,
            "crossfade starts on the old origin and ends on the new one");
        float previousWeight = 1.0f;
        for (std::uint64_t elapsed = 0; elapsed <= 4000; elapsed += 50) {
            const float weight = FO4CS::CrossfadeWeight(elapsed, 4000);
            Require(weight <= previousWeight && weight >= 0.0f && weight <= 1.0f,
                "crossfade weight falls monotonically");
            previousWeight = weight;
        }
        Require(FO4CS::CrossfadeWeight(2000, 4000) == 0.5f,
            "crossfade is symmetric around its midpoint");
        Anchor travelling;
        Require(!travelling.Reanchor(origin), "an unconfirmed anchor cannot be moved by travel");
        Require(travelling.ConfirmMainView(origin) == Result::kEstablished &&
            travelling.Reanchor({ 50000, 0, 0 }) &&
            travelling.Position() == Anchor::Point{ 50000, 0, 0 },
            "a confirmed anchor follows long-distance travel");
        Require(!travelling.Reanchor({ 0, std::numeric_limits<float>::infinity(), 0 }) &&
            travelling.Position() == Anchor::Point{ 50000, 0, 0 },
            "a bad camera cannot move the anchor");
        using FO4CS::ClassifyPresentWorld;
        using Action = FO4CS::PresentWorldAction;
        constexpr std::uintptr_t commonwealth = 0x1000, diamondCity = 0x2000;
        // Regression: an unestablished world (startup, after a load reset, or
        // shadows disabled) used to count as a change on every Present.
        for (int present = 0; present < 1000; ++present)
            Require(ClassifyPresentWorld(false, 0, commonwealth) == Action::kWait,
                "Present waits for the main draw instead of resetting an empty field");
        Require(ClassifyPresentWorld(false, 0, 0) == Action::kWait,
            "no live world and none established is not a transition");
        Require(ClassifyPresentWorld(true, 0, commonwealth) == Action::kReset,
            "an explicit load/interior reset still resets");
        Require(ClassifyPresentWorld(true, commonwealth, commonwealth) == Action::kReset,
            "an explicit reset in an unchanged world still resets");
        Require(ClassifyPresentWorld(false, commonwealth, diamondCity) == Action::kReset,
            "a real world change seen by Present resets the field");
        Require(ClassifyPresentWorld(false, commonwealth, commonwealth) == Action::kContinue,
            "an unchanged established world keeps its field (VR repeated Presents)");
        std::cout << "World cloud anchor tests passed: valid gameplay origin, travel, toggles, resets, "
            "Present world transitions.\n";
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
