#pragma once

#include <exception>
#include <string>
#include <utility>

using namespace std::string_literals;
using namespace std::string_view_literals;

namespace save_fixer
{
    class SaveFixerException
    {
    public:
        SaveFixerException( std::u8string desc ) noexcept : description( std::move( desc ) ) {}
        std::u8string const description;
    };

    inline char const *u8_as_char( char8_t const *s )
    {
        return reinterpret_cast< char const * >( s );
    }
    inline char *u8_as_char( char8_t *s ) { return reinterpret_cast< char * >( s ); }
    inline std::string_view u8_as_char( std::u8string_view const s )
    {
        return std::string_view( u8_as_char( s.data() ), s.size() );
    }

    inline char8_t const *char_as_u8( char const *s )
    {
        return reinterpret_cast< char8_t const * >( s );
    }
    inline char8_t *char_as_u8( char *s ) { return reinterpret_cast< char8_t * >( s ); }
    inline std::u8string_view char_as_u8( std::string_view const s )
    {
        return std::u8string_view( char_as_u8( s.data() ), s.size() );
    }

    // clang-format off
    template < typename T >
    concept HandleTraits = requires( typename T::HandleType h )
    {
        { T::HandleType ( T::null_value ) };
        { h = T::null_value };
        { T::close( h ) };
    };
    // clang-format on

    template < HandleTraits HT >
    class UniqueHandle
    {
    public:
        using Handle = HT::HandleType;

        UniqueHandle() : handle( HT::null_value ) {}
        explicit UniqueHandle( Handle h ) noexcept : handle( h ) {}
        UniqueHandle( UniqueHandle &&other ) noexcept : UniqueHandle() { swap( other ); }

        UniqueHandle const &operator=( UniqueHandle &&other ) noexcept
        {
            if ( this != &other )
            {
                reset();
                swap( other );
            }
            return *this;
        }

        UniqueHandle const &operator=( Handle h ) noexcept
        {
            reset();
            handle = h;
            return *this;
        }

        ~UniqueHandle() { reset(); }

        UniqueHandle( UniqueHandle const & ) = delete;
        UniqueHandle const &operator=( UniqueHandle const & ) = delete;

        void reset() noexcept
        {
            if ( is_valid() )
            {
                HT::close( handle );
                handle = HT::null_value;
            }
        }

        bool is_valid() const { return handle != HT::null_value; }

        Handle get() const { return handle; }

        friend void swap( UniqueHandle &a, UniqueHandle &b ) noexcept { a.swap( b ); }

    private:
        void swap( UniqueHandle &other ) noexcept { std::swap( handle, other.handle ); }

        Handle handle;
    };
}
