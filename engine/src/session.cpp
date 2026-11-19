#include "bt/session.h"

#include <algorithm>
#include <cmath>
#include <stdexcept>

namespace bt {

struct Session::Impl {
    std::vector<FrameSource> frames;
    RegionLoader loader;
    SessionOptions options;
    std::vector<PairConstraint> constraints;
    AssociationResult assoc;
    std::vector<std::vector<int>> face_of;  // [person][frame] -> face index or -1

    struct Crop {
        Image image;
        ImageView view;
    };
    std::map<std::pair<int, int>, std::shared_ptr<Crop>> crops;             // (frame, face)
    std::map<std::pair<int, int>, FaceMeasurements> measurements;           // (frame, face)
    struct Built {
        Compatibility compat;
        Replacement replacement;
    };
    std::map<std::tuple<int, int, int>, Built> built;  // (person, donor, base)

    Rect bounds(int frame, double scale) const {
        return Rect{0, 0, int(std::ceil(frames[frame].width * scale)), int(std::ceil(frames[frame].height * scale))};
    }

    ImageView load(int frame, Rect rect, double scale, Image* storage) const {
        Rect b = bounds(frame, scale);
        rect = intersect(rect, b);
        if (rect.empty()) throw std::invalid_argument("Session: requested region is outside the image");
        *storage = loader(frame, rect, scale);
        if (storage->width != rect.width() || storage->height != rect.height() || storage->channels != 3)
            throw std::invalid_argument("Session: loader returned an image of the wrong size");
        return ImageView{storage, rect, b, scale};
    }

    // Largest power-of-two reduction that keeps `needed` resolution.
    static double pow2_scale(double needed) {
        double s = 1;
        while (s > 0.125 && s * 0.5 >= needed) s *= 0.5;
        return s;
    }

    // Analysis crop of a face: generous margin, reduced to working resolution.
    const Crop& crop(int frame, int face) {
        auto key = std::make_pair(frame, face);
        auto it = crops.find(key);
        if (it != crops.end()) return *it->second;
        const FaceLandmarks& lm = frames[frame].faces[face].landmarks;
        double iod = interocular_distance(lm);
        double scale = pow2_scale(options.build.working_interocular_px / iod);
        double x0 = 1e30, y0 = 1e30, x1 = -1e30, y1 = -1e30;
        for (Vec2 p : lm.points) x0 = std::min(x0, p.x), x1 = std::max(x1, p.x), y0 = std::min(y0, p.y), y1 = std::max(y1, p.y);
        double pad = 1.2 * iod;
        Rect r{int(std::floor((x0 - pad) * scale)), int(std::floor((y0 - pad) * scale)), int(std::ceil((x1 + pad) * scale)),
               int(std::ceil((y1 + pad) * scale))};
        auto c = std::make_shared<Crop>();
        c->view = load(frame, r, scale, &c->image);
        crops[key] = c;
        return *c;
    }

    const FaceMeasurements& measure(int frame, int face) {
        auto key = std::make_pair(frame, face);
        auto it = measurements.find(key);
        if (it != measurements.end()) return it->second;
        const FaceInput& in = frames[frame].faces[face];
        FaceMeasurements m = in.state;
        FaceMeasurements tech = measure_face(crop(frame, face).view, in.landmarks);
        m.sharpness = tech.sharpness;
        m.clipping = tech.clipping;
        m.interocular_px = tech.interocular_px;
        return measurements[key] = m;
    }

    /// Cost of one person's face as photographed. Expression is judged
    /// against that person's own best observation in the set, so narrow eyes
    /// in bright sun or a habitually serious face are not penalized.
    double face_cost(int person, int frame) {
        FaceMeasurements best;
        best.eyes_open = best.smile = best.gaze_forward = 0;
        for (int f = 0; f < int(frames.size()); ++f) {
            int face = face_of[person][f];
            if (face < 0) continue;
            const FaceMeasurements& o = measure(f, face);
            best.eyes_open = std::max(best.eyes_open, o.eyes_open);
            best.smile = std::max(best.smile, o.smile);
            best.gaze_forward = std::max(best.gaze_forward, o.gaze_forward);
        }
        FaceMeasurements m = measure(frame, face_of[person][frame]);
        double technical = technical_cost(m);
        m.eyes_open = best.eyes_open > 0 ? std::min(1.0, m.eyes_open / best.eyes_open) : 1.0;
        m.smile = 1 - std::max(0.0, best.smile - m.smile);
        m.gaze_forward = 1 - std::max(0.0, best.gaze_forward - m.gaze_forward);
        return technical + preference_cost(m, options.preference);
    }

    const Built& build(int person, int donor, int base) {
        auto key = std::make_tuple(person, donor, base);
        auto it = built.find(key);
        if (it != built.end()) return it->second;
        int fd = face_of[person][donor], fb = face_of[person][base];
        if (fd < 0 || fb < 0) throw std::invalid_argument("Session: person is not present in that frame");
        Built b;
        if (assoc.uncertain[donor][fd] || assoc.uncertain[base][fb]) {
            b.compat.rejection = "not sure this is the same person";
        } else if (interocular_distance(frames[donor].faces[fd].landmarks) < options.min_donor_interocular_px) {
            b.compat.rejection = "too small in the source photo";
        } else if (measure(donor, fd).sharpness < options.min_relative_sharpness * measure(base, fb).sharpness) {
            b.compat.rejection = "too blurry";
        } else {
            const Crop& cb = crop(base, fb);
            const Crop& cd = crop(donor, fd);
            b.compat = build_replacement(cb.view, frames[base].faces[fb].landmarks, cd.view,
                                         frames[donor].faces[fd].landmarks, options.build, &b.replacement);
            b.replacement.person_id = person;
            b.replacement.donor_id = frames[donor].id;
        }
        return built[key] = b;
    }

    void run_association() {
        std::vector<std::vector<Detection>> dets(frames.size());
        for (size_t f = 0; f < frames.size(); ++f)
            for (const FaceInput& face : frames[f].faces) {
                std::vector<Vec2> oval = select(face.landmarks, topology::face_oval);
                Vec2 c{};
                double x0 = 1e30, x1 = -1e30;
                for (Vec2 p : oval) c = c + p * (1.0 / oval.size()), x0 = std::min(x0, p.x), x1 = std::max(x1, p.x);
                dets[f].push_back({c, x1 - x0, face.embedding});
            }
        assoc = associate(dets, constraints);
        face_of.assign(assoc.person_count, std::vector<int>(frames.size(), -1));
        for (size_t f = 0; f < frames.size(); ++f)
            for (size_t d = 0; d < assoc.person[f].size(); ++d) face_of[assoc.person[f][d]][f] = int(d);
        built.clear();
    }

    int frame_index(const std::string& id) const {
        for (size_t f = 0; f < frames.size(); ++f)
            if (frames[f].id == id) return int(f);
        throw std::invalid_argument("Session: recipe refers to unknown source '" + id + "'");
    }

    // Two replaced neighbors conflict when their edit supports overlap.
    static bool supports_overlap(const Replacement& a, const Replacement& b) {
        double ra = *std::max_element(a.regions.outer.begin(), a.regions.outer.end());
        double rb = *std::max_element(b.regions.outer.begin(), b.regions.outer.end());
        return norm(a.regions.center - b.regions.center) < ra + rb;
    }
};

Session::Session(std::vector<FrameSource> frames, RegionLoader loader, SessionOptions options) : impl_(new Impl) {
    if (frames.empty()) throw std::invalid_argument("Session: no frames");
    if (!loader) throw std::invalid_argument("Session: region loader is required");
    for (size_t f = 0; f < frames.size(); ++f) {
        if (frames[f].width <= 0 || frames[f].height <= 0) throw std::invalid_argument("Session: frame has no size");
        for (size_t g = 0; g < f; ++g)
            if (frames[g].id == frames[f].id) throw std::invalid_argument("Session: duplicate frame id '" + frames[f].id + "'");
        for (const FaceInput& face : frames[f].faces) require_valid(face.landmarks);
    }
    impl_->frames = std::move(frames);
    impl_->loader = std::move(loader);
    impl_->options = options;
    impl_->run_association();
}

Session::~Session() = default;

int Session::frame_count() const { return int(impl_->frames.size()); }
int Session::person_count() const { return impl_->assoc.person_count; }
const std::vector<FrameSource>& Session::frames() const { return impl_->frames; }
const AssociationResult& Session::association() const { return impl_->assoc; }

int Session::face_index(int person, int frame) const {
    if (person < 0 || person >= person_count() || frame < 0 || frame >= frame_count())
        throw std::invalid_argument("Session::face_index: index out of range");
    return impl_->face_of[person][frame];
}

void Session::set_constraints(const std::vector<PairConstraint>& constraints) {
    std::vector<PairConstraint> previous = impl_->constraints;
    impl_->constraints = constraints;
    try {
        impl_->run_association();
    } catch (...) {
        impl_->constraints = previous;
        impl_->run_association();
        throw;
    }
}

SelectionResult Session::propose(const std::map<int, int>& locks, int locked_base) {
    Impl& s = *impl_;
    SelectionProblem p;
    p.frame_count = frame_count();
    p.person_count = person_count();
    p.locks = locks;
    p.locked_base = locked_base;
    p.scene_cost.resize(p.frame_count);
    for (int f = 0; f < p.frame_count; ++f) p.scene_cost[f] = s.frames[f].scene_cost;
    p.present.assign(p.person_count, std::vector<bool>(p.frame_count, false));
    p.eligible = p.present;
    p.face_cost.assign(p.person_count, std::vector<double>(p.frame_count, 0.0));
    p.required.assign(p.person_count, false);
    // People seen in most frames are the group; a passer-by in one frame is not required.
    for (int q = 0; q < p.person_count; ++q) {
        int seen = 0;
        for (int f = 0; f < p.frame_count; ++f) {
            int face = s.face_of[q][f];
            if (face < 0) continue;
            ++seen;
            p.present[q][f] = true;
            p.eligible[q][f] = !s.assoc.uncertain[f][face];
            p.face_cost[q][f] = s.face_cost(q, f);
        }
        p.required[q] = 2 * seen > p.frame_count;
    }
    p.merge = [&](int q, int donor, int base) { return merge_cost(s.build(q, donor, base).compat); };
    for (int a = 0; a < p.person_count; ++a)
        for (int b = a + 1; b < p.person_count; ++b) p.neighbors.push_back({a, b});
    p.interaction = [&](int a, int b, int sa, int sb, int base) {
        if (sa == base || sb == base) return 0.0;
        const Impl::Built& ba = s.build(a, sa, base);
        const Impl::Built& bb = s.build(b, sb, base);
        if (!ba.compat.feasible || !bb.compat.feasible) return 0.0;
        // Overlapping edits need joint rendering, which this renderer does not do.
        return Impl::supports_overlap(ba.replacement, bb.replacement) ? 100.0 : 0.0;
    };
    SelectionResult r = select_sources(p);
    if (!r.valid) {
        // No frame shows every regular member; fall back to requiring no one.
        p.required.assign(p.person_count, false);
        r = select_sources(p);
    }
    return r;
}

std::vector<Candidate> Session::candidates(int person, int base) {
    Impl& s = *impl_;
    if (person < 0 || person >= person_count() || base < 0 || base >= frame_count())
        throw std::invalid_argument("Session::candidates: index out of range");
    std::vector<Candidate> out;
    if (s.face_of[person][base] < 0) return out;
    for (int f = 0; f < frame_count(); ++f) {
        int face = s.face_of[person][f];
        if (face < 0) continue;
        Candidate c;
        c.frame = f;
        c.is_base = f == base;
        c.face_cost = s.face_cost(person, f);
        if (c.is_base) {
            c.feasible = true;
        } else {
            c.compatibility = s.build(person, f, base).compat;
            MergeEvaluation m = merge_cost(c.compatibility);
            c.feasible = m.feasible;
            c.rejection = m.rejection;
            c.merge_cost = m.cost;
        }
        out.push_back(c);
    }
    std::stable_sort(out.begin(), out.end(), [](const Candidate& a, const Candidate& b) {
        if (a.is_base != b.is_base) return a.is_base;
        if (a.feasible != b.feasible) return a.feasible;
        return a.face_cost + a.merge_cost < b.face_cost + b.merge_cost;
    });
    return out;
}

Recipe Session::build_recipe(int base, const std::vector<int>& sources) {
    Impl& s = *impl_;
    if (base < 0 || base >= frame_count()) throw std::invalid_argument("Session::build_recipe: base out of range");
    if (int(sources.size()) != person_count())
        throw std::invalid_argument("Session::build_recipe: one source per person is required");
    Recipe r;
    r.algorithm = kEngineAlgorithm;
    r.base_id = s.frames[base].id;
    r.base_hash = s.frames[base].hash;
    for (int q = 0; q < person_count(); ++q) {
        int src = sources[q];
        if (src < 0 || src == base) continue;
        if (src >= frame_count()) throw std::invalid_argument("Session::build_recipe: source out of range");
        const Impl::Built& b = s.build(q, src, base);
        if (!b.compat.feasible)
            throw std::invalid_argument("Session::build_recipe: person " + std::to_string(q) + " cannot use '" +
                                        s.frames[src].id + "': " + b.compat.rejection);
        for (const Replacement& other : r.replacements)
            if (Impl::supports_overlap(other, b.replacement))
                throw std::invalid_argument("Session::build_recipe: edits for persons " + std::to_string(other.person_id) +
                                            " and " + std::to_string(q) + " overlap");
        r.replacements.push_back(b.replacement);
        r.donor_hashes[s.frames[src].id] = s.frames[src].hash;
    }
    return r;
}

PatchResult Session::render_replacement_patch(const Recipe& recipe, size_t index, double scale) {
    Impl& s = *impl_;
    if (index >= recipe.replacements.size()) throw std::invalid_argument("Session: replacement index out of range");
    if (recipe.algorithm != kEngineAlgorithm)
        throw std::invalid_argument("Session: recipe was built by algorithm '" + recipe.algorithm + "'");
    const Replacement& rep = recipe.replacements[index];
    int base = s.frame_index(recipe.base_id), donor = s.frame_index(rep.donor_id);
    Image bi, di;
    ImageView probe{nullptr, Rect{}, s.bounds(base, scale), scale};
    ImageView bv = s.load(base, replacement_rect(rep, probe), scale, &bi);
    ImageView dv = s.load(donor, donor_required_rect(rep, scale, s.bounds(donor, scale)), scale, &di);
    return render_replacement(rep, bv, dv);
}

QualityReport Session::validate_replacement_patch(const Recipe& recipe, size_t index, const PatchResult& patch,
                                                  double scale) {
    Impl& s = *impl_;
    if (index >= recipe.replacements.size()) throw std::invalid_argument("Session: replacement index out of range");
    int base = s.frame_index(recipe.base_id);
    Image pixels;
    ImageView view = s.load(base, patch.rect, scale, &pixels);
    return validate_patch(recipe.replacements[index], view, patch);
}

Image Session::render(const Recipe& recipe, double scale, std::vector<QualityReport>* reports) {
    Impl& s = *impl_;
    int base = s.frame_index(recipe.base_id);
    Image canvas;
    ImageView bv = s.load(base, s.bounds(base, scale), scale, &canvas);
    if (reports) reports->clear();
    // Supports are disjoint (enforced by build_recipe), so patches are independent.
    for (size_t i = 0; i < recipe.replacements.size(); ++i) {
        PatchResult patch = render_replacement_patch(recipe, i, scale);
        if (reports) reports->push_back(validate_patch(recipe.replacements[i], bv, patch));
        paste(canvas, patch.pixels, patch.rect.x0, patch.rect.y0);
    }
    return canvas;
}

}  // namespace bt
