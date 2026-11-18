package io.github.alecjacobson.besttake.engine

import kotlinx.serialization.SerialName
import kotlinx.serialization.Serializable
import kotlinx.serialization.json.Json

/** JSON configuration shared by everything that crosses the engine boundary. */
val EngineJson = Json {
    ignoreUnknownKeys = true
    encodeDefaults = true
}

/** One analyzed face: landmarks are a flat [x0, y0, ...] list in upright original pixels. */
@Serializable
data class FaceRecord(
    val landmarks: List<Float>,
    @SerialName("eyes_open") val eyesOpen: Float = 1f,
    val smile: Float = 0f,
    @SerialName("gaze_forward") val gazeForward: Float = 1f,
    val confidence: Float = 1f,
)

@Serializable
data class FrameRecord(
    val id: String,
    val hash: String,
    val width: Int,
    val height: Int,
    val faces: List<FaceRecord>,
)

@Serializable
data class Association(
    @SerialName("person_count") val personCount: Int,
    /** personOf[frame][face] is the project-local person id. */
    @SerialName("person_of") val personOf: List<List<Int>>,
)

@Serializable
data class Conflict(val person: Int, val reason: String)

@Serializable
data class Selection(
    val valid: Boolean,
    val base: Int,
    val cost: Double,
    /** Donor frame per person; the base (or -1 when absent) means unchanged. */
    val source: List<Int>,
    val conflicts: List<Conflict>,
)

@Serializable
data class CandidateInfo(
    val frame: Int,
    val face: Int,
    @SerialName("is_base") val isBase: Boolean,
    val feasible: Boolean,
    val rejection: String,
    @SerialName("face_cost") val faceCost: Double,
    @SerialName("merge_cost") val mergeCost: Double,
)

@Serializable
data class ReplacementQuality(
    val person: Int,
    val donor: String,
    val ok: Boolean,
    val failure: String,
    @SerialName("excess_gradient") val excessGradient: Double,
)
