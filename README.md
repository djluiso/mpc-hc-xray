# X-Ray for MPC-HC

X-Ray shows who is on screen, in the style of Prime Video X-Ray. It is a plugin of
[MPC-HC Plugin Host](https://github.com/djluiso/mpc-hc-plugin-host) and works with an unmodified MPC-HC.

## Build and install

```
build.bat [x64|x86] [PluginsFolder]
```

Needs Visual Studio 2022 (or its Build Tools) with the C++ workload; all headers are in `third_party`
(see [third_party/README.md](third_party/README.md)). Output: `bin\x64\XRay\XRay.dll`.

Copy the `XRay` folder into the `Plugins` folder next to `MpcPluginHost.exe`, or pass that `Plugins` folder to
`build.bat`. A running host loads (or reloads) it within a few seconds. Then start MPC-HC from the host's tray icon.

Scene analysis needs the LAV Filters FFmpeg DLLs (`avcodec-lav-*.dll`...) with the major versions of
`third_party\ffmpeg`. X-Ray looks for them in `XRay\ffmpeg` first, then in the player's `LAVFilters64`
(`LAVFilters` for 32-bit) folder. To ship them with the plugin, `set FFMPEG_DIR=<folder with the DLLs>` before
running `build.bat`.

`src\XRayPlugin.cpp` is the glue with the host; `sdk\MpcPlugin.h` is a copy of the host's SDK header.

## Usage

1. Pause a video. An **X-Ray** button appears at the top-left corner of the video.
2. Click it. A panel opens on the left side:
   * **In Scene** – **On screen now** (the people in the current shot) and **Also in this scene** (people seen in the
     last ~20 seconds, e.g. in other camera angles). Results appear progressively while the scene is scanned.
   * **All Cast** – the complete cast of the movie / episode.
   * Click an actor for the details: photo, date of birth, age and other well-known titles.
3. Seeking or frame stepping while the panel is open analyzes the new position. Resuming playback or pressing
   <kbd>Esc</kbd> closes the panel.

The title is guessed from the file name (e.g. `The.Matrix.1999.1080p.mkv`, `Breaking Bad S01E01.mkv`).
Generic file names such as `VIDEO_TS\VTS_01_1.VOB`, `BDMV\STREAM\00000.m2ts` or `movie.mkv` fall back to the folder name.

## How it works

* **Cast**: [Wikidata](https://www.wikidata.org) (no account needed, photos from Wikimedia Commons), or
  [TMDB](https://www.themoviedb.org) when an API key is configured (better coverage of TV episodes and photos).
  With Wikidata, the character names, the billing order and actors missing from Wikidata are taken from the
  **Cast** section of the English Wikipedia article. Every cast member is shown as "*Actor* as *Character*"
  (several roles are joined with " / ").
* **Scene**: besides the paused frame, X-Ray decodes the scene around it (20 s before to 6 s after, every 0.5 s
  near the pause and every 0.8 s further away) with the FFmpeg libraries shipped with LAV Filters. Camera cuts are
  detected from the colors of the frames. Faces are grouped across frames (by face similarity, and by position within
  a shot), and each group is named by voting over all its views, so a person turned away in one frame is still
  recognized from the others. Strongly recognized views are remembered as additional references for that movie.
  The scan is limited to 45 seconds. Scene analysis requires a local file (not streams or discs). Without the FFmpeg DLLs, X-Ray falls
  back to the paused frame from the web interface (needs *Enable preview* in MPC-HC's web interface options).
* **Faces**: everything is analyzed locally, nothing is uploaded. Faces are detected with
  [YuNet](https://github.com/opencv/opencv_zoo/tree/main/models/face_detection_yunet) and compared with the cast photos
  using [SFace](https://github.com/opencv/opencv_zoo/tree/main/models/face_recognition_sface), both run with
  [ONNX Runtime](https://onnxruntime.ai) (MIT / Apache 2.0). Only the cast of the identified title is searched.
* The runtime and the models (about 100 MB) are downloaded once, after asking for consent, from their official
  GitHub releases and verified with SHA-256. Choosing **Cast list only** skips the download; X-Ray then only lists the cast.
  `onnxruntime.dll` and the `.onnx` models can also be placed next to `XRay.dll`.

## Cache

Titles, cast lists, photos, face signatures and person details are cached. For every video file a timeline
(`timeline` folder) stores which parts were already scanned, the camera cuts and who was recognized where, so pausing
again in or near an analyzed scene is instant and only the parts not yet scanned are decoded.

`XRay` folder in the data folder of the host: `%LOCALAPPDATA%\MPC-HC\XRay`, or next to `MpcPluginHost.exe` in portable mode.

Delete the folder to clear the cache.

## Settings

X-Ray is enabled or disabled from the host's tray menu. Settings are in the `[Plugin.XRay]` section of
`MpcPluginHost.ini`:

| Name            | Description                                                                                    |
|-----------------|------------------------------------------------------------------------------------------------|
| `TMDbApiKey`    | Optional TMDB v3 API key or v4 read access token. The `TMDB_API_KEY` environment variable also works. |
| `EngineConsent` | `1` download the face recognition models, `0` cast list only. Delete it to be asked again.     |

The `XRayTMDbApiKey` / `XRayEngineConsent` values of older builds (MPC-HC `Settings` registry key) are still read.

## Limitations

* The overlay can't be drawn over exclusive D3D fullscreen.
* Recognition works best on clear, front-facing faces; heavy make-up, prosthetics or a large age difference with the
  reference photo may prevent a match.
* The X-Ray texts are English only.
* <kbd>Esc</kbd> is caught with a low-level keyboard hook while the panel is open and the player is in the foreground.
