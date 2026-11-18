#include "bt/image.h"

#include <algorithm>
#include <cmath>
#include <stdexcept>

namespace bt {

Rect intersect(const Rect& a, const Rect& b) {
    Rect r{std::max(a.x0, b.x0), std::max(a.y0, b.y0), std::min(a.x1, b.x1), std::min(a.y1, b.y1)};
    if (r.empty()) return Rect{};
    return r;
}

Image::Image(int w, int h, int c, float fill) : width(w), height(h), channels(c) {
    if (w < 0 || h < 0 || c < 1) throw std::invalid_argument("Image: invalid dimensions");
    data.assign(size_t(w) * h * c, fill);
}

float Image::clamped(int x, int y, int c) const {
    x = std::clamp(x, 0, width - 1);
    y = std::clamp(y, 0, height - 1);
    return at(x, y, c);
}

float srgb_to_linear(float v) {
    return v <= 0.04045f ? v / 12.92f : std::pow((v + 0.055f) / 1.055f, 2.4f);
}

float linear_to_srgb(float v) {
    v = std::clamp(v, 0.f, 1.f);
    return v <= 0.0031308f ? v * 12.92f : 1.055f * std::pow(v, 1.f / 2.4f) - 0.055f;
}

Image from_srgb8(const uint8_t* pixels, int width, int height, int stride_bytes, int src_channels) {
    if (!pixels || (src_channels != 3 && src_channels != 4))
        throw std::invalid_argument("from_srgb8: expected 3 or 4 source channels");
    static const std::vector<float> lut = [] {
        std::vector<float> t(256);
        for (int i = 0; i < 256; ++i) t[i] = srgb_to_linear(i / 255.f);
        return t;
    }();
    Image img(width, height, 3);
    for (int y = 0; y < height; ++y) {
        const uint8_t* row = pixels + size_t(y) * stride_bytes;
        float* out = &img.data[size_t(y) * width * 3];
        for (int x = 0; x < width; ++x)
            for (int c = 0; c < 3; ++c) out[x * 3 + c] = lut[row[x * src_channels + c]];
    }
    return img;
}

void to_srgb8(const Image& img, uint8_t* out, int stride_bytes, int dst_channels) {
    if (img.channels != 3 || (dst_channels != 3 && dst_channels != 4))
        throw std::invalid_argument("to_srgb8: expected a 3-channel image and 3 or 4 output channels");
    for (int y = 0; y < img.height; ++y) {
        uint8_t* row = out + size_t(y) * stride_bytes;
        for (int x = 0; x < img.width; ++x) {
            for (int c = 0; c < 3; ++c)
                row[x * dst_channels + c] = uint8_t(std::lround(linear_to_srgb(img.at(x, y, c)) * 255.f));
            if (dst_channels == 4) row[x * 4 + 3] = 255;
        }
    }
}

Image crop(const Image& img, const Rect& r) {
    if (r.empty()) throw std::invalid_argument("crop: empty rectangle");
    Image out(r.width(), r.height(), img.channels);
    for (int y = 0; y < out.height; ++y)
        for (int x = 0; x < out.width; ++x)
            for (int c = 0; c < img.channels; ++c) out.at(x, y, c) = img.clamped(x + r.x0, y + r.y0, c);
    return out;
}

void paste(Image& dst, const Image& patch, int x0, int y0) {
    if (patch.channels != dst.channels || x0 < 0 || y0 < 0 || x0 + patch.width > dst.width ||
        y0 + patch.height > dst.height)
        throw std::invalid_argument("paste: patch does not fit in destination");
    for (int y = 0; y < patch.height; ++y)
        std::copy_n(&patch.data[size_t(y) * patch.width * patch.channels], size_t(patch.width) * patch.channels,
                    &dst.data[(size_t(y + y0) * dst.width + x0) * dst.channels]);
}

float sample_bilinear(const Image& img, double x, double y, int c) {
    x -= 0.5;
    y -= 0.5;
    int ix = int(std::floor(x)), iy = int(std::floor(y));
    float fx = float(x - ix), fy = float(y - iy);
    float a = img.clamped(ix, iy, c), b = img.clamped(ix + 1, iy, c);
    float d = img.clamped(ix, iy + 1, c), e = img.clamped(ix + 1, iy + 1, c);
    return (a * (1 - fx) + b * fx) * (1 - fy) + (d * (1 - fx) + e * fx) * fy;
}

static inline void catmull_rom(float t, float w[4]) {
    float t2 = t * t, t3 = t2 * t;
    w[0] = 0.5f * (-t3 + 2 * t2 - t);
    w[1] = 0.5f * (3 * t3 - 5 * t2 + 2);
    w[2] = 0.5f * (-3 * t3 + 4 * t2 + t);
    w[3] = 0.5f * (t3 - t2);
}

float sample_bicubic(const Image& img, double x, double y, int c) {
    x -= 0.5;
    y -= 0.5;
    int ix = int(std::floor(x)), iy = int(std::floor(y));
    float wx[4], wy[4];
    catmull_rom(float(x - ix), wx);
    catmull_rom(float(y - iy), wy);
    float sum = 0;
    for (int j = 0; j < 4; ++j) {
        float row = 0;
        for (int i = 0; i < 4; ++i) row += wx[i] * img.clamped(ix - 1 + i, iy - 1 + j, c);
        sum += wy[j] * row;
    }
    return sum;
}

Image gaussian_blur(const Image& img, double sigma) {
    if (sigma <= 0) return img;
    int radius = std::max(1, int(std::ceil(3 * sigma)));
    std::vector<float> k(2 * radius + 1);
    float norm = 0;
    for (int i = -radius; i <= radius; ++i) norm += k[i + radius] = float(std::exp(-0.5 * i * i / (sigma * sigma)));
    for (float& v : k) v /= norm;
    Image tmp(img.width, img.height, img.channels), out(img.width, img.height, img.channels);
    for (int y = 0; y < img.height; ++y)
        for (int x = 0; x < img.width; ++x)
            for (int c = 0; c < img.channels; ++c) {
                float s = 0;
                for (int i = -radius; i <= radius; ++i) s += k[i + radius] * img.clamped(x + i, y, c);
                tmp.at(x, y, c) = s;
            }
    for (int y = 0; y < img.height; ++y)
        for (int x = 0; x < img.width; ++x)
            for (int c = 0; c < img.channels; ++c) {
                float s = 0;
                for (int i = -radius; i <= radius; ++i) s += k[i + radius] * tmp.clamped(x, y + i, c);
                out.at(x, y, c) = s;
            }
    return out;
}

Image resize_area(const Image& img, int nw, int nh) {
    if (nw < 1 || nh < 1) throw std::invalid_argument("resize_area: invalid target size");
    Image out(nw, nh, img.channels);
    double sx = double(img.width) / nw, sy = double(img.height) / nh;
    for (int y = 0; y < nh; ++y) {
        double y0 = y * sy, y1 = (y + 1) * sy;
        for (int x = 0; x < nw; ++x) {
            double x0 = x * sx, x1 = (x + 1) * sx;
            for (int c = 0; c < img.channels; ++c) {
                double sum = 0, wsum = 0;
                for (int j = int(std::floor(y0)); j < int(std::ceil(y1)); ++j) {
                    double wy = std::min(y1, double(j + 1)) - std::max(y0, double(j));
                    for (int i = int(std::floor(x0)); i < int(std::ceil(x1)); ++i) {
                        double wx = std::min(x1, double(i + 1)) - std::max(x0, double(i));
                        sum += wx * wy * img.clamped(i, j, c);
                        wsum += wx * wy;
                    }
                }
                out.at(x, y, c) = float(sum / wsum);
            }
        }
    }
    return out;
}

Image luminance(const Image& img) {
    if (img.channels != 3) throw std::invalid_argument("luminance: expected 3 channels");
    Image out(img.width, img.height, 1);
    for (size_t i = 0, n = size_t(img.width) * img.height; i < n; ++i)
        out.data[i] = 0.2126f * img.data[i * 3] + 0.7152f * img.data[i * 3 + 1] + 0.0722f * img.data[i * 3 + 2];
    return out;
}

}  // namespace bt
