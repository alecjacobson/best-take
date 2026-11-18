# Model manifest

Every learned model packaged with the application is listed here with the
exact artifact that ships. A model is eligible for release only when its
checkpoint and dependency chain are covered by terms that permit it
(design: Model distribution requirements).

| Artifact | Role | Source | SHA-256 | Precision | Runtime |
| - | - | - | - | - | - |
| `blaze_face_short_range.tflite` | Face detection on scene tiles | `https://storage.googleapis.com/mediapipe-models/face_detector/blaze_face_short_range/float16/1/blaze_face_short_range.tflite` | `b4578f35940bf5a1a655214a1cce5cab13eba73c1297cd78e1a04c2380b0152f` | float16 | MediaPipe Tasks (CPU) |
| `face_landmarker.task` | 478 landmarks and 52 blendshapes per face crop | `https://storage.googleapis.com/mediapipe-models/face_landmarker/face_landmarker/float16/1/face_landmarker.task` | `64184e229b263107bc2b804c6625db1341ff2bb731874b0bcc2fe6544e0bc9ff` | float16 | MediaPipe Tasks (CPU) |

The hashes are enforced by the `downloadModels` Gradle task; a changed
artifact fails the build.

## Open items before release

- **License evidence.** Both artifacts come from Google's MediaPipe model
  collection. Record the license stated on each model card, and the terms of
  the MediaPipe Tasks library, here before any release.
- **Detector qualification.** The short-range detector is run on overlapping
  tiles to reach small faces. Its recall on group photographs has not been
  measured; a full-scene detector with shippable terms is tracked separately.
- **Validation reports.** Landmark, blendshape, and pose accuracy on
  group-photo data have not been measured.
- **Runtime.** The design names LiteRT CompiledModel as the primary inference
  interface; the current build uses MediaPipe Tasks. Migration is tracked
  separately.
