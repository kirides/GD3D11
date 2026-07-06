#pragma once

namespace serde {
    template<typename T>
    concept PureScalar = std::is_scalar_v<T> && !std::is_pointer_v<T>;

    // --- Deserialization ---

    template<typename T, typename ReaderFunc>
        requires PureScalar<T>
    inline size_t DeserializeFrom( ReaderFunc&& read_bytes, T& value, void* userdata ) {
        return read_bytes( &value, sizeof( T ), userdata );
    }

    static size_t _serde_readfile( void* ptr, size_t size, void* userdata ) {
        const auto total = size;
        auto* data = reinterpret_cast<std::uint8_t*>(ptr);
        size_t read_bytes = 0;
        auto* fp = reinterpret_cast<std::FILE*>(userdata);
        do {
            const auto n = std::fread( data + read_bytes, 1, total - read_bytes, fp );
            if ( n == 0 ) {
                break;
            }
            read_bytes += n;
        } while ( read_bytes < total );

        return read_bytes;
    }

    template<typename T>
        requires PureScalar<T>
    inline size_t DeserializeFrom( std::FILE* fp, T& value ) {
        return DeserializeFrom( _serde_readfile, value, reinterpret_cast<void*>( fp ) );
    }

    // --- Serialization ---

    template<typename T, typename WriterFunc>
        requires PureScalar<T>
    inline size_t SerializeTo( WriterFunc&& write_bytes, const T value, void* userdata ) {
        return write_bytes( &value, sizeof( T ), userdata );
    }

    static size_t _serde_writefile( const void* ptr, size_t size, void* userdata ) {
        const auto total = size;
        const auto* data = reinterpret_cast<const std::uint8_t*>(ptr);
        auto* fp = reinterpret_cast<std::FILE*>(userdata);
        size_t written = 0;
        do {
            const auto n = std::fwrite( data + written, 1, total - written, fp );
            if ( n == 0 ) {
                break;
            }
            written += n;
        } while ( written < total );

        return written;
    }

    template<typename T>
        requires PureScalar<T>
    inline size_t SerializeTo( std::FILE* fp, const T value ) {
        return SerializeTo( _serde_writefile, value, reinterpret_cast<void*>( fp ) );
    }

    class ByteBufferReader {
    public:
        constexpr ByteBufferReader( const std::uint8_t* buffer, size_t total_length ) noexcept
            : m_buffer( buffer ), m_capacity( total_length ), m_offset( 0 ) {
        }

        // Reads requested bytes. Returns 0 if requested size would go out of bounds.
        static size_t ReadCallback( void* dest, size_t size, void* userdata ) noexcept {
            auto* self = reinterpret_cast<ByteBufferReader*>(userdata);

            // Explicit OOB Safety Check
            if ( self->m_offset + size > self->m_capacity ) {
                return 0;
            }

            std::memcpy( dest, self->m_buffer + self->m_offset, size );
            self->m_offset += size;
            return size;
        }

        [[nodiscard]] constexpr size_t GetOffset() const noexcept { return m_offset; }
        [[nodiscard]] constexpr size_t GetCapacity() const noexcept { return m_capacity; }
        [[nodiscard]] constexpr bool HasOverflowed() const noexcept { return m_offset > m_capacity; }

    private:
        const std::uint8_t* m_buffer;
        size_t m_capacity;
        size_t m_offset;
    };

    class ByteBufferWriter {
    public:
        constexpr ByteBufferWriter( std::uint8_t* buffer, size_t total_length ) noexcept
            : m_buffer( buffer ), m_capacity( total_length ), m_offset( 0 ) {
        }

        // Writes requested bytes. Returns 0 if requested size would go out of bounds.
        static size_t WriteCallback( const void* src, size_t size, void* userdata ) noexcept {
            auto* self = reinterpret_cast<ByteBufferWriter*>(userdata);

            // Explicit OOB Safety Check
            if ( self->m_offset + size > self->m_capacity ) {
                return 0;
            }

            std::memcpy( self->m_buffer + self->m_offset, src, size );
            self->m_offset += size;
            return size;
        }

        [[nodiscard]] constexpr size_t GetOffset() const noexcept { return m_offset; }
        [[nodiscard]] constexpr size_t GetCapacity() const noexcept { return m_capacity; }

    private:
        std::uint8_t* m_buffer;
        size_t m_capacity;
        size_t m_offset;
    };

    template<typename T>
        requires PureScalar<T>
    inline size_t DeserializeFrom( ByteBufferReader& reader, T& value ) {
        return DeserializeFrom( ByteBufferReader::ReadCallback, value, &reader );
    }

    template<typename T>
        requires PureScalar<T>
    inline size_t SerializeTo( ByteBufferWriter& writer, const T value ) {
        return SerializeTo( ByteBufferWriter::WriteCallback, value, &writer );
    }
}
