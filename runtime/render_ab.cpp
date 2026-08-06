// The interleaved render comparer's report: given two per-draw signature files
// produced by the same process, name the first draw at which the renders
// diverge and say what changed.
//
// The point of doing this in-process, and of doing it here rather than in a
// script, is that the two arms must issue the SAME draws for "the first
// divergent draw" to mean anything. A knob that changes the draw stream -- the
// EDRAM tiling collapse does, 550 draws against 724 -- shifts every row after
// it, and a row-by-row comparison then compares two different draws and reports
// hundreds of differences that mean nothing. That is checked below and refused
// loudly, because it is the exact mistake this tool exists to stop.

#include "render_ab.h"

#include <lucent/log.h>

#include <fstream>
#include <sstream>
#include <string>
#include <vector>

namespace gears
{
namespace
{

struct Row
{
    std::string draw, diag, surface, psHash, hash;
    std::string maxR, maxG, maxB, meanR, meanG, meanB;
};

bool ReadRows(const std::string& path, std::vector<Row>& out, std::string& why)
{
    std::ifstream f(path);
    if (!f)
    {
        why = "cannot open " + path;
        return false;
    }
    std::string line;
    if (!std::getline(f, line))
    {
        why = path + " is empty";
        return false;
    }
    while (std::getline(f, line))
    {
        std::istringstream ls(line);
        Row r;
        std::getline(ls, r.draw, '\t');
        std::getline(ls, r.diag, '\t');
        std::getline(ls, r.surface, '\t');
        std::getline(ls, r.psHash, '\t');
        std::getline(ls, r.hash, '\t');
        std::getline(ls, r.maxR, '\t');
        std::getline(ls, r.maxG, '\t');
        std::getline(ls, r.maxB, '\t');
        std::getline(ls, r.meanR, '\t');
        std::getline(ls, r.meanG, '\t');
        std::getline(ls, r.meanB, '\t');
        if (!r.draw.empty())
            out.push_back(r);
    }
    if (out.empty())
    {
        why = path + " has a header but no rows";
        return false;
    }
    return true;
}

void Describe(const char* label, const Row& r)
{
    lucent::info("ab", "    {}: max {} {} {}   mean {} {} {}", label,
                 r.maxR, r.maxG, r.maxB, r.meanR, r.meanG, r.meanB);
}

} // namespace

bool ReportAbDivergence(const std::string& aPath, const std::string& bPath,
                        const std::string& knob)
{
    std::vector<Row> a, b;
    std::string why;
    if (!ReadRows(aPath, a, why) || !ReadRows(bPath, b, why))
    {
        // A comparison whose inputs are missing compared NOTHING, and must not
        // be readable as "the arms agree".
        lucent::error("ab", "REFUSING to report: {}. Nothing was compared", why);
        return false;
    }
    lucent::info("ab", "A/B on {}: arm A has {} draw(s), arm B has {}", knob,
                 a.size(), b.size());

    // The draw streams must line up before any pixel is compared.
    const size_t n = std::min(a.size(), b.size());
    for (size_t i = 0; i < n; ++i)
    {
        if (a[i].diag != b[i].diag)
        {
            lucent::error("ab", "THE DRAW STREAMS DIVERGE at row {}: arm A has"
                " guest draw {}, arm B has {}. The arms are not issuing the same"
                " draws, so no pixel comparison after this row is attributable"
                " to a draw. {} changes WHICH DRAWS RUN, not just how they"
                " render, and this comparer cannot root-cause that", i,
                a[i].diag, b[i].diag, knob);
            return false;
        }
    }
    if (a.size() != b.size())
    {
        lucent::error("ab", "arm A issued {} draws and arm B {}, with the shared"
            " rows identical. {} changes the draw stream's LENGTH, so this"
            " comparer cannot attribute the difference to a draw",
            a.size(), b.size(), knob);
        return false;
    }

    for (size_t i = 0; i < n; ++i)
    {
        if (a[i].hash == b[i].hash)
            continue;
        lucent::info("ab", "FIRST DIVERGENCE at row {} -- {} row(s) identical"
            " before it", i, i);
        lucent::info("ab", "  draw {} (guest {}) surface {} ps {}",
                     a[i].draw, a[i].diag, a[i].surface, a[i].psHash);
        Describe("arm A (no knob)", a[i]);
        Describe((knob + "=1").c_str(), b[i]);
        for (size_t k = (i >= 3 ? i - 3 : 0); k < i; ++k)
            lucent::info("ab", "  still identical: draw {} (guest {}) ps {}  "
                "max {} {} {}", a[k].draw, a[k].diag, a[k].psHash,
                a[k].maxR, a[k].maxG, a[k].maxB);
        lucent::info("ab", "  The first differing row is where the surfaces stop"
            " matching. It is the draw to look at, NOT proof that this draw is at"
            " fault: a difference introduced earlier can hide in the signature"
            " until a later draw magnifies it");
        return true;
    }

    // NO DIFFERENCE IS A REAL ANSWER, AND IT HAS TO SAY WHAT IT COULD NOT SEE.
    // Two arms that agree everywhere mean either the knob changed nothing about
    // this frame, or the knob never applied at all -- a value cached in a static
    // on first read cannot be toggled between two renders in one process. Those
    // are completely different conclusions and the caller cannot tell them apart
    // from silence.
    lucent::warn("ab", "NO DIVERGENCE: all {} rows identical. That means EITHER"
        " {} changes nothing about this frame, OR it never took effect -- a knob"
        " read once into a static cannot be toggled between two renders in one"
        " process. Before reading this as 'the knob does not matter', check that"
        " the run log shows the arm's own effect", n, knob);
    return true;
}

} // namespace gears
