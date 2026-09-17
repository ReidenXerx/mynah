/* mynah/mynah.h — the C API. The only header front ends include.
 *
 * One dictation engine, written once in C++ behind this C API, driving every
 * front end: the Swift app on macOS (imported through a module map, exactly
 * as whisper.cpp is today), the headless `mynah` binary on Omarchy, and
 * mynah-kde on Plasma. See docs/ENGINE-MIGRATION.md.
 *
 * Audio in: 16 kHz mono float samples pushed from the front end's capture
 * thread. Everything out: events delivered to the callback given at create.
 *
 * Threading contract:
 *   - Events are delivered on engine threads. Callbacks must not block;
 *     front ends marshal to their own UI thread.
 *   - mynah_push_audio is the only call made from the real-time capture
 *     thread. It never blocks and never allocates.
 *   - mynah_destroy must run before process exit: ggml aborts at exit if a
 *     Metal context is still alive (docs/SWIFT-APP.md, open issue 9).
 *   - Every other call may be made from any thread.
 *
 * Return codes: 0 on success, -1 on failure.
 *
 * Version: mynah_version() reports the single version number (P9); the macOS
 * Info.plist and the Linux PKGBUILD take theirs from here.
 */

#ifndef MYNAH_MYNAH_H
#define MYNAH_MYNAH_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Opaque handles — no public layout. */
typedef struct mynah_engine mynah_engine;
typedef struct mynah_event mynah_event;

typedef enum {
    MYNAH_IDLE,
    MYNAH_LOADING,
    MYNAH_LISTENING,
    MYNAH_TRANSCRIBING,
} mynah_state;

/* Bands in a MYNAH_EVENT_LEVEL event (P4: 12 spectrum bands everywhere). */
#define MYNAH_SPECTRUM_BANDS 12

typedef enum {
    MYNAH_EVENT_STATE,    /* mynah_event_state */
    MYNAH_EVENT_LEVEL,    /* mynah_event_level 0..1 + mynah_event_bands[12] */
    MYNAH_EVENT_TEXT,     /* mynah_event_text: UTF-8, spacing already applied —
                             the front end types it */
    MYNAH_EVENT_PROBLEM,  /* mynah_event_problem_code (stable string, e.g.
                             "no_model", "model_load_failed", "no_audio") +
                             mynah_event_problem_message (human wording) */
    MYNAH_EVENT_MODEL,    /* mynah_event_model_status + name + tier */
} mynah_event_kind;

typedef enum {
    MYNAH_MODEL_LOADING,
    MYNAH_MODEL_LOADED,
    MYNAH_MODEL_UNLOADED,
} mynah_model_status;

/* Called on engine threads; must not block. */
typedef void (*mynah_event_fn)(const mynah_event *event, void *user);

/* --- lifecycle ----------------------------------------------------------- */

/* The core version, e.g. "0.14.0". Valid until process exit. */
const char *mynah_version(void);

/* Create an engine. config_path is NULL for the default
 * (~/.config/mynah/config.toml, honouring $MYNAH_CONFIG_DIR) — front ends pass
 * NULL and let the core own its config. On failure returns NULL and, if error
 * is non-NULL, sets *error to a malloc'd UTF-8 message the caller frees().
 */
mynah_engine *mynah_create(const char *config_path,
                           mynah_event_fn on_event,
                           void *user,
                           char **error);

/* Free the engine. Frees whisper and VAD contexts before returning, so a
 * process that exits cleanly afterwards does not trip ggml's at-exit check. */
void mynah_destroy(mynah_engine *engine);

/* --- session control ------------------------------------------------------ */

int mynah_toggle(mynah_engine *engine);
int mynah_start(mynah_engine *engine);
/* Returns at once; a session still draining is reported as events. */
int mynah_stop(mynah_engine *engine);
int mynah_ptt_press(mynah_engine *engine);
int mynah_ptt_release(mynah_engine *engine);

/* --- audio ---------------------------------------------------------------- */

/* Push 16 kHz mono samples, normalized float [-1, 1]. Capture thread only;
 * never blocks, never allocates: copies into a ring buffer. */
int mynah_push_audio(mynah_engine *engine, const float *samples, size_t count);

/* --- state and config ------------------------------------------------------ */

mynah_state mynah_get_state(const mynah_engine *engine);

/* Re-read the config file the engine was created with. Values that a live
 * session depends on take effect on the next session. */
int mynah_reload_config(mynah_engine *engine);

/* --- event accessors (no public layout) ----------------------------------- */

mynah_event_kind    mynah_event_get_kind(const mynah_event *event);
/* MYNAH_EVENT_STATE */
mynah_state         mynah_event_state(const mynah_event *event);
/* MYNAH_EVENT_LEVEL: 0..1, and MYNAH_SPECTRUM_BANDS band values 0..1 */
float               mynah_event_level(const mynah_event *event);
const float        *mynah_event_bands(const mynah_event *event);
/* MYNAH_EVENT_TEXT: UTF-8, spacing applied; the front end types it. */
const char         *mynah_event_text(const mynah_event *event);
/* MYNAH_EVENT_PROBLEM: a stable code to match on, and human wording to show. */
const char         *mynah_event_problem_code(const mynah_event *event);
const char         *mynah_event_problem_message(const mynah_event *event);
/* MYNAH_EVENT_MODEL */
mynah_model_status  mynah_event_model_status(const mynah_event *event);
const char         *mynah_event_model_name(const mynah_event *event);
/* "gpu" / "small" / "base" on Linux, NULL on macOS (M4: no tiers there). */
const char         *mynah_event_model_tier(const mynah_event *event);

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* MYNAH_MYNAH_H */