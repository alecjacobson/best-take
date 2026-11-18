package io.github.alecjacobson.besttake.analysis

import io.github.alecjacobson.besttake.imaging.PixelRect

/** A detected face box with its detector score. */
data class FaceBox(val x: Float, val y: Float, val width: Float, val height: Float, val score: Float)

/**
 * Tiling used to find small faces with a short-range detector: the whole
 * image plus overlapping tiles on which the faces appear larger.
 */
object FaceTiling {
    private const val MIN_TILE = 192

    /** Tiles covering a [width] x [height] image, coarsest first. */
    fun tiles(width: Int, height: Int): List<PixelRect> {
        require(width > 0 && height > 0) { "Image size must be positive" }
        val out = mutableListOf(PixelRect(0, 0, width, height))
        for (grid in 2..4) {
            val tileW = width.toFloat() / grid
            val tileH = height.toFloat() / grid
            if (minOf(tileW, tileH) < MIN_TILE) break
            val steps = 2 * grid - 1  // 50% overlap
            for (iy in 0 until steps) for (ix in 0 until steps) {
                val x0 = (ix * tileW / 2).toInt()
                val y0 = (iy * tileH / 2).toInt()
                out += PixelRect(x0, y0, minOf(width, (x0 + tileW).toInt()), minOf(height, (y0 + tileH).toInt()))
            }
        }
        return out
    }

    /**
     * True when a box found in [tile] touches a tile edge that is not an image
     * edge; such a face is seen whole in a neighboring tile.
     */
    fun isCutByTile(box: FaceBox, tile: PixelRect, imageWidth: Int, imageHeight: Int): Boolean {
        val margin = 2f
        return (box.x <= tile.x0 + margin && tile.x0 > 0) ||
            (box.y <= tile.y0 + margin && tile.y0 > 0) ||
            (box.x + box.width >= tile.x1 - margin && tile.x1 < imageWidth) ||
            (box.y + box.height >= tile.y1 - margin && tile.y1 < imageHeight)
    }

    /** Keep the highest-scoring box of every group that overlaps substantially. */
    fun suppressDuplicates(boxes: List<FaceBox>): List<FaceBox> {
        val kept = mutableListOf<FaceBox>()
        for (b in boxes.sortedByDescending { it.score }) {
            val duplicate = kept.any { k ->
                val ix = maxOf(0f, minOf(b.x + b.width, k.x + k.width) - maxOf(b.x, k.x))
                val iy = maxOf(0f, minOf(b.y + b.height, k.y + k.height) - maxOf(b.y, k.y))
                ix * iy > 0.3f * minOf(b.width * b.height, k.width * k.height)
            }
            if (!duplicate) kept += b
        }
        return kept
    }
}
