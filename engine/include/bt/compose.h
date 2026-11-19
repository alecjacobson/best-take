// Deterministic face replacement: color fit, seam placement, anchored
// blending, and validation of one donor face against a base photograph.
#pragma once
#include <string>
#include <vector>

#include "bt/regions.h"

namespace bt {

/// Pixels of part of a source image. `image` covers `rect`, expressed in
/// pixels at `scale` relative to the upright original; `bounds` is the whole
/// source image at the same scale.
struct ImageView {
    const Image* image = nullptr;
    Rect rect;
    Rect bounds;
    double scale = 1;
};
/// View of a complete image held at `scale`.
ImageView full_view(const Image& image, double scale = 1);

/// Bounded low-frequency exposure and color correction applied to the donor:
/// per channel, log gain = c0 + c1 u + c2 v with (u, v) the offset from the
/// region center in base interocular distances.
struct ColorFit {
    double coef[3][3] = {{0, 0, 0}, {0, 0, 0}, {0, 0, 0}};
    double unit = 1;  ///< Base interocular distance in original pixels.
};

enum class BlendMode {
    Multiband = 0,         ///< Laplacian-pyramid blend anchored at the core and exterior.
    ScreenedGradient = 1,  ///< Screened gradient-domain solve on the transition band.
};

/// Everything needed to re-render one replacement at any resolution.
struct Replacement {
    int person_id = -1;
    std::string donor_id;
    Similarity base_from_donor;
    EditRegions regions;
    std::vector<double> seam;  ///< Radial profile between core and outer.
    double feather = 0;        ///< Largest half-width of the alpha ramp, original pixels.
    ColorFit color;
    BlendMode blend = BlendMode::Multiband;
};

struct BuildOptions {
    BlendMode blend = BlendMode::Multiband;
    /// Analysis resolution: interocular distance in pixels, never above the base view's scale.
    double working_interocular_px = 96;
    /// Largest accepted rigid-support residual (interocular units).
    double max_support_residual = 0.09;
    /// Largest accepted donor enlargement (base pixels per donor pixel).
    double max_enlargement = 1.6;
    /// Smallest accepted common silhouette overlap.
    double min_silhouette_overlap = 0.72;
    /// Largest accepted share of expression-independent core skin whose
    /// brightness differs strongly between the photos after color matching.
    double max_core_mismatch = 0.2;
};

/// Measurements deciding whether a donor can fit a particular base.
struct Compatibility {
    bool feasible = false;
    std::string rejection;  ///< User-presentable reason; empty when feasible.
    double support_residual = 0;
    double enlargement = 1;
    double min_band_width = 0;
    double silhouette_overlap = 0;
    double color_log_gain = 0;  ///< Largest absolute fitted log gain.
    double seam_cost = 0;       ///< Mean base/donor disagreement along the chosen seam.
    double core_mismatch = 0;   ///< Share of expression-independent core skin that differs strongly.
};

/// Fit alignment, regions, color, and seam for placing `donor` into `base`.
/// Both views must cover the face. On an infeasible pair the returned report
/// names the reason and `out` is left unspecified.
Compatibility build_replacement(const ImageView& base, const FaceLandmarks& base_face, const ImageView& donor,
                                const FaceLandmarks& donor_face, const BuildOptions& options, Replacement* out);

/// Base-view pixels (at the base view's scale) that a replacement may modify.
Rect replacement_rect(const Replacement& r, const ImageView& base);
/// Donor pixels at `donor_scale` needed to render the replacement.
Rect donor_required_rect(const Replacement& r, double donor_scale, const Rect& donor_bounds);

struct PatchResult {
    Image pixels;  ///< Rendered pixels for `rect`; equal to the base outside the support.
    Rect rect;     ///< In base-view pixels.
    /// Mean gradient magnitude in the band beyond what either source has
    /// there (gamma-encoded units); a visible seam raises this value.
    double excess_gradient = 0;
};

/// Render one replacement at the base view's scale. Throws
/// std::invalid_argument if a view does not cover the pixels required.
PatchResult render_replacement(const Replacement& r, const ImageView& base, const ImageView& donor);

struct QualityReport {
    bool ok = false;
    std::string failure;
    int exterior_changed_pixels = 0;
    double excess_gradient = 0;
};

/// Provisional seam threshold; calibrate against reviewed real composites.
constexpr double kDefaultMaxExcessGradient = 0.003;

/// Check a rendered patch: finite values, no change outside the declared
/// support, and no excessive seam gradient.
QualityReport validate_patch(const Replacement& r, const ImageView& base, const PatchResult& patch,
                             double max_excess_gradient = kDefaultMaxExcessGradient);

}  // namespace bt
