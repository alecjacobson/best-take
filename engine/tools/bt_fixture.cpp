// Writes a procedural group-photo set: frames.json (analyzer output in the
// session input format) and frame<i>.rgb (8-bit sRGB, RGB interleaved).
//
// Usage: bt_fixture output_directory
#include <fstream>
#include <iostream>

#include "bt/json.h"
#include "fixture.h"

using namespace bt;

int main(int argc, char** argv) {
    if (argc != 2) {
        std::cerr << "usage: bt_fixture output_directory\n";
        return 2;
    }
    const std::string dir = argv[1];
    // Two people; blink[frame][person].
    const bool blink[3][2] = {{true, false}, {false, true}, {true, true}};
    json::Value frames = json::Value::make_array();
    for (int f = 0; f < 3; ++f) {
        Image img(800, 400, 3);
        json::Value frame = json::Value::make_object();
        frame.set("id", "source_" + std::to_string(f));
        frame.set("hash", "hash" + std::to_string(f));
        frame.set("width", 800);
        frame.set("height", 400);
        json::Value faces = json::Value::make_array();
        for (int p = 0; p < 2; ++p) {
            fixture::Pose pose{fixture::make_similarity(88 + 2.0 * f, 1.5 * f, 200 + 3.0 * f, 190 - 2.0 * f),
                               blink[f][p] ? 0.0 : 1.0, blink[f][p] ? 1.0 : 0.0, 1.0 + 0.05 * f};
            paste(img, fixture::render(400, 400, pose), 400 * p, 0);
            json::Value lm = json::Value::make_array();
            for (Vec2 q : fixture::landmarks(pose).points) {
                lm.array.emplace_back(q.x + 400 * p);
                lm.array.emplace_back(q.y);
            }
            json::Value face = json::Value::make_object();
            face.set("landmarks", lm);
            face.set("eyes_open", blink[f][p] ? 0.05 : 0.95);
            face.set("smile", blink[f][p] ? 0.0 : 1.0);
            faces.array.push_back(face);
        }
        frame.set("faces", faces);
        frames.array.push_back(frame);
        std::vector<uint8_t> rgb(size_t(img.width) * img.height * 3);
        to_srgb8(img, rgb.data(), img.width * 3, 3);
        std::ofstream out(dir + "/frame" + std::to_string(f) + ".rgb", std::ios::binary);
        out.write(reinterpret_cast<const char*>(rgb.data()), std::streamsize(rgb.size()));
        if (!out) {
            std::cerr << "bt_fixture: cannot write to " << dir << "\n";
            return 1;
        }
    }
    std::ofstream out(dir + "/frames.json", std::ios::binary);
    out << json::dump(frames);
    return out ? 0 : 1;
}
