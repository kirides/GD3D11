#include "pch.h"
#include "SqliteBlobStore.h"
#include "Toolbox.h"
#include "Logger.h"

#include <sqlite3.h>
#include <algorithm>
#include <filesystem>
#include <vector>

namespace {
    bool Exec( sqlite3* db, const char* sql ) {
        char* errMsg = nullptr;
        const int rc = sqlite3_exec( db, sql, nullptr, nullptr, &errMsg );
        if ( rc != SQLITE_OK ) {
            LogWarn() << "SqliteBlobStore: " << ( errMsg ? errMsg : sqlite3_errstr( rc ) );
            sqlite3_free( errMsg );
            return false;
        }
        return true;
    }

    // Every live SqliteBlobStore registers itself here so CloseAll() can find it - see the .h comment on
    // why this can't just be "let them destruct as function-local statics".
    std::mutex g_RegistryMutex;
    std::vector<SqliteBlobStore*> g_Instances;
}

SqliteBlobStore::SqliteBlobStore( const std::string& path ) {
    {
        std::scoped_lock lock( g_RegistryMutex );
        g_Instances.push_back( this );
    }

    const std::filesystem::path p( path );
    if ( p.has_parent_path() ) {
        Toolbox::CreateDirectoryRecursive( p.parent_path().string() );
    }

    sqlite3* db = nullptr;
    if ( sqlite3_open_v2( path.c_str(), &db, SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE, nullptr ) != SQLITE_OK ) {
        LogWarn() << "SqliteBlobStore: failed to open " << path << " - caching disabled for this store.";
        if ( db ) sqlite3_close( db );
        return;
    }

    // WAL: a reader on another thread (once it has m_mutex) never blocks behind a writer mid-transaction.
    // NORMAL sync: this is a cache, not a ledger - losing the last few writes to a power cut is exactly
    // as safe as never having cached them, and trading that away is the entire point of WAL+NORMAL.
    if ( !Exec( db, "PRAGMA journal_mode=WAL;" ) ||
        !Exec( db, "PRAGMA synchronous=NORMAL;" ) ||
        !Exec( db, "CREATE TABLE IF NOT EXISTS blobs (key INTEGER PRIMARY KEY, data BLOB NOT NULL);" ) ) {
        sqlite3_close( db );
        return;
    }

    // Upsert pre-sizes the row's blob with zeroblob() so Put() can then open it for incremental WRITE by
    // rowid - `key` IS the rowid (INTEGER PRIMARY KEY), so no SELECT is needed to find it afterward.
    sqlite3_stmt* upsertStmt = nullptr;
    sqlite3_stmt* deleteStmt = nullptr;
    if ( sqlite3_prepare_v2( db,
            "INSERT INTO blobs(key, data) VALUES(?1, zeroblob(?2)) "
            "ON CONFLICT(key) DO UPDATE SET data=zeroblob(?2);",
            -1, &upsertStmt, nullptr ) != SQLITE_OK ||
        sqlite3_prepare_v2( db, "DELETE FROM blobs WHERE key=?1;", -1, &deleteStmt, nullptr ) != SQLITE_OK ) {
        if ( upsertStmt ) sqlite3_finalize( upsertStmt );
        if ( deleteStmt ) sqlite3_finalize( deleteStmt );
        sqlite3_close( db );
        return;
    }

    m_db = db;
    m_upsertStmt = upsertStmt;
    m_deleteStmt = deleteStmt;
}

SqliteBlobStore::~SqliteBlobStore() {
    {
        std::scoped_lock lock( g_RegistryMutex );
        g_Instances.erase( std::remove( g_Instances.begin(), g_Instances.end(), this ), g_Instances.end() );
    }
    Close();
}

void SqliteBlobStore::Close() {
    std::scoped_lock lock( m_mutex );
    if ( m_upsertStmt ) { sqlite3_finalize( m_upsertStmt ); m_upsertStmt = nullptr; }
    if ( m_deleteStmt ) { sqlite3_finalize( m_deleteStmt ); m_deleteStmt = nullptr; }
    if ( m_db ) {
        // The last connection to a WAL-mode database closing cleanly is what makes SQLite checkpoint
        // and delete its -wal/-shm files - see the .h comment on CloseAll(). If some other handle to
        // the same file is still open (shouldn't happen - one SqliteBlobStore per db path, one
        // connection per instance) that checkpoint is simply skipped; sqlite3_close() itself still
        // succeeds either way.
        sqlite3_close( m_db );
        m_db = nullptr;
    }
}

void SqliteBlobStore::CloseAll() {
    std::vector<SqliteBlobStore*> instances;
    {
        std::scoped_lock lock( g_RegistryMutex );
        instances = g_Instances;   // snapshot - Close() below never touches g_Instances itself
    }
    for ( SqliteBlobStore* store : instances ) {
        store->Close();
    }
}

bool SqliteBlobStore::TryGet( uint64_t key, std::vector<uint8_t>& outData ) const {
    if ( !m_db ) return false;
    std::scoped_lock lock( m_mutex );

    sqlite3_blob* blob = nullptr;
    if ( sqlite3_blob_open( m_db, "main", "blobs", "data", static_cast<sqlite3_int64>( key ), 0, &blob ) != SQLITE_OK ) {
        return false;   // no row for this key - a miss, not an error
    }

    const int size = sqlite3_blob_bytes( blob );
    outData.resize( static_cast<size_t>( size ) );
    const bool ok = size == 0 || sqlite3_blob_read( blob, outData.data(), size, 0 ) == SQLITE_OK;
    sqlite3_blob_close( blob );
    return ok;
}

void SqliteBlobStore::Put( uint64_t key, const void* data, size_t size ) {
    if ( !m_db || size == 0 ) return;
    std::scoped_lock lock( m_mutex );

    sqlite3_reset( m_upsertStmt );
    sqlite3_bind_int64( m_upsertStmt, 1, static_cast<sqlite3_int64>( key ) );
    sqlite3_bind_int64( m_upsertStmt, 2, static_cast<sqlite3_int64>( size ) );
    if ( sqlite3_step( m_upsertStmt ) != SQLITE_DONE ) {
        return;
    }

    sqlite3_blob* blob = nullptr;
    if ( sqlite3_blob_open( m_db, "main", "blobs", "data", static_cast<sqlite3_int64>( key ), 1, &blob ) != SQLITE_OK ) {
        return;
    }
    sqlite3_blob_write( blob, data, static_cast<int>( size ), 0 );
    sqlite3_blob_close( blob );
}

void SqliteBlobStore::Erase( uint64_t key ) {
    if ( !m_db ) return;
    std::scoped_lock lock( m_mutex );
    sqlite3_reset( m_deleteStmt );
    sqlite3_bind_int64( m_deleteStmt, 1, static_cast<sqlite3_int64>( key ) );
    sqlite3_step( m_deleteStmt );
}
