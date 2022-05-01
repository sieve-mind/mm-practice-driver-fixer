#include "SaveFile.h"

#include "FileSystem.h"

#include "lz4.h"

#include <algorithm>
#include <assert.h>

using namespace save_fixer;

// The steps for reading the save file are:
//
// * Read the header, check the magic and version numbers match
// * Decompress the info bytes and the data bytes using LZ4, which gives
//   you the info JSON and data JSON
// * Look through the info bytes for "saveInfo":{ ..., "name": <SAVE_NAME>, ... } and
//   remember where the save name string is
// * Look through the data JSON for "mPlayerTeam": { ..., "$id": "<ID>", ... }, where <ID> is
//   a string identifying the player team
// * Using that ID, look for objects in the data JSON containing
//   `"contract: { ..., "mEmployeerTeam": { "ref": "<ID>" }`, ... }`. Those objects represent
//   the player team's employees
// * Get the mCarID, mFirstName, mLastName values from those objects. Only drivers will have a
//   mCarID, and there should be exactly 3 drivers in the player team.
// * Translate the mCarID into a DriverPosition, and remember where the original value is in
//   the file
//
// In order to write a new save file later:
//
// * Replace the save name in the info JSON
// * Update the mCarID values as required
// * Compress all the info and data JSON with LZ4
// * Write the header and the compressed data to the file

// TODO: deselect any drivers if save is at start of practice

namespace
{
    //-------------------------------------------------------------------------
    // LZ4 compression
    //-------------------------------------------------------------------------

    void lz4_decompress( std::span< std::byte const > compressed_data,
                         std::span< std::byte > output_buffer, std::u8string const &file_path )
    {
        int const result = LZ4_decompress_safe( reinterpret_cast< char const * >( compressed_data.data() ),
                                                reinterpret_cast< char * >( output_buffer.data() ),
                                                static_cast< int >( compressed_data.size() ),
                                                static_cast< int >( output_buffer.size() ) );
        if ( std::cmp_not_equal( result, output_buffer.size() ) )
        {
            throw SaveFixerException( file_path + u8" is invalid or corrupted" );
        }
    }

    template < typename T >
    size_t lz4_max_compressed_size( std::span< T > const input_data )
    {
        if ( std::cmp_greater( input_data.size_bytes(), std::numeric_limits< int >::max() ) )
        {
            throw SaveFixerException( u8"output too large" );
        }
        return static_cast< size_t >( LZ4_compressBound( static_cast< int >( input_data.size_bytes() ) ) );
    }

    // Returns the size of the compressed data in the output buffer
    size_t lz4_compress( std::span< std::byte const > input_data, std::span< std::byte > output_buffer )
    {
        int const result = LZ4_compress_default( reinterpret_cast< char const * >( input_data.data() ),
                                                 reinterpret_cast< char * >( output_buffer.data() ),
                                                 static_cast< int >( input_data.size() ),
                                                 static_cast< int >( output_buffer.size() ) );
        if ( result == 0 )
        {
            throw SaveFixerException( u8"internal error: compression failure" );
        }
        return static_cast< size_t >( result );
    }

    //-------------------------------------------------------------------------
    // Reading the save file
    //-------------------------------------------------------------------------

    constexpr int mm_save_file_magic = 1932684653;
    constexpr int mm_save_file_supported_version = 4;
    constexpr size_t max_decompressed_buffer_size = 4ULL * 1024ULL * 1024ULL * 1024ULL;

    struct SaveFileHeader
    {
        int magic;
        int version;
        int compressed_info_size;
        int decompressed_info_size;
        int compressed_data_size;
        int decompressed_data_size;
    };

    SaveFileHeader const *read_save_file_header( std::span< std::byte const > file_data,
                                                 std::u8string const &file_path )
    {
        if ( file_data.size() < sizeof( SaveFileHeader ) )
        {
            throw SaveFixerException( file_path + u8" is not a valid Motorsport Manager save file" );
        }

        SaveFileHeader const *header = reinterpret_cast< SaveFileHeader const * >( file_data.data() );

        if ( header->magic != mm_save_file_magic || header->compressed_info_size <= 0 ||
             header->decompressed_info_size <= 0 || header->compressed_data_size <= 0 ||
             header->decompressed_data_size <= 0 )
        {
            throw SaveFixerException( file_path + u8" is not a valid Motorsport Manager save file" );
        }

        if ( header->version != mm_save_file_supported_version )
        {
            std::u8string err( file_path );
            err.append( u8" save file version ("s )
                .append( char_as_u8( std::to_string( header->version ) ) )
                .append( u8") is unsupported"s );
            throw SaveFixerException( err );
        }

        size_t const total_decompressed_size = static_cast< size_t >( header->decompressed_info_size ) +
                                               static_cast< size_t >( header->decompressed_data_size );
        if ( total_decompressed_size > max_decompressed_buffer_size )
        {
            throw SaveFixerException( file_path + u8" save file is too large"s );
        }

        return header;
    }

    template < typename T >
    std::pair< std::span< T >, std::span< T > > split_span( std::span< T > s, size_t n )
    {
        std::span< T > first = s.first( n );
        std::span< T > second = s.subspan( n );
        return { first, second };
    }

    //-------------------------------------------------------------------------
    // Navigating the JSON data
    //-------------------------------------------------------------------------

    // Note that for the for the sake of performance this code does not fully parse the JSON,
    // instead it relies on string searches and just enough parsing to find a key within an
    // object. It also tends to assume the JSON is valid and does not contain any optional whitespace.

    [[noreturn]] void throw_for_invalid_json()
    {
        throw SaveFixerException( u8"invalid save file"s );
    }

    std::u8string_view string_view_between( std::u8string_view const s, size_t const a, size_t const b )
    {
        return s.substr( a + 1, ( b - a ) - 1 );
    }

    size_t count_preceeding_backslashes( std::u8string_view const json_data, size_t const offset )
    {
        if ( offset == 0 )
        {
            return 0;
        }

        for ( size_t i = offset - 1;; --i )
        {
            if ( json_data[ i ] != u8'\\' )
            {
                return ( offset - i ) - 1;
            }
            if ( i == 0 )
            {
                return offset - i;
            }
        }
    }

    // Returns the offset of the matching quote, or throws
    size_t find_closing_quote( std::u8string_view const json_data, size_t const offset_of_opening_quote )
    {
        bool escaped = false;
        for ( size_t i = offset_of_opening_quote + 1; i < json_data.size(); ++i )
        {
            switch ( json_data[ i ] )
            {
                case u8'\\':
                    escaped = !escaped;
                    break;
                case u8'"':
                    if ( !escaped )
                    {
                        return i;
                    }
                    [[fallthrough]];
                default:
                    escaped = false;
                    break;
            }
        }
        throw_for_invalid_json();
    }

    // Returns the offset of the matching quote, or throws
    size_t rfind_opening_quote( std::u8string_view const json_data, size_t const offset_of_closing_quote )
    {
        if ( offset_of_closing_quote != 0 )
        {
            for ( size_t i = offset_of_closing_quote - 1;; --i )
            {
                if ( json_data[ i ] == u8'"' )
                {
                    if ( size_t const escape_count = count_preceeding_backslashes( json_data, i );
                         escape_count == 0 )
                    {
                        return i;
                    }
                    else if ( escape_count % 2 == 1 )
                    {
                        i -= escape_count;
                    }
                    else
                    {
                        throw_for_invalid_json();
                    }
                }
                if ( i == 0 )
                {
                    break;
                }
            }
        }
        throw_for_invalid_json();
    }

    // Returns the offset of the closing brace, or throws
    // starting_offset must be after the opening brace, before or on the target closing brace,
    //     not within a string, not at the closing quote of a string, and not within a sub-object or array
    // brace must be '}' or ']'
    size_t find_closing_brace( std::u8string_view const json_data, size_t const starting_offset, char8_t const brace )
    {
        size_t closing_brace_stack_after_push = 0;
        size_t closing_brace_stack_after_pop = 0;

        std::vector< char8_t > closing_brace_stack{ brace };
        for ( size_t i = starting_offset; i < json_data.size(); ++i )
        {
            switch ( json_data[ i ] )
            {
                case u8'"':
                    i = find_closing_quote( json_data, i );
                    break;
                case u8'{':
                case u8'[':
                    closing_brace_stack.push_back( json_data[ i ] == u8'{' ? u8'}' : u8']' );
                    closing_brace_stack_after_push = closing_brace_stack.size();
                    break;
                case u8'}':
                case u8']':
                    if ( json_data[ i ] == closing_brace_stack.back() )
                    {
                        closing_brace_stack.pop_back();
                        closing_brace_stack_after_pop = closing_brace_stack.size();
                        if ( closing_brace_stack.empty() )
                        {
                            return i;
                        }
                    }
                    else
                    {
                        throw_for_invalid_json();
                    }
                    break;
                default:
                    break;    // Skip other characters
            }
        }
        throw_for_invalid_json();
    }

    // Returns the offset of the opening brace, or throws
    // starting_offset must be before the closing brace, after or on the target opening brace,
    //     not within a string, not at the opening quote of a string, and not within a sub-object or array
    // brace must be '{' or '['
    size_t rfind_opening_brace( std::u8string_view const json_data, size_t const starting_offset, char8_t const brace )
    {
        std::vector< char8_t > opening_brace_stack{ brace };
        for ( size_t i = starting_offset;; --i )
        {
            switch ( json_data[ i ] )
            {
                case u8'"':
                    i = rfind_opening_quote( json_data, i );
                    break;
                case u8'}':
                case u8']':
                    opening_brace_stack.push_back( json_data[ i ] == u8'}' ? u8'{' : u8'[' );
                    break;
                case u8'{':
                case u8'[':
                    if ( json_data[ i ] == opening_brace_stack.back() )
                    {
                        opening_brace_stack.pop_back();
                        if ( opening_brace_stack.empty() )
                        {
                            return i;
                        }
                    }
                    else
                    {
                        throw_for_invalid_json();
                    }
                    break;
                default:
                    break;    // Skip other characters
            }
            if ( i == 0 )
            {
                throw_for_invalid_json();
            }
        }
    }

    // Returns the offset of the matching brace, or throws
    size_t find_matching_brace( std::u8string_view const json_data, size_t const offset_of_brace )
    {
        switch ( json_data[ offset_of_brace ] )
        {
            case u8'{':
            case u8'[':
                if ( offset_of_brace + 1 < json_data.size() )
                {
                    return find_closing_brace( json_data, offset_of_brace + 1,
                                               json_data[ offset_of_brace ] == u8'{' ? u8'}' : u8']' );
                }
            case u8'}':
            case u8']':
                if ( offset_of_brace != 0 )
                {
                    return rfind_opening_brace( json_data, offset_of_brace - 1,
                                                json_data[ offset_of_brace ] == u8'}' ? u8'{' : u8'[' );
                }
                break;
            default:
                break;
        }
        throw_for_invalid_json();
    }

    // clang-format off
    template < typename F >
    concept KeyValueCallback = requires( F f, std::u8string_view json, std::u8string_view key, size_t value_offset )
    {
        { f( json, key, value_offset ) } -> std::same_as< bool >;
    };
    // clang-format on

    // Calls the callback for every key value pair in the object. Breaks if callback returns false.
    // start_offset must be the opening quote of one of the keys in the object.
    template < KeyValueCallback F >
    void for_sibling_key_values_in_object( std::u8string_view const json_data, size_t const start_offset, F callback )
    {
        // Start with the keys on and after start_offset
        for ( size_t i = start_offset; i < json_data.size() && json_data[ i ] == u8'"'; )
        {
            // Key
            size_t const key_start_quote_pos = i;
            size_t const key_end_quote_pos = find_closing_quote( json_data, key_start_quote_pos );
            std::u8string_view const key =
                string_view_between( json_data, key_start_quote_pos, key_end_quote_pos );

            // Value start
            if ( key_end_quote_pos + 2 >= json_data.size() || json_data[ key_end_quote_pos + 1 ] != u8':' )
            {
                throw_for_invalid_json();
            }
            size_t const value_start_pos = key_end_quote_pos + 2;

            if ( !callback( json_data, key, value_start_pos ) )
            {
                return;
            }

            // Skip the value
            size_t const value_end_pos = [ & ]() {
                switch ( json_data[ value_start_pos ] )
                {
                    case u8'"':
                        return find_closing_quote( json_data, value_start_pos );
                    case u8'{':
                    case u8'[':
                        return find_matching_brace( json_data, value_start_pos );
                    default:
                        for ( size_t j = value_start_pos + 1; j < json_data.size(); ++j )
                        {
                            if ( json_data[ j ] == u8',' || json_data[ j ] == u8'}' )
                            {
                                return j - 1;
                            }
                        }
                }
                throw_for_invalid_json();
            }();

            // Skip any comma
            i = value_end_pos + 1;
            if ( i < json_data.size() && json_data[ i ] == u8',' )
            {
                ++i;
            }
        }

        // Then look backwards for keys before start_offset
        for ( size_t i = start_offset - 1; i != 0 && json_data[ i ] != u8'{'; )
        {
            if ( json_data[ i ] != u8',' )
            {
                throw_for_invalid_json();
            }

            // Value
            size_t const value_start_pos = [ & ]() {
                size_t const value_end_pos = i - 1;
                switch ( json_data[ value_end_pos ] )
                {
                    case u8'"':
                        return rfind_opening_quote( json_data, value_end_pos );
                    case u8'}':
                    case u8']':
                        return find_matching_brace( json_data, value_end_pos );
                    default:
                        for ( size_t j = value_end_pos; j != 0; --j )
                        {
                            if ( json_data[ j - 1 ] == u8':' )
                            {
                                return j;
                            }
                        }
                }
                throw_for_invalid_json();
            }();

            // Key
            if ( value_start_pos < 3 || json_data[ value_start_pos - 1 ] != u8':' ||
                 json_data[ value_start_pos - 2 ] != u8'"' )
            {
                throw_for_invalid_json();
            }
            size_t const key_end_quote_pos = value_start_pos - 2;
            size_t const key_start_quote_pos = rfind_opening_quote( json_data, key_end_quote_pos );
            if ( key_start_quote_pos == 0 )
            {
                throw_for_invalid_json();
            }

            std::u8string_view const key =
                string_view_between( json_data, key_start_quote_pos, key_end_quote_pos );
            if ( !callback( json_data, key, value_start_pos ) )
            {
                return;
            }

            i = key_start_quote_pos - 1;
        }
    }

    // Calls the callback for every key value pair in the object. Breaks if callback returns false.
    template < KeyValueCallback F >
    void for_key_values_in_object( std::u8string_view const json_data,
                                   size_t const object_opening_brace_offset, F callback )
    {
        size_t const start_pos = object_opening_brace_offset + 1;
        if ( start_pos < json_data.size() )
        {
            switch ( json_data[ start_pos ] )
            {
                case u8'"':
                    for_sibling_key_values_in_object( json_data, start_pos, callback );
                    return;
                case u8'}':
                    return;
                default:
                    break;
            }
        }
        throw_for_invalid_json();
    }

    // Return the start offset of the value with the given key, else nullopt
    std::optional< size_t > lookup_value_in_object( std::u8string_view const json_data,
                                                    size_t const object_opening_brace_offset,
                                                    std::u8string_view const sought_key )
    {
        std::optional< size_t > value_pos;
        for_key_values_in_object( json_data, object_opening_brace_offset,
                                  [ & ]( std::u8string_view, std::u8string_view key, size_t value_offset ) {
                                      if ( key == sought_key )
                                      {
                                          value_pos = value_offset;
                                          return false;
                                      }
                                      return true;
                                  } );
        return value_pos;
    }

    std::u8string_view get_player_team_id( std::u8string_view const json_data )
    {
        // Look for:
        //   "mPlayerTeam":{...,"$id":"<ID>",...}

        std::u8string_view const player_team_obj_start = u8"\"mPlayerTeam\":{";

        if ( size_t const player_team_key_start = json_data.find( player_team_obj_start );
             player_team_key_start != std::u8string_view::npos )
        {
            size_t const play_team_obj_opening_brace_pos =
                player_team_key_start + player_team_obj_start.size() - 1;

            std::optional< size_t > const id_value_pos =
                lookup_value_in_object( json_data, play_team_obj_opening_brace_pos, u8"$id" );
            if ( id_value_pos.has_value() && json_data[ id_value_pos.value() ] == u8'"' )
            {
                size_t const value_closing_quote = find_closing_quote( json_data, id_value_pos.value() );
                return string_view_between( json_data, id_value_pos.value(), value_closing_quote );
            }
        }
        throw SaveFixerException( u8"could not find player team data in save file"s );
    }

    template < typename F >
    void for_each_employeer_team_ref( std::u8string_view const json_data, std::u8string_view const team_id, F f )
    {
        // Look for:
        //   "mEmployeerTeam":{"ref":"<team_id>"}

        std::u8string const employeer_team_ref_str =
            u8"\"mEmployeerTeam\":{\"$ref\":\""s + std::u8string( team_id ) + u8"\"}"s;
        for ( size_t start_pos = 0;; )
        {
            if ( size_t const ref_pos = json_data.find( employeer_team_ref_str, start_pos );
                 ref_pos == std::u8string_view::npos )
            {
                break;
            }
            else
            {
                f( ref_pos );
                start_pos = ref_pos + 1;
            }
        }
    }

    // Return the offset of the start of the "contract" key if the employeer team ref was inside an
    // object with that key, else return npos
    size_t find_employeer_team_ref_contract_offset( std::u8string_view const json_data, size_t const employeer_team_ref_offset )
    {
        // Look for the opening brace of the object containing the employeer team ref, and
        // check if it is preceded by a "contract" key.

        if ( employeer_team_ref_offset != 0 )
        {
            std::u8string_view contract_key = u8"\"contract\":";
            size_t const object_start_pos =
                rfind_opening_brace( json_data, employeer_team_ref_offset - 1, u8'{' );
            if ( object_start_pos > contract_key.size() &&
                 json_data.substr( object_start_pos - contract_key.size(), contract_key.size() ) == contract_key )
            {
                return object_start_pos - contract_key.size();
            }
        }
        return std::u8string_view::npos;
    }

    SaveFile::DriverPosition parse_driver_position( std::u8string_view const json_data, size_t value_offset )
    {
        if ( value_offset + 2 < json_data.size() )
        {
            if ( json_data[ value_offset ] == u8'-' && json_data[ value_offset + 1 ] == u8'1' &&
                 ( json_data[ value_offset + 2 ] == u8',' || json_data[ value_offset + 2 ] == u8'}' ) )
            {
                return SaveFile::DriverPosition::reserve;
            }
            else if ( json_data[ value_offset ] == u8'0' &&
                      ( json_data[ value_offset + 1 ] == u8',' || json_data[ value_offset + 1 ] == u8'}' ) )
            {
                return SaveFile::DriverPosition::car1;
            }
            else if ( json_data[ value_offset ] == u8'1' &&
                      ( json_data[ value_offset + 1 ] == u8',' || json_data[ value_offset + 1 ] == u8'}' ) )
            {
                return SaveFile::DriverPosition::car2;
            }
            throw SaveFixerException( u8"invalid driver position in save file"s );
        }
        throw_for_invalid_json();
    }

    std::u8string_view parse_driver_name_string( std::u8string_view const json_data, size_t value_offset )
    {
        if ( json_data[ value_offset ] == u8'"' )
        {
            return string_view_between( json_data, value_offset, find_closing_quote( json_data, value_offset ) );
        }
        throw SaveFixerException( u8"invalid driver name in save file"s );
    }

    std::optional< SaveFile::Driver > maybe_get_driver( std::u8string_view const json_data, size_t const contract_key_offset )
    {
        // Look for in the object containing the contract:
        //     "mCarID":<-1|0|1>
        //     "mFirstName":<string>
        //     "mLastName":<string>

        std::optional< size_t > car_id_pos;
        std::optional< std::u8string_view > first_name;
        std::optional< std::u8string_view > last_name;

        for_sibling_key_values_in_object( json_data, contract_key_offset,
                                          [ & ]( std::u8string_view json, std::u8string_view key, size_t value_offset ) {
                                              if ( key == u8"mCarID"sv )
                                              {
                                                  car_id_pos = value_offset;
                                              }
                                              else if ( key == u8"mFirstName"sv )
                                              {
                                                  first_name = parse_driver_name_string( json, value_offset );
                                              }
                                              else if ( key == u8"mLastName"sv )
                                              {
                                                  last_name = parse_driver_name_string( json, value_offset );
                                              }
                                              return !( car_id_pos.has_value() && first_name.has_value() &&
                                                        last_name.has_value() );
                                          } );

        if ( car_id_pos.has_value() && first_name.has_value() && last_name.has_value() )
        {
            std::u8string name;
            name.append( first_name.value() ).append( u8" "s ).append( last_name.value() );
            return SaveFile::Driver( std::move( name ),
                                     parse_driver_position( json_data, car_id_pos.value() ),
                                     car_id_pos.value() );
        }
        return std::nullopt;
    }

    //-------------------------------------------------------------------------
    // Writing the save file
    //-------------------------------------------------------------------------

    std::u8string_view get_position_as_json_value( SaveFile::DriverPosition const p )
    {
        switch ( p )
        {
            case SaveFile::DriverPosition::reserve:
                return u8"-1";
            case SaveFile::DriverPosition::car1:
                return u8"0";
            case SaveFile::DriverPosition::car2:
                return u8"1";
            default:
                throw SaveFixerException( u8"internal error: unreachable"s );
        }
    }

    // Unfortunatly there is no way to compress one LZ4 block from multiple sources, so
    // we have to allocate and copy the complete output data.
    struct UncompressedOutput
    {
        std::unique_ptr< char8_t[] > buffer;
        std::span< char8_t > info;
        std::span< char8_t > data;
    };

    UncompressedOutput create_uncompressed_output_buffer( std::u8string_view const original_save_info,
                                                          std::u8string_view const original_save_data,
                                                          size_t const original_save_name_size,
                                                          std::u8string_view const new_save_name,
                                                          std::array< SaveFile::Driver, 3 > const &drivers )
    {
        // Figure out sizes
        size_t const info_out_size =
            original_save_info.size() - original_save_name_size + new_save_name.size();
        size_t const data_out_size = [ & ]() {
            size_t size = original_save_data.size();
            for ( SaveFile::Driver const &d : drivers )
            {
                size -= get_position_as_json_value( d.original_position ).size();
                size += get_position_as_json_value( d.position ).size();
            }
            return size;
        }();
        size_t const total_out_size = info_out_size + data_out_size;

        // Create buffer
        auto out_buffer = std::make_unique_for_overwrite< char8_t[] >( total_out_size );
        auto const [ info_buffer, data_buffer ] =
            split_span( std::span( out_buffer.get(), total_out_size ), info_out_size );

        return UncompressedOutput{ std::move( out_buffer ), info_buffer, data_buffer };
    }

    UncompressedOutput create_uncompressed_output( std::u8string_view const original_save_info,
                                                   std::u8string_view const original_save_data,
                                                   size_t const original_save_name_offset,
                                                   size_t const original_save_name_size,
                                                   std::u8string_view const new_save_name,
                                                   std::array< SaveFile::Driver, 3 > const &drivers )
    {
        UncompressedOutput output =
            create_uncompressed_output_buffer( original_save_info, original_save_data,
                                               original_save_name_size, new_save_name, drivers );
        std::span< char8_t > remaining_info_buffer = output.info;
        std::span< char8_t > remaining_data_buffer = output.data;

        auto const copy_to_buffer = []( std::u8string_view s, std::span< char8_t > &b ) {
            if ( s.size() > b.size() )
            {
                throw SaveFixerException( u8"internal error: copy out of memory"s );
            }
            std::copy( s.begin(), s.end(), b.begin() );
            b = b.subspan( s.size() );
        };
        auto const copy_to_info_buffer = [ & ]( std::u8string_view s ) {
            copy_to_buffer( s, remaining_info_buffer );
        };
        auto const copy_to_data_buffer = [ & ]( std::u8string_view s ) {
            copy_to_buffer( s, remaining_data_buffer );
        };

        // Copy info with new save name
        copy_to_info_buffer( original_save_info.substr( 0, original_save_name_offset ) );
        copy_to_info_buffer( new_save_name );
        copy_to_info_buffer( original_save_info.substr( original_save_name_offset + original_save_name_size ) );

        // Copy data with new driver positions
        size_t not_copied_offset = 0;
        assert( std::is_sorted( drivers.begin(), drivers.end(),
                                []( SaveFile::Driver const &a, SaveFile::Driver const &b ) {
                                    return a.car_id_file_offset < b.car_id_file_offset;
                                } ) );
        for ( SaveFile::Driver const &d : drivers )
        {
            if ( d.position != d.original_position )
            {
                copy_to_data_buffer( original_save_data.substr( not_copied_offset,
                                                                d.car_id_file_offset - not_copied_offset ) );
                copy_to_data_buffer( get_position_as_json_value( d.position ) );
                not_copied_offset =
                    d.car_id_file_offset + get_position_as_json_value( d.original_position ).size();
            }
        }
        copy_to_data_buffer( original_save_data.substr( not_copied_offset ) );

        if ( !remaining_info_buffer.empty() || !remaining_data_buffer.empty() )
        {
            throw SaveFixerException( u8"internal error: bad copy"s );
        }

        return output;
    }
}

//-----------------------------------------------------------------------------
// SaveFile
//-----------------------------------------------------------------------------

SaveFile::SaveFile( std::u8string_view const &file_path ) : original_file_path( file_path )
{
    open_and_decompress_save( original_file_path );
    get_save_name();
    get_driver_data_from_json();
}

void SaveFile::open_and_decompress_save( std::u8string const &file_path )
{
    ReadFileMapping const save_file( file_path );
    std::span< std::byte const > remaining_file_data = save_file.bytes();

    SaveFileHeader const *header = read_save_file_header( remaining_file_data, file_path );
    remaining_file_data =
        remaining_file_data.subspan( sizeof( SaveFileHeader ),
                                     static_cast< size_t >( header->compressed_info_size ) +
                                         static_cast< size_t >( header->compressed_data_size ) );

    auto const [ compressed_save_info, compressed_save_data ] =
        split_span( remaining_file_data, static_cast< size_t >( header->compressed_info_size ) );

    size_t const total_decompressed_size = static_cast< size_t >( header->decompressed_info_size ) +
                                           static_cast< size_t >( header->decompressed_data_size );
    decompressed_buffer = std::make_unique_for_overwrite< std::byte[] >( total_decompressed_size );
    auto const [ save_info_buffer, save_data_buffer ] =
        split_span( std::span( decompressed_buffer.get(), total_decompressed_size ),
                    static_cast< size_t >( header->decompressed_info_size ) );

    lz4_decompress( compressed_save_info, save_info_buffer, file_path );
    lz4_decompress( compressed_save_data, save_data_buffer, file_path );

    save_info = std::u8string_view( reinterpret_cast< char8_t const * >( save_info_buffer.data() ),
                                    save_info_buffer.size() );
    save_data = std::u8string_view( reinterpret_cast< char8_t const * >( save_data_buffer.data() ),
                                    save_data_buffer.size() );
}

void SaveFile::get_save_name()
{
    std::u8string_view const save_info_obj_start = u8"\"saveInfo\":{";

    if ( size_t const save_info_key_start = save_info.find( save_info_obj_start );
         save_info_key_start != std::u8string_view::npos )
    {
        size_t const save_info_obj_opening_brace_pos = save_info_key_start + save_info_obj_start.size() - 1;

        std::optional< size_t > const name_value_pos =
            lookup_value_in_object( save_info, save_info_obj_opening_brace_pos, u8"name" );
        if ( name_value_pos.has_value() && save_info[ name_value_pos.value() ] == u8'"' )
        {
            size_t const name_closing_quote = find_closing_quote( save_info, name_value_pos.value() );
            save_name_offset = name_value_pos.value() + 1;
            save_name_size = name_closing_quote - save_name_offset;
            return;
        }
    }
    throw SaveFixerException( u8"could not find save name in save file"s );
}

void SaveFile::get_driver_data_from_json()
{
    std::u8string_view const player_team_id = get_player_team_id( save_data );

    std::vector< Driver > found_drivers;
    for_each_employeer_team_ref( save_data, player_team_id, [ & ]( size_t const employeer_team_ref_offset ) {
        size_t const contract_key_offset =
            find_employeer_team_ref_contract_offset( save_data, employeer_team_ref_offset );
        if ( contract_key_offset != std::u8string_view::npos )
        {
            if ( std::optional< Driver > d = maybe_get_driver( save_data, contract_key_offset ); d.has_value() )
            {
                found_drivers.emplace_back( std::move( d.value() ) );
            }
        }
    } );

    if ( found_drivers.size() != 3U )
    {
        throw SaveFixerException( u8"unable to locate team's 3 drivers in save file"s );
    }

    std::sort( found_drivers.begin(), found_drivers.end(), []( Driver const &a, Driver const &b ) {
        return a.car_id_file_offset < b.car_id_file_offset;
    } );
    std::copy( found_drivers.begin(), found_drivers.end(), drivers.begin() );
}

std::array< SaveFile::DriverRef, 3 > SaveFile::get_drivers()
{
    return { drivers[ 0 ].ref(), drivers[ 1 ].ref(), drivers[ 2 ].ref() };
}

void SaveFile::write( std::u8string const &file_path, std::u8string const &new_save_name, bool allow_overwrite ) const
{
    UncompressedOutput output = create_uncompressed_output( save_info, save_data, save_name_offset,
                                                            save_name_size, new_save_name, drivers );

    constexpr bool overwrite_temp_file = true;
    size_t const max_output_size = sizeof( SaveFileHeader ) + lz4_max_compressed_size( output.info ) +
                                   lz4_max_compressed_size( output.data );
    WriteFileMapping file_out( file_path + u8".mmsftmp"s, max_output_size, overwrite_temp_file );

    SaveFileHeader *save_header = reinterpret_cast< SaveFileHeader * >( file_out.data() );
    std::span< std::byte > file_out_remaining = file_out.bytes().subspan( sizeof( SaveFileHeader ) );

    size_t const compressed_info_size =
        lz4_compress( std::as_writable_bytes( output.info ), file_out_remaining );
    file_out_remaining = file_out_remaining.subspan( compressed_info_size );

    size_t const compressed_data_size =
        lz4_compress( std::as_writable_bytes( output.data ), file_out_remaining );
    file_out_remaining = file_out_remaining.subspan( compressed_data_size );

    if ( std::cmp_greater( compressed_info_size, std::numeric_limits< int >::max() ) ||
         std::cmp_greater( output.info.size(), std::numeric_limits< int >::max() ) ||
         std::cmp_greater( compressed_data_size, std::numeric_limits< int >::max() ) ||
         std::cmp_greater( output.data.size(), std::numeric_limits< int >::max() ) )
    {
        throw SaveFixerException( u8"output too large"s );
    }

    save_header->magic = mm_save_file_magic;
    save_header->version = mm_save_file_supported_version;
    save_header->compressed_info_size = static_cast< int >( compressed_info_size );
    save_header->decompressed_info_size = static_cast< int >( output.info.size() );
    save_header->compressed_data_size = static_cast< int >( compressed_data_size );
    save_header->decompressed_data_size = static_cast< int >( output.data.size() );

    WriteFileMapping::write_truncate_and_rename( std::move( file_out ), file_path,
                                                 max_output_size - file_out_remaining.size(), allow_overwrite );
}
