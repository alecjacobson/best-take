// JSON marshaling of session inputs and results, shared by the host tool and
// the Android JNI bridge.
#pragma once
#include <string>
#include <vector>

#include "bt/json.h"
#include "bt/session.h"

namespace bt {

/// Parse the "frames" array of an analysis document. Each frame has "id",
/// "width", "height", optional "hash" and "scene_cost", and "faces"; each
/// face has "landmarks" as a flat [x0, y0, x1, y1, ...] list in upright
/// original pixels plus optional facial-state numbers ("eyes_open", "smile",
/// "gaze_forward", "yaw", "pitch", "confidence", "visibility").
/// Throws std::invalid_argument for malformed input.
std::vector<FrameSource> frames_from_json(const json::Value& frames);

json::Value selection_to_json(const SelectionResult& selection);
/// Candidates of one person including the face index in each frame.
json::Value candidates_to_json(const Session& session, int person, const std::vector<Candidate>& candidates);
json::Value quality_to_json(const Recipe& recipe, const std::vector<QualityReport>& reports);
/// Parse [{"frame_a":..,"detection_a":..,"frame_b":..,"detection_b":..,"same_person":bool}, ...].
std::vector<PairConstraint> constraints_from_json(const json::Value& array);

}  // namespace bt
