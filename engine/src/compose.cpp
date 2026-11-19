#include "bt/compose.h"

#include <algorithm>
#include <cmath>
#include <stdexcept>

namespace bt {
namespace {

constexpr double kEps = 1e-3;

struct Polar {
    double r, core, outer, seam;
};

Polar polar_at(const Replacement& rep, double ox, double oy) {
    double dx = ox - rep.regions.center.x, dy = oy - rep.regions.center.y;
    double th = std::atan2(dy, dx);
    return {std::hypot(dx, dy), profile_radius(rep.regions.core, th), profile_radius(rep.regions.outer, th),
            rep.seam.empty() ? 0.0 : profile_radius(rep.seam, th)};
}

double smoothstep(double t) {
    t = std::clamp(t, 0.0, 1.0);
    return t * t * (3 - 2 * t);
}

void require_cover(const ImageView& v, const Rect& need, const char* what) {
    if (!v.image || v.image->width != v.rect.width() || v.image->height != v.rect.height())
        throw std::invalid_argument(std::string(what) + ": view image does not match its rectangle");
    Rect n = intersect(need, v.bounds);
    if (!n.empty() && !v.rect.contains(n))
        throw std::invalid_argument(std::string(what) + ": view does not cover the required pixels");
}

// Resample part of a view into dst_rect at dst_scale through dst_from_src,
// low-pass filtering first when the mapping minifies.
Image warp_view(const ImageView& src, const Similarity& dst_from_src, const Rect& dst_rect, double dst_scale) {
    Similarity inv = dst_from_src.inverse();
    double x0 = 1e30, y0 = 1e30, x1 = -1e30, y1 = -1e30;
    for (int k = 0; k < 4; ++k) {
        Vec2 p{(k & 1 ? dst_rect.x1 : dst_rect.x0) / dst_scale, (k & 2 ? dst_rect.y1 : dst_rect.y0) / dst_scale};
        Vec2 q = inv.apply(p) * src.scale;
        x0 = std::min(x0, q.x), x1 = std::max(x1, q.x), y0 = std::min(y0, q.y), y1 = std::max(y1, q.y);
    }
    double minify = src.scale / (dst_scale * dst_from_src.scale());  // source pixels per output pixel
    double sigma = minify > 1.05 ? 0.45 * std::sqrt(minify * minify - 1) : 0;
    int pad = 3 + int(std::ceil(3 * sigma));
    Rect need{int(std::floor(x0)) - pad, int(std::floor(y0)) - pad, int(std::ceil(x1)) + pad,
              int(std::ceil(y1)) + pad};
    require_cover(src, need, "donor");
    Rect have = intersect(need, src.rect);
    if (have.empty()) throw std::invalid_argument("donor: view does not overlap the required pixels");
    Image part = crop(*src.image, Rect{have.x0 - src.rect.x0, have.y0 - src.rect.y0, have.x1 - src.rect.x0,
                                       have.y1 - src.rect.y0});
    if (sigma > 0) part = gaussian_blur(part, sigma);
    return warp_similarity(part, dst_from_src, dst_rect, dst_scale, src.scale, {double(have.x0), double(have.y0)});
}

Image extract(const ImageView& src, const Rect& dst_rect, double dst_scale) {
    if (dst_scale == src.scale) {
        require_cover(src, dst_rect, "base");
        return crop(*src.image, Rect{dst_rect.x0 - src.rect.x0, dst_rect.y0 - src.rect.y0, dst_rect.x1 - src.rect.x0,
                                     dst_rect.y1 - src.rect.y0});
    }
    return warp_view(src, Similarity{}, dst_rect, dst_scale);
}

void apply_color(Image& donor, const Rect& rect, double scale, const Replacement& rep) {
    for (int y = 0; y < donor.height; ++y)
        for (int x = 0; x < donor.width; ++x) {
            double u = ((rect.x0 + x + 0.5) / scale - rep.regions.center.x) / rep.color.unit;
            double v = ((rect.y0 + y + 0.5) / scale - rep.regions.center.y) / rep.color.unit;
            for (int c = 0; c < 3; ++c) {
                const double* k = rep.color.coef[c];
                donor.at(x, y, c) *= float(std::exp(k[0] + k[1] * u + k[2] * v));
            }
        }
}

// Robust plane fit y ~ c0 + c1 u + c2 v (Huber IRLS).
void fit_plane(const std::vector<double>& u, const std::vector<double>& v, const std::vector<double>& y,
               double c[3]) {
    std::vector<double> w(y.size(), 1.0);
    c[0] = c[1] = c[2] = 0;
    for (int iter = 0; iter < 6; ++iter) {
        double A[3][3] = {}, b[3] = {};
        for (size_t i = 0; i < y.size(); ++i) {
            double f[3] = {1, u[i], v[i]};
            for (int r = 0; r < 3; ++r) {
                b[r] += w[i] * f[r] * y[i];
                for (int s = 0; s < 3; ++s) A[r][s] += w[i] * f[r] * f[s];
            }
        }
        for (int r = 0; r < 3; ++r) A[r][r] += 1e-9;
        // Gaussian elimination with partial pivoting on the 3x3 system.
        int idx[3] = {0, 1, 2};
        for (int col = 0; col < 3; ++col) {
            int piv = col;
            for (int r = col + 1; r < 3; ++r)
                if (std::abs(A[idx[r]][col]) > std::abs(A[idx[piv]][col])) piv = r;
            std::swap(idx[col], idx[piv]);
            for (int r = col + 1; r < 3; ++r) {
                double f = A[idx[r]][col] / A[idx[col]][col];
                for (int s = col; s < 3; ++s) A[idx[r]][s] -= f * A[idx[col]][s];
                b[idx[r]] -= f * b[idx[col]];
            }
        }
        for (int r = 2; r >= 0; --r) {
            double s = b[idx[r]];
            for (int k = r + 1; k < 3; ++k) s -= A[idx[r]][k] * c[k];
            c[r] = s / A[idx[r]][r];
        }
        const double huber = 0.05;
        for (size_t i = 0; i < y.size(); ++i) {
            double res = std::abs(y[i] - (c[0] + c[1] * u[i] + c[2] * v[i]));
            w[i] = res <= huber ? 1.0 : huber / res;
        }
    }
}

Image encode(const Image& lin) {
    Image e = lin;
    for (float& v : e.data) v = std::sqrt(std::max(v, 0.f));
    return e;
}

Image pyr_down(const Image& img) {
    static const float k[5] = {1 / 16.f, 4 / 16.f, 6 / 16.f, 4 / 16.f, 1 / 16.f};
    Image out((img.width + 1) / 2, (img.height + 1) / 2, img.channels);
    for (int y = 0; y < out.height; ++y)
        for (int x = 0; x < out.width; ++x)
            for (int c = 0; c < img.channels; ++c) {
                float s = 0;
                for (int j = -2; j <= 2; ++j)
                    for (int i = -2; i <= 2; ++i) s += k[i + 2] * k[j + 2] * img.clamped(2 * x + i, 2 * y + j, c);
                out.at(x, y, c) = s;
            }
    return out;
}

Image pyr_up(const Image& img, int w, int h) {
    static const float k[5] = {1 / 16.f, 4 / 16.f, 6 / 16.f, 4 / 16.f, 1 / 16.f};
    Image out(w, h, img.channels);
    for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x)
            for (int c = 0; c < img.channels; ++c) {
                float s = 0;
                for (int j = -2; j <= 2; ++j) {
                    if ((y - j) & 1) continue;
                    for (int i = -2; i <= 2; ++i) {
                        if ((x - i) & 1) continue;
                        s += k[i + 2] * k[j + 2] * img.clamped((x - i) / 2, (y - j) / 2, c);
                    }
                }
                out.at(x, y, c) = 4 * s;
            }
    return out;
}

Image multiband(const Image& donor, const Image& base, const Image& alpha, int levels) {
    std::vector<Image> gd{donor}, gb{base}, ga{alpha};
    for (int l = 0; l < levels; ++l) {
        gd.push_back(pyr_down(gd.back()));
        gb.push_back(pyr_down(gb.back()));
        ga.push_back(pyr_down(ga.back()));
    }
    auto mix = [&](int l, const Image& d, const Image& b) {
        Image o(d.width, d.height, d.channels);
        for (int y = 0; y < d.height; ++y)
            for (int x = 0; x < d.width; ++x) {
                float a = ga[l].at(x, y);
                for (int c = 0; c < d.channels; ++c) o.at(x, y, c) = a * d.at(x, y, c) + (1 - a) * b.at(x, y, c);
            }
        return o;
    };
    Image acc = mix(levels, gd[levels], gb[levels]);
    for (int l = levels - 1; l >= 0; --l) {
        Image ud = pyr_up(gd[l + 1], gd[l].width, gd[l].height), ub = pyr_up(gb[l + 1], gb[l].width, gb[l].height);
        Image ld = gd[l], lb = gb[l];
        for (size_t i = 0; i < ld.data.size(); ++i) ld.data[i] -= ud.data[i], lb.data[i] -= ub.data[i];
        Image band = mix(l, ld, lb);
        acc = pyr_up(acc, gd[l].width, gd[l].height);
        for (size_t i = 0; i < acc.data.size(); ++i) acc.data[i] += band.data[i];
    }
    return acc;
}

// Screened gradient-domain blend. Unknowns are the band pixels; the donor
// core and the base exterior are fixed boundary values.
void screened_gradient(Image& out, const Image& donor, const Image& base, const Image& alpha,
                       const std::vector<uint8_t>& band) {
    const int w = out.width, h = out.height;
    const float lambda = 0.02f, omega = 1.85f;
    const int iterations = 300;
    static const int dx[4] = {1, -1, 0, 0}, dy[4] = {0, 0, 1, -1};
    for (int it = 0; it < iterations; ++it)
        for (int y = 0; y < h; ++y)
            for (int x = 0; x < w; ++x) {
                if (!band[size_t(y) * w + x]) continue;
                float a = alpha.at(x, y);
                for (int c = 0; c < 3; ++c) {
                    float u0 = a * donor.at(x, y, c) + (1 - a) * base.at(x, y, c);
                    float sum = lambda * u0;
                    int n = 0;
                    for (int k = 0; k < 4; ++k) {
                        int qx = x + dx[k], qy = y + dy[k];
                        if (qx < 0 || qy < 0 || qx >= w || qy >= h) continue;
                        float am = 0.5f * (a + alpha.at(qx, qy));
                        float g = am * (donor.at(x, y, c) - donor.at(qx, qy, c)) +
                                  (1 - am) * (base.at(x, y, c) - base.at(qx, qy, c));
                        sum += out.at(qx, qy, c) + g;
                        ++n;
                    }
                    float target = sum / (n + lambda);
                    out.at(x, y, c) += omega * (target - out.at(x, y, c));
                }
            }
}

}  // namespace

ImageView full_view(const Image& image, double scale) {
    Rect r{0, 0, image.width, image.height};
    return ImageView{&image, r, r, scale};
}

Rect replacement_rect(const Replacement& r, const ImageView& base) {
    return intersect(support_rect(r.regions, base.scale), base.bounds);
}

Rect donor_required_rect(const Replacement& r, double donor_scale, const Rect& donor_bounds) {
    Rect s = support_rect(r.regions, 1.0);
    Similarity inv = r.base_from_donor.inverse();
    double x0 = 1e30, y0 = 1e30, x1 = -1e30, y1 = -1e30;
    for (int k = 0; k < 4; ++k) {
        Vec2 q = inv.apply({double(k & 1 ? s.x1 : s.x0), double(k & 2 ? s.y1 : s.y0)}) * donor_scale;
        x0 = std::min(x0, q.x), x1 = std::max(x1, q.x), y0 = std::min(y0, q.y), y1 = std::max(y1, q.y);
    }
    // Margin for the interpolation kernel and for prefiltering when minifying.
    double pad = 8 + 0.1 * std::max(x1 - x0, y1 - y0);
    return intersect(Rect{int(std::floor(x0 - pad)), int(std::floor(y0 - pad)), int(std::ceil(x1 + pad)),
                          int(std::ceil(y1 + pad))},
                     donor_bounds);
}

Compatibility build_replacement(const ImageView& base, const FaceLandmarks& base_face, const ImageView& donor,
                                const FaceLandmarks& donor_face, const BuildOptions& opt, Replacement* out) {
    if (!out) throw std::invalid_argument("build_replacement: out must not be null");
    Compatibility rep;
    HeadAlignment align = estimate_head_alignment(donor_face, base_face);
    const double iod = interocular_distance(base_face);
    rep.support_residual = align.support_residual;
    rep.enlargement = align.base_from_donor.scale();
    if (rep.support_residual > opt.max_support_residual) {
        rep.rejection = "turned too far";
        return rep;
    }
    if (rep.enlargement > opt.max_enlargement) {
        rep.rejection = "too small in the source photo";
        return rep;
    }
    Replacement r;
    r.base_from_donor = align.base_from_donor;
    r.blend = opt.blend;
    RegionReport rr = build_regions(base_face, donor_face, r.base_from_donor, &r.regions);
    rep.min_band_width = rr.min_band_width;
    rep.silhouette_overlap = rr.silhouette_overlap;
    if (!rr.feasible) {
        rep.rejection = rr.rejection;
        return rep;
    }
    if (rr.silhouette_overlap < opt.min_silhouette_overlap) {
        rep.rejection = "head pose or jaw position differs too much";
        return rep;
    }

    // Working rasters at analysis resolution.
    const double ws = std::min(base.scale, opt.working_interocular_px / iod);
    Rect full{0, 0, int(std::ceil(base.bounds.x1 * ws / base.scale)), int(std::ceil(base.bounds.y1 * ws / base.scale))};
    Rect rect = intersect(support_rect(r.regions, ws), full);
    if (rect.empty()) {
        rep.rejection = "face is outside the photo";
        return rep;
    }
    Image B = extract(base, rect, ws);
    Image D = warp_view(donor, r.base_from_donor, rect, ws);

    // Color: low-frequency log ratio over the band, where both images show skin.
    r.color.unit = iod;
    {
        double sigma = std::max(1.0, 0.08 * iod * ws);
        Image Bb = gaussian_blur(B, sigma), Db = gaussian_blur(D, sigma);
        std::vector<double> u, v, y[3];
        for (int py = 0; py < B.height; ++py)
            for (int px = 0; px < B.width; ++px) {
                double ox = (rect.x0 + px + 0.5) / ws, oy = (rect.y0 + py + 0.5) / ws;
                Polar p = polar_at(r, ox, oy);
                if (p.r <= p.core || p.r >= p.outer) continue;
                u.push_back((ox - r.regions.center.x) / iod);
                v.push_back((oy - r.regions.center.y) / iod);
                for (int c = 0; c < 3; ++c)
                    y[c].push_back(std::log((Bb.at(px, py, c) + kEps) / (Db.at(px, py, c) + kEps)));
            }
        if (u.size() < 32) {
            rep.rejection = "face is too small to edit";
            return rep;
        }
        double mean0 = 0;
        for (int c = 0; c < 3; ++c) {
            fit_plane(u, v, y[c], r.color.coef[c]);
            mean0 += r.color.coef[c][0] / 3;
        }
        mean0 = std::clamp(mean0, -0.5, 0.5);
        for (int c = 0; c < 3; ++c) {
            double* k = r.color.coef[c];
            // Bound exposure, chroma shift, and spatial variation so the fit
            // cannot change skin tone or follow unrelated content.
            k[0] = std::clamp(k[0], mean0 - 0.12, mean0 + 0.12);
            k[1] = std::clamp(k[1], -0.15, 0.15);
            k[2] = std::clamp(k[2], -0.15, 0.15);
            rep.color_log_gain = std::max(rep.color_log_gain, std::abs(k[0]));
        }
    }
    apply_color(D, rect, ws, r);

    // Covered or changed appearance: skin inside the core that expression does
    // not move (away from the eyes, brows, and lips) should look alike in both
    // photos once color is matched. Sunglasses, a hand, or a hard shadow shows
    // up as a large share of such pixels differing strongly.
    {
        std::vector<Vec2> moving;
        for (const std::vector<int>* group :
             {&topology::left_eye, &topology::right_eye, &topology::left_brow, &topology::right_brow, &topology::lips})
            for (int i : *group) {
                moving.push_back(base_face.points[size_t(i)]);
                moving.push_back(r.base_from_donor.apply(donor_face.points[size_t(i)]));
            }
        const double keep_out = 0.12 * iod;
        double sigma = std::max(1.0, 0.05 * iod * ws);
        Image lb = gaussian_blur(luminance(B), sigma), ld = gaussian_blur(luminance(D), sigma);
        int counted = 0, differing = 0;
        for (int py = 0; py < B.height; ++py)
            for (int px = 0; px < B.width; ++px) {
                double ox = (rect.x0 + px + 0.5) / ws, oy = (rect.y0 + py + 0.5) / ws;
                Polar p = polar_at(r, ox, oy);
                if (p.r > p.core) continue;
                bool near_moving = false;
                for (const Vec2& m : moving)
                    if (std::hypot(m.x - ox, m.y - oy) < keep_out) {
                        near_moving = true;
                        break;
                    }
                if (near_moving) continue;
                ++counted;
                if (std::abs(std::log((lb.at(px, py) + kEps) / (ld.at(px, py) + kEps))) > 0.5) ++differing;
            }
        rep.core_mismatch = counted > 0 ? double(differing) / counted : 0;
        if (rep.core_mismatch > opt.max_core_mismatch) {
            rep.rejection = "something covers the face in one photo";
            return rep;
        }
    }

    // Seam: closed minimum-cost path in polar coordinates through the band.
    {
        Image eb = encode(B), ed = encode(D);
        Image cost(B.width, B.height, 1);
        for (int py = 0; py < B.height; ++py)
            for (int px = 0; px < B.width; ++px) {
                float s = 0;
                for (int c = 0; c < 3; ++c) {
                    float d0 = ed.at(px, py, c) - eb.at(px, py, c);
                    float gx = (ed.clamped(px + 1, py, c) - ed.clamped(px - 1, py, c)) -
                               (eb.clamped(px + 1, py, c) - eb.clamped(px - 1, py, c));
                    float gy = (ed.clamped(px, py + 1, c) - ed.clamped(px, py - 1, c)) -
                               (eb.clamped(px, py + 1, c) - eb.clamped(px, py - 1, c));
                    s += d0 * d0 + 0.5f * (gx * gx + gy * gy);
                }
                cost.at(px, py) = s;
            }
        cost = gaussian_blur(cost, 1.0);
        const int K = kRadialSamples, R = 17;
        std::vector<double> C(size_t(K) * R);
        double mean_cost = 0;
        auto frac = [&](int j) { return 0.25 + 0.5 * j / (R - 1); };
        for (int k = 0; k < K; ++k) {
            double th = 2 * M_PI * k / K;
            for (int j = 0; j < R; ++j) {
                double rad = r.regions.core[k] + frac(j) * (r.regions.outer[k] - r.regions.core[k]);
                double ox = r.regions.center.x + rad * std::cos(th), oy = r.regions.center.y + rad * std::sin(th);
                double centered = frac(j) - 0.5;
                C[size_t(k) * R + j] =
                    sample_bilinear(cost, ox * ws - rect.x0, oy * ws - rect.y0, 0) + 1e-5 * centered * centered;
                mean_cost += C[size_t(k) * R + j] / (K * R);
            }
        }
        const double step_penalty = 0.5 * mean_cost + 1e-7;
        double best = 1e300;
        std::vector<int> best_path(K);
        std::vector<double> acc(size_t(K) * R);
        std::vector<int> from(size_t(K) * R);
        for (int start = 0; start < R; ++start) {
            for (int j = 0; j < R; ++j) acc[j] = j == start ? C[j] : 1e300;
            for (int k = 1; k < K; ++k)
                for (int j = 0; j < R; ++j) {
                    double b = 1e300;
                    int bf = j;
                    for (int d = -1; d <= 1; ++d) {
                        int q = j + d;
                        if (q < 0 || q >= R) continue;
                        double val = acc[size_t(k - 1) * R + q] + (d ? step_penalty : 0);
                        if (val < b) b = val, bf = q;
                    }
                    acc[size_t(k) * R + j] = b >= 1e300 ? 1e300 : b + C[size_t(k) * R + j];
                    from[size_t(k) * R + j] = bf;
                }
            for (int d = -1; d <= 1; ++d) {
                int j = start + d;
                if (j < 0 || j >= R) continue;
                double total = acc[size_t(K - 1) * R + j] + (d ? step_penalty : 0);
                if (total < best) {
                    best = total;
                    int cur = j;
                    for (int k = K - 1; k >= 0; --k) {
                        best_path[k] = cur;
                        if (k > 0) cur = from[size_t(k) * R + cur];
                    }
                }
            }
        }
        r.seam.resize(K);
        for (int k = 0; k < K; ++k) {
            double f = (frac(best_path[(k + K - 1) % K]) + 2 * frac(best_path[k]) + frac(best_path[(k + 1) % K])) / 4;
            r.seam[k] = r.regions.core[k] + f * (r.regions.outer[k] - r.regions.core[k]);
            rep.seam_cost += C[size_t(k) * R + best_path[k]] / K;
        }
        r.feather = 0.15 * iod;
    }
    rep.feasible = true;
    *out = r;
    return rep;
}

PatchResult render_replacement(const Replacement& rep, const ImageView& base, const ImageView& donor) {
    if (rep.seam.size() != rep.regions.core.size() || rep.regions.core.size() != rep.regions.outer.size() ||
        rep.seam.empty())
        throw std::invalid_argument("render_replacement: incomplete replacement regions");
    PatchResult res;
    res.rect = replacement_rect(rep, base);
    if (res.rect.empty()) throw std::invalid_argument("render_replacement: support is outside the base image");
    const double s = base.scale;
    Image B = extract(base, res.rect, s);
    Image D = warp_view(donor, rep.base_from_donor, res.rect, s);
    apply_color(D, res.rect, s, rep);

    const int w = B.width, h = B.height;
    Image alpha(w, h, 1), mid(w, h, 1);
    std::vector<uint8_t> band(size_t(w) * h, 0), inside(size_t(w) * h, 0);
    double band_px = 0;
    int band_n = 0;
    for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x) {
            Polar p = polar_at(rep, (res.rect.x0 + x + 0.5) / s, (res.rect.y0 + y + 0.5) / s);
            if (p.r >= p.outer) continue;  // exterior: alpha 0
            inside[size_t(y) * w + x] = 1;
            if (p.r <= p.core) {
                alpha.at(x, y) = 1;
                continue;
            }
            band[size_t(y) * w + x] = 1;
            double half = std::max(1e-6, std::min({rep.feather, p.seam - p.core, p.outer - p.seam}));
            alpha.at(x, y) = float(smoothstep(0.5 - (p.r - p.seam) / (2 * half)));
            double width = p.outer - p.core;
            mid.at(x, y) = float(smoothstep((p.r - p.core) / (0.35 * width)) *
                                 smoothstep((p.outer - p.r) / (0.35 * width)));
            band_px += width * s;
            ++band_n;
        }
    res.pixels = B;
    if (band_n == 0) throw std::invalid_argument("render_replacement: transition band has no pixels at this scale");
    band_px /= band_n;

    if (rep.blend == BlendMode::Multiband) {
        int levels = std::clamp(int(std::floor(std::log2(std::max(2.0, band_px)))) - 1, 1, 5);
        Image M = multiband(D, B, alpha, levels);
        for (int y = 0; y < h; ++y)
            for (int x = 0; x < w; ++x) {
                if (!inside[size_t(y) * w + x]) continue;
                float a = alpha.at(x, y), m = mid.at(x, y);
                for (int c = 0; c < 3; ++c) {
                    float plain = a * D.at(x, y, c) + (1 - a) * B.at(x, y, c);
                    res.pixels.at(x, y, c) = m * M.at(x, y, c) + (1 - m) * plain;
                }
            }
    } else {
        for (int y = 0; y < h; ++y)
            for (int x = 0; x < w; ++x) {
                if (!inside[size_t(y) * w + x]) continue;
                float a = alpha.at(x, y);
                for (int c = 0; c < 3; ++c) res.pixels.at(x, y, c) = a * D.at(x, y, c) + (1 - a) * B.at(x, y, c);
            }
        screened_gradient(res.pixels, D, B, alpha, band);
    }

    // Seam visibility: band gradient beyond what either source shows there.
    Image eo = encode(res.pixels), ed = encode(D), eb = encode(B);
    double excess = 0;
    for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x) {
            if (!band[size_t(y) * w + x]) continue;
            auto grad = [&](const Image& im) {
                double g = 0;
                for (int c = 0; c < 3; ++c) {
                    double gx = im.clamped(x + 1, y, c) - im.clamped(x - 1, y, c);
                    double gy = im.clamped(x, y + 1, c) - im.clamped(x, y - 1, c);
                    g += gx * gx + gy * gy;
                }
                return 0.5 * std::sqrt(g);
            };
            excess += std::max(0.0, grad(eo) - std::max(grad(ed), grad(eb)));
        }
    res.excess_gradient = excess / band_n;
    return res;
}

QualityReport validate_patch(const Replacement& rep, const ImageView& base, const PatchResult& patch,
                             double max_excess_gradient) {
    QualityReport q;
    q.excess_gradient = patch.excess_gradient;
    Image B = extract(base, patch.rect, base.scale);
    if (B.width != patch.pixels.width || B.height != patch.pixels.height)
        throw std::invalid_argument("validate_patch: patch size does not match its rectangle");
    bool finite = true;
    for (int y = 0; y < B.height; ++y)
        for (int x = 0; x < B.width; ++x) {
            Polar p = polar_at(rep, (patch.rect.x0 + x + 0.5) / base.scale, (patch.rect.y0 + y + 0.5) / base.scale);
            bool changed = false;
            for (int c = 0; c < 3; ++c) {
                float v = patch.pixels.at(x, y, c);
                if (!std::isfinite(v)) finite = false;
                if (v != B.at(x, y, c)) changed = true;
            }
            if (changed && p.r >= p.outer) ++q.exterior_changed_pixels;
        }
    if (!finite)
        q.failure = "rendered patch contains non-finite values";
    else if (q.exterior_changed_pixels > 0)
        q.failure = "pixels outside the declared support changed";
    else if (q.excess_gradient > max_excess_gradient)
        q.failure = "visible seam";
    q.ok = q.failure.empty();
    return q;
}

}  // namespace bt
