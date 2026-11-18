package io.github.alecjacobson.besttake

import io.github.alecjacobson.besttake.analysis.FaceBox
import io.github.alecjacobson.besttake.analysis.FaceTiling
import io.github.alecjacobson.besttake.imaging.PixelRect
import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
import org.junit.Assert.assertTrue
import org.junit.Test

class FaceTilingTest {
    @Test
    fun tilesStartWithTheWholeImageAndStayInside() {
        val tiles = FaceTiling.tiles(2040, 1530)
        assertEquals(PixelRect(0, 0, 2040, 1530), tiles.first())
        assertEquals(1 + 9 + 25 + 49, tiles.size)
        for (t in tiles) assertTrue(t.x0 >= 0 && t.y0 >= 0 && t.x1 <= 2040 && t.y1 <= 1530 && !t.isEmpty)
    }

    @Test
    fun smallImagesAreNotTiledBelowTheDetectorInput() {
        assertEquals(1, FaceTiling.tiles(300, 300).size)
        assertEquals(1 + 9, FaceTiling.tiles(600, 400).size)
    }

    @Test
    fun everyPointIsWellInsideSomeFineTile() {
        // A face anywhere must appear whole in at least one tile of the finest grid.
        val tiles = FaceTiling.tiles(2040, 1530).drop(1 + 9 + 25)
        val face = 100
        for (x in 0..(2040 - face) step 37) for (y in 0..(1530 - face) step 41) {
            val box = FaceBox(x.toFloat(), y.toFloat(), face.toFloat(), face.toFloat(), 1f)
            assertTrue("face at $x,$y", tiles.any { t ->
                box.x >= t.x0 && box.y >= t.y0 && box.x + face <= t.x1 && box.y + face <= t.y1 &&
                    !FaceTiling.isCutByTile(box, t, 2040, 1530)
            })
        }
    }

    @Test
    fun boxesOnInteriorTileEdgesAreCut() {
        val tile = PixelRect(100, 100, 300, 300)
        assertTrue(FaceTiling.isCutByTile(FaceBox(100f, 150f, 50f, 50f, 1f), tile, 1000, 1000))
        assertTrue(FaceTiling.isCutByTile(FaceBox(250f, 150f, 50f, 50f, 1f), tile, 1000, 1000))
        assertFalse(FaceTiling.isCutByTile(FaceBox(150f, 150f, 50f, 50f, 1f), tile, 1000, 1000))
        // Touching a real image edge is not a cut.
        assertFalse(FaceTiling.isCutByTile(FaceBox(0f, 20f, 50f, 50f, 1f), PixelRect(0, 0, 200, 200), 1000, 1000))
    }

    @Test
    fun duplicatesKeepTheHighestScore() {
        val kept = FaceTiling.suppressDuplicates(
            listOf(
                FaceBox(10f, 10f, 100f, 100f, 0.6f),
                FaceBox(14f, 12f, 96f, 104f, 0.9f),
                FaceBox(300f, 10f, 80f, 80f, 0.7f),
                FaceBox(30f, 30f, 20f, 20f, 0.5f),  // small box inside a larger face
            ),
        )
        assertEquals(listOf(0.9f, 0.7f), kept.map { it.score })
    }
}
