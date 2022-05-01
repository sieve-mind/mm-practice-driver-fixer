set(include_files
    "src/Common.h"
    "src/FileSystem.h"
    "src/SaveFile.h"
    "src/GUI.h"
    "src/Version.h"
)

set(source_files
    "src/main.cpp"
    "src/SaveFile.cpp"
)

if(WIN32)
    list(APPEND include_files
         src/WindowsFileDialog.h
         src/WindowsCommon.h
    )

    list(APPEND source_files
         src/WindowsFileDialog.cpp
         src/WindowsFileSystem.cpp
         src/WindowsGUI.cpp
         src/Resource.rc
    )
endif()

add_executable(MMPracticeDriverFixer ${source_files} ${include_files})

target_link_libraries(MMPracticeDriverFixer PRIVATE lz4)

if(WIN32)
    target_compile_definitions(MMPracticeDriverFixer PRIVATE _UNICODE )
    target_link_options(MMPracticeDriverFixer PRIVATE "/SUBSYSTEM:WINDOWS" "/ENTRY:mainCRTStartup")
endif()
