#pragma once

#define STRINGIFY( s ) STRINGIFYX( s )
#define STRINGIFYX( s ) #s

#define SAVE_FIXER_MAJOR_VERSION_NUMBER 1
#define SAVE_FIXER_MINOR_VERSION_NUMBER 0
#define SAVE_FIXER_PATCH_VERSION_NUMBER 0

#define SAVE_FIXER_VERSION_STRING                                                                  \
    STRINGIFY( SAVE_FIXER_MAJOR_VERSION_NUMBER )                                                   \
    "." STRINGIFY( SAVE_FIXER_MINOR_VERSION_NUMBER ) "." STRINGIFY( SAVE_FIXER_PATCH_VERSION_NUMBER )

#undef xstr
#undef str
