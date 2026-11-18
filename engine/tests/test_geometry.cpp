#include "bt/geometry.h"
#include "bt/landmarks.h"
#include "bt/regions.h"
#include "fixture.h"
#include "test.h"

using namespace bt;

TEST(similarity_recovers_known_transform) {
    Similarity t = fixture::make_similarity(1.3, 17, 40, -12);
    std::vector<Vec2> src = {{0, 0}, {10, 0}, {3, 8}, {-5, 2}}, dst;
    for (Vec2 p : src) dst.push_back(t.apply(p));
    Similarity e = estimate_similarity(src, dst, {1, 2, 0.5, 1});
    CHECK_NEAR(e.a, t.a, 1e-12);
    CHECK_NEAR(e.b, t.b, 1e-12);
    CHECK_NEAR(e.tx, t.tx, 1e-10);
    CHECK_NEAR(e.ty, t.ty, 1e-10);
    Vec2 back = t.inverse().apply(t.apply({3, 4}));
    CHECK_NEAR(back.x, 3, 1e-12);
    CHECK_NEAR(back.y, 4, 1e-12);
    Vec2 c = compose(t, t.inverse()).apply({7, -2});
    CHECK_NEAR(c.x, 7, 1e-10);
    CHECK_NEAR(c.y, -2, 1e-10);
}

TEST(similarity_rejects_degenerate_input) {
    CHECK_THROWS(estimate_similarity({{0, 0}}, {{1, 1}}, {1}));
    CHECK_THROWS(estimate_similarity({{1, 1}, {1, 1}}, {{0, 0}, {2, 2}}, {1, 1}));
    CHECK_THROWS(estimate_similarity({{0, 0}, {1, 0}}, {{0, 0}}, {1, 1}));
}

TEST(convex_polygon_operations) {
    std::vector<Vec2> sq = convex_hull({{0, 0}, {4, 0}, {4, 4}, {0, 4}, {2, 2}, {1, 3}});
    CHECK(sq.size() == 4);
    std::vector<Vec2> other = {{2, 2}, {6, 2}, {6, 6}, {2, 6}};
    std::vector<Vec2> inter = convex_intersection(sq, other);
    Vec2 c = centroid(inter);
    CHECK_NEAR(c.x, 3, 1e-9);
    CHECK_NEAR(c.y, 3, 1e-9);
    std::vector<Vec2> inset = convex_offset(sq, 1);
    std::vector<double> r = radial_profile(inset, {2, 2}, 4);
    for (double v : r) CHECK_NEAR(v, 1, 1e-9);
    std::vector<double> grown = radial_profile(convex_offset(sq, -1), {2, 2}, 4);
    for (double v : grown) CHECK_NEAR(v, 3, 1e-6);
    CHECK(convex_offset(sq, 3).empty());
    CHECK_THROWS(radial_profile(sq, {10, 10}, 8));
}

TEST(head_alignment_ignores_expression_landmarks) {
    fixture::Pose base{fixture::make_similarity(100, 0, 300, 300)};
    fixture::Pose donor{fixture::make_similarity(110, 4, 320, 290), 1.0, 1.0};
    FaceLandmarks bl = fixture::landmarks(base), dl = fixture::landmarks(donor);
    // Exaggerate the expression: move every lip, eyelid, brow, and jaw landmark.
    for (int i : topology::lips) dl.points[i].y += 25;
    for (int i : topology::left_brow) dl.points[i].y -= 12;
    for (int i : {152, 148, 176, 377, 400}) dl.points[i].y += 30;
    HeadAlignment a = estimate_head_alignment(dl, bl);
    Similarity truth = compose(base.image_from_face, donor.image_from_face.inverse());
    CHECK_NEAR(a.base_from_donor.a, truth.a, 1e-9);
    CHECK_NEAR(a.base_from_donor.b, truth.b, 1e-9);
    CHECK_NEAR(a.base_from_donor.tx, truth.tx, 1e-6);
    CHECK_NEAR(a.base_from_donor.ty, truth.ty, 1e-6);
    CHECK_NEAR(a.support_residual, 0, 1e-9);
    // The donor's own mouth geometry survives alignment instead of being matched to the base.
    double donor_mouth = norm(a.base_from_donor.apply(dl.points[61]) - a.base_from_donor.apply(dl.points[291]));
    double base_mouth = norm(bl.points[61] - bl.points[291]);
    CHECK(donor_mouth > 1.2 * base_mouth);
}

TEST(head_alignment_is_robust_to_one_bad_support_point) {
    fixture::Pose base{fixture::make_similarity(100, 0, 300, 300)};
    FaceLandmarks bl = fixture::landmarks(base), dl = bl;
    dl.points[127].x -= 40;  // temple landmark hidden by hair
    HeadAlignment a = estimate_head_alignment(dl, bl);
    CHECK_NEAR(a.base_from_donor.scale(), 1, 0.01);
    CHECK_NEAR(a.base_from_donor.tx, 0, 1.5);
}

TEST(landmarks_validation) {
    FaceLandmarks f;
    f.points.assign(100, Vec2{0, 0});
    CHECK_THROWS(interocular_distance(f));
    f = fixture::landmarks({fixture::make_similarity(100, 0, 300, 300)});
    CHECK_NEAR(interocular_distance(f), 100, 1e-9);
    f.points[5].x = std::nan("");
    CHECK_THROWS(require_valid(f));
}

TEST(regions_cover_features_and_stay_inside_silhouette) {
    fixture::Pose base{fixture::make_similarity(100, 0, 300, 300)};
    fixture::Pose donor{fixture::make_similarity(100, 0, 300, 300), 1.0};
    FaceLandmarks bl = fixture::landmarks(base), dl = fixture::landmarks(donor);
    EditRegions g;
    RegionReport rep = build_regions(bl, dl, Similarity{}, &g);
    CHECK(rep.feasible);
    CHECK(rep.min_band_width >= 0.05);
    CHECK_NEAR(rep.silhouette_overlap, 1, 1e-9);
    auto radius_and_angle = [&](Vec2 p, double* th) {
        *th = std::atan2(p.y - g.center.y, p.x - g.center.x);
        return norm(p - g.center);
    };
    for (const FaceLandmarks* f : {&bl, &dl})
        for (const std::vector<int>* grp : {&topology::lips, &topology::left_eye, &topology::right_eye,
                                            &topology::left_brow, &topology::right_brow, &topology::nose})
            for (int i : *grp) {
                double th, r = radius_and_angle(f->points[i], &th);
                CHECK(r < profile_radius(g.core, th));
            }
    for (int i : topology::face_oval) {
        double th, r = radius_and_angle(bl.points[i], &th);
        CHECK(r > profile_radius(g.outer, th));
    }
    Rect s = support_rect(g, 1.0);
    CHECK(s.x0 > 150 && s.x1 < 450 && s.y0 > 100 && s.y1 < 500);
    Rect half = support_rect(g, 0.5);
    CHECK(std::abs(half.width() * 2 - s.width()) <= 6);
}

TEST(regions_reject_incompatible_silhouette) {
    fixture::Pose base{fixture::make_similarity(100, 0, 300, 300)};
    FaceLandmarks bl = fixture::landmarks(base), dl = bl;
    // Donor silhouette squeezed on one side, as in a strong head turn.
    for (int i : topology::face_oval)
        if (dl.points[i].x > 300) dl.points[i].x = 300 + 0.45 * (dl.points[i].x - 300);
    EditRegions g;
    RegionReport rep = build_regions(bl, dl, Similarity{}, &g);
    CHECK(!rep.feasible);
    CHECK(!rep.rejection.empty());
}
