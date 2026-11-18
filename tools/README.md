# Host tools

`best_take.py` runs the whole pipeline off-device so merges can be inspected
at native resolution: MediaPipe face detection on overlapping tiles, Face
Landmarker on each face crop, then the engine (`engine/build/tools/bt_cli`)
for association, selection, recipe building, rendering, and validation.

```sh
cmake -S engine -B engine/build && cmake --build engine/build -j 8
pip install mediapipe opencv-python
python3 tools/best_take.py photos/*.jpg --output merged.jpg --report report.json
```

Useful options: `--base N` fixes the base photo, `--lock PERSON=N` forces a
person's face to come from photo `N`, `--blend screened_gradient` selects the
gradient-domain blend, and `--debug-dir DIR` keeps the engine job, the recipe,
and landmark overlays. The exit status is 3 when a rendered replacement fails
its quality check.

The MediaPipe models are downloaded on first use to `~/.cache/best-take/models`.
