import CWhisper

/// One-time registration of ggml's compute backends.
///
/// ggml ships Metal, BLAS and CPU as separately-loadable modules that are *not*
/// registered automatically. Any `whisper_*_init_*` call made before
/// `ggml_backend_load_all()` finds an empty device registry and hits
/// `GGML_ASSERT(device)`, which calls `abort()` — it does not return null, so it
/// cannot be caught. The process simply dies.
///
/// This lived inside `WhisperEngine` at first, which meant `SileroVAD` only
/// worked if a `WhisperEngine` happened to have been loaded before it. That held
/// in the app by accident of ordering and crashed the moment the VAD was used on
/// its own. Shared here so neither has to know about the other.
enum GGMLBackends {

    /// Idempotent. The first call compiles the Metal library and can take
    /// several seconds; later calls are free.
    static func registerOnce() {
        _ = registered
    }

    /// A `let` global is initialised lazily and exactly once by the runtime,
    /// which gives `dispatch_once` semantics and satisfies Swift 6's rules on
    /// global mutable state.
    private static let registered: Bool = {
        ggml_backend_load_all()
        return true
    }()
}
