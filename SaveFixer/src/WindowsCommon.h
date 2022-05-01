#pragma once

#ifndef _WIN32
#error
#endif

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <Windows.h>

#include "Common.h"

#include <vector>

namespace save_fixer
{
    [[noreturn]] inline void throw_windows_error( std::u8string_view const description, DWORD const last_error )
    {
        std::u8string err( description );
        err.append( u8" ("s ).append( char_as_u8( std::to_string( last_error ) ) ).push_back( u8')' );
        throw SaveFixerException( std::move( err ) );
    }

    [[noreturn]] inline void throw_windows_error( std::u8string_view const description )
    {
        throw_windows_error( description, ::GetLastError() );
    }

    [[noreturn]] inline void throw_windows_error( std::u8string_view const description,
                                                  std::u8string_view const file_path, DWORD const last_error )
    {
        std::u8string err( description );
        err.append( u8" \""s )
            .append( file_path )
            .append( u8"\" ("s )
            .append( char_as_u8( std::to_string( last_error ) ) )
            .push_back( u8')' );
        throw SaveFixerException( std::move( err ) );
    }

    [[noreturn]] inline void throw_windows_error( std::u8string_view const description,
                                                  std::u8string_view const file_path )
    {
        throw_windows_error( description, file_path, ::GetLastError() );
    }

    inline std::wstring utf8_to_wide( std::u8string_view const utf8_in )
    {
        auto const convert = [ & ]( wchar_t *buffer, size_t buffer_size ) {
            size_t const size = static_cast< size_t >(
                ::MultiByteToWideChar( CP_UTF8, MB_ERR_INVALID_CHARS,
                                       reinterpret_cast< char const * >( utf8_in.data() ),
                                       static_cast< int >( utf8_in.size() ), buffer,
                                       static_cast< int >( buffer_size ) ) );
            if ( size == 0U || ( buffer_size != 0U && size != buffer_size ) )
            {
                throw_windows_error( u8"internal error: failed to convert string" );
            };
            return size;
        };

        std::vector< wchar_t > w_out;
        w_out.resize( convert( nullptr, 0U ) );
        convert( w_out.data(), w_out.size() );
        return std::wstring( w_out.data(), w_out.size() );
    }

    inline std::u8string wide_to_utf8( std::wstring_view const w_in )
    {
        auto const convert = [ & ]( char8_t *buffer, size_t buffer_size ) {
            size_t const size = static_cast< size_t >(
                ::WideCharToMultiByte( CP_UTF8, WC_ERR_INVALID_CHARS, w_in.data(),
                                       static_cast< int >( w_in.size() ), reinterpret_cast< char * >( buffer ),
                                       static_cast< int >( buffer_size ), nullptr, nullptr ) );
            if ( size == 0U || ( buffer_size != 0U && size != buffer_size ) )
            {
                throw_windows_error( u8"internal error: failed to convert string" );
            };
            return size;
        };

        std::vector< char8_t > utf8_out;
        utf8_out.resize( convert( nullptr, 0U ) );
        convert( utf8_out.data(), utf8_out.size() );
        return std::u8string( utf8_out.data(), utf8_out.size() );
    }
}
