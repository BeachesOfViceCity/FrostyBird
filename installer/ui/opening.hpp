#pragma once
#include "menu_layout.hpp"
#include <algorithm>
#include <optional>

namespace tb {
// Displacements in the menu's 1280 × 720 coordinate system. The same pose
// positions the cached lettering on the GPU and unprojects pointer events.
struct OpeningPose {
    float light=1, railX=0, choicesX=0, footerY=0;
};

class Opening {
public:
    static constexpr double duration=1.0;

    void firstPresentation(double now) {
        if (!start_) start_=now;
    }
    void finish() { skipped_=true; }

    OpeningPose pose(double now) const {
        const double t=start_?now-*start_:0.;
        if (skipped_ || t>=duration) return {};
        auto phase=[&](double begin,double end) {
            return static_cast<float>(std::clamp((t-begin)/(end-begin),0.,1.));
        };
        auto travel=[&](double begin,double end) {
            float u=1-phase(begin,end);
            return u*u*u; // ease out to a stop, no bounce
        };
        float u=phase(0.,.66);
        return {u*u*(3-2*u),
                -layout::rail*travel(.045,.72),
                (1280-layout::left+2)*travel(.16,duration),
                (720-layout::footerTop)*travel(.34,.92)};
    }
private:
    std::optional<double> start_;
    bool skipped_{};
};
}
