#include "bt/association.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <stdexcept>

namespace bt {

std::vector<int> solve_assignment(const std::vector<double>& cost, int rows, int cols) {
    if (rows < 0 || cols < 0 || cost.size() != size_t(rows) * cols)
        throw std::invalid_argument("solve_assignment: cost matrix size mismatch");
    // Hungarian algorithm (potentials form) on a square padded matrix.
    int n = std::max(rows, cols);
    std::vector<int> result(rows, -1);
    if (n == 0) return result;
    auto at = [&](int i, int j) { return i < rows && j < cols ? cost[size_t(i) * cols + j] : 0.0; };
    const double inf = std::numeric_limits<double>::infinity();
    std::vector<double> u(n + 1, 0), v(n + 1, 0);
    std::vector<int> p(n + 1, 0), way(n + 1, 0);
    for (int i = 1; i <= n; ++i) {
        p[0] = i;
        int j0 = 0;
        std::vector<double> minv(n + 1, inf);
        std::vector<char> used(n + 1, 0);
        do {
            used[j0] = 1;
            int i0 = p[j0], j1 = 0;
            double delta = inf;
            for (int j = 1; j <= n; ++j) {
                if (used[j]) continue;
                double cur = at(i0 - 1, j - 1) - u[i0] - v[j];
                if (cur < minv[j]) minv[j] = cur, way[j] = j0;
                if (minv[j] < delta) delta = minv[j], j1 = j;
            }
            for (int j = 0; j <= n; ++j) {
                if (used[j])
                    u[p[j]] += delta, v[j] -= delta;
                else
                    minv[j] -= delta;
            }
            j0 = j1;
        } while (p[j0] != 0);
        do {
            int j1 = way[j0];
            p[j0] = p[j1];
            j0 = j1;
        } while (j0);
    }
    for (int j = 1; j <= n; ++j)
        if (p[j] >= 1 && p[j] <= rows && j <= cols) result[p[j] - 1] = j - 1;
    return result;
}

namespace {

constexpr double kForbidden = 1e6;
constexpr double kForced = -1e3;

struct Track {
    Vec2 center;  // reference-frame coordinates
    double size;
    std::vector<float> embedding;
    std::vector<std::pair<int, int>> members;  // (frame, detection)
};

double embedding_distance(const std::vector<float>& a, const std::vector<float>& b) {
    if (a.empty() || b.empty() || a.size() != b.size()) return -1;
    double dot = 0, na = 0, nb = 0;
    for (size_t i = 0; i < a.size(); ++i) dot += a[i] * b[i], na += a[i] * a[i], nb += b[i] * b[i];
    if (na <= 0 || nb <= 0) return -1;
    return 1 - dot / std::sqrt(na * nb);
}

double median(std::vector<double> v) {
    std::sort(v.begin(), v.end());
    return v.empty() ? 0 : v[v.size() / 2];
}

}  // namespace

AssociationResult associate(const std::vector<std::vector<Detection>>& frames,
                            const std::vector<PairConstraint>& constraints, const AssociationOptions& opt) {
    const int F = int(frames.size());
    for (const PairConstraint& c : constraints) {
        auto ok = [&](int f, int d) { return f >= 0 && f < F && d >= 0 && d < int(frames[f].size()); };
        if (!ok(c.frame_a, c.detection_a) || !ok(c.frame_b, c.detection_b))
            throw std::invalid_argument("associate: constraint references a missing detection");
        if (c.same_person && c.frame_a == c.frame_b && c.detection_a != c.detection_b)
            throw std::invalid_argument("associate: two faces in one frame cannot be the same person");
    }
    AssociationResult res;
    res.person.resize(F);
    res.uncertain.resize(F);
    res.frame_shift.assign(F, Vec2{});
    for (int f = 0; f < F; ++f) {
        res.person[f].assign(frames[f].size(), -1);
        res.uncertain[f].assign(frames[f].size(), false);
    }
    if (F == 0) return res;

    // Reference: the frame showing the most people (earliest on ties).
    int ref = 0;
    for (int f = 1; f < F; ++f)
        if (frames[f].size() > frames[ref].size()) ref = f;
    std::vector<Track> tracks;
    for (size_t d = 0; d < frames[ref].size(); ++d) {
        tracks.push_back({frames[ref][d].center, frames[ref][d].size, frames[ref][d].embedding, {{ref, int(d)}}});
        res.person[ref][d] = int(d);
    }

    // -1: forbidden, +1: forced, 0: unconstrained.
    auto relation = [&](const Track& t, int f, int d) {
        int rel = 0;
        for (const PairConstraint& c : constraints)
            for (auto [mf, md] : t.members) {
                bool hit = (c.frame_a == mf && c.detection_a == md && c.frame_b == f && c.detection_b == d) ||
                           (c.frame_b == mf && c.detection_b == md && c.frame_a == f && c.detection_a == d);
                if (!hit) continue;
                if (!c.same_person) return -1;
                rel = 1;
            }
        return rel;
    };

    std::vector<int> order;
    for (int k = 1; k < F; ++k) {
        if (ref - k >= 0) order.push_back(ref - k);
        if (ref + k < F) order.push_back(ref + k);
    }
    for (int f : order) {
        const std::vector<Detection>& dets = frames[f];
        const int D = int(dets.size());
        if (D == 0) continue;
        Vec2 shift{};
        std::vector<int> match(D, -1);
        std::vector<double> match_cost(D, 0), runner_up(D, kForbidden);
        // Two passes: match, estimate the scene shift from the matches, match again.
        for (int pass = 0; pass < 2; ++pass) {
            const int T = int(tracks.size());
            // Columns: existing tracks, then one "new person" slot per detection.
            const int cols = T + D;
            std::vector<double> cost(size_t(D) * cols, kForbidden);
            for (int d = 0; d < D; ++d) {
                for (int t = 0; t < T; ++t) {
                    int rel = relation(tracks[t], f, d);
                    double c;
                    if (rel < 0) {
                        c = kForbidden;
                    } else if (rel > 0) {
                        c = kForced;
                    } else {
                        double mean_size = 0.5 * (dets[d].size + tracks[t].size);
                        c = norm(dets[d].center + shift - tracks[t].center) / mean_size +
                            0.5 * std::abs(std::log(dets[d].size / tracks[t].size));
                        double e = embedding_distance(dets[d].embedding, tracks[t].embedding);
                        if (e >= 0) c += opt.embedding_weight * e;
                    }
                    cost[size_t(d) * cols + t] = c;
                }
                cost[size_t(d) * cols + T + d] = opt.gate;
            }
            std::vector<int> assign = solve_assignment(cost, D, cols);
            std::vector<double> dx, dy;
            for (int d = 0; d < D; ++d) {
                int t = assign[d];
                match[d] = t < T && cost[size_t(d) * cols + t] < opt.gate ? t : -1;
                match_cost[d] = match[d] >= 0 ? cost[size_t(d) * cols + t] : opt.gate;
                runner_up[d] = kForbidden;
                for (int o = 0; o < T; ++o)
                    if (o != match[d]) runner_up[d] = std::min(runner_up[d], cost[size_t(d) * cols + o]);
                if (match[d] >= 0) {
                    dx.push_back(tracks[match[d]].center.x - dets[d].center.x);
                    dy.push_back(tracks[match[d]].center.y - dets[d].center.y);
                }
            }
            if (pass == 0 && !dx.empty()) {
                // Adopt a scene shift only when most matches agree on it;
                // people moving independently are not camera motion.
                Vec2 m{median(dx), median(dy)};
                size_t inliers = 0;
                for (size_t i = 0; i < dx.size(); ++i)
                    if (std::hypot(dx[i] - m.x, dy[i] - m.y) < 0.5 * dets[0].size) ++inliers;
                if (2 * inliers > dx.size()) shift = m;
            }
        }
        res.frame_shift[f] = shift;
        for (int d = 0; d < D; ++d) {
            int t = match[d];
            if (t < 0) {
                // Starting a new person next to a plausible existing one is itself ambiguous.
                res.uncertain[f][d] = runner_up[d] < opt.gate + opt.ambiguity_margin;
                t = int(tracks.size());
                tracks.push_back({dets[d].center + shift, dets[d].size, dets[d].embedding, {}});
            } else {
                bool forced = match_cost[d] <= kForced / 2;
                res.uncertain[f][d] = !forced && runner_up[d] - match_cost[d] < opt.ambiguity_margin;
                if (tracks[t].embedding.empty()) tracks[t].embedding = dets[d].embedding;
            }
            tracks[t].members.push_back({f, d});
            res.person[f][d] = t;
        }
    }
    res.person_count = int(tracks.size());
    // Explicit corrections must hold in the result; report contradictions instead of guessing.
    for (const PairConstraint& c : constraints) {
        bool same = res.person[c.frame_a][c.detection_a] == res.person[c.frame_b][c.detection_b];
        if (same != c.same_person) throw std::invalid_argument("associate: constraints cannot all be satisfied");
    }
    return res;
}

}  // namespace bt
