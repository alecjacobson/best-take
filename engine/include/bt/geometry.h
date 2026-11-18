// Planar geometry: similarity transforms, convex polygons, warping.
#pragma once
#include <vector>

#include "bt/image.h"

namespace bt {

struct Vec2 {
    double x = 0, y = 0;
};
inline Vec2 operator+(Vec2 a, Vec2 b) { return {a.x + b.x, a.y + b.y}; }
inline Vec2 operator-(Vec2 a, Vec2 b) { return {a.x - b.x, a.y - b.y}; }
inline Vec2 operator*(Vec2 a, double s) { return {a.x * s, a.y * s}; }
double norm(Vec2 v);

/// Orientation-preserving similarity: x' = a x - b y + tx, y' = b x + a y + ty.
struct Similarity {
    double a = 1, b = 0, tx = 0, ty = 0;
    Vec2 apply(Vec2 p) const { return {a * p.x - b * p.y + tx, b * p.x + a * p.y + ty}; }
    double scale() const;
    double rotation() const;  ///< Radians, counterclockwise in image coordinates.
    Similarity inverse() const;
};
/// The transform that applies `first`, then `second`.
Similarity compose(const Similarity& second, const Similarity& first);

/// Weighted least-squares similarity mapping src points onto dst points.
/// Throws std::invalid_argument for mismatched sizes, fewer than two points,
/// or degenerate (coincident) source points.
Similarity estimate_similarity(const std::vector<Vec2>& src, const std::vector<Vec2>& dst,
                               const std::vector<double>& weights);

/// Resample src into dst_rect. Coordinates: dst pixel p (in dst-scaled pixels)
/// corresponds to original position p / dst_scale; dst_from_src maps original
/// source coordinates to original destination coordinates; src holds the
/// source rectangle src_origin + [0, w) x [0, h) at src_scale.
Image warp_similarity(const Image& src, const Similarity& dst_from_src, const Rect& dst_rect, double dst_scale = 1,
                      double src_scale = 1, Vec2 src_origin = {0, 0});

/// Counterclockwise-in-math-orientation convex hull (monotone chain).
std::vector<Vec2> convex_hull(std::vector<Vec2> pts);
/// Intersection of two convex polygons (either winding). May be empty.
std::vector<Vec2> convex_intersection(const std::vector<Vec2>& a, const std::vector<Vec2>& b);
/// Convex polygon moved inward (positive) or outward (negative) by `distance`.
std::vector<Vec2> convex_offset(const std::vector<Vec2>& poly, double distance);
Vec2 centroid(const std::vector<Vec2>& poly);
/// Distance from `center` (inside the polygon) to the boundary along each of
/// `count` evenly spaced directions, angle k being 2 pi k / count.
std::vector<double> radial_profile(const std::vector<Vec2>& convex_poly, Vec2 center, int count);

}  // namespace bt
