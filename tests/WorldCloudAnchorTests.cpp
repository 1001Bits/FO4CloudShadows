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
        std::cout << "World cloud anchor tests passed: valid gameplay origin, travel, toggles, resets.\n";
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
