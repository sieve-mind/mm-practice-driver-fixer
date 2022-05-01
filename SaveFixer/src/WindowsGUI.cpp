#ifndef _WIN32
#error
#endif

#include "GUI.h"

#include "FileSystem.h"
#include "SaveFile.h"
#include "WindowsCommon.h"
#include "WindowsFileDialog.h"

#include <windowsx.h>

#include <assert.h>
#include <optional>

using namespace save_fixer;

namespace
{
    constexpr int app_window_fixed_width = 600;

    constexpr char const *documentation_label_text =
        "Glitches with assigning drivers can happen when drivers are paired with the wrong cars in the save file. "
        "This program lets you fix the pairings and then create a new save file.\n\n"
        "Instructions:\n"
        "1) Save your game and close Motorsport Manager\n"
        "2) Click the \"Open Motorsport Manager Save File...\" button and pick a save file\n"
        "3) Select which driver should be in car 1 (purple), which driver should be in car 2 (orange), "
        "and which driver should be in reserve. Make sure none of the drivers overlap.\n"
        "4) Click the \"Save Changes As...\" button and save the new file.\n"
        "5) Open Motorsport Manager and load the new save file, the glitch should now be fixed.";

    std::u8string get_suggested_output_file_name( std::u8string_view const original_path )
    {
        std::u8string_view output_name = original_path;
        constexpr std::u8string_view extension = u8".sav";

        if ( size_t const last_sep = output_name.find_last_of( u8"\\/" );
             last_sep != std::u8string_view::npos && output_name.ends_with( extension ) )
        {
            output_name = output_name.substr( last_sep + 1 );
            output_name = output_name.substr( 0, output_name.size() - extension.size() );
            std::u8string result( output_name );
            result.append( u8"(fixed).sav"s );
            return result;
        }
        return std::u8string();
    }

    std::u8string extract_save_name_from_save_path( std::u8string_view const path )
    {
        std::u8string_view save_name = path;
        constexpr std::u8string_view extension = u8".sav";
        if ( save_name.ends_with( extension ) )
        {
            save_name = save_name.substr( 0, save_name.size() - extension.size() );
        }
        if ( size_t const last_sep = path.find_last_of( u8"\\/" ); last_sep != std::u8string_view::npos )
        {
            save_name = save_name.substr( last_sep + 1 );
        }
        if ( save_name.empty() )
        {
            assert( false );
            return u8"Practice Driver Fixed Save"s;
        }
        return std::u8string( save_name );
    }

    struct HWNDHandleTraits
    {
        using HandleType = HWND;
        inline static HWND const null_value = nullptr;
        static void close( HWND h ) { DestroyWindow( h ); }
    };
    struct FontHandleTraits
    {
        using HandleType = HFONT;
        inline static HFONT const null_value = nullptr;
        static void close( HFONT f ) { DeleteObject( f ); }
    };

    using UniqueHWND = UniqueHandle< HWNDHandleTraits >;
    using UniqueFontHandle = UniqueHandle< FontHandleTraits >;

    UniqueHWND create_window( LPCSTR class_name, LPCSTR window_name, DWORD style, int x, int y,
                              int width, int height, UniqueHWND const &parent = UniqueHWND() )
    {
        UniqueHWND window( ::CreateWindowA( class_name, window_name, style, x, y, width, height,
                                            parent.get(), nullptr, ::GetModuleHandle( nullptr ), nullptr ) );
        if ( !window.is_valid() )
        {
            throw_windows_error( u8"internal error: create window" );
        }
        return window;
    }
    UniqueHWND create_window( LPCSTR class_name, LPCSTR window_name, DWORD style,
                              UniqueHWND const &parent = UniqueHWND() )
    {
        return create_window( class_name, window_name, style, 0, 0, 0, 0, parent );
    }

    void set_pos( UniqueHWND const &window, int x, int y, int width, int height, DWORD extra_flags = 0 )
    {
        if ( !::SetWindowPos( window.get(), 0, x, y, width, height, SWP_NOZORDER | extra_flags ) )
        {
            throw_windows_error( u8"internal error: position controls" );
        }
    }

    void resize( UniqueHWND const &window, int width, int height, DWORD extra_flags = 0 )
    {
        set_pos( window, 0, 0, width, height, SWP_NOMOVE | SWP_NOREPOSITION | extra_flags );
    }

    //-------------------------------------------------------------------------
    // SaveFixerWindow
    //-------------------------------------------------------------------------

    class SaveFixerWindow
    {
    public:
        SaveFixerWindow()
        {
            create_app_window();
            create_controls();
            position_controls();
        }

        int run()
        {
            ::ShowWindow( app_window.get(), SW_NORMAL );
            ::UpdateWindow( app_window.get() );

            while ( true )
            {
                MSG msg;
                int const ret = ::GetMessage( &msg, nullptr, 0U, 0U );
                if ( ret == 0 )
                {
                    break;
                }
                else if ( ret == -1 )
                {
                    throw_windows_error( u8"internal error: message" );
                }
                else if ( !IsDialogMessage( app_window.get(), &msg ) )
                {
                    ::TranslateMessage( &msg );
                    ::DispatchMessage( &msg );
                }
            }

            return 0;
        }

    private:
        static LPCSTR get_window_class()
        {
            static ATOM class_atom = 0;

            if ( !class_atom )
            {
                WNDCLASSEXA wc = { sizeof( WNDCLASSEXA ) };
                wc.lpfnWndProc = WndProc;
                wc.hInstance = ::GetModuleHandle( nullptr );
                wc.hbrBackground = reinterpret_cast< HBRUSH >( COLOR_WINDOW + 1 );
                wc.lpszClassName = "MMPracticeDriverFixer";

                class_atom = ::RegisterClassExA( &wc );
                if ( !class_atom )
                {
                    throw_windows_error( u8"internal error: register class" );
                }
            }

            // CreateWindow can take a class atom as the lower word of a string pointer
            intptr_t result = class_atom;
            return reinterpret_cast< LPCSTR >( result );
        }

        void create_app_window()
        {
            app_window = create_window( get_window_class(), "Motorsport Manager Practice Driver Fixer", window_style,
                                        CW_USEDEFAULT, CW_USEDEFAULT, app_window_fixed_width, 0 );

            ::SetLastError( 0 );
            ::SetWindowLongPtrA( app_window.get(), GWLP_USERDATA, reinterpret_cast< LONG_PTR >( this ) );
            if ( DWORD const err = ::GetLastError(); err != 0 )
            {
                throw_windows_error( u8"internal error: set window user data", err );
            }

            window_font = ::CreateFontA( 20, 0, 0, 0, FW_DONTCARE, FALSE, FALSE, FALSE,
                                         ANSI_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                                         DEFAULT_QUALITY, DEFAULT_PITCH | FF_DONTCARE, "Segoe UI" );
        }

        void create_controls()
        {
            auto const create_control = [ & ]( LPCSTR class_name, LPCSTR window_name, DWORD style,
                                               bool set_font = true ) {
                UniqueHWND control = create_window( class_name, window_name, style, app_window );
                if ( set_font && window_font.is_valid() )
                {
                    SendMessage( control.get(), WM_SETFONT, WPARAM( window_font.get() ), FALSE );
                }
                return control;
            };
            auto const create_button = [ & ]( LPCSTR text ) {
                return create_control( "BUTTON", text, BS_PUSHBUTTON | WS_CHILD | WS_VISIBLE | WS_GROUP | WS_TABSTOP );
            };
            auto const create_label = [ & ]( LPCSTR text, DWORD align_flag ) {
                return create_control( "STATIC", text, align_flag | WS_CHILD | WS_VISIBLE );
            };
            auto const create_radio = [ & ]( LPCSTR text, DWORD extra_flags = 0 ) {
                constexpr bool set_font = false;
                return create_control( "BUTTON", text,
                                       BS_AUTORADIOBUTTON | WS_CHILD | WS_VISIBLE | extra_flags, set_font );
            };

            doc_label = create_label( documentation_label_text, SS_LEFT );
            open_button = create_button( "Open Motorsport Manager Save File..." );

            car_1_label = create_label( "Car 1 (Purple)", SS_LEFT );
            car_2_label = create_label( "Car 2 (Orange)", SS_LEFT );
            reserve_label = create_label( "Reserve", SS_LEFT );

            driver_0_label = create_label( "", SS_RIGHT );
            driver_1_label = create_label( "", SS_RIGHT );
            driver_2_label = create_label( "", SS_RIGHT );

            driver_0_car_1_radio = create_radio( "", WS_GROUP | WS_TABSTOP );
            driver_0_car_2_radio = create_radio( "" );
            driver_0_reserve_radio = create_radio( "" );
            driver_1_car_1_radio = create_radio( "", WS_GROUP | WS_TABSTOP );
            driver_1_car_2_radio = create_radio( "" );
            driver_1_reserve_radio = create_radio( "" );
            driver_2_car_1_radio = create_radio( "", WS_GROUP | WS_TABSTOP );
            driver_2_car_2_radio = create_radio( "" );
            driver_2_reserve_radio = create_radio( "" );

            save_button = create_button( "Save Changes As..." );
        }

        void position_controls()
        {
            auto const error_if_not = []( bool b ) {
                if ( !b )
                {
                    throw_windows_error( u8"internal error: position controls" );
                }
            };

            RECT client_rect{ 0 };
            error_if_not( ::GetClientRect( app_window.get(), &client_rect ) );

            int const x_pad = 10;
            int const y_pad = 5;
            int const fill_width = client_rect.right - ( x_pad * 2 );
            int current_y = 10;

            int const driver_label_col_width = 200;
            int const position_col_width = ( fill_width - ( driver_label_col_width + x_pad * 3 ) ) / 3;

            int const driver_label_col_x = x_pad;
            int const car_1_col_x = driver_label_col_x + driver_label_col_width + x_pad;
            int const car_2_col_x = car_1_col_x + position_col_width + x_pad;
            int const reserve_col_x = car_2_col_x + position_col_width + x_pad;

            // Doc label
            int const doc_label_height = [ & ]() {
                RECT text_rect{ .right = fill_width };
                HDC dc = GetDC( doc_label.get() );
                error_if_not( dc );

                HFONT old_font = reinterpret_cast< HFONT >( ::SelectObject( dc, window_font.get() ) );
                error_if_not( ::DrawText( dc, documentation_label_text, -1, &text_rect,
                                          DT_CALCRECT | DT_LEFT | DT_WORDBREAK ) );
                ::SelectObject( dc, old_font );

                return text_rect.bottom + 3;
            }();
            set_pos( doc_label, x_pad, current_y, fill_width, doc_label_height );
            current_y += doc_label_height + y_pad;

            // Open button
            set_pos( open_button, x_pad, current_y, fill_width, 23 * 2 );
            current_y += 23 * 2 + y_pad;

            if ( save_file.has_value() )
            {
                // Postition labels
                set_pos( car_1_label, car_1_col_x, current_y, position_col_width, 23, SWP_SHOWWINDOW );
                set_pos( car_2_label, car_2_col_x, current_y, position_col_width, 23, SWP_SHOWWINDOW );
                set_pos( reserve_label, reserve_col_x, current_y, position_col_width, 23, SWP_SHOWWINDOW );
                current_y += 23 + y_pad;

                // Driver labels
                int const driver_0_y = current_y;
                set_pos( driver_0_label, x_pad, current_y, driver_label_col_width, 23, SWP_SHOWWINDOW );
                current_y += 23 + y_pad;

                int const driver_1_y = current_y;
                set_pos( driver_1_label, x_pad, current_y, driver_label_col_width, 23, SWP_SHOWWINDOW );
                current_y += 23 + y_pad;

                int const driver_2_y = current_y;
                set_pos( driver_2_label, x_pad, current_y, driver_label_col_width, 23, SWP_SHOWWINDOW );
                current_y += 23 + y_pad;

                // Radio buttons
                set_pos( driver_0_car_1_radio, car_1_col_x, driver_0_y, position_col_width, 23, SWP_SHOWWINDOW );
                set_pos( driver_0_car_2_radio, car_2_col_x, driver_0_y, position_col_width, 23, SWP_SHOWWINDOW );
                set_pos( driver_0_reserve_radio, reserve_col_x, driver_0_y, position_col_width, 23, SWP_SHOWWINDOW );

                set_pos( driver_1_car_1_radio, car_1_col_x, driver_1_y, position_col_width, 23, SWP_SHOWWINDOW );
                set_pos( driver_1_car_2_radio, car_2_col_x, driver_1_y, position_col_width, 23, SWP_SHOWWINDOW );
                set_pos( driver_1_reserve_radio, reserve_col_x, driver_1_y, position_col_width, 23, SWP_SHOWWINDOW );

                set_pos( driver_2_car_1_radio, car_1_col_x, driver_2_y, position_col_width, 23, SWP_SHOWWINDOW );
                set_pos( driver_2_car_2_radio, car_2_col_x, driver_2_y, position_col_width, 23, SWP_SHOWWINDOW );
                set_pos( driver_2_reserve_radio, reserve_col_x, driver_2_y, position_col_width, 23, SWP_SHOWWINDOW );

                // Save button
                set_pos( save_button, x_pad, current_y, fill_width, 23 * 2, SWP_SHOWWINDOW );
                current_y += 23 * 2 + y_pad;
            }
            else
            {
                ::ShowWindow( car_1_label.get(), SW_HIDE );
                ::ShowWindow( car_2_label.get(), SW_HIDE );
                ::ShowWindow( reserve_label.get(), SW_HIDE );
                ::ShowWindow( driver_0_label.get(), SW_HIDE );
                ::ShowWindow( driver_1_label.get(), SW_HIDE );
                ::ShowWindow( driver_2_label.get(), SW_HIDE );
                ::ShowWindow( driver_0_car_1_radio.get(), SW_HIDE );
                ::ShowWindow( driver_0_car_2_radio.get(), SW_HIDE );
                ::ShowWindow( driver_0_reserve_radio.get(), SW_HIDE );
                ::ShowWindow( driver_1_car_1_radio.get(), SW_HIDE );
                ::ShowWindow( driver_1_car_2_radio.get(), SW_HIDE );
                ::ShowWindow( driver_1_reserve_radio.get(), SW_HIDE );
                ::ShowWindow( driver_2_car_1_radio.get(), SW_HIDE );
                ::ShowWindow( driver_2_car_2_radio.get(), SW_HIDE );
                ::ShowWindow( driver_2_reserve_radio.get(), SW_HIDE );
                ::ShowWindow( save_button.get(), SW_HIDE );
            }

            // Adjust the window height
            client_rect.bottom = current_y + 5;
            error_if_not( AdjustWindowRect( &client_rect, window_style & ~WS_OVERLAPPED, FALSE ) );
            resize( app_window, client_rect.right - client_rect.left,
                    client_rect.bottom - client_rect.top );
        }

        bool selected_driver_positions_are_unique()
        {
            auto const one_selected = []( UniqueHWND const &r1, UniqueHWND const &r2, UniqueHWND const &r3 ) {
                int count = 0;
                count += ( Button_GetCheck( r1.get() ) == BST_CHECKED ) ? 1 : 0;
                count += ( Button_GetCheck( r2.get() ) == BST_CHECKED ) ? 1 : 0;
                count += ( Button_GetCheck( r3.get() ) == BST_CHECKED ) ? 1 : 0;
                return count == 1;
            };

            return ( one_selected( driver_0_car_1_radio, driver_1_car_1_radio, driver_2_car_1_radio ) &&
                     one_selected( driver_0_car_2_radio, driver_1_car_2_radio, driver_2_car_2_radio ) &&
                     one_selected( driver_0_reserve_radio, driver_1_reserve_radio, driver_2_reserve_radio ) );
        }

        void open_button_pressed()
        {
            try
            {
                std::optional< std::u8string > save_path = win_open_mm_sav_file( app_window.get() );
                if ( !save_path.has_value() || save_path->empty() )
                {
                    // Cancelled
                    return;
                }
                save_file.emplace( std::move( save_path.value() ) );
            }
            catch ( SaveFixerException const &ex )
            {
                ::MessageBox( app_window.get(), u8_as_char( ex.description.c_str() ),
                              "Error opening file", MB_ICONERROR );
                return;
            }

            std::array< SaveFile::DriverRef, 3 > const drivers = save_file->get_drivers();

            ::SetWindowTextW( driver_0_label.get(), utf8_to_wide( drivers[ 0 ].name ).c_str() );
            ::SetWindowTextW( driver_1_label.get(), utf8_to_wide( drivers[ 1 ].name ).c_str() );
            ::SetWindowTextW( driver_2_label.get(), utf8_to_wide( drivers[ 2 ].name ).c_str() );

            auto const set_radio_buttons = []( SaveFile::DriverRef const driver, UniqueHWND const &car1,
                                               UniqueHWND const &car2, UniqueHWND const &reserve ) {
                Button_SetCheck( car1.get(), BST_UNCHECKED );
                Button_SetCheck( car2.get(), BST_UNCHECKED );
                Button_SetCheck( reserve.get(), BST_UNCHECKED );
                switch ( driver.position )
                {
                    case SaveFile::DriverPosition::car1:
                        Button_SetCheck( car1.get(), BST_CHECKED );
                        break;
                    case SaveFile::DriverPosition::car2:
                        Button_SetCheck( car2.get(), BST_CHECKED );
                        break;
                    case SaveFile::DriverPosition::reserve:
                        Button_SetCheck( reserve.get(), BST_CHECKED );
                        break;
                }
            };
            set_radio_buttons( drivers[ 0 ], driver_0_car_1_radio, driver_0_car_2_radio, driver_0_reserve_radio );
            set_radio_buttons( drivers[ 1 ], driver_1_car_1_radio, driver_1_car_2_radio, driver_1_reserve_radio );
            set_radio_buttons( drivers[ 2 ], driver_2_car_1_radio, driver_2_car_2_radio, driver_2_reserve_radio );

            radio_buttons_changed();
            position_controls();
        }

        void radio_buttons_changed()
        {
            ::EnableWindow( save_button.get(), selected_driver_positions_are_unique() );
        }

        void save_button_pressed()
        {
            if ( !save_file.has_value() || !selected_driver_positions_are_unique() )
            {
                assert( false );
                return;
            }

            // Set driver positions from radio controls
            auto const set_position = []( SaveFile::DriverRef const driver, UniqueHWND const &car1,
                                          UniqueHWND const &car2, UniqueHWND const &reserve ) {
                if ( Button_GetCheck( car1.get() ) == BST_CHECKED )
                {
                    driver.position = SaveFile::DriverPosition::car1;
                }
                else if ( Button_GetCheck( car2.get() ) == BST_CHECKED )
                {
                    driver.position = SaveFile::DriverPosition::car2;
                }
                else
                {
                    ( void ) reserve;
                    assert( Button_GetCheck( reserve.get() ) == BST_CHECKED );
                    driver.position = SaveFile::DriverPosition::reserve;
                }
            };
            std::array< SaveFile::DriverRef, 3 > drivers = save_file->get_drivers();
            set_position( drivers[ 0 ], driver_0_car_1_radio, driver_0_car_2_radio, driver_0_reserve_radio );
            set_position( drivers[ 1 ], driver_1_car_1_radio, driver_1_car_2_radio, driver_1_reserve_radio );
            set_position( drivers[ 2 ], driver_2_car_1_radio, driver_2_car_2_radio, driver_2_reserve_radio );

            // Write the save file
            try
            {
                std::u8string const suggested_file_name =
                    get_suggested_output_file_name( save_file->get_original_file_path() );
                std::optional< std::u8string > const save_path =
                    win_save_mm_sav_file( app_window.get(), suggested_file_name );
                if ( !save_path.has_value() || save_path->empty() )
                {
                    // Cancelled
                    return;
                }
                bool allow_overwrite = false;
                if ( query_file( save_path.value() ) != path_state::does_not_exist )
                {
                    if ( ::MessageBoxA( app_window.get(), "Do you want to overwrite the existing file?",
                                        "File already exists", MB_YESNO | MB_ICONWARNING | MB_DEFBUTTON2 ) != IDYES )
                    {
                        // Cancelled
                        return;
                    }
                    else
                    {
                        allow_overwrite = true;
                    }
                }
                std::u8string const save_name = extract_save_name_from_save_path( save_path.value() );
                save_file->write( save_path.value(), save_name, allow_overwrite );
            }
            catch ( SaveFixerException const &ex )
            {
                ::MessageBox( app_window.get(), u8_as_char( ex.description.c_str() ),
                              "Error saving file", MB_ICONERROR );
                return;
            }
        }

        LRESULT handle_message( HWND hwnd, UINT msg, WPARAM w_param, LPARAM l_param )
        {
            switch ( msg )
            {
                case WM_CLOSE:
                    app_window.reset();
                    return 0;

                case WM_DESTROY:
                    ::PostQuitMessage( 0 );
                    return 0;

                case WM_CTLCOLORSTATIC:
                    return reinterpret_cast< INT_PTR >( ::GetSysColorBrush( COLOR_WINDOW ) );

                case WM_COMMAND:
                    if ( HIWORD( w_param ) == BN_CLICKED )
                    {
                        if ( HWND const clicked_handle = reinterpret_cast< HWND >( l_param );
                             clicked_handle == open_button.get() )
                        {
                            open_button_pressed();
                            return 0;
                        }
                        else if ( clicked_handle == save_button.get() )
                        {
                            save_button_pressed();
                            return 0;
                        }
                        else if ( clicked_handle == driver_0_car_1_radio.get() ||
                                  clicked_handle == driver_0_car_2_radio.get() ||
                                  clicked_handle == driver_0_reserve_radio.get() ||
                                  clicked_handle == driver_1_car_1_radio.get() ||
                                  clicked_handle == driver_1_car_2_radio.get() ||
                                  clicked_handle == driver_1_reserve_radio.get() ||
                                  clicked_handle == driver_2_car_1_radio.get() ||
                                  clicked_handle == driver_2_car_2_radio.get() ||
                                  clicked_handle == driver_2_reserve_radio.get() )
                        {
                            radio_buttons_changed();
                            return 0;
                        }
                    }
            }
            return ::DefWindowProc( hwnd, msg, w_param, l_param );
        }

        static LRESULT CALLBACK WndProc( HWND hwnd, UINT msg, WPARAM w_param, LPARAM l_param )
        {
            auto self = reinterpret_cast< SaveFixerWindow * >( GetWindowLongPtr( hwnd, GWLP_USERDATA ) );
            if ( self != nullptr )
            {
                return self->handle_message( hwnd, msg, w_param, l_param );
            }
            return ::DefWindowProc( hwnd, msg, w_param, l_param );
        }

        std::optional< SaveFile > save_file;

        static constexpr DWORD window_style = WS_OVERLAPPEDWINDOW & ~WS_THICKFRAME;
        UniqueFontHandle window_font;

        UniqueHWND app_window;
        UniqueHWND doc_label;
        UniqueHWND open_button;

        UniqueHWND car_1_label;
        UniqueHWND car_2_label;
        UniqueHWND reserve_label;
        UniqueHWND driver_0_label;
        UniqueHWND driver_1_label;
        UniqueHWND driver_2_label;

        UniqueHWND driver_0_car_1_radio;
        UniqueHWND driver_0_car_2_radio;
        UniqueHWND driver_0_reserve_radio;
        UniqueHWND driver_1_car_1_radio;
        UniqueHWND driver_1_car_2_radio;
        UniqueHWND driver_1_reserve_radio;
        UniqueHWND driver_2_car_1_radio;
        UniqueHWND driver_2_car_2_radio;
        UniqueHWND driver_2_reserve_radio;

        UniqueHWND save_button;
    };
}

int save_fixer::run_gui()
{
    try
    {
        SaveFixerWindow window;
        return window.run();
    }
    catch ( SaveFixerException const &ex )
    {
        ::MessageBox( nullptr, u8_as_char( ex.description.c_str() ), "Unexpected Error", MB_ICONERROR );
        return 1;
    }
    catch ( ... )
    {
        ::MessageBox( nullptr, "Unknown Error", "Unexpected Error", MB_ICONERROR );
        return 1;
    }
}
