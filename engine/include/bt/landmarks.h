// Face landmark topology (MediaPipe Face Mesh, 468 or 478 points) and the
// expression-preserving head alignment built on it.
#pragma once
#include <vector>

#include "bt/geometry.h"

namespace bt {

/// Landmarks of one face in upright original-image pixel coordinates.
struct FaceLandmarks {
    std::vector<Vec2> points;
};

namespace topology {
extern const std::vector<int> face_oval;      ///< Ordered loop around the face silhouette.
extern const std::vector<int> left_eye;       ///< Eyelid contour (image-left eye).
extern const std::vector<int> right_eye;
extern const std::vector<int> left_brow;
extern const std::vector<int> right_brow;
extern const std::vector<int> lips;           ///< Outer and inner lip contours.
extern const std::vector<int> nose;           ///< Tip, base, and alar points.
constexpr int kMinLandmarks = 468;
}  // namespace topology

/// Throws std::invalid_argument unless the face has a full mesh of finite points.
void require_valid(const FaceLandmarks& face);

/// Distance between the eye centers (mean of each eye's corners).
double interocular_distance(const FaceLandmarks& face);

/// Points of the listed indices.
std::vector<Vec2> select(const FaceLandmarks& face, const std::vector<int>& indices);

struct HeadAlignment {
    Similarity base_from_donor;
    /// RMS residual on the rigid support after alignment, divided by the
    /// base interocular distance. Large values mean the head pose differs.
    double support_residual = 0;
};

/// Align the donor head to the base head with a similarity fitted only to
/// landmarks that stay rigid across expressions (nose bridge, eye corners,
/// forehead, temples). Mouth, eyelid, brow, and jaw landmarks are excluded so
/// the alignment cannot cancel the donor's expression.
HeadAlignment estimate_head_alignment(const FaceLandmarks& donor, const FaceLandmarks& base);

}  // namespace bt
