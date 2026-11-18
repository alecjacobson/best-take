#include "bt/regions.h"

#include <algorithm>
#include <cmath>
#include <stdexcept>

namespace bt {

double profile_radius(const std::vector<double>& p, double theta) {
    const int n = int(p.size());
    double u = theta / (2 * M_PI) * n;
    u -= std::floor(u / n) * n;
    int i = int(std::floor(u)) % n;
    double f = u - std::floor(u);
    return p[i] * (1 - f) + p[(i + 1) % n] * f;
}

static double polygon_area(const std::vector<Vec2>& p) {
    double a = 0;
    for (size_t i = 0; i < p.size(); ++i) {
        Vec2 u = p[i], v = p[(i + 1) % p.size()];
        a += u.x * v.y - v.x * u.y;
    }
    return std::abs(a) * 0.5;
}

RegionReport build_regions(const FaceLandmarks& base, const FaceLandmarks& donor, const Similarity& T,
                           EditRegions* regions) {
    if (!regions) throw std::invalid_argument("build_regions: regions must not be null");
    require_valid(base);
    require_valid(donor);
    RegionReport rep;
    const double iod = interocular_distance(base);

    // Features that must be owned by the donor: both faces' eyes, brows, nose, lips.
    std::vector<Vec2> feat;
    for (const std::vector<int>* g : {&topology::left_eye, &topology::right_eye, &topology::left_brow,
                                      &topology::right_brow, &topology::lips, &topology::nose}) {
        for (int i : *g) {
            feat.push_back(base.points[i]);
            feat.push_back(T.apply(donor.points[i]));
        }
    }
    std::vector<Vec2> must = convex_offset(convex_hull(feat), -0.04 * iod);
    std::vector<Vec2> wish = convex_offset(must, -0.10 * iod);

    std::vector<Vec2> base_oval = convex_hull(select(base, topology::face_oval));
    std::vector<Vec2> donor_oval;
    for (Vec2 p : select(donor, topology::face_oval)) donor_oval.push_back(T.apply(p));
    donor_oval = convex_hull(donor_oval);
    std::vector<Vec2> common = convex_intersection(base_oval, donor_oval);
    if (common.size() < 3) {
        rep.rejection = "faces do not overlap after alignment";
        return rep;
    }
    rep.silhouette_overlap = polygon_area(common) / std::max(polygon_area(base_oval), polygon_area(donor_oval));
    std::vector<Vec2> outer_poly = convex_offset(common, 0.05 * iod);
    if (outer_poly.size() < 3) {
        rep.rejection = "face is too small to edit";
        return rep;
    }

    regions->center = centroid(must);
    const double min_band = 0.05 * iod;
    std::vector<double> r_must, r_wish, r_outer;
    try {
        r_must = radial_profile(must, regions->center, kRadialSamples);
        r_wish = radial_profile(wish, regions->center, kRadialSamples);
        r_outer = radial_profile(outer_poly, regions->center, kRadialSamples);
    } catch (const std::invalid_argument&) {
        rep.rejection = "facial features fall outside the shared silhouette";
        return rep;
    }
    regions->outer = r_outer;
    regions->core.resize(kRadialSamples);
    rep.min_band_width = 1e30;
    for (int k = 0; k < kRadialSamples; ++k) {
        // Prefer the generous core, but give way to keep a usable band; never
        // retreat inside the features that must be replaced.
        double core = std::max(r_must[k], std::min(r_wish[k], r_outer[k] - 3 * min_band));
        regions->core[k] = core;
        rep.min_band_width = std::min(rep.min_band_width, (r_outer[k] - core) / iod);
    }
    if (rep.min_band_width * iod < min_band) {
        rep.rejection = "head pose or jaw position differs too much";
        return rep;
    }
    rep.feasible = true;
    return rep;
}

Rect support_rect(const EditRegions& g, double scale) {
    double x0 = 1e30, y0 = 1e30, x1 = -1e30, y1 = -1e30;
    const int n = int(g.outer.size());
    for (int k = 0; k < n; ++k) {
        double t = 2 * M_PI * k / n;
        // Bound the linear-in-angle interpolation between samples by the larger neighbor.
        double r = std::max({g.outer[k], g.outer[(k + 1) % n], g.outer[(k + n - 1) % n]});
        x0 = std::min(x0, g.center.x + r * std::cos(t));
        x1 = std::max(x1, g.center.x + r * std::cos(t));
        y0 = std::min(y0, g.center.y + r * std::sin(t));
        y1 = std::max(y1, g.center.y + r * std::sin(t));
    }
    double pad = 2.0 / scale + (x1 - x0) * 0.02;
    return Rect{int(std::floor((x0 - pad) * scale)), int(std::floor((y0 - pad) * scale)),
                int(std::ceil((x1 + pad) * scale)), int(std::ceil((y1 + pad) * scale))};
}

}  // namespace bt
