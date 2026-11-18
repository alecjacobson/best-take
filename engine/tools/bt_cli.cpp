// Host command-line driver for the engine. Reads a job description (JSON)
// with per-frame PPM images and analyzer output, proposes or loads a recipe,
// renders it at full resolution, and writes the result, recipe, and report.
//
// Usage: bt_cli job.json
#include <cmath>
#include <cstdio>
#include <fstream>
#include <iostream>
#include <sstream>
#include <stdexcept>

#include "bt/json.h"
#include "bt/session_io.h"

using namespace bt;

namespace {

struct Ppm {
    int width = 0, height = 0;
    std::vector<uint8_t> rgb;
};

std::string read_text(const std::string& path) {
    std::ifstream in(path, std::ios::binary);
    if (!in) throw std::runtime_error("cannot read " + path);
    std::ostringstream ss;
    ss << in.rdbuf();
    return ss.str();
}

void write_text(const std::string& path, const std::string& text) {
    std::ofstream out(path, std::ios::binary);
    if (!out) throw std::runtime_error("cannot write " + path);
    out << text;
}

Ppm read_ppm(const std::string& path) {
    std::ifstream in(path, std::ios::binary);
    if (!in) throw std::runtime_error("cannot read " + path);
    std::string magic;
    int maxval = 0;
    Ppm p;
    in >> magic >> p.width >> p.height >> maxval;
    if (magic != "P6" || maxval != 255 || p.width <= 0 || p.height <= 0)
        throw std::runtime_error(path + ": expected a binary 8-bit PPM");
    in.get();
    p.rgb.resize(size_t(p.width) * p.height * 3);
    in.read(reinterpret_cast<char*>(p.rgb.data()), std::streamsize(p.rgb.size()));
    if (!in) throw std::runtime_error(path + ": truncated pixel data");
    return p;
}

void write_ppm(const std::string& path, const Image& img) {
    std::vector<uint8_t> buf(size_t(img.width) * img.height * 3);
    to_srgb8(img, buf.data(), img.width * 3, 3);
    std::ofstream out(path, std::ios::binary);
    if (!out) throw std::runtime_error("cannot write " + path);
    out << "P6\n" << img.width << " " << img.height << "\n255\n";
    out.write(reinterpret_cast<const char*>(buf.data()), std::streamsize(buf.size()));
}

double number_or(const json::Value& o, const char* key, double fallback) {
    return o.has(key) ? o.at(key).as_number() : fallback;
}

}  // namespace

int main(int argc, char** argv) {
    if (argc != 2) {
        std::cerr << "usage: bt_cli job.json\n";
        return 2;
    }
    try {
        json::Value job = json::parse(read_text(argv[1]));
        std::vector<Ppm> images;
        json::Value frame_list = job.at("frames");
        for (json::Value& jf : frame_list.array) {
            images.push_back(read_ppm(jf.at("ppm").as_string()));
            jf.set("width", images.back().width);
            jf.set("height", images.back().height);
        }
        std::vector<FrameSource> frames = frames_from_json(frame_list);

        RegionLoader loader = [&](int frame, const Rect& rect, double scale) {
            const Ppm& p = images[frame];
            Rect src{int(std::floor(rect.x0 / scale)), int(std::floor(rect.y0 / scale)),
                     std::min(p.width, int(std::ceil(rect.x1 / scale))), std::min(p.height, int(std::ceil(rect.y1 / scale)))};
            Image part = from_srgb8(p.rgb.data() + (size_t(src.y0) * p.width + src.x0) * 3, src.width(), src.height(),
                                    p.width * 3, 3);
            return scale == 1 ? part : resize_area(part, rect.width(), rect.height());
        };

        SessionOptions options;
        if (job.has("preference")) {
            const json::Value& jp = job.at("preference");
            options.preference.eyes_open = number_or(jp, "eyes_open", options.preference.eyes_open);
            options.preference.smile = number_or(jp, "smile", options.preference.smile);
            options.preference.gaze = number_or(jp, "gaze", options.preference.gaze);
        }
        if (job.has("blend") && job.at("blend").as_string() == "screened_gradient")
            options.build.blend = BlendMode::ScreenedGradient;
        Session session(frames, loader, options);

        json::Value report = json::Value::make_object();
        Recipe recipe;
        if (job.has("recipe_in")) {
            recipe = recipe_from_json(read_text(job.at("recipe_in").as_string()));
        } else {
            std::map<int, int> locks;
            if (job.has("locks"))
                for (const auto& kv : job.at("locks").object) locks[std::stoi(kv.first)] = int(kv.second.as_number());
            SelectionResult sel = session.propose(locks, int(number_or(job, "base", -1)));
            if (!sel.valid) throw std::runtime_error("no frame can serve as the base photo");
            if (!sel.conflicts.empty()) {
                std::string msg = "unresolved conflicts:";
                for (const Conflict& c : sel.conflicts) msg += " person " + std::to_string(c.person) + " (" + c.reason + ")";
                throw std::runtime_error(msg);
            }
            recipe = session.build_recipe(sel.base, sel.source);
            report.set("base", sel.base);
            report.set("cost", sel.cost);
            json::Value persons = json::Value::make_array();
            for (int q = 0; q < session.person_count(); ++q) {
                json::Value jp = json::Value::make_object();
                jp.set("person", q);
                jp.set("source", sel.source[q]);
                jp.set("candidates", candidates_to_json(session, q, session.candidates(q, sel.base)));
                persons.array.push_back(jp);
            }
            report.set("persons", persons);
        }

        std::vector<QualityReport> quality;
        Image out = session.render(recipe, 1.0, &quality);
        bool all_ok = true;
        for (const QualityReport& q : quality) all_ok = all_ok && q.ok;
        json::Value jq = quality_to_json(recipe, quality);
        report.set("replacements", jq);
        report.set("base_id", recipe.base_id);
        write_ppm(job.at("output_ppm").as_string(), out);
        if (job.has("recipe_out")) write_text(job.at("recipe_out").as_string(), recipe_to_json(recipe));
        if (job.has("report_out")) write_text(job.at("report_out").as_string(), json::dump(report));
        std::cout << "base " << recipe.base_id << ", " << recipe.replacements.size() << " replacement(s)"
                  << (all_ok ? "" : ", QUALITY CHECK FAILED") << "\n";
        return all_ok ? 0 : 3;
    } catch (const std::exception& e) {
        std::cerr << "bt_cli: " << e.what() << "\n";
        return 1;
    }
}
