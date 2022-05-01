#ifndef _WIN32
#error
#endif

#include "FileSystem.h"

#include "WindowsCommon.h"

using namespace save_fixer;

namespace
{
    [[noreturn]] void throw_file_error( std::u8string_view const description, std::u8string_view const file_path )
    {
        std::u8string err;
        err.append( description ).append( u8" \""s ).append( file_path ).push_back( u8'"' );
        throw SaveFixerException( std::move( err ) );
    }

    struct FileHandleTraits
    {
        using HandleType = HANDLE;
        inline static const HANDLE null_value = INVALID_HANDLE_VALUE;
        static void close( HANDLE h ) { CloseHandle( h ); }
    };
    struct MappingHandleTraits
    {
        using HandleType = HANDLE;
        inline static const HANDLE null_value = nullptr;
        static void close( HANDLE h ) { CloseHandle( h ); }
    };
    struct ViewHandleTraits
    {
        using HandleType = void *;
        inline static void *const null_value = nullptr;
        static void close( void *v ) { UnmapViewOfFile( v ); }
    };

    using UniqueFileHandle = UniqueHandle< FileHandleTraits >;
    using UniqueMappingHandle = UniqueHandle< MappingHandleTraits >;
    using UniqueViewHandle = UniqueHandle< ViewHandleTraits >;

    UniqueFileHandle create_file( std::u8string const &file_path, DWORD const access, DWORD const creation_disposition )
    {
        std::wstring const wfile_path = utf8_to_wide( file_path );

        UniqueFileHandle handle( ::CreateFileW( wfile_path.c_str(), access, 0, nullptr,
                                                creation_disposition, FILE_ATTRIBUTE_NORMAL, nullptr ) );
        if ( handle.is_valid() )
        {
            return handle;
        }
        else if ( creation_disposition == CREATE_NEW || creation_disposition == CREATE_ALWAYS )
        {
            throw_windows_error( u8"failed to create file", file_path );
        }
        else
        {
            if ( DWORD const err = ::GetLastError(); err == ERROR_FILE_NOT_FOUND )
            {
                throw_file_error( u8"could not find file", file_path );
            }
            else
            {
                throw_windows_error( u8"failed to open file", file_path, err );
            }
        }
    }

    size_t get_file_size( UniqueFileHandle const &file, std::u8string const &file_path )
    {
        LARGE_INTEGER size;
        if ( !::GetFileSizeEx( file.get(), &size ) )
        {
            throw_windows_error( u8"internal error: failed to get file size", file_path );
        }
        return size.QuadPart;
    }

    UniqueMappingHandle create_read_file_mapping( UniqueFileHandle const &file,
                                                  std::u8string const &file_path, size_t const file_size )
    {
        if ( file_size == 0 )
        {
            return UniqueMappingHandle();
        }

        UniqueMappingHandle mapping( ::CreateFileMapping( file.get(), nullptr, PAGE_READONLY, 0, 0, nullptr ) );
        if ( mapping.is_valid() )
        {
            return mapping;
        }
        else
        {
            throw_windows_error( u8"internal error: failed to map file", file_path );
        }
    }

    UniqueMappingHandle create_write_file_mapping( UniqueFileHandle const &file,
                                                   std::u8string const &file_path, size_t const size )
    {
        if ( size == 0 )
        {
            return UniqueMappingHandle();
        }

        ULARGE_INTEGER const create_size = { .QuadPart = size };
        UniqueMappingHandle mapping( ::CreateFileMapping( file.get(), nullptr, PAGE_READWRITE,
                                                          create_size.HighPart, create_size.LowPart, nullptr ) );
        if ( mapping.is_valid() )
        {
            return mapping;
        }
        else
        {
            throw_windows_error( u8"internal error: failed to map file", file_path );
        }
    }

    std::pair< UniqueViewHandle, std::span< std::byte const > >
    map_read_view_of_file( UniqueMappingHandle const &mapping, std::u8string const &file_path, size_t const size )
    {
        if ( size == 0 )
        {
            return { UniqueViewHandle(), std::span< std::byte const >() };
        }

        LPVOID view = ::MapViewOfFile( mapping.get(), FILE_MAP_READ, 0, 0, size );
        if ( view == nullptr )
        {
            throw_windows_error( u8"internal error: failed to map view of file", file_path );
        }
        else
        {
            return { UniqueViewHandle( view ), std::span( static_cast< std::byte const * >( view ), size ) };
        }
    }

    std::pair< UniqueViewHandle, std::span< std::byte > >
    map_write_view_of_file( UniqueMappingHandle const &mapping, std::u8string const &file_path, size_t const size )
    {
        if ( size == 0 )
        {
            return { UniqueViewHandle(), std::span< std::byte >() };
        }

        LPVOID view = ::MapViewOfFile( mapping.get(), FILE_MAP_WRITE, 0, 0, size );
        if ( view == nullptr )
        {
            throw_windows_error( u8"internal error: failed to map view of file", file_path );
        }
        else
        {
            return { UniqueViewHandle( view ), std::span( static_cast< std::byte * >( view ), size ) };
        }
    }
}

path_state save_fixer::query_file( std::u8string const &file_path )
{
    std::wstring const wfile_path = utf8_to_wide( file_path );
    DWORD const attrs = ::GetFileAttributesW( wfile_path.c_str() );

    if ( attrs != INVALID_FILE_ATTRIBUTES )
    {
        if ( attrs & FILE_ATTRIBUTE_DIRECTORY )
        {
            return path_state::directory;
        }
        else if ( attrs & FILE_ATTRIBUTE_READONLY )
        {
            return path_state::file_readonly;
        }
        else
        {
            return path_state::file;
        }
    }
    else if ( DWORD const err = ::GetLastError(); err == ERROR_FILE_NOT_FOUND || err == ERROR_PATH_NOT_FOUND )
    {
        return path_state::does_not_exist;
    }
    else
    {
        throw_windows_error( u8"internal error: failed to query file", file_path );
    }
}

//-----------------------------------------------------------------------------
// ReadFileMapping
//-----------------------------------------------------------------------------

class ReadFileMapping::impl
{
public:
    impl( UniqueMappingHandle m, UniqueViewHandle vh, std::span< std::byte const > v )
        : mapping( std::move( m ) ), view_handle( std::move( vh ) ), view( v )
    {
    }

    UniqueMappingHandle mapping;
    UniqueViewHandle view_handle;
    std::span< std::byte const > view;
};

ReadFileMapping::ReadFileMapping( std::u8string const &file_path )
{
    // The file handle does not need to remain open after the mapping has been created.
    UniqueFileHandle const file_handle = create_file( file_path, GENERIC_READ, OPEN_EXISTING );
    size_t const file_size = get_file_size( file_handle, file_path );

    UniqueMappingHandle file_mapping = create_read_file_mapping( file_handle, file_path, file_size );
    auto [ view_handle, view_span ] = map_read_view_of_file( file_mapping, file_path, file_size );

    pimpl = std::make_unique< impl >( std::move( file_mapping ), std::move( view_handle ), view_span );
}

ReadFileMapping::~ReadFileMapping() = default;
ReadFileMapping::ReadFileMapping( ReadFileMapping && ) noexcept = default;
ReadFileMapping &ReadFileMapping::operator=( ReadFileMapping && ) noexcept = default;

std::span< std::byte const > ReadFileMapping::bytes() const
{
    return pimpl->view;
}

//-----------------------------------------------------------------------------
// WriteFileMapping
//-----------------------------------------------------------------------------

class WriteFileMapping::impl
{
public:
    impl( std::u8string path, UniqueFileHandle f, UniqueMappingHandle m, UniqueViewHandle vh,
          std::span< std::byte > v )
        : file_path( std::move( path ) )
        , file( std::move( f ) )
        , mapping( std::move( m ) )
        , view_handle( std::move( vh ) )
        , view( v )
    {
    }

    std::u8string file_path;
    UniqueFileHandle file;
    UniqueMappingHandle mapping;
    UniqueViewHandle view_handle;
    std::span< std::byte > view;
};

WriteFileMapping::WriteFileMapping( std::u8string const &file_path, size_t const size, bool const allow_overwrite )
{
    DWORD const access = allow_overwrite ? CREATE_ALWAYS : CREATE_NEW;
    UniqueFileHandle file_handle = create_file( file_path, GENERIC_READ | GENERIC_WRITE | DELETE, access );
    UniqueMappingHandle file_mapping = create_write_file_mapping( file_handle, file_path, size );
    auto [ view_handle, view_span ] = map_write_view_of_file( file_mapping, file_path, size );

    pimpl = std::make_unique< impl >( file_path, std::move( file_handle ), std::move( file_mapping ),
                                      std::move( view_handle ), view_span );
}

WriteFileMapping::~WriteFileMapping() = default;
WriteFileMapping::WriteFileMapping( WriteFileMapping && ) noexcept = default;
WriteFileMapping &WriteFileMapping::operator=( WriteFileMapping && ) noexcept = default;

std::span< std::byte > WriteFileMapping::bytes()
{
    return pimpl->view;
}
size_t WriteFileMapping::size() const
{
    return pimpl->view.size();
}

void WriteFileMapping::write_truncate_and_rename( WriteFileMapping &&mapping, std::u8string const &new_file_path,
                                                  size_t new_size, bool allow_overwrite )
{
    UniqueFileHandle const file = std::move( mapping.pimpl->file );
    size_t const old_size = mapping.pimpl->view.size();

    mapping.pimpl->mapping.reset();
    mapping.pimpl->view_handle.reset();
    mapping.pimpl->view = std::span< std::byte >();

    if ( new_size != old_size )
    {
        LARGE_INTEGER const distance = { .QuadPart = static_cast< LONGLONG >( new_size ) };
        if ( std::cmp_not_equal( distance.QuadPart, new_size ) ||
             !::SetFilePointerEx( file.get(), distance, nullptr, FILE_BEGIN ) ||
             !::SetEndOfFile( file.get() ) )
        {
            throw_windows_error( u8"internal error: failed to truncate file", mapping.pimpl->file_path );
        }
    }

    std::wstring const wnew_file_path = utf8_to_wide( new_file_path );

    size_t const rename_info_buffer_size =
        sizeof( FILE_RENAME_INFO ) + ( sizeof( wchar_t ) * wnew_file_path.size() );
    auto const rename_info_buffer = std::make_unique< std::byte[] >( rename_info_buffer_size );
    FILE_RENAME_INFO *rename_info = reinterpret_cast< FILE_RENAME_INFO * >( rename_info_buffer.get() );

    rename_info->ReplaceIfExists = allow_overwrite ? TRUE : FALSE;
    rename_info->RootDirectory = nullptr;
    rename_info->FileNameLength = static_cast< DWORD >( wnew_file_path.size() );
    std::copy( wnew_file_path.begin(), wnew_file_path.end(), rename_info->FileName );
    rename_info->FileName[ wnew_file_path.size() ] = L'\0';

    if ( !::SetFileInformationByHandle( file.get(), FileRenameInfo, rename_info,
                                        static_cast< DWORD >( rename_info_buffer_size ) ) )
    {
        throw_windows_error( u8"failed to write file", new_file_path );
    }
}
