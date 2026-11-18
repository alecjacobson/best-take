// Scoring of face instances and joint base/donor selection.
#pragma once
#include <functional>
#include <map>
#include <string>
#include <utility>
#include <vector>

#include "bt/compose.h"

namespace bt {

/// Measurements of one face instance. All scores are normalized to [0, 1].
struct FaceMeasurements {
    double sharpness = 1;     ///< 1 = crisp at its native size, 0 = heavily blurred.
    double clipping = 0;      ///< Fraction of face pixels clipped dark or bright.
    double interocular_px = 0;
    double visibility = 1;    ///< Fraction of the face that is visible.
    double eyes_open = 1;     ///< Opening of the less open eye.
    double smile = 0;
    double gaze_forward = 1;  ///< 1 = looking toward the camera.
    double yaw_degrees = 0, pitch_degrees = 0;
    double confidence = 1;    ///< Confidence of the facial-state estimates.
};

/// Image-derived technical measurements (sharpness, clipping, size) of a face.
/// Facial-state fields of the result keep their defaults.
FaceMeasurements measure_face(const ImageView& image, const FaceLandmarks& face);

/// Technical suitability cost in [0, 1]; independent of expression preference.
double technical_cost(const FaceMeasurements& m);

/// How much each expression property matters to the user. A zero weight
/// makes that property irrelevant, so neutral or serious looks stay valid.
struct Preference {
    double eyes_open = 1.0;
    double smile = 0.25;
    double gaze = 0.35;
};
/// Expression preference cost in [0, 1], scaled by estimate confidence.
double preference_cost(const FaceMeasurements& m, const Preference& pref);

struct MergeEvaluation {
    bool feasible = false;
    double cost = 0;
    std::string rejection;
};
/// Merge cost from compatibility measurements (0 = fits effortlessly).
MergeEvaluation merge_cost(const Compatibility& c);

struct SelectionProblem {
    int frame_count = 0;
    int person_count = 0;
    std::vector<double> scene_cost;               ///< [frame] framing, background, overall quality.
    std::vector<std::vector<bool>> present;       ///< [person][frame] an observation exists.
    std::vector<std::vector<bool>> eligible;      ///< [person][frame] usable as donor (identity certain, adequate detail).
    std::vector<std::vector<double>> face_cost;   ///< [person][frame] technical + preference.
    std::vector<bool> required;                   ///< [person] must appear in the base.
    /// Compatibility of donor frame `s` for person `p` in base `b` (s != b).
    std::function<MergeEvaluation(int p, int s, int b)> merge;
    /// People whose edits touch; optional cost when both are replaced.
    std::vector<std::pair<int, int>> neighbors;
    std::function<double(int p, int q, int sp, int sq, int b)> interaction;
    std::map<int, int> locks;  ///< person -> donor frame chosen by the user.
    int locked_base = -1;      ///< Base chosen by the user, or -1.
    double merge_weight = 1.0;   ///< lambda
    double edit_penalty = 0.12;  ///< eta
    int max_bases = 4;
    int max_donors = 4;
    /// Donors whose face cost is within this of an already listed donor are
    /// treated as near-duplicates unless they are the best available.
    double diversity_epsilon = 0.01;
};

struct Conflict {
    int person = -1;
    std::string reason;
};

struct SelectionResult {
    bool valid = false;        ///< False when no frame can serve as base.
    int base = -1;
    std::vector<int> source;   ///< [person] donor frame; equals base when unchanged; -1 when absent.
    double cost = 0;
    std::vector<Conflict> conflicts;  ///< Locked choices that cannot be honored with this base.
    int edits() const;
};

/// Minimize E(b, s) = scene(b) + sum_p [face(p, s_p) + lambda merge(p, s_p, b)
/// + eta 1[s_p != b]] + interactions, subject to feasibility and user locks.
/// A locked choice is never replaced: if it is infeasible for the chosen
/// base it is reported in `conflicts` and the person keeps the lock.
SelectionResult select_sources(const SelectionProblem& problem);

}  // namespace bt
