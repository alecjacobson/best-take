// Procedural face scene with exact ground truth: the same "person" can be
// rendered under any similarity, exposure gain, and expression.
#pragma once
#include <cmath>

#include "bt/compose.h"

namespace fixture {

struct Pose {
    bt::Similarity image_from_face;  ///< Face units (interocular distance = 1) to image pixels.
    double smile = 0;                ///< 0 = closed neutral mouth, 1 = wide open smile.
    double blink = 0;                ///< 0 = eyes open, 1 = closed.
    double gain = 1;                 ///< Exposure gain on the whole image.
};

inline double hash_noise(int x, int y) {
    unsigned h = unsigned(x) * 374761393u + unsigned(y) * 668265263u;
    h = (h ^ (h >> 13)) * 1274126177u;
    return ((h ^ (h >> 16)) & 0xffff) / 65535.0;
}

inline double smooth_noise(double x, double y) {
    int ix = int(std::floor(x)), iy = int(std::floor(y));
    double fx = x - ix, fy = y - iy;
    fx = fx * fx * (3 - 2 * fx), fy = fy * fy * (3 - 2 * fy);
    double a = hash_noise(ix, iy), b = hash_noise(ix + 1, iy), c = hash_noise(ix, iy + 1), d = hash_noise(ix + 1, iy + 1);
    return (a * (1 - fx) + b * fx) * (1 - fy) + (c * (1 - fx) + d * fx) * fy;
}

inline bool in_ellipse(double x, double y, double cx, double cy, double rx, double ry) {
    double u = (x - cx) / rx, v = (y - cy) / ry;
    return u * u + v * v <= 1;
}

/// Linear RGB of the scene at face coordinates (x, y).
inline void scene_color(double x, double y, const Pose& p, float rgb[3]) {
    double tex = 0.6 * smooth_noise(x * 9 + 40, y * 9 + 40) + 0.4 * smooth_noise(x * 31 + 7, y * 31 + 90);
    if (in_ellipse(x, y, 0, 0.1, 1.15, 1.5)) {
        double shade = 0.85 + 0.15 * tex + 0.08 * x;  // skin with a lighting gradient
        rgb[0] = float(0.55 * shade), rgb[1] = float(0.36 * shade), rgb[2] = float(0.27 * shade);
        double open = 0.11 * (1 - p.blink) + 0.012;
        for (double ex : {-0.5, 0.5})
            if (in_ellipse(x, y, ex, -0.3, 0.2, open)) rgb[0] = 0.9f * float(tex) * 0.2f + 0.04f, rgb[1] = 0.05f, rgb[2] = 0.06f;
        for (double ex : {-0.5, 0.5})
            if (in_ellipse(x, y, ex, -0.55, 0.26, 0.035)) rgb[0] = 0.10f, rgb[1] = 0.07f, rgb[2] = 0.05f;
        if (in_ellipse(x, y, 0, 0.75, 0.3 + 0.12 * p.smile, 0.05 + 0.13 * p.smile)) {
            rgb[0] = 0.45f, rgb[1] = 0.08f, rgb[2] = 0.09f;
            if (p.smile > 0.3 && in_ellipse(x, y, 0, 0.72, 0.24 + 0.1 * p.smile, 0.07 * p.smile))
                rgb[0] = rgb[1] = rgb[2] = 0.8f;  // teeth
        }
    } else {
        double b = 0.25 + 0.5 * smooth_noise(x * 2.5 + 11, y * 2.5 + 3) + 0.15 * tex;
        rgb[0] = float(0.3 * b), rgb[1] = float(0.5 * b), rgb[2] = float(0.6 * b);
    }
    for (int c = 0; c < 3; ++c) rgb[c] *= float(p.gain);
}

inline bt::Image render(int w, int h, const Pose& p) {
    bt::Image img(w, h, 3);
    bt::Similarity inv = p.image_from_face.inverse();
    // 2x2 supersampling keeps feature edges band-limited like a real photo.
    for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x) {
            float acc[3] = {0, 0, 0}, rgb[3];
            for (int k = 0; k < 4; ++k) {
                bt::Vec2 f = inv.apply({x + 0.25 + 0.5 * (k & 1), y + 0.25 + 0.5 * (k >> 1)});
                scene_color(f.x, f.y, p, rgb);
                for (int c = 0; c < 3; ++c) acc[c] += rgb[c] / 4;
            }
            for (int c = 0; c < 3; ++c) img.at(x, y, c) = acc[c];
        }
    return img;
}

/// Landmarks consistent with the rendered scene.
inline bt::FaceLandmarks landmarks(const Pose& p) {
    using namespace bt;
    std::vector<Vec2> f(478, Vec2{0, 0.1});
    auto ring = [&](const std::vector<int>& idx, double cx, double cy, double rx, double ry, double phase) {
        for (size_t i = 0; i < idx.size(); ++i) {
            double t = phase + 2 * M_PI * i / idx.size();
            f[idx[i]] = {cx + rx * std::cos(t), cy + ry * std::sin(t)};
        }
    };
    ring(topology::face_oval, 0, 0.1, 1.15, 1.5, -M_PI / 2);
    double open = 0.11 * (1 - p.blink) + 0.012;
    ring(topology::left_eye, -0.5, -0.3, 0.2, open, M_PI);   // 33 outer corner, 133 inner
    ring(topology::right_eye, 0.5, -0.3, 0.2, open, M_PI);   // 362 inner corner, 263 outer
    ring(topology::left_brow, -0.5, -0.55, 0.26, 0.035, 0);
    ring(topology::right_brow, 0.5, -0.55, 0.26, 0.035, 0);
    ring(topology::lips, 0, 0.75, 0.3 + 0.12 * p.smile, 0.05 + 0.13 * p.smile, 0);
    const int nose_ids[8] = {1, 2, 4, 94, 98, 327, 129, 358};
    const Vec2 nose_pos[8] = {{0, 0.3}, {0, 0.4}, {0, 0.27}, {0, 0.38}, {-0.15, 0.36}, {0.15, 0.36}, {-0.2, 0.33}, {0.2, 0.33}};
    for (int i = 0; i < 8; ++i) f[nose_ids[i]] = nose_pos[i];
    f[6] = {0, -0.3}, f[168] = {0, -0.36}, f[197] = {0, -0.1}, f[195] = {0, 0.05}, f[5] = {0, 0.18};
    f[151] = {0, -0.95}, f[9] = {0, -0.62}, f[8] = {0, -0.45};
    FaceLandmarks out;
    for (Vec2 q : f) out.points.push_back(p.image_from_face.apply(q));
    return out;
}

inline bt::Similarity make_similarity(double scale, double degrees, double tx, double ty) {
    double t = degrees * M_PI / 180;
    return bt::Similarity{scale * std::cos(t), scale * std::sin(t), tx, ty};
}

}  // namespace fixture
