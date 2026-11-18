#include <cstdio>
#include <cstdlib>

#include "bt/recipe.h"
#include "fixture.h"
#include "test.h"

using namespace bt;

namespace {

struct Scene {
    fixture::Pose base_pose, donor_pose;
    Image base, donor;
    FaceLandmarks base_lm, donor_lm;
};

// Base: neutral mouth, eyes closed. Donor: smiling, eyes open, moved, rotated, brighter.
Scene make_scene(double donor_scale = 1.06, double donor_gain = 1.15) {
    Scene s;
    s.base_pose = {fixture::make_similarity(90, 0, 200, 190), 0.0, 1.0, 1.0};
    s.donor_pose = {fixture::make_similarity(90 * donor_scale, 3, 215, 200), 1.0, 0.0, donor_gain};
    s.base = fixture::render(400, 400, s.base_pose);
    s.donor = fixture::render(440, 440, s.donor_pose);
    s.base_lm = fixture::landmarks(s.base_pose);
    s.donor_lm = fixture::landmarks(s.donor_pose);
    return s;
}

// What the base photo would show had the person held the donor expression.
Image ideal(const Scene& s) {
    fixture::Pose p = s.base_pose;
    p.smile = s.donor_pose.smile;
    p.blink = s.donor_pose.blink;
    return fixture::render(s.base.width, s.base.height, p);
}

void maybe_dump(const char* name, const Image& img) {
    const char* dir = std::getenv("BT_TEST_DUMP_DIR");
    if (!dir) return;
    std::string path = std::string(dir) + "/" + name + ".ppm";
    FILE* f = std::fopen(path.c_str(), "wb");
    if (!f) return;
    std::fprintf(f, "P6\n%d %d\n255\n", img.width, img.height);
    std::vector<uint8_t> buf(size_t(img.width) * img.height * 3);
    to_srgb8(img, buf.data(), img.width * 3, 3);
    std::fwrite(buf.data(), 1, buf.size(), f);
    std::fclose(f);
}

double polar_radius(const Replacement& r, int x, int y, double* core, double* outer) {
    double dx = x + 0.5 - r.regions.center.x, dy = y + 0.5 - r.regions.center.y;
    double th = std::atan2(dy, dx);
    *core = profile_radius(r.regions.core, th);
    *outer = profile_radius(r.regions.outer, th);
    return std::hypot(dx, dy);
}

void check_composite(BlendMode mode, const char* dump_name) {
    Scene s = make_scene();
    Replacement rep;
    BuildOptions opt;
    opt.blend = mode;
    Compatibility c = build_replacement(full_view(s.base), s.base_lm, full_view(s.donor), s.donor_lm, opt, &rep);
    CHECK(c.feasible);
    CHECK(c.rejection.empty());
    CHECK_NEAR(c.enlargement, 1 / 1.06, 1e-6);
    // The exposure difference is recovered by the color fit.
    for (int ch = 0; ch < 3; ++ch) CHECK_NEAR(rep.color.coef[ch][0], -std::log(1.15), 0.02);

    PatchResult patch = render_replacement(rep, full_view(s.base), full_view(s.donor));
    QualityReport q = validate_patch(rep, full_view(s.base), patch);
    CHECK(q.ok);
    CHECK(q.exterior_changed_pixels == 0);

    Image out = s.base;
    paste(out, patch.pixels, patch.rect.x0, patch.rect.y0);
    Image want = ideal(s);
    maybe_dump((std::string(dump_name) + "_base").c_str(), s.base);
    maybe_dump((std::string(dump_name) + "_donor").c_str(), s.donor);
    maybe_dump((std::string(dump_name) + "_out").c_str(), out);

    double core_err = 0, band_err = 0, max_band_err = 0;
    int core_n = 0, band_n = 0, exterior_diff = 0;
    for (int y = 0; y < out.height; ++y)
        for (int x = 0; x < out.width; ++x) {
            double core, outer, r = polar_radius(rep, x, y, &core, &outer);
            for (int ch = 0; ch < 3; ++ch) {
                double e = std::abs(out.at(x, y, ch) - want.at(x, y, ch));
                if (r >= outer) {
                    if (out.at(x, y, ch) != s.base.at(x, y, ch)) ++exterior_diff;
                } else if (r <= core) {
                    core_err += e, ++core_n;
                } else {
                    band_err += e, ++band_n;
                    max_band_err = std::max(max_band_err, e);
                }
            }
        }
    CHECK(exterior_diff == 0);                 // bit-exact base outside the support
    CHECK(core_err / core_n < 0.012);          // donor expression and detail preserved
    CHECK(band_err / band_n < 0.012);          // transition agrees with both sources
    CHECK(max_band_err < 0.25);                // no blowup (brow tails cross the band)
    // The edit actually happened: the mouth is open and the eyes are open.
    Vec2 mouth = s.base_pose.image_from_face.apply({0, 0.72});
    CHECK(out.at(int(mouth.x), int(mouth.y), 1) > 0.5f);
    CHECK(s.base.at(int(mouth.x), int(mouth.y), 1) < 0.2f);
}

}  // namespace

TEST(composite_multiband_matches_ground_truth) { check_composite(BlendMode::Multiband, "multiband"); }

TEST(composite_screened_gradient_matches_ground_truth) { check_composite(BlendMode::ScreenedGradient, "screened"); }

TEST(uncorrected_exposure_would_be_caught_as_a_seam) {
    // Rendering with the color fit removed leaves a brightness step that validation flags.
    Scene s = make_scene(1.0, 1.6);
    Replacement rep;
    Compatibility c = build_replacement(full_view(s.base), s.base_lm, full_view(s.donor), s.donor_lm, {}, &rep);
    CHECK(c.feasible);
    PatchResult good = render_replacement(rep, full_view(s.base), full_view(s.donor));
    CHECK(validate_patch(rep, full_view(s.base), good).ok);
    Replacement raw = rep;
    for (auto& row : raw.color.coef) row[0] = row[1] = row[2] = 0;
    raw.feather = 0.5;  // and a hard edge
    PatchResult bad = render_replacement(raw, full_view(s.base), full_view(s.donor));
    QualityReport q = validate_patch(raw, full_view(s.base), bad);
    CHECK(!q.ok);
    CHECK(q.failure == "visible seam");
    CHECK(bad.excess_gradient > 4 * good.excess_gradient);
}

TEST(recipe_round_trip_renders_identical_pixels) {
    Scene s = make_scene();
    Recipe recipe;
    recipe.algorithm = kEngineAlgorithm;
    recipe.base_id = "base";
    recipe.base_hash = "abc";
    recipe.donor_hashes["frame \"2\""] = "def";
    Replacement rep;
    CHECK(build_replacement(full_view(s.base), s.base_lm, full_view(s.donor), s.donor_lm, {}, &rep).feasible);
    rep.person_id = 3;
    rep.donor_id = "frame \"2\"";
    recipe.replacements.push_back(rep);

    std::string text = recipe_to_json(recipe);
    Recipe again = recipe_from_json(text);
    CHECK(recipe_to_json(again) == text);
    CHECK(again.replacements.size() == 1 && again.replacements[0].person_id == 3);

    std::map<std::string, ImageView> donors{{rep.donor_id, full_view(s.donor)}};
    Image a = s.base, b = s.base;
    std::vector<QualityReport> qa = render_recipe(recipe, a, full_view(a), donors);
    render_recipe(again, b, full_view(b), donors);
    CHECK(qa.size() == 1 && qa[0].ok);
    CHECK(a.data == b.data);
    CHECK(a.data != s.base.data);
}

TEST(recipe_rejects_unknown_versions_and_missing_donors) {
    Recipe recipe;
    recipe.algorithm = kEngineAlgorithm;
    std::string text = recipe_to_json(recipe);
    std::string bumped = text;
    bumped.replace(bumped.find("\"format_version\":1"), 18, "\"format_version\":9");
    CHECK_THROWS(recipe_from_json(bumped));
    CHECK_THROWS(recipe_from_json("{\"format_version\":1"));
    CHECK_THROWS(recipe_from_json(text + "x"));

    Scene s = make_scene();
    Replacement rep;
    CHECK(build_replacement(full_view(s.base), s.base_lm, full_view(s.donor), s.donor_lm, {}, &rep).feasible);
    rep.donor_id = "missing";
    recipe.replacements.push_back(rep);
    Image canvas = s.base;
    CHECK_THROWS(render_recipe(recipe, canvas, full_view(canvas), {}));
    recipe.algorithm = "some-future-engine";
    CHECK_THROWS(render_recipe(recipe, canvas, full_view(canvas), {{"missing", full_view(s.donor)}}));
    CHECK(canvas.data == s.base.data);
}

TEST(preview_matches_downsampled_full_resolution_render) {
    Scene s = make_scene();
    Replacement rep;
    CHECK(build_replacement(full_view(s.base), s.base_lm, full_view(s.donor), s.donor_lm, {}, &rep).feasible);
    Image full = s.base;
    PatchResult p = render_replacement(rep, full_view(s.base), full_view(s.donor));
    paste(full, p.pixels, p.rect.x0, p.rect.y0);

    Image base_half = resize_area(s.base, 200, 200), donor_half = resize_area(s.donor, 220, 220);
    Image preview = base_half;
    PatchResult ph = render_replacement(rep, full_view(base_half, 0.5), full_view(donor_half, 0.5));
    CHECK(validate_patch(rep, full_view(base_half, 0.5), ph).ok);
    paste(preview, ph.pixels, ph.rect.x0, ph.rect.y0);

    Image full_half = resize_area(full, 200, 200);
    double err = 0, max_err = 0;
    for (size_t i = 0; i < preview.data.size(); ++i) {
        double e = std::abs(preview.data[i] - full_half.data[i]);
        err += e;
        max_err = std::max(max_err, e);
    }
    CHECK(err / preview.data.size() < 0.002);
    CHECK(max_err < 0.2);  // isolated differences on high-contrast feature edges
}

TEST(cropped_views_render_identically_and_missing_pixels_throw) {
    Scene s = make_scene();
    Replacement rep;
    CHECK(build_replacement(full_view(s.base), s.base_lm, full_view(s.donor), s.donor_lm, {}, &rep).feasible);
    PatchResult whole = render_replacement(rep, full_view(s.base), full_view(s.donor));

    ImageView bv = full_view(s.base), dv = full_view(s.donor);
    Rect br = replacement_rect(rep, bv), dr = donor_required_rect(rep, 1.0, dv.bounds);
    CHECK(dr.width() < s.donor.width && dr.height() < s.donor.height);
    Image bc = crop(s.base, br), dc = crop(s.donor, dr);
    ImageView bcv{&bc, br, bv.bounds, 1.0}, dcv{&dc, dr, dv.bounds, 1.0};
    PatchResult part = render_replacement(rep, bcv, dcv);
    CHECK(part.rect == whole.rect);
    CHECK(part.pixels.data == whole.pixels.data);

    Rect small{dr.x0 + 40, dr.y0, dr.x1, dr.y1};
    Image sc = crop(s.donor, small);
    ImageView scv{&sc, small, dv.bounds, 1.0};
    CHECK_THROWS(render_replacement(rep, bcv, scv));
    Rect bsmall{br.x0, br.y0 + 10, br.x1, br.y1};
    Image bs = crop(s.base, bsmall);
    ImageView bsv{&bs, bsmall, bv.bounds, 1.0};
    CHECK_THROWS(render_replacement(rep, bsv, dcv));
}

TEST(incompatible_donors_are_rejected_with_reasons) {
    Scene s = make_scene();
    Replacement rep;
    // Donor face far smaller than needed.
    Scene tiny = make_scene(0.4);
    Compatibility c =
        build_replacement(full_view(s.base), s.base_lm, full_view(tiny.donor), tiny.donor_lm, {}, &rep);
    CHECK(!c.feasible);
    CHECK(c.rejection == "too small in the source photo");
    // Head turned: rigid support no longer fits a similarity.
    FaceLandmarks turned = s.donor_lm;
    Vec2 mid = s.donor_pose.image_from_face.apply({0, 0});
    for (int i : {33, 133, 6, 168, 197, 195, 5, 4, 127, 162, 21, 234})
        turned.points[i].x = mid.x + 0.45 * (turned.points[i].x - mid.x) - 14;
    c = build_replacement(full_view(s.base), s.base_lm, full_view(s.donor), turned, {}, &rep);
    CHECK(!c.feasible);
    CHECK(c.rejection == "turned too far");
}

TEST(identical_donor_reproduces_the_base) {
    Scene s = make_scene();
    Replacement rep;
    CHECK(build_replacement(full_view(s.base), s.base_lm, full_view(s.base), s.base_lm, {}, &rep).feasible);
    PatchResult p = render_replacement(rep, full_view(s.base), full_view(s.base));
    Image out = s.base;
    paste(out, p.pixels, p.rect.x0, p.rect.y0);
    double max_err = 0;
    for (size_t i = 0; i < out.data.size(); ++i) max_err = std::max(max_err, double(std::abs(out.data[i] - s.base.data[i])));
    CHECK(max_err < 1e-4);
}
