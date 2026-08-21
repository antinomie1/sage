export module sage.archive;

// Facade of the native streaming Tar+Zstd engine. The implementation lives in
// module partitions:
//   :detail  -- internal: RAII handles, anchored path traversal, shared types
//   :tape    -- USTAR format, streaming walker, files.idx codec, inspection
//   :extract -- anchored cleanup and streaming extraction into a target root
//   :pack    -- package creation and local repository indexing

export import :tape;
export import :extract;
export import :pack;
