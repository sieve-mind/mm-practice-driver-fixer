#ifndef _WIN32
#error
#endif

#include "WindowsFileDialog.h"

#include <objbase.h>
#include <shobjidl_core.h>
#include <Shlobj.h>

#pragma comment( lib, "Ole32.lib" )

using namespace save_fixer;

namespace
{
    struct COMObjectHandleTraits
    {
        using HandleType = IUnknown *;
        inline static IUnknown *null_value = nullptr;
        static void close( IUnknown *u ) { u->Release(); }
    };
    using UniqueCOMObjectHandle = UniqueHandle< COMObjectHandleTraits >;

    void check_hresult( HRESULT hr )
    {
        if ( !SUCCEEDED( hr ) )
        {
            std::string const error_num_str = std::to_string( hr );
            std::u8string err = u8"internal error: file dialogue ("s;
            err.append( reinterpret_cast< char8_t const * >( error_num_str.c_str() ), error_num_str.size() )
                .push_back( u8')' );
            throw SaveFixerException( std::move( err ) );
        }
    }

    std::wstring get_mm_save_directory()
    {
        std::wstring result;
        {
            PWSTR base_dir = nullptr;
            HRESULT const hr =
                ::SHGetKnownFolderPath( FOLDERID_LocalAppDataLow, KF_FLAG_DEFAULT, nullptr, &base_dir );
            if ( SUCCEEDED( hr ) )
            {
                result = base_dir;
            }
            CoTaskMemFree( base_dir );
            check_hresult( hr );
        }
        result.append( L"\\Playsport Games\\Motorsport Manager\\Cloud\\Saves" );
        return result;
    }

    std::optional< std::u8string > show_mm_save_file_dialog( HWND owner, IFileDialog *dialog )
    {
        // Options
        {
            FILEOPENDIALOGOPTIONS options;
            check_hresult( dialog->GetOptions( &options ) );
            options |= FOS_STRICTFILETYPES | FOS_NOCHANGEDIR | FOS_FORCEFILESYSTEM |
                       FOS_HIDEMRUPLACES | FOS_HIDEPINNEDPLACES | FOS_DONTADDTORECENT;
            options &= ~FOS_OVERWRITEPROMPT;    // Overwrite check is done by the application
            check_hresult( dialog->SetOptions( options ) );
        }

        // Folder
        {
            std::wstring save_dir_path = get_mm_save_directory();
            IShellItem *shell_item = nullptr;
            check_hresult( SHCreateItemFromParsingName( save_dir_path.c_str(), nullptr,
                                                        IID_PPV_ARGS( &shell_item ) ) );
            UniqueCOMObjectHandle shell_item_raii_wrapper( shell_item );
            check_hresult( dialog->SetFolder( shell_item ) );
        }

        // File type
        {
            const COMDLG_FILTERSPEC spec = { L"Motorsport Manager Save (*.sav)", L"*.sav" };
            check_hresult( dialog->SetFileTypes( 1U, &spec ) );
            check_hresult( dialog->SetFileTypeIndex( 1U ) );    // 1 based index
        }

        // Default extension
        check_hresult( dialog->SetDefaultExtension( L"sav" ) );

        // Show the dialog
        {
            HRESULT const hr = dialog->Show( owner );
            if ( hr == HRESULT_FROM_WIN32( ERROR_CANCELLED ) )
            {
                return std::nullopt;
            }
            check_hresult( hr );
        }

        // Get the result
        IShellItem *result_shell_item = nullptr;
        check_hresult( dialog->GetResult( &result_shell_item ) );
        UniqueCOMObjectHandle result_sell_item_raii_wrapper( dialog );

        // Get path
        PWSTR result_path = nullptr;
        check_hresult( result_shell_item->GetDisplayName( SIGDN_FILESYSPATH, &result_path ) );
        std::wstring result_path_str = result_path;
        CoTaskMemFree( result_path );
        return wide_to_utf8( result_path_str );
    }
}

std::optional< std::u8string > save_fixer::win_open_mm_sav_file( HWND owner )
{
    IFileOpenDialog *dialog = NULL;
    check_hresult( ::CoCreateInstance( CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER,
                                       IID_PPV_ARGS( &dialog ) ) );
    UniqueCOMObjectHandle dialog_raii_wrapper( dialog );

    return show_mm_save_file_dialog( owner, dialog );
}

std::optional< std::u8string > save_fixer::win_save_mm_sav_file( HWND owner, std::u8string_view suggested_file_name )
{
    IFileSaveDialog *dialog = NULL;
    check_hresult( ::CoCreateInstance( CLSID_FileSaveDialog, nullptr, CLSCTX_INPROC_SERVER,
                                       IID_PPV_ARGS( &dialog ) ) );
    UniqueCOMObjectHandle dialog_raii_wrapper( dialog );

    if ( !suggested_file_name.empty() )
    {
        check_hresult( dialog->SetFileName( utf8_to_wide( suggested_file_name ).c_str() ) );
    }

    return show_mm_save_file_dialog( owner, dialog );
}
