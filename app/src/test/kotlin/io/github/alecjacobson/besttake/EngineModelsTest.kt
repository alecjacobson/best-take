package io.github.alecjacobson.besttake

import io.github.alecjacobson.besttake.engine.CandidateInfo
import io.github.alecjacobson.besttake.engine.EngineJson
import io.github.alecjacobson.besttake.engine.FaceRecord
import io.github.alecjacobson.besttake.engine.FrameRecord
import io.github.alecjacobson.besttake.engine.Selection
import kotlinx.serialization.builtins.ListSerializer
import org.junit.Assert.assertEquals
import org.junit.Assert.assertTrue
import org.junit.Test

/** The JSON field names are the contract with the native engine. */
class EngineModelsTest {
    @Test
    fun framesUseEngineFieldNames() {
        val json = EngineJson.encodeToString(
            ListSerializer(FrameRecord.serializer()),
            listOf(FrameRecord("source_0", "abc", 4000, 3000, listOf(FaceRecord(listOf(1f, 2f), eyesOpen = 0.25f)))),
        )
        for (key in listOf("\"id\"", "\"hash\"", "\"width\"", "\"height\"", "\"faces\"", "\"landmarks\"",
            "\"eyes_open\":0.25", "\"smile\"", "\"gaze_forward\"", "\"confidence\"")) {
            assertTrue("$key in $json", json.contains(key))
        }
    }

    @Test
    fun engineResultsDecodeAndIgnoreExtraMeasurements() {
        val selection = EngineJson.decodeFromString(
            Selection.serializer(),
            """{"valid":true,"base":1,"cost":0.5,"source":[0,1,-1],"conflicts":[{"person":2,"reason":"turned too far"}]}""",
        )
        assertEquals(listOf(0, 1, -1), selection.source)
        assertEquals("turned too far", selection.conflicts.single().reason)
        val candidates = EngineJson.decodeFromString(
            ListSerializer(CandidateInfo.serializer()),
            """[{"frame":3,"face":0,"is_base":false,"feasible":false,"rejection":"too blurry","face_cost":0.4,
               "merge_cost":0,"support_residual":0.2,"seam_cost":0.001}]""",
        )
        assertEquals("too blurry", candidates.single().rejection)
    }
}
