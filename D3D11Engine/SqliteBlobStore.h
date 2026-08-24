#pragma once

/** Generic key -> blob cache backed by one SQLite database file, using SQLite's incremental BLOB I/O API
    (sqlite3_blob_open/read/write, sqlite3.org/c3ref/blob_open.html) so a lookup or store never needs a
    full row copied through bound parameters. Consumers never see SQLite: this is a plain
    uint64-key -> bytes store (TryGet/Put/Erase) - no schema, no SQL, nothing sqlite-shaped leaks into the
    header or the call sites that use it.

    One instance IS one database file/connection. Every call is internally serialized (see .cpp): this
    exists to back the mesh-optimize and shader on-disk caches, which are all load-time-only work (never
    the frame path), so a single mutex per store costs nothing that matters here and keeps correctness
    independent of how the vendored SQLite was built (threading mode etc). Safe to call from multiple
    threads - callers on GD3D11's worker pool do exactly that.

    If the database can't be opened or created (read-only install, locked file, first run before the
    directory exists, ...) the store silently becomes a no-op: every TryGet is a miss, every Put/Erase
    does nothing. A broken cache degrades to "no caching", not a failed world/shader load. */

#include <cstdint>
#include <mutex>
#include <string>
#include <vector>

class SqliteBlobStore {
public:
    /** Opens (creating if needed, including the parent directory) the database at `path`. */
    explicit SqliteBlobStore( const std::string& path );
    ~SqliteBlobStore();

    SqliteBlobStore( const SqliteBlobStore& ) = delete;
    SqliteBlobStore& operator=( const SqliteBlobStore& ) = delete;

    bool IsOpen() const { return m_db != nullptr; }

    /** Reads the blob stored under `key` into `outData` and returns true, or leaves `outData` untouched
        and returns false if absent (or the store failed to open). */
    bool TryGet( uint64_t key, std::vector<uint8_t>& outData ) const;

    /** Stores, or overwrites, the blob under `key`. No-op if the store failed to open. */
    void Put( uint64_t key, const void* data, size_t size );

    /** Removes `key` if present. No-op if the store failed to open or the key is absent. */
    void Erase( uint64_t key );

    /** Finalizes the prepared statements and closes the connection. Idempotent, and safe to call even
        though every TryGet/Put/Erase already null-checks - after this, the instance is just a permanent
        no-op store (matches what "failed to open" already looks like) rather than a dangling handle.
        Closing the LAST connection to a WAL-mode database is what makes SQLite checkpoint and delete its
        -wal/-shm files; see CloseAll(). */
    void Close();

    /** Closes every SqliteBlobStore instance currently alive (every on-disk cache: mesh, D3D11 shader,
        D3D12 shader). Call this once from the engine's own shutdown path.

        Why this exists instead of relying on these being function-local statics that destruct
        themselves: Engine::OnShutDown() calls exit(0) as a deliberate workaround for a crash on the
        normal teardown path, and it does so from DllMain(DLL_PROCESS_DETACH) - under the loader lock,
        with no guarantee any other thread still touching a store has been stopped first. Whether static
        destructors still run cleanly through that is not something to bet a "did the -wal/-shm file get
        deleted" outcome on. An explicit, single, ordered CloseAll() call is deterministic regardless. */
    static void CloseAll();

private:
    struct sqlite3* m_db = nullptr;
    struct sqlite3_stmt* m_upsertStmt = nullptr;
    struct sqlite3_stmt* m_deleteStmt = nullptr;
    mutable std::mutex m_mutex;
};
