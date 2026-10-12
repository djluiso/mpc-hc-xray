# Third-party headers

Only headers are vendored; nothing here is linked statically.

| Folder        | Source                                                                 | License                       |
|---------------|------------------------------------------------------------------------|-------------------------------|
| `onnxruntime` | ONNX Runtime 1.20.1 C API header                                       | MIT (`onnxruntime/LICENSE`)   |
| `rapidjson`   | [RapidJSON](https://github.com/Tencent/rapidjson) headers              | MIT (`rapidjson/license.txt`) |
| `stb`         | [stb_image.h](https://github.com/nothings/stb)                         | MIT / public domain (end of the file) |
| `ffmpeg`      | Public headers of libavutil, libavcodec, libavformat and libswscale used by X-Ray, from the FFmpeg tree of [LAV Filters](https://github.com/Nevcairiel/LAVFilters) as shipped with MPC-HC (FFmpeg commit `29e0b0813d85`) | LGPL 2.1+ (`ffmpeg/COPYING.LGPLv2.1`) |

The FFmpeg DLLs are loaded at run time (`avutil-lav-*.dll`, `avcodec-lav-*.dll`, `avformat-lav-*.dll`,
`swscale-lav-*.dll`) and must have the same major versions as these headers, which X-Ray checks:
avutil 61, avcodec 63, avformat 63, swscale 10. Update the headers together with the DLLs.