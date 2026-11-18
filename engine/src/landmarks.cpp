#include "bt/landmarks.h"

#include <cmath>
#include <stdexcept>

namespace bt {
namespace topology {
const std::vector<int> face_oval = {10,  338, 297, 332, 284, 251, 389, 356, 454, 323, 361, 288,
                                    397, 365, 379, 378, 400, 377, 152, 148, 176, 149, 150, 136,
                                    172, 58,  132, 93,  234, 127, 162, 21,  54,  103, 67,  109};
const std::vector<int> left_eye = {33, 7, 163, 144, 145, 153, 154, 155, 133, 173, 157, 158, 159, 160, 161, 246};
const std::vector<int> right_eye = {362, 382, 381, 380, 374, 373, 390, 249, 263, 466, 388, 387, 386, 385, 384, 398};
const std::vector<int> left_brow = {70, 63, 105, 66, 107, 55, 65, 52, 53, 46};
const std::vector<int> right_brow = {300, 293, 334, 296, 336, 285, 295, 282, 283, 276};
const std::vector<int> lips = {61, 146, 91,  181, 84,  17,  314, 405, 321, 375, 291, 409, 270, 269,
                               267, 0,  37,  39,  40,  185, 78,  95,  88,  178, 87,  14,  317, 402,
                               318, 324, 308, 415, 310, 311, 312, 13,  82,  81,  80,  191};
const std::vector<int> nose = {1, 2, 4, 94, 98, 327, 129, 358};
}  // namespace topology

namespace {
struct Support {
    int index;
    double weight;
};
// Rigid across expressions. Eye corners and the nose bridge carry the most weight.
const std::vector<Support> kSupport = {
    {33, 2.0},  {133, 2.0}, {362, 2.0}, {263, 2.0},                         // eye corners
    {6, 2.0},   {168, 2.0}, {197, 2.0}, {195, 2.0}, {5, 1.5}, {4, 1.0},     // nose bridge to tip
    {10, 1.0},  {151, 1.0}, {9, 1.0},   {8, 1.5},                           // forehead midline
    {127, 1.0}, {356, 1.0}, {162, 1.0}, {389, 1.0}, {21, 0.7}, {251, 0.7},  // temples
    {234, 0.7}, {454, 0.7},                                                 // upper cheek edge
};
}  // namespace

void require_valid(const FaceLandmarks& face) {
    if (int(face.points.size()) < topology::kMinLandmarks)
        throw std::invalid_argument("FaceLandmarks: expected a full face mesh of at least 468 points");
    for (Vec2 p : face.points)
        if (!std::isfinite(p.x) || !std::isfinite(p.y))
            throw std::invalid_argument("FaceLandmarks: non-finite landmark");
}

double interocular_distance(const FaceLandmarks& f) {
    require_valid(f);
    Vec2 l = (f.points[33] + f.points[133]) * 0.5, r = (f.points[362] + f.points[263]) * 0.5;
    return norm(l - r);
}

std::vector<Vec2> select(const FaceLandmarks& face, const std::vector<int>& indices) {
    std::vector<Vec2> out;
    out.reserve(indices.size());
    for (int i : indices) out.push_back(face.points.at(i));
    return out;
}

HeadAlignment estimate_head_alignment(const FaceLandmarks& donor, const FaceLandmarks& base) {
    require_valid(donor);
    require_valid(base);
    std::vector<Vec2> src, dst;
    std::vector<double> w0, w;
    for (const Support& s : kSupport) {
        src.push_back(donor.points[s.index]);
        dst.push_back(base.points[s.index]);
        w0.push_back(s.weight);
    }
    w = w0;
    double iod = interocular_distance(base);
    if (iod <= 0) throw std::invalid_argument("estimate_head_alignment: degenerate base face");
    HeadAlignment out;
    // Iteratively reweighted fit: down-weight support points that disagree
    // (for example a temple hidden by hair) without letting them drag the fit.
    for (int iter = 0; iter < 4; ++iter) {
        out.base_from_donor = estimate_similarity(src, dst, w);
        const double huber = 0.03 * iod;
        for (size_t i = 0; i < src.size(); ++i) {
            double r = norm(out.base_from_donor.apply(src[i]) - dst[i]);
            w[i] = w0[i] * (r <= huber ? 1.0 : huber / r);
        }
    }
    double ss = 0, ws = 0;
    for (size_t i = 0; i < src.size(); ++i) {
        double r = norm(out.base_from_donor.apply(src[i]) - dst[i]);
        ss += w0[i] * r * r;
        ws += w0[i];
    }
    out.support_residual = std::sqrt(ss / ws) / iod;
    return out;
}

}  // namespace bt
