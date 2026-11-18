package io.github.alecjacobson.besttake.ui

import android.app.Application
import android.content.Intent
import android.net.Uri
import android.os.Bundle
import androidx.activity.ComponentActivity
import androidx.activity.compose.setContent
import androidx.activity.enableEdgeToEdge
import androidx.activity.viewModels
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.Surface
import androidx.compose.material3.darkColorScheme
import androidx.compose.runtime.getValue
import androidx.lifecycle.AndroidViewModel
import androidx.lifecycle.compose.collectAsStateWithLifecycle
import androidx.lifecycle.viewModelScope
import androidx.lifecycle.viewmodel.compose.viewModel
import io.github.alecjacobson.besttake.R
import io.github.alecjacobson.besttake.data.ProjectEntity
import io.github.alecjacobson.besttake.data.ProjectRepository
import kotlinx.coroutines.flow.MutableStateFlow
import kotlinx.coroutines.flow.SharingStarted
import kotlinx.coroutines.flow.StateFlow
import kotlinx.coroutines.flow.stateIn
import kotlinx.coroutines.flow.update
import kotlinx.coroutines.launch

data class HomeState(val openProject: Long? = null, val importing: Boolean = false, val message: String? = null)

class MainViewModel(application: Application) : AndroidViewModel(application) {
    private val repository = ProjectRepository(application)
    val projects: StateFlow<List<ProjectEntity>> =
        repository.projects.stateIn(viewModelScope, SharingStarted.WhileSubscribed(5000), emptyList())

    private val _state = MutableStateFlow(HomeState())
    val state: StateFlow<HomeState> = _state

    fun importPhotos(uris: List<Uri>) {
        val app = getApplication<Application>()
        when {
            uris.size < ProjectRepository.MIN_PHOTOS -> {
                _state.update { it.copy(message = app.getString(R.string.need_two_photos)) }
                return
            }
            uris.size > ProjectRepository.MAX_PHOTOS -> {
                _state.update { it.copy(message = app.getString(R.string.too_many_photos)) }
                return
            }
        }
        if (_state.value.importing) return
        _state.update { it.copy(importing = true) }
        viewModelScope.launch {
            try {
                val id = repository.createProject(uris)
                _state.update { it.copy(importing = false, openProject = id) }
            } catch (e: Exception) {
                _state.update { it.copy(importing = false, message = e.message ?: e.toString()) }
            }
        }
    }

    fun open(id: Long?) = _state.update { it.copy(openProject = id) }

    fun delete(id: Long) = viewModelScope.launch { repository.deleteProject(id) }

    fun dismissMessage() = _state.update { it.copy(message = null) }
}

class MainActivity : ComponentActivity() {
    private val model: MainViewModel by viewModels()

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        enableEdgeToEdge()
        if (savedInstanceState == null) handleShare(intent)
        setContent {
            MaterialTheme(colorScheme = darkColorScheme()) {
                Surface {
                    val state by model.state.collectAsStateWithLifecycle()
                    val projects by model.projects.collectAsStateWithLifecycle()
                    val open = state.openProject
                    if (open == null) {
                        HomeScreen(
                            state = state, projects = projects,
                            onPicked = model::importPhotos, onOpen = model::open, onDelete = { model.delete(it) },
                            onDismissMessage = model::dismissMessage,
                        )
                    } else {
                        val editor: EditorViewModel = viewModel(
                            key = "editor-$open", factory = EditorViewModel.Factory(application, open),
                        )
                        EditorScreen(editor, onBack = { model.open(null) })
                    }
                }
            }
        }
    }

    override fun onNewIntent(intent: Intent) {
        super.onNewIntent(intent)
        handleShare(intent)
    }

    private fun handleShare(intent: Intent?) {
        if (intent?.action != Intent.ACTION_SEND_MULTIPLE) return
        val uris = intent.getParcelableArrayListExtra(Intent.EXTRA_STREAM, Uri::class.java) ?: return
        model.importPhotos(uris)
    }
}
