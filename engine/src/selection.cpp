#include "bt/selection.h"

#include <algorithm>
#include <cmath>
#include <numeric>
#include <stdexcept>

namespace bt {

int SelectionResult::edits() const {
    int n = 0;
    for (int s : source)
        if (s >= 0 && s != base) ++n;
    return n;
}

FaceMeasurements measure_face(const ImageView& view, const FaceLandmarks& face) {
    require_valid(face);
    if (!view.image) throw std::invalid_argument("measure_face: missing image");
    FaceMeasurements m;
    const double iod = interocular_distance(face);
    m.interocular_px = iod;
    double x0 = 1e30, y0 = 1e30, x1 = -1e30, y1 = -1e30;
    for (int i : topology::face_oval) {
        Vec2 p = face.points[i] * view.scale;
        x0 = std::min(x0, p.x), x1 = std::max(x1, p.x), y0 = std::min(y0, p.y), y1 = std::max(y1, p.y);
    }
    Rect r = intersect(Rect{int(x0), int(y0), int(std::ceil(x1)), int(std::ceil(y1))}, view.rect);
    if (r.empty()) throw std::invalid_argument("measure_face: view does not cover the face");
    Image c = crop(*view.image, Rect{r.x0 - view.rect.x0, r.y0 - view.rect.y0, r.x1 - view.rect.x0, r.y1 - view.rect.y0});
    Image lum = luminance(c);
    int clipped = 0;
    for (float& v : lum.data) {
        if (v <= 0.002f || v >= 0.98f) ++clipped;
        v = std::sqrt(std::max(v, 0.f));
    }
    m.clipping = double(clipped) / lum.data.size();
    // Sharpness: Laplacian energy at a canonical face size, so the score
    // reflects detail available per face rather than per pixel.
    const double canon = 64.0 / (iod * view.scale);
    if (canon < 1) lum = resize_area(lum, std::max(8, int(lum.width * canon)), std::max(8, int(lum.height * canon)));
    double sum = 0, sum2 = 0;
    int n = 0;
    for (int y = 1; y + 1 < lum.height; ++y)
        for (int x = 1; x + 1 < lum.width; ++x) {
            double l = 4 * lum.at(x, y) - lum.at(x - 1, y) - lum.at(x + 1, y) - lum.at(x, y - 1) - lum.at(x, y + 1);
            sum += l, sum2 += l * l, ++n;
        }
    double var = n ? sum2 / n - (sum / n) * (sum / n) : 0;
    // Saturating map; a face enlarged from few pixels cannot score as sharp.
    m.sharpness = (1 - std::exp(-var / 4e-4)) * std::min(1.0, 1.0 / std::max(canon, 1e-6));
    return m;
}

double technical_cost(const FaceMeasurements& m) {
    double blur = 1 - std::clamp(m.sharpness, 0.0, 1.0);
    double small = std::clamp((60 - m.interocular_px) / 45, 0.0, 1.0);  // below ~60 px interocular detail runs out
    double hidden = 1 - std::clamp(m.visibility, 0.0, 1.0);
    double clip = std::clamp(m.clipping * 4, 0.0, 1.0);
    return std::clamp(0.45 * blur + 0.2 * small + 0.25 * hidden + 0.1 * clip, 0.0, 1.0);
}

double preference_cost(const FaceMeasurements& m, const Preference& pref) {
    // Eye opening is judged on a ramp: small variations among open eyes cost
    // nothing, and a visible squint or blink quickly costs the full weight.
    double eyes = std::clamp((0.95 - std::clamp(m.eyes_open, 0.0, 1.0)) / 0.30, 0.0, 1.0);
    double c = pref.eyes_open * eyes + pref.smile * (1 - std::clamp(m.smile, 0.0, 1.0)) +
               pref.gaze * (1 - std::clamp(m.gaze_forward, 0.0, 1.0));
    return std::clamp(std::clamp(m.confidence, 0.0, 1.0) * c, 0.0, 1.0);
}

MergeEvaluation merge_cost(const Compatibility& c) {
    MergeEvaluation e;
    e.feasible = c.feasible;
    e.rejection = c.rejection;
    if (!c.feasible) return e;
    e.cost = std::clamp(1.5 * c.support_residual + 0.5 * std::max(0.0, c.enlargement - 1) +
                            0.4 * (1 - c.silhouette_overlap) + 0.3 * c.color_log_gain + 10.0 * c.seam_cost +
                            0.5 * c.core_mismatch,
                        0.0, 1.0);
    return e;
}

namespace {

struct Option {
    int source;
    double unary;  // face + lambda * merge + eta
};

void validate(const SelectionProblem& p) {
    auto shape = [&](const auto& t) {
        if (int(t.size()) != p.person_count) return false;
        for (const auto& row : t)
            if (int(row.size()) != p.frame_count) return false;
        return true;
    };
    if (p.frame_count < 1 || p.person_count < 0 || int(p.scene_cost.size()) != p.frame_count || !shape(p.present) ||
        !shape(p.eligible) || !shape(p.face_cost) || int(p.required.size()) != p.person_count)
        throw std::invalid_argument("select_sources: table sizes do not match frame and person counts");
    if (!p.merge) throw std::invalid_argument("select_sources: merge callback is required");
    if (p.locked_base >= p.frame_count) throw std::invalid_argument("select_sources: locked base out of range");
    for (auto [person, frame] : p.locks)
        if (person < 0 || person >= p.person_count || frame < 0 || frame >= p.frame_count || !p.present[person][frame])
            throw std::invalid_argument("select_sources: lock references a missing observation");
}

bool base_is_valid(const SelectionProblem& p, int b) {
    for (int q = 0; q < p.person_count; ++q)
        if (p.required[q] && !p.present[q][b]) return false;
    return true;
}

SelectionResult solve_for_base(const SelectionProblem& p, int b) {
    SelectionResult r;
    r.valid = true;
    r.base = b;
    r.source.assign(p.person_count, -1);
    r.cost = p.scene_cost[b];
    std::vector<std::vector<Option>> options(p.person_count);
    for (int q = 0; q < p.person_count; ++q) {
        if (!p.present[q][b]) continue;  // replacement never inserts a missing person
        auto lock = p.locks.find(q);
        if (lock != p.locks.end()) {
            int s = lock->second;
            double unary = p.face_cost[q][s];
            if (s != b) {
                MergeEvaluation m = p.merge(q, s, b);
                if (!m.feasible) {
                    r.conflicts.push_back({q, m.rejection.empty() ? "not compatible with this base photo" : m.rejection});
                    r.cost += 10;  // prefer bases that honor every lock
                } else {
                    unary += p.merge_weight * m.cost + p.edit_penalty;
                }
            }
            options[q] = {{s, unary}};
            continue;
        }
        options[q].push_back({b, p.face_cost[q][b]});
        // Short, diverse donor list ordered by face cost.
        std::vector<int> order;
        for (int s = 0; s < p.frame_count; ++s)
            if (s != b && p.present[q][s] && p.eligible[q][s] && p.face_cost[q][s] < p.face_cost[q][b]) order.push_back(s);
        std::stable_sort(order.begin(), order.end(), [&](int a, int c) { return p.face_cost[q][a] < p.face_cost[q][c]; });
        std::vector<double> listed;
        for (int s : order) {
            if (int(listed.size()) >= p.max_donors) break;
            bool duplicate = false;
            for (double c : listed)
                if (std::abs(c - p.face_cost[q][s]) < p.diversity_epsilon) duplicate = true;
            if (duplicate && !listed.empty()) continue;
            MergeEvaluation m = p.merge(q, s, b);
            if (!m.feasible) continue;
            listed.push_back(p.face_cost[q][s]);
            options[q].push_back({s, p.face_cost[q][s] + p.merge_weight * m.cost + p.edit_penalty});
        }
    }

    // Connected groups of interacting people are optimized jointly.
    std::vector<int> group(p.person_count);
    std::iota(group.begin(), group.end(), 0);
    std::function<int(int)> find = [&](int x) { return group[x] == x ? x : group[x] = find(group[x]); };
    std::vector<std::pair<int, int>> pairs;
    if (p.interaction)
        for (auto [a, c] : p.neighbors) {
            if (a < 0 || c < 0 || a >= p.person_count || c >= p.person_count || a == c)
                throw std::invalid_argument("select_sources: invalid neighbor pair");
            if (options[a].empty() || options[c].empty()) continue;
            pairs.push_back({a, c});
            group[find(a)] = find(c);
        }
    for (int root = 0; root < p.person_count; ++root) {
        std::vector<int> members;
        for (int q = 0; q < p.person_count; ++q)
            if (find(q) == root && !options[q].empty()) members.push_back(q);
        if (members.empty()) continue;
        // Beam search over members; exhaustive whenever the beam is not exceeded.
        struct State {
            std::vector<int> pick;
            double cost;
        };
        const size_t beam = 256;
        std::vector<State> states{{{}, 0.0}};
        for (size_t i = 0; i < members.size(); ++i) {
            int q = members[i];
            std::vector<State> next;
            for (const State& st : states)
                for (size_t o = 0; o < options[q].size(); ++o) {
                    State n = st;
                    n.pick.push_back(int(o));
                    n.cost += options[q][o].unary;
                    for (auto [a, c] : pairs) {
                        int other = a == q ? c : (c == q ? a : -1);
                        if (other < 0) continue;
                        auto it = std::find(members.begin(), members.begin() + i, other);
                        if (it == members.begin() + i) continue;
                        int so = options[other][st.pick[it - members.begin()]].source, sq = options[q][o].source;
                        n.cost += a == q ? p.interaction(q, other, sq, so, b) : p.interaction(other, q, so, sq, b);
                    }
                    next.push_back(std::move(n));
                }
            std::stable_sort(next.begin(), next.end(), [](const State& x, const State& y) { return x.cost < y.cost; });
            if (next.size() > beam) next.resize(beam);
            states = std::move(next);
        }
        r.cost += states[0].cost;
        for (size_t i = 0; i < members.size(); ++i) r.source[members[i]] = options[members[i]][states[0].pick[i]].source;
    }
    return r;
}

}  // namespace

SelectionResult select_sources(const SelectionProblem& p) {
    validate(p);
    std::vector<int> bases;
    if (p.locked_base >= 0) {
        // The user's own choice of base decides who is retained, so it is not
        // checked against the people inferred to be required.
        bases = {p.locked_base};
    } else {
        // Promising bases: lowest scene cost plus cost of the faces as photographed.
        std::vector<std::pair<double, int>> ranked;
        for (int b = 0; b < p.frame_count; ++b) {
            if (!base_is_valid(p, b)) continue;
            double c = p.scene_cost[b];
            for (int q = 0; q < p.person_count; ++q)
                if (p.present[q][b]) c += p.face_cost[q][b];
            ranked.push_back({c, b});
        }
        std::stable_sort(ranked.begin(), ranked.end());
        for (size_t i = 0; i < ranked.size() && int(i) < p.max_bases; ++i) bases.push_back(ranked[i].second);
    }
    SelectionResult best;
    for (int b : bases) {
        SelectionResult r = solve_for_base(p, b);
        if (!best.valid || r.cost < best.cost) best = std::move(r);
    }
    return best;
}

}  // namespace bt
