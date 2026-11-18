#include "bt/association.h"
#include "bt/selection.h"
#include "fixture.h"
#include "test.h"

using namespace bt;

namespace {

SelectionProblem make_problem(int frames, int persons) {
    SelectionProblem p;
    p.frame_count = frames;
    p.person_count = persons;
    p.scene_cost.assign(frames, 0.0);
    p.present.assign(persons, std::vector<bool>(frames, true));
    p.eligible = p.present;
    p.face_cost.assign(persons, std::vector<double>(frames, 0.5));
    p.required.assign(persons, true);
    p.merge = [](int, int, int) { return MergeEvaluation{true, 0.05, ""}; };
    return p;
}

}  // namespace

TEST(assignment_finds_minimum_cost_matching) {
    std::vector<double> c = {4, 1, 3, 2, 0, 5, 3, 2, 2};
    std::vector<int> a = solve_assignment(c, 3, 3);
    CHECK(a[0] == 1 && a[1] == 0 && a[2] == 2);
    std::vector<int> wide = solve_assignment({5, 1, 9, 1, 5, 9}, 2, 3);
    CHECK(wide[0] == 1 && wide[1] == 0);
    std::vector<int> tall = solve_assignment({1, 9, 9, 1, 5, 5}, 3, 2);
    CHECK(tall[0] == 0 && tall[1] == 1 && tall[2] == -1);
    CHECK_THROWS(solve_assignment({1, 2, 3}, 2, 2));
}

TEST(association_tracks_people_through_camera_shift_and_reordering) {
    std::vector<std::vector<Detection>> frames(3);
    std::vector<Vec2> people = {{200, 300}, {500, 320}, {800, 290}};
    Vec2 shifts[3] = {{0, 0}, {60, -20}, {-45, 30}};
    int orders[3][3] = {{0, 1, 2}, {2, 0, 1}, {1, 2, 0}};
    for (int f = 0; f < 3; ++f)
        for (int k = 0; k < 3; ++k) frames[f].push_back({people[orders[f][k]] + shifts[f], 120, {}});
    AssociationResult r = associate(frames);
    CHECK(r.person_count == 3);
    for (int f = 0; f < 3; ++f)
        for (int k = 0; k < 3; ++k) {
            CHECK(r.person[f][k] == r.person[0][orders[f][k]]);
            CHECK(!r.uncertain[f][k]);
        }
    CHECK_NEAR(r.frame_shift[1].x, -60, 1e-9);
}

TEST(association_starts_new_person_and_keeps_frames_one_to_one) {
    std::vector<std::vector<Detection>> frames(2);
    frames[0] = {{{200, 300}, 100, {}}, {{600, 300}, 100, {}}};
    frames[1] = {{{205, 300}, 100, {}}, {{210, 310}, 100, {}}};  // two faces near person 0
    AssociationResult r = associate(frames);
    CHECK(r.person[1][0] != r.person[1][1]);
    CHECK(r.person[1][0] == 0);
    CHECK(r.uncertain[1][0] || r.uncertain[1][1]);  // crossing faces stay conservative
}

TEST(association_uses_embeddings_and_flags_ambiguity) {
    std::vector<std::vector<Detection>> frames(2);
    // Siblings swap places between frames; appearance resolves it.
    frames[0] = {{{300, 300}, 100, {1, 0}}, {{380, 300}, 100, {0, 1}}};
    frames[1] = {{{300, 300}, 100, {0, 1}}, {{380, 300}, 100, {1, 0}}};
    AssociationResult r = associate(frames);
    CHECK(r.person[1][0] == r.person[0][1]);
    CHECK(r.person[1][1] == r.person[0][0]);
}

TEST(association_honors_user_corrections) {
    std::vector<std::vector<Detection>> frames(2);
    frames[0] = {{{300, 300}, 100, {}}, {{380, 300}, 100, {}}};
    frames[1] = {{{300, 300}, 100, {}}, {{380, 300}, 100, {}}};
    // The user says the people swapped places.
    std::vector<PairConstraint> fix = {{0, 0, 1, 1, true}};
    AssociationResult r = associate(frames, fix);
    CHECK(r.person[1][1] == r.person[0][0]);
    CHECK(r.person[1][0] == r.person[0][1]);
    CHECK(!r.uncertain[1][1]);
    // "Wrong person": forbid the natural match.
    AssociationResult s = associate(frames, {{0, 0, 1, 0, false}});
    CHECK(s.person[1][0] != s.person[0][0]);
    CHECK_THROWS(associate(frames, {{0, 0, 0, 1, true}}));
    CHECK_THROWS(associate(frames, {{0, 0, 5, 0, true}}));
    CHECK_THROWS(associate(frames, {{0, 0, 1, 0, true}, {0, 0, 1, 0, false}}));
}

TEST(selection_recommends_complete_frame_when_one_exists) {
    SelectionProblem p = make_problem(4, 3);
    for (int q = 0; q < 3; ++q) p.face_cost[q][2] = 0.1;  // everyone looks good in frame 2
    p.face_cost[0][1] = 0.08;                             // marginally better elsewhere
    SelectionResult r = select_sources(p);
    CHECK(r.valid && r.base == 2);
    CHECK(r.edits() == 0);  // not worth an edit
}

TEST(selection_composes_when_people_peak_in_different_frames) {
    SelectionProblem p = make_problem(4, 3);
    p.face_cost[0] = {0.1, 0.6, 0.6, 0.6};
    p.face_cost[1] = {0.6, 0.1, 0.6, 0.6};
    p.face_cost[2] = {0.6, 0.1, 0.6, 0.6};
    SelectionResult r = select_sources(p);
    CHECK(r.base == 1);  // fewest changes
    CHECK(r.source[0] == 0 && r.source[1] == 1 && r.source[2] == 1);
    CHECK(r.edits() == 1);
    CHECK_NEAR(r.cost, 0.1 * 3 + 0.05 + 0.12, 1e-12);
}

TEST(selection_enforces_feasibility_and_required_people) {
    SelectionProblem p = make_problem(3, 2);
    p.face_cost[0] = {0.1, 0.7, 0.3};
    p.face_cost[1] = {0.7, 0.1, 0.2};
    p.present[1][0] = false;  // person 1 missing from frame 0: cannot be the base
    p.merge = [](int, int s, int) { return s == 0 ? MergeEvaluation{false, 0, "turned too far"} : MergeEvaluation{true, 0.05, ""}; };
    SelectionResult r = select_sources(p);
    CHECK(r.base != 0);
    CHECK(r.source[0] != 0);  // infeasible donor never used
    p.eligible[0][2] = false;  // uncertain identity in frame 2
    p.merge = [](int, int, int) { return MergeEvaluation{true, 0.05, ""}; };
    r = select_sources(p);
    CHECK(r.base == 1 && r.source[0] == 0);
    p.eligible[0][0] = false;
    r = select_sources(p);
    CHECK(r.source[0] == r.base);  // no eligible donor: keep the original
    for (int f = 0; f < 3; ++f) p.present[1][f] = false;
    p.present[1][1] = true;
    p.required[1] = true;
    CHECK(select_sources(p).base == 1);
    p.present[1][1] = false;
    CHECK(!select_sources(p).valid);
}

TEST(selection_never_replaces_locked_choices) {
    SelectionProblem p = make_problem(3, 2);
    p.face_cost[0] = {0.1, 0.5, 0.5};
    p.face_cost[1] = {0.5, 0.1, 0.5};
    p.locks[0] = 2;  // the user prefers the "worse" frame 2 for person 0
    SelectionResult r = select_sources(p);
    CHECK(r.source[0] == 2);
    CHECK(r.conflicts.empty());
    // Changing the base to one the lock cannot merge into reports a conflict, keeps the lock.
    p.locked_base = 1;
    p.merge = [](int q, int s, int b) {
        return q == 0 && s == 2 && b == 1 ? MergeEvaluation{false, 0, "turned too far"} : MergeEvaluation{true, 0.05, ""};
    };
    r = select_sources(p);
    CHECK(r.base == 1 && r.source[0] == 2);
    CHECK(r.conflicts.size() == 1 && r.conflicts[0].person == 0 && r.conflicts[0].reason == "turned too far");
    // Without a locked base, a base honoring the lock is preferred.
    p.locked_base = -1;
    r = select_sources(p);
    CHECK(r.conflicts.empty() && r.source[0] == 2);
    p.locks[1] = 7;
    CHECK_THROWS(select_sources(p));
}

TEST(selection_optimizes_interacting_neighbors_jointly) {
    SelectionProblem p = make_problem(3, 2);
    p.face_cost[0] = {0.5, 0.1, 0.2};
    p.face_cost[1] = {0.5, 0.2, 0.1};
    p.locked_base = 0;
    p.neighbors = {{0, 1}};
    // Their heads overlap unless both come from the same frame.
    p.interaction = [](int, int, int sp, int sq, int b) { return sp != b && sq != b && sp != sq ? 5.0 : 0.0; };
    SelectionResult r = select_sources(p);
    CHECK(r.source[0] == r.source[1]);
    CHECK(r.source[0] != 0);
    p.interaction = nullptr;
    r = select_sources(p);
    CHECK(r.source[0] == 1 && r.source[1] == 2);
}

TEST(selection_lists_diverse_donors) {
    SelectionProblem p = make_problem(6, 1);
    p.face_cost[0] = {0.5, 0.100, 0.101, 0.102, 0.103, 0.3};
    p.locked_base = 0;
    int calls = 0;
    p.merge = [&](int, int, int) {
        ++calls;
        return MergeEvaluation{true, 0.05, ""};
    };
    select_sources(p);
    CHECK(calls == 2);  // one of the near-identical smiles plus the distinct option
}

TEST(scoring_separates_technical_quality_from_preference) {
    fixture::Pose pose{fixture::make_similarity(90, 0, 200, 190)};
    Image sharp = fixture::render(400, 400, pose);
    Image blurred = gaussian_blur(sharp, 4.0);
    FaceLandmarks lm = fixture::landmarks(pose);
    FaceMeasurements ms = measure_face(full_view(sharp), lm), mb = measure_face(full_view(blurred), lm);
    CHECK_NEAR(ms.interocular_px, 90, 1e-9);
    CHECK(ms.sharpness > 3 * mb.sharpness);
    CHECK(technical_cost(mb) > technical_cost(ms) + 0.1);
    // Preference: a serious face is not penalized when smiles carry no weight.
    FaceMeasurements serious, blink;
    serious.smile = 0;
    blink.eyes_open = 0.05;
    blink.smile = 1;
    Preference neutral{1.0, 0.0, 0.0};
    CHECK_NEAR(preference_cost(serious, neutral), 0, 1e-12);
    CHECK(preference_cost(blink, neutral) > 0.9);
    CHECK(preference_cost(blink, Preference{}) > preference_cost(serious, Preference{}));
    blink.confidence = 0;
    CHECK_NEAR(preference_cost(blink, neutral), 0, 1e-12);
    // Technical cost is unaffected by expression.
    FaceMeasurements a = ms, b = ms;
    b.smile = 1, b.eyes_open = 0;
    CHECK_NEAR(technical_cost(a), technical_cost(b), 1e-12);
    Compatibility bad;
    bad.rejection = "turned too far";
    CHECK(!merge_cost(bad).feasible && merge_cost(bad).rejection == "turned too far");
}
