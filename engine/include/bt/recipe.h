// The authoritative edit recipe: selected sources and processing decisions.
#pragma once
#include <map>
#include <string>
#include <vector>

#include "bt/compose.h"

namespace bt {

constexpr int kRecipeFormatVersion = 1;
/// Identifies the rendering algorithm; change when output for a stored recipe would differ.
extern const char* const kEngineAlgorithm;

struct Recipe {
    int format_version = kRecipeFormatVersion;
    std::string algorithm;  ///< Engine algorithm that built the recipe.
    std::string base_id;
    std::string base_hash;
    std::vector<Replacement> replacements;         ///< Applied in order (layer order).
    std::map<std::string, std::string> donor_hashes;  ///< donor_id -> content hash.
};

std::string recipe_to_json(const Recipe& recipe);
/// Throws std::invalid_argument for malformed input or an unsupported format
/// version; a recipe is never reinterpreted silently.
Recipe recipe_from_json(const std::string& text);

/// Render every replacement of a recipe onto `canvas`, which must hold the
/// base image region described by `base` (canvas is modified in place and
/// `base.image` must point at it). `donors` maps donor_id to a view.
/// Returns the per-replacement quality reports.
std::vector<QualityReport> render_recipe(const Recipe& recipe, Image& canvas, const ImageView& base,
                                         const std::map<std::string, ImageView>& donors);

}  // namespace bt
