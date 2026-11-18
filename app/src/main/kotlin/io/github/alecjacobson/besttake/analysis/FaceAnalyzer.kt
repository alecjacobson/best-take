package io.github.alecjacobson.besttake.analysis

import android.content.Context
import android.graphics.Bitmap
import android.graphics.Canvas
import com.google.mediapipe.framework.image.BitmapImageBuilder
import com.google.mediapipe.tasks.core.BaseOptions
import com.google.mediapipe.tasks.vision.core.RunningMode
import com.google.mediapipe.tasks.vision.facedetector.FaceDetector
import com.google.mediapipe.tasks.vision.facelandmarker.FaceLandmarker
import io.github.alecjacobson.besttake.engine.FaceRecord
import io.github.alecjacobson.besttake.imaging.PixelRect
import io.github.alecjacobson.besttake.imaging.SourceImage
import kotlin.math.roundToInt

/**
 * Finds faces in a source photo and measures landmarks and facial state.
 *
 * Detection runs on a reduced scene image and on overlapping tiles, because
 * the packaged detector is a short-range model. Landmarks are then estimated
 * on an individual crop of each face at the detail the source provides.
 * Models are loaded from packaged assets, so analysis works offline.
 */
class FaceAnalyzer(context: Context) : AutoCloseable {
    private val detector: FaceDetector = FaceDetector.createFromOptions(
        context,
        FaceDetector.FaceDetectorOptions.builder()
            .setBaseOptions(BaseOptions.builder().setModelAssetPath(DETECTOR_ASSET).build())
            .setRunningMode(RunningMode.IMAGE)
            .setMinDetectionConfidence(0.5f)
            .build(),
    )
    private val landmarker: FaceLandmarker = FaceLandmarker.createFromOptions(
        context,
        FaceLandmarker.FaceLandmarkerOptions.builder()
            .setBaseOptions(BaseOptions.builder().setModelAssetPath(LANDMARKER_ASSET).build())
            .setRunningMode(RunningMode.IMAGE)
            .setNumFaces(1)
            .setOutputFaceBlendshapes(true)
            .build(),
    )

    /** Faces of [source] with landmarks in upright original pixels. */
    fun analyze(source: SourceImage): List<FaceRecord> {
        val sample = SourceImage.sampleSizeFor(source.width, source.height, SCENE_LONG_SIDE)
        val scene = source.decodeFull(sample)
        val boxes = try {
            detect(scene).map {
                FaceBox(it.x * sample, it.y * sample, it.width * sample, it.height * sample, it.score)
            }
        } finally {
            scene.recycle()
        }
        return boxes.mapNotNull { landmarks(source, it) }
    }

    private fun detect(scene: Bitmap): List<FaceBox> {
        val found = mutableListOf<FaceBox>()
        val tiles = FaceTiling.tiles(scene.width, scene.height)
        for ((index, tile) in tiles.withIndex()) {
            val whole = index == 0
            val bitmap = if (whole) scene else Bitmap.createBitmap(scene, tile.x0, tile.y0, tile.width, tile.height)
            try {
                val result = detector.detect(BitmapImageBuilder(bitmap).build())
                for (detection in result.detections()) {
                    val b = detection.boundingBox()
                    val score = detection.categories().firstOrNull()?.score() ?: 0f
                    val box = FaceBox(b.left + tile.x0, b.top + tile.y0, b.width(), b.height(), score)
                    if (whole || !FaceTiling.isCutByTile(box, tile, scene.width, scene.height)) found += box
                }
            } finally {
                if (!whole) bitmap.recycle()
            }
        }
        return FaceTiling.suppressDuplicates(found)
    }

    private fun landmarks(source: SourceImage, box: FaceBox): FaceRecord? {
        // Square crop around the detection, padded where it leaves the image.
        val side = (CROP_SCALE * maxOf(box.width, box.height)).roundToInt().coerceAtLeast(16)
        val x0 = (box.x + box.width / 2 - side / 2f).roundToInt()
        val y0 = (box.y + box.height / 2 - side / 2f).roundToInt()
        var sample = 1
        while (sample < 8 && side / (sample * 2) >= MODEL_SIDE) sample *= 2
        val inside = PixelRect(
            maxOf(0, x0) / sample, maxOf(0, y0) / sample,
            minOf(source.scaledWidth(sample), (x0 + side + sample - 1) / sample),
            minOf(source.scaledHeight(sample), (y0 + side + sample - 1) / sample),
        )
        if (inside.isEmpty) return null
        val scaledSide = (side + sample - 1) / sample
        val crop = Bitmap.createBitmap(scaledSide, scaledSide, Bitmap.Config.ARGB_8888)
        val part = source.decodeRegion(inside, sample)
        Canvas(crop).drawBitmap(part, inside.x0 - x0.toFloat() / sample, inside.y0 - y0.toFloat() / sample, null)
        part.recycle()
        val result = try {
            landmarker.detect(BitmapImageBuilder(crop).build())
        } finally {
            crop.recycle()
        }
        val mesh = result.faceLandmarks().firstOrNull() ?: return null
        val cropSize = scaledSide.toFloat() * sample
        val flat = ArrayList<Float>(mesh.size * 2)
        for (point in mesh) {
            flat += x0 + point.x() * cropSize
            flat += y0 + point.y() * cropSize
        }
        val shapes = result.faceBlendshapes().orElse(emptyList()).firstOrNull()
            ?.associate { it.categoryName() to it.score() }
            ?: emptyMap()
        fun shape(name: String) = shapes[name] ?: 0f
        val look = LOOK_SHAPES.maxOf { shape(it) }
        return FaceRecord(
            landmarks = flat,
            eyesOpen = 1f - maxOf(shape("eyeBlinkLeft"), shape("eyeBlinkRight")),
            smile = 0.5f * (shape("mouthSmileLeft") + shape("mouthSmileRight")),
            gazeForward = 1f - look,
            // Without blendshapes the facial state is unknown and must not drive preference.
            confidence = if (shapes.isEmpty()) 0f else 1f,
        )
    }

    override fun close() {
        detector.close()
        landmarker.close()
    }

    companion object {
        const val DETECTOR_ASSET = "models/blaze_face_short_range.tflite"
        const val LANDMARKER_ASSET = "models/face_landmarker.task"
        private const val SCENE_LONG_SIDE = 2048
        private const val MODEL_SIDE = 384
        private const val CROP_SCALE = 1.8f
        private val LOOK_SHAPES = listOf("In", "Out", "Up", "Down").flatMap { direction ->
            listOf("Left", "Right").map { side -> "eyeLook$direction$side" }
        }

        /** Throws if a packaged model is missing, so offline readiness is verified up front. */
        fun verifyAssets(context: Context) {
            for (asset in listOf(DETECTOR_ASSET, LANDMARKER_ASSET)) {
                context.assets.open(asset).use { check(it.read() >= 0) { "Model asset $asset is empty" } }
            }
        }
    }
}
