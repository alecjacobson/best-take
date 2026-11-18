#!/usr/bin/env python3
"""Merge the best expression of each person from a set of related photos.

Host-side driver: runs face analysis (MediaPipe) on every photo, hands the
observations to the engine command-line tool, and writes the composite, the
recipe, and a report.

Example:
    python3 tools/best_take.py photos/*.jpg --output merged.jpg
"""

import argparse
import hashlib
import json
import math
import ssl
import subprocess
import sys
import tempfile
import urllib.request
from pathlib import Path

import cv2
import mediapipe as mp
import numpy as np

MODEL_URLS = {
    "face_landmarker.task": "https://storage.googleapis.com/mediapipe-models/face_landmarker/face_landmarker/float16/1/face_landmarker.task",
    "blaze_face_short_range.tflite": "https://storage.googleapis.com/mediapipe-models/face_detector/blaze_face_short_range/float16/1/blaze_face_short_range.tflite",
}


def model_path(name: str, cache: Path) -> Path:
    """Return the local path of a model, downloading it on first use."""
    cache.mkdir(parents=True, exist_ok=True)
    path = cache / name
    if not path.exists():
        print(f"downloading {name}", file=sys.stderr)
        try:  # python.org builds ship without a system CA bundle
            import certifi

            context = ssl.create_default_context(cafile=certifi.where())
        except ImportError:
            context = ssl.create_default_context()
        with urllib.request.urlopen(MODEL_URLS[name], context=context, timeout=120) as response:
            data = response.read()
        partial = path.with_suffix(path.suffix + ".part")
        partial.write_bytes(data)
        partial.rename(path)
    return path


def detect_faces(detector, rgb: np.ndarray) -> list:
    """Detect faces on the whole image and on overlapping tiles.

    The bundled detector is a short-range model, so small faces in a group
    photo are found by running it on tiles where they appear larger.

    Returns:
        Boxes as (x, y, w, h, score) in image pixels after non-maximum suppression.
    """
    h, w = rgb.shape[:2]
    boxes = []
    for grid in (1, 2, 3, 4):
        tile_w, tile_h = w / grid, h / grid
        if grid > 1 and min(tile_w, tile_h) < 192:
            break
        steps = 1 if grid == 1 else 2 * grid - 1  # 50% overlap
        for iy in range(steps):
            for ix in range(steps):
                x0 = int(ix * tile_w / 2) if grid > 1 else 0
                y0 = int(iy * tile_h / 2) if grid > 1 else 0
                x1, y1 = min(w, int(x0 + tile_w)), min(h, int(y0 + tile_h))
                tile = np.ascontiguousarray(rgb[y0:y1, x0:x1])
                result = detector.detect(mp.Image(image_format=mp.ImageFormat.SRGB, data=tile))
                for det in result.detections:
                    b = det.bounding_box
                    # Faces cut by a tile edge are found whole in a neighboring tile.
                    cut = grid > 1 and (
                        (b.origin_x <= 2 and x0 > 0)
                        or (b.origin_y <= 2 and y0 > 0)
                        or (b.origin_x + b.width >= x1 - x0 - 2 and x1 < w)
                        or (b.origin_y + b.height >= y1 - y0 - 2 and y1 < h)
                    )
                    if not cut:
                        boxes.append((b.origin_x + x0, b.origin_y + y0, b.width, b.height, det.categories[0].score))
    boxes.sort(key=lambda b: -b[4])
    kept = []
    for b in boxes:
        duplicate = False
        for k in kept:
            ix = max(0, min(b[0] + b[2], k[0] + k[2]) - max(b[0], k[0]))
            iy = max(0, min(b[1] + b[3], k[1] + k[3]) - max(b[1], k[1]))
            inter = ix * iy
            if inter > 0.3 * min(b[2] * b[3], k[2] * k[3]):
                duplicate = True
                break
        if not duplicate:
            kept.append(b)
    return kept


def analyze_face(landmarker, rgb: np.ndarray, box) -> dict | None:
    """Run the landmarker on a crop around one detection.

    Returns:
        The face record for the engine job (landmarks in image pixels plus
        facial-state measurements), or None if no face mesh was found.
    """
    h, w = rgb.shape[:2]
    x, y, bw, bh, _ = box
    size = 1.8 * max(bw, bh)
    cx, cy = x + bw / 2, y + bh / 2
    x0, y0 = int(round(cx - size / 2)), int(round(cy - size / 2))
    side = int(round(size))
    crop = np.zeros((side, side, 3), np.uint8)
    sx0, sy0, sx1, sy1 = max(0, x0), max(0, y0), min(w, x0 + side), min(h, y0 + side)
    if sx1 <= sx0 or sy1 <= sy0:
        return None
    crop[sy0 - y0 : sy1 - y0, sx0 - x0 : sx1 - x0] = rgb[sy0:sy1, sx0:sx1]
    model_side = min(side, 512)
    if model_side != side:
        crop = cv2.resize(crop, (model_side, model_side), interpolation=cv2.INTER_AREA)
    result = landmarker.detect(mp.Image(image_format=mp.ImageFormat.SRGB, data=np.ascontiguousarray(crop)))
    if not result.face_landmarks:
        return None
    lm = result.face_landmarks[0]
    flat = []
    for p in lm:
        flat += [x0 + p.x * side, y0 + p.y * side]
    shapes = {c.category_name: c.score for c in result.face_blendshapes[0]} if result.face_blendshapes else {}
    look = max(
        [shapes.get(f"eyeLook{d}{s}", 0.0) for d in ("In", "Out", "Up", "Down") for s in ("Left", "Right")] or [0.0]
    )
    record = {
        "landmarks": flat,
        "eyes_open": 1.0 - max(shapes.get("eyeBlinkLeft", 0.0), shapes.get("eyeBlinkRight", 0.0)),
        "smile": 0.5 * (shapes.get("mouthSmileLeft", 0.0) + shapes.get("mouthSmileRight", 0.0)),
        "gaze_forward": 1.0 - look,
        "confidence": 1.0 if shapes else 0.0,
    }
    if result.facial_transformation_matrixes:
        m = np.array(result.facial_transformation_matrixes[0])
        record["yaw"] = math.degrees(math.atan2(m[0, 2], m[2, 2]))
        record["pitch"] = math.degrees(math.asin(max(-1.0, min(1.0, -m[1, 2] / np.linalg.norm(m[:3, 2])))))
    return record


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("photos", nargs="+", type=Path, help="2-20 related photos")
    parser.add_argument("--output", type=Path, required=True, help="composite image to write")
    parser.add_argument("--recipe", type=Path, help="write the edit recipe (JSON) here")
    parser.add_argument("--report", type=Path, help="write the selection and quality report (JSON) here")
    parser.add_argument("--base", type=int, default=-1, help="index of the photo to use as base (default: automatic)")
    parser.add_argument(
        "--lock",
        action="append",
        default=[],
        metavar="PERSON=PHOTO",
        help="force a person (id from the report) to use the face from the photo with this index",
    )
    parser.add_argument("--blend", choices=["multiband", "screened_gradient"], default="multiband")
    parser.add_argument("--smile-weight", type=float, help="preference weight for smiling (0 keeps serious faces equal)")
    parser.add_argument("--cli", type=Path, default=Path(__file__).resolve().parent.parent / "engine/build/tools/bt_cli")
    parser.add_argument("--model-cache", type=Path, default=Path.home() / ".cache/best-take/models")
    parser.add_argument("--debug-dir", type=Path, help="keep the engine job and annotated inputs here")
    args = parser.parse_args()

    if not 2 <= len(args.photos) <= 20:
        parser.error("between 2 and 20 photos are supported")
    if not args.cli.exists():
        parser.error(f"engine tool not found at {args.cli}; build the engine first")

    vision = mp.tasks.vision
    base_options = mp.tasks.BaseOptions
    detector = vision.FaceDetector.create_from_options(
        vision.FaceDetectorOptions(
            base_options=base_options(model_asset_path=str(model_path("blaze_face_short_range.tflite", args.model_cache))),
            min_detection_confidence=0.5,
        )
    )
    landmarker = vision.FaceLandmarker.create_from_options(
        vision.FaceLandmarkerOptions(
            base_options=base_options(model_asset_path=str(model_path("face_landmarker.task", args.model_cache))),
            num_faces=1,
            output_face_blendshapes=True,
            output_facial_transformation_matrixes=True,
        )
    )

    with tempfile.TemporaryDirectory() as tmp:
        work = args.debug_dir or Path(tmp)
        work.mkdir(parents=True, exist_ok=True)
        frames = []
        for i, photo in enumerate(args.photos):
            bgr = cv2.imread(str(photo), cv2.IMREAD_COLOR)  # applies EXIF orientation
            if bgr is None:
                parser.error(f"cannot decode {photo}")
            rgb = cv2.cvtColor(bgr, cv2.COLOR_BGR2RGB)
            faces = [f for f in (analyze_face(landmarker, rgb, b) for b in detect_faces(detector, rgb)) if f]
            print(f"{photo.name}: {len(faces)} face(s)", file=sys.stderr)
            ppm = work / f"frame{i}.ppm"
            cv2.imwrite(str(ppm), bgr)
            frames.append(
                {
                    "id": photo.name,
                    "hash": hashlib.sha256(photo.read_bytes()).hexdigest(),
                    "ppm": str(ppm),
                    "faces": faces,
                }
            )
            if args.debug_dir:
                annotated = bgr.copy()
                for f in faces:
                    pts = np.array(f["landmarks"]).reshape(-1, 2)
                    for px, py in pts[::4]:
                        cv2.circle(annotated, (int(px), int(py)), 1, (0, 255, 0), -1)
                cv2.imwrite(str(work / f"annotated{i}.jpg"), annotated)

        out_ppm = work / "out.ppm"
        recipe_path = args.recipe or work / "recipe.json"
        report_path = args.report or work / "report.json"
        job = {
            "frames": frames,
            "blend": args.blend,
            "base": args.base,
            "output_ppm": str(out_ppm),
            "recipe_out": str(recipe_path),
            "report_out": str(report_path),
        }
        if args.lock:
            job["locks"] = {person: int(photo) for person, photo in (item.split("=") for item in args.lock)}
        if args.smile_weight is not None:
            job["preference"] = {"smile": args.smile_weight}
        job_path = work / "job.json"
        job_path.write_text(json.dumps(job))
        status = subprocess.run([str(args.cli), str(job_path)], timeout=600).returncode
        if status not in (0, 3):
            return status
        cv2.imwrite(str(args.output), cv2.imread(str(out_ppm), cv2.IMREAD_COLOR), [cv2.IMWRITE_JPEG_QUALITY, 97])
        report = json.loads(report_path.read_text())
        for person in report.get("persons", []):
            src = person["source"]
            note = "unchanged" if src in (-1, report["base"]) else f"from {args.photos[src].name}"
            print(f"person {person['person']}: {note}", file=sys.stderr)
        return status


if __name__ == "__main__":
    sys.exit(main())
