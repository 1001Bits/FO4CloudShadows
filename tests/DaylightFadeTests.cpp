#include "DaylightFade.h"
#include <cmath>
#include <iostream>
#include <limits>
#include <stdexcept>

void Require(bool value, const char* reason)
{
    if (!value) throw std::runtime_error(reason);
}

int main()
{
    using namespace FO4CS::DaylightFade;
    try {
        Require(HoursFromClimateCount(36) == 6.0f, "a climate count is ten game minutes");
        Require(HoursFromClimateCount(0x9C) == 26.0f, "climate counts are unsigned");

        // Fallout4.esm DefaultClimate: sunrise 05:00-09:00, sunset 17:00-21:00.
        const ClimateHours climate{ 5.0f, 9.0f, 17.0f, 21.0f };
        Require(Factor(12.0f, climate) == 1.0f, "full strength at noon");
        Require(Factor(8.0f, climate) == 1.0f && Factor(18.0f, climate) == 1.0f,
            "full strength until the fade starts");
        Require(Factor(19.0f, climate) == 0.0f && Factor(7.0f, climate) == 0.0f,
            "gone once the disc reaches the horizon");
        Require(Factor(23.0f, climate) == 0.0f && Factor(0.0f, climate) == 0.0f &&
            Factor(3.0f, climate) == 0.0f, "off all night, on both sides of midnight");
        Require(Factor(20.0f, climate) == 0.0f && Factor(6.0f, climate) == 0.0f,
            "off while the hidden disc retraces the day's arc");
        Require(std::abs(Factor(18.5f, climate) - 0.5f) < 1e-5f &&
            std::abs(Factor(7.5f, climate) - 0.5f) < 1e-5f, "the fades are symmetric");

        // Fallout4.esm DiamondCityClimate: sunset 16:00-20:30, a different clock.
        const ClimateHours diamondCity{ 5.5f, 8.0f, 16.0f, 20.5f };
        Require(Factor(18.25f, diamondCity) == 0.0f && Factor(18.25f, climate) > 0.0f,
            "the current climate's sun times decide, not a fixed clock");

        float previous = 1.0f;
        for (float hour = 17.9f; hour <= 19.1f; hour += 1.0f / 60.0f) {
            const float value = Factor(hour, climate);
            Require(value <= previous, "dusk never brightens");
            Require(previous - value < 0.05f, "no visible step between two game minutes");
            previous = value;
        }

        const ClimateHours shortDay{ 11.0f, 11.0f, 12.0f, 12.0f };
        Require(Factor(11.5f, shortDay) == 1.0f && Factor(11.0f, shortDay) == 0.0f &&
            Factor(12.0f, shortDay) == 0.0f, "a short day still reaches full strength at its middle");

        const float nan = std::numeric_limits<float>::quiet_NaN();
        Require(Factor(23.0f, { 0.0f, 0.0f, 0.0f, 0.0f }) == 1.0f,
            "an unauthored climate leaves shadows on");
        Require(Factor(23.0f, { 20.0f, 21.0f, 5.0f, 6.0f }) == 1.0f,
            "a reversed climate leaves shadows on");
        Require(Factor(nan, climate) == 1.0f, "an unreadable clock leaves shadows on");
        Require(Factor(23.0f, { nan, 7.0f, 17.0f, 21.0f }) == 1.0f,
            "an unreadable climate leaves shadows on");

        std::cout << "PASS: daylight fade contract\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "FAIL: " << error.what() << "\n";
        return 1;
    }
}
