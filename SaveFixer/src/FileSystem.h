#pragma once

#include "Common.h"

#include <span>
#include <memory>

namespace save_fixer
{
    enum class path_state
    {
        does_not_exist,
        file,
        file_readonly,
        directory,
    };
    path_state query_file( std::u8string const &file_path );

    class ReadFileMapping
    {
    public:
        // Throws SaveFixerException on error
        ReadFileMapping( std::u8string const &file_path );

        ~ReadFileMapping();

        ReadFileMapping( ReadFileMapping && ) noexcept;
        ReadFileMapping &operator=( ReadFileMapping && ) noexcept;

        std::span< std::byte const > bytes() const;
        std::byte const *data() const { return bytes().data(); }
        size_t size() const { return bytes().size(); }

    private:
        class impl;
        std::unique_ptr< impl > pimpl;
    };

    class WriteFileMapping
    {
    public:
        // Throws SaveFixerException on error
        WriteFileMapping( std::u8string const &file_path, size_t size, bool allow_overwrite = false );

        ~WriteFileMapping();

        WriteFileMapping( WriteFileMapping && ) noexcept;
        WriteFileMapping &operator=( WriteFileMapping && ) noexcept;

        std::span< std::byte > bytes();
        std::byte *data() { return bytes().data(); }
        size_t size() const;

        // Used to add a file as 'atomically' as possible
        static void write_truncate_and_rename( WriteFileMapping &&mapping, std::u8string const &new_file_path,
                                               size_t new_size, bool allow_overwrite = false );

    private:
        class impl;
        std::unique_ptr< impl > pimpl;
    };
}
