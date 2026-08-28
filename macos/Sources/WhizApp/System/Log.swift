import OSLog

/// Unified logging, so a failing session can be diagnosed without a debugger:
///
///     log stream --predicate 'subsystem == "com.reidenxerx.whiz"' --level debug
///
/// Note that `info` and `debug` are not persisted to the log store, so
/// `log show` will not find them after the fact — lifecycle events worth
/// retrieving later use `notice`.
///
/// The Python engine leaned on a LaunchAgent log file for this. An app has no
/// stdout anyone will see, so `os.Logger` is the equivalent — and unlike
/// `NSLog` it is structured and filterable.
enum Log {
    static let session = Logger(subsystem: "com.reidenxerx.whiz", category: "session")
    static let audio = Logger(subsystem: "com.reidenxerx.whiz", category: "audio")
    static let stt = Logger(subsystem: "com.reidenxerx.whiz", category: "stt")
    static let ui = Logger(subsystem: "com.reidenxerx.whiz", category: "ui")
}
