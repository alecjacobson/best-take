// Project-local association of face detections across frames.
#pragma once
#include <vector>

#include "bt/geometry.h"

namespace bt {

struct Detection {
    Vec2 center;                   ///< Face center in upright original pixels.
    double size = 0;               ///< Face width in pixels.
    std::vector<float> embedding;  ///< Optional appearance embedding; empty when unavailable.
};

/// A user correction that later analysis cannot override.
struct PairConstraint {
    int frame_a = 0, detection_a = 0, frame_b = 0, detection_b = 0;
    bool same_person = true;
};

struct AssociationOptions {
    /// Matching cost above which a detection starts a new person.
    double gate = 1.0;
    /// A match whose runner-up is within this margin is flagged uncertain.
    double ambiguity_margin = 0.25;
    double embedding_weight = 2.0;
};

struct AssociationResult {
    int person_count = 0;
    std::vector<std::vector<int>> person;       ///< [frame][detection] -> person id.
    std::vector<std::vector<bool>> uncertain;   ///< [frame][detection]: conservative callers must not merge.
    std::vector<Vec2> frame_shift;              ///< Estimated scene translation of each frame to the reference.
};

/// Assign detections to persons with a one-to-one assignment inside every
/// frame. Throws std::invalid_argument for constraints that reference
/// missing detections or that contradict each other within one frame.
AssociationResult associate(const std::vector<std::vector<Detection>>& frames,
                            const std::vector<PairConstraint>& constraints = {},
                            const AssociationOptions& options = {});

/// Minimum-cost one-to-one assignment for a rows x cols cost matrix
/// (row-major). Returns the column of each row, or -1 when rows exceed columns.
std::vector<int> solve_assignment(const std::vector<double>& cost, int rows, int cols);

}  // namespace bt
