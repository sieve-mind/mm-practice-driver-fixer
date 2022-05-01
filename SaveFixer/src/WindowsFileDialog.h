#pragma once

#ifndef _WIN32
#error
#endif

#include "WindowsCommon.h"

#include <optional>
#include <string>

namespace save_fixer
{
    std::optional< std::u8string > win_open_mm_sav_file( HWND owner );
    std::optional< std::u8string > win_save_mm_sav_file( HWND owner, std::u8string_view suggested_file_name );
}
