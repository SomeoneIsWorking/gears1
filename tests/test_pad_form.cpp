#include "pad_form.h"

#include <cstdlib>
#include <iostream>
#include <string>
#include <string_view>

namespace
{

using gears::PadState;
using gears::product::ParsePadForm;

void Require(bool condition, std::string_view message)
{
    if (!condition)
    {
        std::cerr << "pad form: " << message << '\n';
        std::exit(EXIT_FAILURE);
    }
}

void RequireRefused(std::string_view fields, std::string_view expected)
{
    PadState pad;
    std::string error;
    Require(!ParsePadForm(fields, pad, error), "accepted invalid fields: " + std::string(fields));
    Require(error.find(expected) != std::string::npos,
            "refusal '" + error + "' does not name: " + std::string(expected));
}

} // namespace

int main()
{
    PadState pad;
    std::string error;
    Require(ParsePadForm("buttons=A,START&lx=-32767&ly=32767&rx=5&ry=-5&lt=255&rt=0", pad, error),
            error);
    PadState expected;
    expected.buttons = gears::kPadA | gears::kPadStart;
    expected.thumbLX = -32767;
    expected.thumbLY = 32767;
    expected.thumbRX = 5;
    expected.thumbRY = -5;
    expected.leftTrigger = 255;
    Require(pad == expected, "every field was not read");

    Require(ParsePadForm("", pad, error) && pad == PadState{}, "no fields is not a neutral pad");
    Require(ParsePadForm("buttons=", pad, error) && pad == PadState{},
            "an empty button list is not a release");
    Require(ParsePadForm("lt=9", pad, error) && pad.leftTrigger == 9 && pad.buttons == 0,
            "a previous state leaked into the next parse");

    RequireRefused("buttons=A,TYPO", "buttons");
    RequireRefused("buttons=A,", "buttons");
    RequireRefused("buttons=A,,B", "buttons");
    RequireRefused("lx=32768", "lx");
    RequireRefused("ly=-32768", "ly");
    RequireRefused("rt=256", "rt");
    RequireRefused("lt=1x", "lt");
    RequireRefused("wheel=1", "unknown pad field 'wheel'");
    std::cout << "pad form: 4 accepted, 8 refused\n";
    return EXIT_SUCCESS;
}
