# ONNX Runtime C API header

`include/onnxruntime_c_api.h` is taken unmodified from the official
[ONNX Runtime](https://github.com/microsoft/onnxruntime) v1.20.1 release (MIT, see `LICENSE`).

X-Ray does not link against ONNX Runtime. It loads `onnxruntime.dll` dynamically at run time
(`LoadLibrary` + `OrtGetApiBase`), from the plugin's folder or from the X-Ray data folder, where it is
downloaded on demand (with SHA-256 verification) the first time face recognition is used.