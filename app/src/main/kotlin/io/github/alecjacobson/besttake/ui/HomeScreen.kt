package io.github.alecjacobson.besttake.ui

import androidx.activity.compose.rememberLauncherForActivityResult
import androidx.activity.result.PickVisualMediaRequest
import androidx.activity.result.contract.ActivityResultContracts
import androidx.compose.foundation.clickable
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.PaddingValues
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.heightIn
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.safeDrawingPadding
import androidx.compose.foundation.lazy.LazyColumn
import androidx.compose.foundation.lazy.items
import androidx.compose.material3.Button
import androidx.compose.material3.CircularProgressIndicator
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.Text
import androidx.compose.material3.TextButton
import androidx.compose.runtime.Composable
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.res.stringResource
import androidx.compose.ui.unit.dp
import io.github.alecjacobson.besttake.R
import io.github.alecjacobson.besttake.data.ProjectEntity
import io.github.alecjacobson.besttake.data.ProjectRepository

@Composable
fun HomeScreen(
    state: HomeState,
    projects: List<ProjectEntity>,
    onPicked: (List<android.net.Uri>) -> Unit,
    onOpen: (Long) -> Unit,
    onDelete: (Long) -> Unit,
    onDismissMessage: () -> Unit,
) {
    val picker = rememberLauncherForActivityResult(
        ActivityResultContracts.PickMultipleVisualMedia(ProjectRepository.MAX_PHOTOS),
    ) { uris -> if (uris.isNotEmpty()) onPicked(uris) }

    Column(
        modifier = Modifier.fillMaxSize().safeDrawingPadding().padding(16.dp),
        verticalArrangement = Arrangement.spacedBy(12.dp),
    ) {
        Text(stringResource(R.string.app_name), style = MaterialTheme.typography.headlineMedium)
        Text(stringResource(R.string.select_photos_hint), style = MaterialTheme.typography.bodyLarge)
        Row(verticalAlignment = Alignment.CenterVertically, horizontalArrangement = Arrangement.spacedBy(12.dp)) {
            Button(
                enabled = !state.importing,
                modifier = Modifier.heightIn(min = 48.dp),
                onClick = {
                    onDismissMessage()
                    picker.launch(PickVisualMediaRequest(ActivityResultContracts.PickVisualMedia.ImageOnly))
                },
            ) { Text(stringResource(R.string.select_photos)) }
            if (state.importing) {
                CircularProgressIndicator()
                Text(stringResource(R.string.copying_photos))
            }
        }
        state.message?.let { Text(it, color = MaterialTheme.colorScheme.error) }
        Text(stringResource(R.string.projects), style = MaterialTheme.typography.titleMedium)
        if (projects.isEmpty()) Text(stringResource(R.string.no_projects))
        LazyColumn(contentPadding = PaddingValues(vertical = 4.dp)) {
            items(projects, key = { it.id }) { project ->
                Row(
                    modifier = Modifier.fillMaxWidth().heightIn(min = 56.dp).clickable { onOpen(project.id) },
                    verticalAlignment = Alignment.CenterVertically,
                    horizontalArrangement = Arrangement.SpaceBetween,
                ) {
                    Text(project.name, style = MaterialTheme.typography.bodyLarge)
                    TextButton(onClick = { onDelete(project.id) }) { Text(stringResource(R.string.delete_project)) }
                }
            }
        }
    }
}
