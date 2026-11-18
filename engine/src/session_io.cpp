#include "bt/session_io.h"

#include <stdexcept>

namespace bt {

static double number_or(const json::Value& o, const char* key, double fallback) {
    return o.has(key) ? o.at(key).as_number() : fallback;
}

std::vector<FrameSource> frames_from_json(const json::Value& array) {
    std::vector<FrameSource> frames;
    for (const json::Value& jf : array.as_array()) {
        FrameSource f;
        f.id = jf.at("id").as_string();
        f.hash = jf.has("hash") ? jf.at("hash").as_string() : "";
        f.width = int(jf.at("width").as_number());
        f.height = int(jf.at("height").as_number());
        f.scene_cost = number_or(jf, "scene_cost", 0);
        for (const json::Value& jface : jf.at("faces").as_array()) {
            FaceInput face;
            const std::vector<json::Value>& lm = jface.at("landmarks").as_array();
            if (lm.size() % 2) throw std::invalid_argument("landmarks must be a flat [x, y, ...] list");
            for (size_t i = 0; i < lm.size(); i += 2)
                face.landmarks.points.push_back({lm[i].as_number(), lm[i + 1].as_number()});
            face.state.eyes_open = number_or(jface, "eyes_open", 1);
            face.state.smile = number_or(jface, "smile", 0);
            face.state.gaze_forward = number_or(jface, "gaze_forward", 1);
            face.state.yaw_degrees = number_or(jface, "yaw", 0);
            face.state.pitch_degrees = number_or(jface, "pitch", 0);
            face.state.confidence = number_or(jface, "confidence", 1);
            face.state.visibility = number_or(jface, "visibility", 1);
            f.faces.push_back(face);
        }
        frames.push_back(f);
    }
    return frames;
}

json::Value selection_to_json(const SelectionResult& sel) {
    json::Value o = json::Value::make_object();
    o.set("valid", sel.valid);
    o.set("base", sel.base);
    o.set("cost", sel.cost);
    json::Value src = json::Value::make_array();
    for (int s : sel.source) src.array.emplace_back(s);
    o.set("source", src);
    json::Value conflicts = json::Value::make_array();
    for (const Conflict& c : sel.conflicts) {
        json::Value jc = json::Value::make_object();
        jc.set("person", c.person);
        jc.set("reason", c.reason);
        conflicts.array.push_back(jc);
    }
    o.set("conflicts", conflicts);
    return o;
}

json::Value candidates_to_json(const Session& session, int person, const std::vector<Candidate>& candidates) {
    json::Value out = json::Value::make_array();
    for (const Candidate& c : candidates) {
        json::Value jc = json::Value::make_object();
        jc.set("frame", c.frame);
        jc.set("face", session.face_index(person, c.frame));
        jc.set("is_base", c.is_base);
        jc.set("feasible", c.feasible);
        jc.set("rejection", c.rejection);
        jc.set("face_cost", c.face_cost);
        jc.set("merge_cost", c.merge_cost);
        jc.set("support_residual", c.compatibility.support_residual);
        jc.set("enlargement", c.compatibility.enlargement);
        jc.set("silhouette_overlap", c.compatibility.silhouette_overlap);
        jc.set("min_band_width", c.compatibility.min_band_width);
        jc.set("seam_cost", c.compatibility.seam_cost);
        out.array.push_back(jc);
    }
    return out;
}

json::Value quality_to_json(const Recipe& recipe, const std::vector<QualityReport>& reports) {
    if (reports.size() != recipe.replacements.size())
        throw std::invalid_argument("quality_to_json: one report per replacement is required");
    json::Value out = json::Value::make_array();
    for (size_t i = 0; i < reports.size(); ++i) {
        json::Value q = json::Value::make_object();
        q.set("person", recipe.replacements[i].person_id);
        q.set("donor", recipe.replacements[i].donor_id);
        q.set("ok", reports[i].ok);
        q.set("failure", reports[i].failure);
        q.set("excess_gradient", reports[i].excess_gradient);
        out.array.push_back(q);
    }
    return out;
}

std::vector<PairConstraint> constraints_from_json(const json::Value& array) {
    std::vector<PairConstraint> out;
    for (const json::Value& j : array.as_array()) {
        PairConstraint c;
        c.frame_a = int(j.at("frame_a").as_number());
        c.detection_a = int(j.at("detection_a").as_number());
        c.frame_b = int(j.at("frame_b").as_number());
        c.detection_b = int(j.at("detection_b").as_number());
        const json::Value& same = j.at("same_person");
        if (same.type != json::Value::Type::Bool) throw std::invalid_argument("same_person must be a boolean");
        c.same_person = same.boolean;
        out.push_back(c);
    }
    return out;
}

}  // namespace bt
