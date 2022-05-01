#pragma once

#include "Common.h"

#include <array>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace save_fixer
{
    // Class that handles the reading of a Motorsport Manager save file to find the player
    // team's drivers and their car IDs (DriverPosition). The practice driver bug occurs
    // when these car IDs are incorrect. This class also allows the positions to be updated
    // and then a new save file to be written with those updates.
    class SaveFile
    {
    public:
        SaveFile( std::u8string_view const &file_path );

        enum class DriverPosition
        {
            reserve,
            car1,
            car2,
        };

        struct DriverRef
        {
            std::u8string_view const name;
            DriverPosition &position;
        };

        struct Driver
        {
            Driver() = default;
            Driver( std::u8string driver_name, DriverPosition const pos, size_t const offset )
                : name( std::move( driver_name ) )
                , position( pos )
                , original_position( pos )
                , car_id_file_offset( offset )
            {
            }

            DriverRef ref() { return DriverRef{ name, position }; }

            std::u8string name;
            DriverPosition position;
            DriverPosition original_position;
            size_t car_id_file_offset;
        };

        std::u8string const &get_original_file_path() const { return original_file_path; }
        std::array< DriverRef, 3 > get_drivers();

        void write( std::u8string const &file_path, std::u8string const &save_name,
                    bool allow_overwrite = false ) const;

    private:
        void open_and_decompress_save( std::u8string const &file_path );
        void get_save_name();
        void get_driver_data_from_json();

        std::u8string const original_file_path;

        std::unique_ptr< std::byte[] > decompressed_buffer;
        std::u8string_view save_info;
        std::u8string_view save_data;

        size_t save_name_offset;
        size_t save_name_size;

        std::array< Driver, 3 > drivers;
    };
}
