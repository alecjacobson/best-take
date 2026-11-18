#include <array>
#include "bt/session.h"
#include "fixture.h"
#include "test.h"

using namespace bt;

namespace {

struct Group {
    std::vector<Image> images;
    std::vector<FrameSource> frames;
    int loads = 0;
    double min_scale = 1;
};

// Two people side by side; blink[f][p] says whether person p blinks in frame f.
// Each frame has a slightly different camera shift and exposure.
Group make_group(const std::vector<std::array<bool, 2>>& blink) {
    Group g;
    for (size_t f = 0; f < blink.size(); ++f) {
        Image img(800, 400, 3);
        FrameSource src;
        src.id = "frame" + std::to_string(f);
        src.hash = "hash" + std::to_string(f);
        src.width = 800, src.height = 400;
        for (int p = 0; p < 2; ++p) {
            fixture::Pose pose{fixture::make_similarity(88 + 2.0 * f, 1.5 * f, 200 + 3.0 * f, 190 - 2.0 * f),
                               blink[f][p] ? 0.0 : 1.0, blink[f][p] ? 1.0 : 0.0, 1.0 + 0.05 * f};
            Image half = fixture::render(400, 400, pose);
            paste(img, half, 400 * p, 0);
            FaceInput face;
            face.landmarks = fixture::landmarks(pose);
            for (Vec2& q : face.landmarks.points) q.x += 400 * p;
            face.state.eyes_open = blink[f][p] ? 0.05 : 0.95;
            face.state.smile = blink[f][p] ? 0.0 : 1.0;
            src.faces.push_back(face);
        }
        g.images.push_back(img);
        g.frames.push_back(src);
    }
    return g;
}

RegionLoader loader_for(Group& g) {
    return [&g](int frame, const Rect& rect, double scale) {
        ++g.loads;
        g.min_scale = std::min(g.min_scale, scale);
        const Image& full = g.images[frame];
        if (scale == 1) return crop(full, rect);
        Image reduced = resize_area(full, int(std::ceil(full.width * scale)), int(std::ceil(full.height * scale)));
        return crop(reduced, rect);
    };
}

float eye_darkness(const Image& img, const FrameSource& base, int person) {
    Vec2 a = base.faces[person].landmarks.points[33], b = base.faces[person].landmarks.points[133];
    // Just above the lid line: iris when the eye is open, skin when closed.
    Vec2 c = (a + b) * 0.5;
    return img.at(int(c.x), int(c.y - 0.3 * norm(a - b)), 1);
}

}  // namespace

TEST(session_proposes_and_renders_a_composite) {
    Group g = make_group({{true, false}, {false, true}, {true, true}});
    Session s(g.frames, loader_for(g));
    CHECK(s.frame_count() == 3 && s.person_count() == 2);
    SelectionResult sel = s.propose();
    CHECK(sel.valid && sel.conflicts.empty());
    CHECK(sel.base == 0 || sel.base == 1);
    CHECK(sel.edits() == 1);
    int left = s.association().person[0][0], right = s.association().person[0][1];
    CHECK(sel.source[left] == 1 && sel.source[right] == 0);

    Recipe recipe = s.build_recipe(sel.base, sel.source);
    CHECK(recipe.replacements.size() == 1);
    CHECK(recipe.base_hash == g.frames[sel.base].hash);
    CHECK(recipe.donor_hashes.size() == 1);
    std::vector<QualityReport> reports;
    Image out = s.render(recipe, 1.0, &reports);
    CHECK(reports.size() == 1 && reports[0].ok);
    // Both people now have open (dark) eyes; the base had one closed (skin-colored).
    const FrameSource& base = g.frames[sel.base];
    CHECK(eye_darkness(out, base, 0) < 0.1f && eye_darkness(out, base, 1) < 0.1f);
    CHECK(std::max(eye_darkness(g.images[sel.base], base, 0), eye_darkness(g.images[sel.base], base, 1)) > 0.25f);

    // The stored recipe reproduces the same pixels, including after a restart.
    Group g2 = make_group({{true, false}, {false, true}, {true, true}});
    Session restarted(g2.frames, loader_for(g2));
    Image again = restarted.render(recipe_from_json(recipe_to_json(recipe)), 1.0);
    CHECK(again.data == out.data);

    // Preview render uses the same recipe at reduced scale.
    Image preview = s.render(recipe, 0.5, &reports);
    CHECK(preview.width == 400 && reports[0].ok);
    CHECK(g.min_scale <= 0.5);
}

TEST(session_recommends_untouched_frame_when_everyone_looks_good) {
    Group g = make_group({{true, false}, {false, false}, {false, true}});
    Session s(g.frames, loader_for(g));
    SelectionResult sel = s.propose();
    CHECK(sel.base == 1 && sel.edits() == 0);
    CHECK(s.build_recipe(sel.base, sel.source).replacements.empty());
}

TEST(session_candidates_locks_and_explicit_choices) {
    Group g = make_group({{true, false}, {false, true}, {true, true}});
    Session s(g.frames, loader_for(g));
    std::vector<Candidate> c = s.candidates(0, 0);
    CHECK(c.size() == 3);
    CHECK(c[0].is_base && c[0].frame == 0 && c[0].feasible);
    CHECK(c[1].frame == 1 && c[1].feasible);  // open eyes rank ahead of the other blink
    CHECK(c[1].face_cost < c[2].face_cost);

    // A lock is honored even though it is the worse expression.
    SelectionResult sel = s.propose({{0, 2}}, 0);
    CHECK(sel.base == 0 && sel.source[0] == 2);

    // Explicit infeasible or out-of-range choices are refused, not substituted.
    CHECK_THROWS(s.build_recipe(0, {1}));
    CHECK_THROWS(s.build_recipe(0, {9, 0}));
    CHECK_THROWS(s.build_recipe(7, {0, 0}));
    Recipe r = s.build_recipe(0, {1, -1});
    CHECK(r.replacements.size() == 1 && r.replacements[0].donor_id == "frame1");
    r.replacements[0].donor_id = "gone";
    CHECK_THROWS(s.render(r, 1.0));
}

TEST(session_applies_association_corrections) {
    Group g = make_group({{true, false}, {false, true}});
    Session s(g.frames, loader_for(g));
    int before = s.association().person[1][0];
    CHECK(before == s.association().person[0][0]);
    // The user marks the left face of frame 1 as a different person.
    s.set_constraints({{0, 0, 1, 0, false}});
    CHECK(s.association().person[1][0] != s.association().person[0][0]);
    // A contradictory correction is rejected and the previous state kept.
    CHECK_THROWS(s.set_constraints({{0, 0, 0, 1, true}}));
    CHECK(s.association().person[1][0] != s.association().person[0][0]);
}

TEST(session_validates_input) {
    Group g = make_group({{false, false}});
    CHECK_THROWS(Session({}, loader_for(g)));
    CHECK_THROWS(Session(g.frames, nullptr));
    std::vector<FrameSource> dup = {g.frames[0], g.frames[0]};
    CHECK_THROWS(Session(dup, loader_for(g)));
    Session bad(g.frames, [](int, const Rect&, double) { return Image(3, 3, 3); });
    CHECK_THROWS(bad.propose());
}
