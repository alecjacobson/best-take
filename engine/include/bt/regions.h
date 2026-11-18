// Resolution-independent edit regions: protected donor core, transition band,
// and untouched base exterior, described as radial profiles about a center.
#pragma once
#include <string>
#include <vector>

#include "bt/landmarks.h"

namespace bt {

constexpr int kRadialSamples = 180;

/// Star-shaped regions about `center` in base original coordinates. For the
/// direction at angle 2 pi k / N: radius <= core[k] is protected donor
/// content, core[k] < radius < outer[k] is the transition band, and
/// radius >= outer[k] is untouched base.
struct EditRegions {
    Vec2 center;
    std::vector<double> core, outer;
};

/// Interpolated radius of a profile at angle theta (radians).
double profile_radius(const std::vector<double>& profile, double theta);

struct RegionReport {
    bool feasible = false;
    /// Narrowest band width divided by the base interocular distance.
    double min_band_width = 0;
    /// Area of the common silhouette over the area of the larger silhouette.
    double silhouette_overlap = 0;
    std::string rejection;  ///< Empty when feasible.
};

/// Build the conservative-path regions. The core covers the eyes, inner brows,
/// nose, and lips of both the aligned donor and the base, so no base feature
/// survives beside its replacement. The outer boundary stays inside the face
/// silhouette of both images, so the band crosses skin in both.
RegionReport build_regions(const FaceLandmarks& base, const FaceLandmarks& donor, const Similarity& base_from_donor,
                           EditRegions* regions);

/// Smallest pixel rectangle containing the outer boundary at `scale`.
Rect support_rect(const EditRegions& regions, double scale);

}  // namespace bt
