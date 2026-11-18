// Editing session: the engine boundary used by the application and tools.
// Holds analysis for one set of related photos and produces proposals,
// candidates, recipes, and renders while loading only the pixels it needs.
#pragma once
#include <functional>
#include <map>
#include <memory>
#include <string>
#include <tuple>
#include <vector>

#include "bt/association.h"
#include "bt/recipe.h"
#include "bt/selection.h"

namespace bt {

/// One detected face as delivered by the analyzer.
struct FaceInput {
    FaceLandmarks landmarks;
    /// Facial-state fields (eyes_open, smile, gaze_forward, yaw, pitch,
    /// confidence, visibility) filled by the analyzer; technical fields are
    /// measured by the session.
    FaceMeasurements state;
    std::vector<float> embedding;  ///< Optional.
};

struct FrameSource {
    std::string id;
    std::string hash;
    int width = 0, height = 0;  ///< Upright original size.
    std::vector<FaceInput> faces;
    double scene_cost = 0;
};

/// Returns linear RGB pixels of `frame` for `rect`, where rect is expressed
/// in pixels at `scale` (1, 1/2, 1/4, or 1/8 of the original).
using RegionLoader = std::function<Image(int frame, const Rect& rect, double scale)>;

struct SessionOptions {
    Preference preference;
    BuildOptions build;
    /// Smallest interocular distance, in pixels, for a face to serve as donor.
    double min_donor_interocular_px = 24;
    /// A donor is rejected as too blurry when its sharpness is below this
    /// fraction of the sharpness of the base face it would replace.
    double min_relative_sharpness = 0.5;
};

struct Candidate {
    int frame = -1;
    bool is_base = false;      ///< The original face in the base photo.
    bool feasible = false;
    std::string rejection;     ///< User-presentable reason when not feasible.
    double face_cost = 0;
    double merge_cost = 0;
    Compatibility compatibility;  ///< Measurements behind the decision (defaults for the base).
};

class Session {
public:
    /// Throws std::invalid_argument for empty input or invalid landmarks.
    Session(std::vector<FrameSource> frames, RegionLoader loader, SessionOptions options = {});
    ~Session();

    int frame_count() const;
    int person_count() const;
    const std::vector<FrameSource>& frames() const;
    const AssociationResult& association() const;
    /// Index of the face of `person` in `frame`, or -1.
    int face_index(int person, int frame) const;

    /// Replace the user's association corrections and re-associate.
    void set_constraints(const std::vector<PairConstraint>& constraints);

    /// Recommended base and sources. `locks` maps person -> donor frame.
    SelectionResult propose(const std::map<int, int>& locks = {}, int locked_base = -1);

    /// Every observation of `person`, the base's own face first, then by
    /// increasing total cost; infeasible ones carry their reason.
    std::vector<Candidate> candidates(int person, int base);

    /// Recipe for explicit choices: sources[person] is a frame, or the base
    /// (or -1) for no change. Throws std::invalid_argument if a chosen donor
    /// is not feasible; nothing is substituted.
    Recipe build_recipe(int base, const std::vector<int>& sources);

    /// Render one replacement of a recipe at `scale`. Replacements of a
    /// recipe from build_recipe have disjoint supports, so their patches can
    /// be pasted onto the base independently.
    PatchResult render_replacement_patch(const Recipe& recipe, size_t index, double scale);
    /// Validate a patch from render_replacement_patch against the base pixels.
    QualityReport validate_replacement_patch(const Recipe& recipe, size_t index, const PatchResult& patch, double scale);
    /// Render the whole recipe at `scale`; `reports` receives one entry per replacement.
    Image render(const Recipe& recipe, double scale, std::vector<QualityReport>* reports = nullptr);

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace bt
