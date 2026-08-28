// Indirection so the module map does not hardcode an absolute path; the real
// headers are found via the include paths set in Package.swift.
//
// ggml-backend.h is needed as well as whisper.h: ggml ships its Metal, BLAS and
// CPU backends as separately-loadable modules, and they must be registered with
// `ggml_backend_load_all()` before a context is created. Without it the backend
// registry is empty and `whisper_init_from_file_with_params` aborts on
// `GGML_ASSERT(device)`.
#include <whisper.h>
#include <ggml-backend.h>
