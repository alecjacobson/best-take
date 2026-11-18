# best-take

An Android application for the standard Google Pixel 10 that combines the
preferred photographed expression of each person from a short sequence into
one natural group photo. Editing runs locally on the device.

The implementation specification is [docs/design.md](docs/design.md). Work is
tracked in [GitHub issues](https://github.com/alecjacobson/best-take/issues),
grouped by the milestones in the design's delivery plan.

## Layout

| Path | Contents |
| - | - |
| `docs/` | Design specification and engineering notes. |
| `engine/` | C++ image engine: geometry, masks, color, compositing, selection, validation. Independent of the UI, with a CPU reference implementation and host tests. |
| `app/` | Kotlin and Jetpack Compose Android application. |
| `tools/` | Host-side scripts for running the engine on photo sets. |

## Building

Engine and host tests (any platform with CMake and a C++17 compiler):

```sh
cmake -S engine -B engine/build && cmake --build engine/build -j 8
ctest --test-dir engine/build --output-on-failure
```

Android application (JDK 17, Android SDK with platform 36, NDK 27.0.12077973, CMake 3.22.1):

```sh
./gradlew :app:testDebugUnitTest :app:assembleDebug
```

The build downloads the analysis models listed in [docs/models.md](docs/models.md),
verifies their hashes, and packages them so editing works offline. The app
requires Android 16 on arm64.

To try a merge without a phone, see [tools/README.md](tools/README.md).

## Status

Early development. See the milestone issues for what is implemented and what
still requires measurement on a Pixel 10.
