package io.github.alecjacobson.besttake.ui

import androidx.activity.compose.BackHandler
import androidx.compose.foundation.Image
import androidx.compose.foundation.border
import androidx.compose.foundation.clickable
import androidx.compose.foundation.gestures.awaitEachGesture
import androidx.compose.foundation.gestures.awaitFirstDown
import androidx.compose.foundation.gestures.detectTransformGestures
import androidx.compose.foundation.gestures.waitForUpOrCancellation
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.BoxWithConstraints
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.heightIn
import androidx.compose.foundation.layout.offset
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.safeDrawingPadding
import androidx.compose.foundation.layout.size
import androidx.compose.foundation.layout.widthIn
import androidx.compose.foundation.lazy.LazyRow
import androidx.compose.foundation.lazy.items
import androidx.compose.foundation.shape.CircleShape
import androidx.compose.foundation.shape.RoundedCornerShape
import androidx.compose.material3.CircularProgressIndicator
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.OutlinedButton
import androidx.compose.material3.Text
import androidx.compose.material3.TextButton
import androidx.compose.runtime.Composable
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableFloatStateOf
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.setValue
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.draw.clip
import androidx.compose.ui.geometry.Offset
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.graphics.asImageBitmap
import androidx.compose.ui.graphics.graphicsLayer
import androidx.compose.ui.input.pointer.pointerInput
import androidx.compose.ui.layout.ContentScale
import androidx.compose.ui.platform.LocalDensity
import androidx.compose.ui.res.pluralStringResource
import androidx.compose.ui.res.stringResource
import androidx.compose.ui.semantics.Role
import androidx.compose.ui.semantics.contentDescription
import androidx.compose.ui.semantics.selected
import androidx.compose.ui.semantics.semantics
import androidx.compose.ui.unit.dp
import androidx.lifecycle.compose.collectAsStateWithLifecycle
import io.github.alecjacobson.besttake.R

@Composable
fun EditorScreen(model: EditorViewModel, onBack: () -> Unit) {
    val state by model.state.collectAsStateWithLifecycle()
    var comparing by remember { mutableStateOf(false) }
    BackHandler {
        if (state.selectedPerson != null) model.selectPerson(null) else onBack()
    }

    Column(modifier = Modifier.fillMaxSize().safeDrawingPadding()) {
        Row(
            modifier = Modifier.fillMaxWidth().padding(horizontal = 4.dp),
            verticalAlignment = Alignment.CenterVertically,
        ) {
            TextButton(onClick = onBack) { Text(stringResource(R.string.back)) }
            Box(modifier = Modifier.weight(1f))
            val idle = state.busy == null && state.fatalError == null
            TextButton(enabled = idle && state.canUndo, onClick = model::undo) { Text(stringResource(R.string.undo)) }
            TextButton(enabled = idle && state.canRedo, onClick = model::redo) { Text(stringResource(R.string.redo)) }
            TextButton(enabled = idle && state.preview != null, onClick = model::saveCopy) {
                Text(stringResource(R.string.save_copy))
            }
        }

        Box(modifier = Modifier.weight(1f).fillMaxWidth(), contentAlignment = Alignment.Center) {
            val shown = if (comparing) state.original else state.preview
            if (shown != null) {
                PhotoWithFaces(
                    state = state, bitmap = shown, showMarkers = !comparing,
                    onTapFace = { model.selectPerson(it) }, onTapBackground = { model.selectPerson(null) },
                )
            }
            state.fatalError?.let {
                Text(it, color = MaterialTheme.colorScheme.error, modifier = Modifier.padding(24.dp))
            }
            state.busy?.let { busy ->
                Column(horizontalAlignment = Alignment.CenterHorizontally) {
                    CircularProgressIndicator()
                    Text(
                        when (busy) {
                            is Busy.Analyzing -> stringResource(R.string.analyzing, busy.index, busy.count)
                            Busy.Choosing -> stringResource(R.string.choosing)
                            Busy.Rendering -> stringResource(R.string.rendering)
                            Busy.Saving -> stringResource(R.string.saving)
                        },
                        modifier = Modifier.padding(top = 8.dp),
                    )
                }
            }
        }

        // Bottom sheet: never covers the photograph.
        Column(modifier = Modifier.fillMaxWidth().padding(12.dp), verticalArrangement = Arrangement.spacedBy(8.dp)) {
            state.message?.let { message ->
                Text(
                    message, color = MaterialTheme.colorScheme.tertiary,
                    modifier = Modifier.clickable { model.dismissMessage() },
                )
            }
            if (state.preview != null) {
                Row(verticalAlignment = Alignment.CenterVertically, horizontalArrangement = Arrangement.spacedBy(8.dp)) {
                    Text(
                        if (state.edits == 0) stringResource(R.string.no_change_recommended)
                        else pluralStringResource(R.plurals.expressions_changed, state.edits, state.edits),
                        modifier = Modifier.weight(1f),
                    )
                    val compareLabel = stringResource(R.string.hold_to_compare)
                    Box(
                        modifier = Modifier
                            .heightIn(min = 48.dp).widthIn(min = 48.dp)
                            .border(1.dp, MaterialTheme.colorScheme.outline, RoundedCornerShape(24.dp))
                            .semantics { contentDescription = compareLabel }
                            .pointerInput(Unit) {
                                awaitEachGesture {
                                    awaitFirstDown()
                                    comparing = true
                                    waitForUpOrCancellation()
                                    comparing = false
                                }
                            }
                            .padding(horizontal = 16.dp),
                        contentAlignment = Alignment.Center,
                    ) { Text(stringResource(R.string.original)) }
                }
            }
            val person = state.selectedPerson
            if (person != null) {
                CandidateStrip(state, onChoose = { model.choose(person, it) })
                Row(horizontalArrangement = Arrangement.spacedBy(8.dp)) {
                    OutlinedButton(onClick = { model.resetPerson(person) }) { Text(stringResource(R.string.reset_person)) }
                    OutlinedButton(onClick = model::resetAll) { Text(stringResource(R.string.reset_all)) }
                }
            }
        }
    }
}

@Composable
private fun PhotoWithFaces(
    state: EditorState,
    bitmap: android.graphics.Bitmap,
    showMarkers: Boolean,
    onTapFace: (Int) -> Unit,
    onTapBackground: () -> Unit,
) {
    // Zoom and pan persist across comparisons because they live outside the image content.
    var zoom by remember { mutableFloatStateOf(1f) }
    var pan by remember { mutableStateOf(Offset.Zero) }
    BoxWithConstraints(
        modifier = Modifier
            .fillMaxSize()
            .pointerInput(Unit) {
                detectTransformGestures { _, panChange, zoomChange, _ ->
                    zoom = (zoom * zoomChange).coerceIn(1f, 8f)
                    pan = if (zoom == 1f) Offset.Zero else pan + panChange
                }
            }
            .clickable(onClick = onTapBackground, role = Role.Image),
        contentAlignment = Alignment.Center,
    ) {
        val density = LocalDensity.current
        val boxW = with(density) { maxWidth.toPx() }
        val boxH = with(density) { maxHeight.toPx() }
        val fit = minOf(boxW / state.imageWidth, boxH / state.imageHeight)
        val left = (boxW - state.imageWidth * fit) / 2
        val top = (boxH - state.imageHeight * fit) / 2
        Box(
            modifier = Modifier.fillMaxSize().graphicsLayer {
                scaleX = zoom; scaleY = zoom; translationX = pan.x; translationY = pan.y
            },
        ) {
            Image(
                bitmap = bitmap.asImageBitmap(), contentDescription = null,
                contentScale = ContentScale.Fit, modifier = Modifier.fillMaxSize(),
            )
            if (showMarkers) {
                for (marker in state.markers) {
                    val diameter = maxOf(with(density) { 48.dp.toPx() }, 2 * marker.radius * fit)
                    val label = stringResource(R.string.person_face, marker.person + 1)
                    val selected = state.selectedPerson == marker.person
                    Box(
                        modifier = Modifier
                            .offset(
                                x = with(density) { (left + marker.centerX * fit - diameter / 2).toDp() },
                                y = with(density) { (top + marker.centerY * fit - diameter / 2).toDp() },
                            )
                            .size(with(density) { diameter.toDp() })
                            .clip(CircleShape)
                            .border(
                                width = if (selected) 3.dp else 1.5.dp,
                                color = when {
                                    selected -> MaterialTheme.colorScheme.primary
                                    marker.changed -> MaterialTheme.colorScheme.tertiary
                                    else -> Color.White.copy(alpha = 0.7f)
                                },
                                shape = CircleShape,
                            )
                            .semantics { contentDescription = label; this.selected = selected }
                            .clickable(role = Role.Button) { onTapFace(marker.person) },
                    )
                }
            }
        }
    }
}

@Composable
private fun CandidateStrip(state: EditorState, onChoose: (Int) -> Unit) {
    LazyRow(horizontalArrangement = Arrangement.spacedBy(8.dp)) {
        items(state.candidates, key = { it.frame }) { candidate ->
            val label = when {
                candidate.isOriginal -> stringResource(R.string.original)
                !candidate.feasible ->
                    stringResource(R.string.candidate_unavailable, candidate.frame + 1, candidate.rejection)
                else -> stringResource(R.string.candidate_from_photo, candidate.frame + 1)
            }
            Column(
                horizontalAlignment = Alignment.CenterHorizontally,
                modifier = Modifier
                    .widthIn(max = 96.dp)
                    .semantics { contentDescription = label; selected = candidate.selected }
                    .clickable(enabled = candidate.feasible && state.busy == null, role = Role.Button) {
                        onChoose(candidate.frame)
                    },
            ) {
                Box(
                    modifier = Modifier
                        .size(80.dp)
                        .clip(RoundedCornerShape(12.dp))
                        .border(
                            width = if (candidate.selected) 3.dp else 1.dp,
                            color = if (candidate.selected) MaterialTheme.colorScheme.primary
                            else MaterialTheme.colorScheme.outline,
                            shape = RoundedCornerShape(12.dp),
                        ),
                    contentAlignment = Alignment.Center,
                ) {
                    val thumbnail = candidate.thumbnail
                    if (thumbnail != null) {
                        Image(
                            bitmap = thumbnail.asImageBitmap(), contentDescription = null,
                            contentScale = ContentScale.Crop, modifier = Modifier.fillMaxSize(),
                        )
                    } else {
                        Text(candidate.rejection, style = MaterialTheme.typography.labelSmall, modifier = Modifier.padding(4.dp))
                    }
                }
                Text(
                    when {
                        candidate.isOriginal -> stringResource(R.string.original)
                        candidate.recommended -> stringResource(R.string.recommended)
                        else -> "${candidate.frame + 1}"
                    },
                    style = MaterialTheme.typography.labelMedium,
                )
            }
        }
    }
}
