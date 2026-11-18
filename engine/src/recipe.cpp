#include "bt/recipe.h"

#include <stdexcept>

#include "bt/json.h"

namespace bt {

const char* const kEngineAlgorithm = "bt-deterministic-1";

static json::Value numbers(const std::vector<double>& v) {
    json::Value a = json::Value::make_array();
    for (double d : v) a.array.emplace_back(d);
    return a;
}

static std::vector<double> read_numbers(const json::Value& v) {
    std::vector<double> out;
    for (const json::Value& e : v.as_array()) out.push_back(e.as_number());
    return out;
}

std::string recipe_to_json(const Recipe& r) {
    using json::Value;
    Value root = Value::make_object();
    root.set("format_version", r.format_version);
    root.set("algorithm", r.algorithm);
    root.set("base_id", r.base_id);
    root.set("base_hash", r.base_hash);
    Value hashes = Value::make_object();
    for (const auto& kv : r.donor_hashes) hashes.set(kv.first, kv.second);
    root.set("donor_hashes", hashes);
    Value reps = Value::make_array();
    for (const Replacement& p : r.replacements) {
        Value o = Value::make_object();
        o.set("person_id", p.person_id);
        o.set("donor_id", p.donor_id);
        o.set("warp", numbers({p.base_from_donor.a, p.base_from_donor.b, p.base_from_donor.tx, p.base_from_donor.ty}));
        o.set("center", numbers({p.regions.center.x, p.regions.center.y}));
        o.set("core", numbers(p.regions.core));
        o.set("outer", numbers(p.regions.outer));
        o.set("seam", numbers(p.seam));
        o.set("feather", p.feather);
        std::vector<double> c;
        for (int i = 0; i < 3; ++i)
            for (int j = 0; j < 3; ++j) c.push_back(p.color.coef[i][j]);
        o.set("color", numbers(c));
        o.set("color_unit", p.color.unit);
        o.set("blend", p.blend == BlendMode::Multiband ? "multiband" : "screened_gradient");
        reps.array.push_back(o);
    }
    root.set("replacements", reps);
    return json::dump(root);
}

Recipe recipe_from_json(const std::string& text) {
    json::Value root = json::parse(text);
    Recipe r;
    r.format_version = int(root.at("format_version").as_number());
    if (r.format_version != kRecipeFormatVersion)
        throw std::invalid_argument("recipe: unsupported format version " + std::to_string(r.format_version));
    r.algorithm = root.at("algorithm").as_string();
    r.base_id = root.at("base_id").as_string();
    r.base_hash = root.at("base_hash").as_string();
    for (const auto& kv : root.at("donor_hashes").object) r.donor_hashes[kv.first] = kv.second.as_string();
    for (const json::Value& o : root.at("replacements").as_array()) {
        Replacement p;
        p.person_id = int(o.at("person_id").as_number());
        p.donor_id = o.at("donor_id").as_string();
        std::vector<double> w = read_numbers(o.at("warp")), c = read_numbers(o.at("center")),
                            col = read_numbers(o.at("color"));
        if (w.size() != 4 || c.size() != 2 || col.size() != 9) throw std::invalid_argument("recipe: malformed replacement");
        p.base_from_donor = Similarity{w[0], w[1], w[2], w[3]};
        p.regions.center = {c[0], c[1]};
        p.regions.core = read_numbers(o.at("core"));
        p.regions.outer = read_numbers(o.at("outer"));
        p.seam = read_numbers(o.at("seam"));
        if (p.regions.core.size() != p.regions.outer.size() || p.seam.size() != p.regions.core.size() || p.seam.size() < 3)
            throw std::invalid_argument("recipe: region profiles have inconsistent sizes");
        p.feather = o.at("feather").as_number();
        for (int i = 0; i < 3; ++i)
            for (int j = 0; j < 3; ++j) p.color.coef[i][j] = col[i * 3 + j];
        p.color.unit = o.at("color_unit").as_number();
        const std::string& b = o.at("blend").as_string();
        if (b == "multiband")
            p.blend = BlendMode::Multiband;
        else if (b == "screened_gradient")
            p.blend = BlendMode::ScreenedGradient;
        else
            throw std::invalid_argument("recipe: unknown blend mode '" + b + "'");
        r.replacements.push_back(p);
    }
    return r;
}

std::vector<QualityReport> render_recipe(const Recipe& recipe, Image& canvas, const ImageView& base,
                                         const std::map<std::string, ImageView>& donors) {
    if (base.image != &canvas) throw std::invalid_argument("render_recipe: base view must refer to the canvas");
    if (recipe.algorithm != kEngineAlgorithm)
        throw std::invalid_argument("render_recipe: recipe was built by algorithm '" + recipe.algorithm +
                                    "'; migrate it explicitly before rendering");
    std::vector<QualityReport> reports;
    for (const Replacement& rep : recipe.replacements) {
        auto it = donors.find(rep.donor_id);
        if (it == donors.end()) throw std::invalid_argument("render_recipe: missing donor '" + rep.donor_id + "'");
        PatchResult patch = render_replacement(rep, base, it->second);
        reports.push_back(validate_patch(rep, base, patch));
        paste(canvas, patch.pixels, patch.rect.x0 - base.rect.x0, patch.rect.y0 - base.rect.y0);
    }
    return reports;
}

}  // namespace bt
