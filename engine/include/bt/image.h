// Image buffers in the linear working representation.
#pragma once
#include <cstdint>
#include <vector>

namespace bt {

/// Half-open integer pixel rectangle [x0, x1) x [y0, y1).
struct Rect {
    int x0 = 0, y0 = 0, x1 = 0, y1 = 0;
    int width() const { return x1 - x0; }
    int height() const { return y1 - y0; }
    bool empty() const { return x1 <= x0 || y1 <= y0; }
    bool contains(const Rect& o) const { return o.x0 >= x0 && o.y0 >= y0 && o.x1 <= x1 && o.y1 <= y1; }
    bool operator==(const Rect& o) const { return x0 == o.x0 && y0 == o.y0 && x1 == o.x1 && y1 == o.y1; }
};
Rect intersect(const Rect& a, const Rect& b);

/// Interleaved float image. Color images hold linear-light values with the
/// primaries of their source; pixel (i, j) has its center at (i + 0.5, j + 0.5).
struct Image {
    int width = 0, height = 0, channels = 0;
    std::vector<float> data;

    Image() = default;
    Image(int w, int h, int c, float fill = 0.f);
    bool empty() const { return data.empty(); }
    float& at(int x, int y, int c = 0) { return data[(size_t(y) * width + x) * channels + c]; }
    float at(int x, int y, int c = 0) const { return data[(size_t(y) * width + x) * channels + c]; }
    /// Value with coordinates clamped to the image border.
    float clamped(int x, int y, int c = 0) const;
};

float srgb_to_linear(float v);
float linear_to_srgb(float v);

/// Decode 8-bit sRGB-encoded pixels (3 or 4 channels, alpha ignored) to linear RGB.
Image from_srgb8(const uint8_t* pixels, int width, int height, int stride_bytes, int src_channels);
/// Encode linear RGB to 8-bit sRGB. With dst_channels == 4, alpha is written as 255.
void to_srgb8(const Image& img, uint8_t* out, int stride_bytes, int dst_channels);

/// Copy a rectangle; coordinates outside the image replicate the border.
Image crop(const Image& img, const Rect& r);
/// Write patch into dst with its top-left pixel at (x0, y0); the patch must fit.
void paste(Image& dst, const Image& patch, int x0, int y0);

/// Catmull-Rom sample at continuous coordinates (pixel centers at +0.5), border clamped.
float sample_bicubic(const Image& img, double x, double y, int c);
float sample_bilinear(const Image& img, double x, double y, int c);

Image gaussian_blur(const Image& img, double sigma);
/// Area-averaging resize; suitable for reductions.
Image resize_area(const Image& img, int new_width, int new_height);
/// Rec. 709 luminance of a 3-channel image (returns 1 channel).
Image luminance(const Image& img);

}  // namespace bt
