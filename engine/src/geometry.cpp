#include "bt/geometry.h"

#include <algorithm>
#include <cmath>
#include <stdexcept>

namespace bt {

double norm(Vec2 v) { return std::hypot(v.x, v.y); }
double Similarity::scale() const { return std::hypot(a, b); }
double Similarity::rotation() const { return std::atan2(b, a); }

Similarity Similarity::inverse() const {
    double d = a * a + b * b;
    if (d <= 0) throw std::invalid_argument("Similarity::inverse: singular transform");
    Similarity r;
    r.a = a / d;
    r.b = -b / d;
    r.tx = -(r.a * tx - r.b * ty);
    r.ty = -(r.b * tx + r.a * ty);
    return r;
}

Similarity compose(const Similarity& s, const Similarity& f) {
    Similarity r;
    r.a = s.a * f.a - s.b * f.b;
    r.b = s.b * f.a + s.a * f.b;
    Vec2 t = s.apply({f.tx, f.ty});
    r.tx = t.x;
    r.ty = t.y;
    return r;
}

Similarity estimate_similarity(const std::vector<Vec2>& src, const std::vector<Vec2>& dst,
                               const std::vector<double>& w) {
    if (src.size() != dst.size() || src.size() != w.size())
        throw std::invalid_argument("estimate_similarity: size mismatch");
    if (src.size() < 2) throw std::invalid_argument("estimate_similarity: need at least two points");
    double ws = 0;
    Vec2 cs, cd;
    for (size_t i = 0; i < src.size(); ++i) {
        if (w[i] < 0) throw std::invalid_argument("estimate_similarity: negative weight");
        ws += w[i];
        cs = cs + src[i] * w[i];
        cd = cd + dst[i] * w[i];
    }
    if (ws <= 0) throw std::invalid_argument("estimate_similarity: weights sum to zero");
    cs = cs * (1 / ws);
    cd = cd * (1 / ws);
    double num_a = 0, num_b = 0, den = 0;
    for (size_t i = 0; i < src.size(); ++i) {
        Vec2 p = src[i] - cs, q = dst[i] - cd;
        num_a += w[i] * (p.x * q.x + p.y * q.y);
        num_b += w[i] * (p.x * q.y - p.y * q.x);
        den += w[i] * (p.x * p.x + p.y * p.y);
    }
    if (den <= 1e-12) throw std::invalid_argument("estimate_similarity: degenerate source points");
    Similarity t;
    t.a = num_a / den;
    t.b = num_b / den;
    t.tx = cd.x - (t.a * cs.x - t.b * cs.y);
    t.ty = cd.y - (t.b * cs.x + t.a * cs.y);
    return t;
}

Image warp_similarity(const Image& src, const Similarity& dst_from_src, const Rect& r, double dst_scale,
                      double src_scale, Vec2 src_origin) {
    if (r.empty()) throw std::invalid_argument("warp_similarity: empty destination rectangle");
    Similarity inv = dst_from_src.inverse();
    Image out(r.width(), r.height(), src.channels);
    for (int y = 0; y < out.height; ++y)
        for (int x = 0; x < out.width; ++x) {
            Vec2 p{(r.x0 + x + 0.5) / dst_scale, (r.y0 + y + 0.5) / dst_scale};
            Vec2 q = inv.apply(p);
            double sx = q.x * src_scale - src_origin.x, sy = q.y * src_scale - src_origin.y;
            for (int c = 0; c < src.channels; ++c) out.at(x, y, c) = sample_bicubic(src, sx, sy, c);
        }
    return out;
}

static double cross(Vec2 o, Vec2 a, Vec2 b) { return (a.x - o.x) * (b.y - o.y) - (a.y - o.y) * (b.x - o.x); }

std::vector<Vec2> convex_hull(std::vector<Vec2> p) {
    std::sort(p.begin(), p.end(), [](Vec2 a, Vec2 b) { return a.x < b.x || (a.x == b.x && a.y < b.y); });
    size_t n = p.size(), k = 0;
    if (n < 3) return p;
    std::vector<Vec2> h(2 * n);
    for (size_t i = 0; i < n; ++i) {
        while (k >= 2 && cross(h[k - 2], h[k - 1], p[i]) <= 0) --k;
        h[k++] = p[i];
    }
    for (size_t i = n - 1, t = k + 1; i-- > 0;) {
        while (k >= t && cross(h[k - 2], h[k - 1], p[i]) <= 0) --k;
        h[k++] = p[i];
    }
    h.resize(k - 1);
    return h;
}

static double signed_area(const std::vector<Vec2>& p) {
    double a = 0;
    for (size_t i = 0; i < p.size(); ++i) {
        Vec2 u = p[i], v = p[(i + 1) % p.size()];
        a += u.x * v.y - v.x * u.y;
    }
    return 0.5 * a;
}

// Keep the part of `poly` on the left of the directed line a->b shifted left by `shift`.
static std::vector<Vec2> clip_half_plane(const std::vector<Vec2>& poly, Vec2 a, Vec2 b, double shift) {
    Vec2 d = b - a;
    double len = norm(d);
    if (len <= 0) return poly;
    Vec2 n{-d.y / len, d.x / len};
    auto side = [&](Vec2 p) { return (p.x - a.x) * n.x + (p.y - a.y) * n.y - shift; };
    std::vector<Vec2> out;
    for (size_t i = 0; i < poly.size(); ++i) {
        Vec2 p = poly[i], q = poly[(i + 1) % poly.size()];
        double sp = side(p), sq = side(q);
        if (sp >= 0) out.push_back(p);
        if ((sp >= 0) != (sq >= 0)) out.push_back(p + (q - p) * (sp / (sp - sq)));
    }
    return out;
}

static std::vector<Vec2> ccw(std::vector<Vec2> p) {
    if (signed_area(p) < 0) std::reverse(p.begin(), p.end());
    return p;
}

std::vector<Vec2> convex_intersection(const std::vector<Vec2>& a, const std::vector<Vec2>& b_in) {
    std::vector<Vec2> out = a, b = ccw(b_in);
    for (size_t i = 0; i < b.size() && !out.empty(); ++i) out = clip_half_plane(out, b[i], b[(i + 1) % b.size()], 0);
    return out;
}

std::vector<Vec2> convex_offset(const std::vector<Vec2>& poly_in, double distance) {
    std::vector<Vec2> poly = ccw(poly_in);
    if (distance >= 0) {
        std::vector<Vec2> out = poly;
        for (size_t i = 0; i < poly.size() && !out.empty(); ++i)
            out = clip_half_plane(out, poly[i], poly[(i + 1) % poly.size()], distance);
        return out;
    }
    // Outward: hull of discs around every vertex.
    std::vector<Vec2> pts;
    const int steps = 16;
    for (Vec2 v : poly)
        for (int k = 0; k < steps; ++k) {
            double t = 2 * M_PI * k / steps;
            pts.push_back({v.x - distance * std::cos(t), v.y - distance * std::sin(t)});
        }
    return convex_hull(pts);
}

Vec2 centroid(const std::vector<Vec2>& p) {
    double a = 0;
    Vec2 c;
    for (size_t i = 0; i < p.size(); ++i) {
        Vec2 u = p[i], v = p[(i + 1) % p.size()];
        double w = u.x * v.y - v.x * u.y;
        a += w;
        c.x += (u.x + v.x) * w;
        c.y += (u.y + v.y) * w;
    }
    if (std::abs(a) < 1e-12) throw std::invalid_argument("centroid: degenerate polygon");
    return {c.x / (3 * a), c.y / (3 * a)};
}

std::vector<double> radial_profile(const std::vector<Vec2>& poly, Vec2 c, int count) {
    if (poly.size() < 3 || count < 3) throw std::invalid_argument("radial_profile: need a polygon and >= 3 angles");
    std::vector<double> r(count, 0.0);
    for (int k = 0; k < count; ++k) {
        double t = 2 * M_PI * k / count;
        Vec2 d{std::cos(t), std::sin(t)};
        double best = -1;
        for (size_t i = 0; i < poly.size(); ++i) {
            Vec2 p = poly[i] - c, e = poly[(i + 1) % poly.size()] - poly[i];
            double den = d.x * e.y - d.y * e.x;
            if (std::abs(den) < 1e-14) continue;
            double s = (p.x * e.y - p.y * e.x) / den;  // distance along the ray
            double u = (p.x * d.y - p.y * d.x) / den;  // position along the edge
            if (s >= 0 && u >= -1e-9 && u <= 1 + 1e-9) best = std::max(best, s);
        }
        if (best < 0) throw std::invalid_argument("radial_profile: center is outside the polygon");
        r[k] = best;
    }
    return r;
}

}  // namespace bt
