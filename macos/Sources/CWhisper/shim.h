// Indirection so the module map does not hardcode an absolute path; the real
// headers are found via the include paths set by build-app.sh / Package.swift,
// which point at vendor/install/include.
//
// ggml-backend.h is needed as well as whisper.h so Swift can see
// `ggml_backend_load_all` — see GGMLBackends.swift for why it must be called.
#include <whisper.h>
#include <ggml-backend.h>
