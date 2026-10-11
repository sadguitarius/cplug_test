# Checks that the object file built from src/gui_libs.c has no writable statics
# except the ones listed in ALLOWED. gui_libs.c redirects sokol's single state
# struct per library through a pointer (see the comment there). Any other
# writable static in that file would be shared by every plugin instance.
#
# Run it after bumping lib/sokol:
#   cmake --build <build dir> --target check_statics
#
# Inputs, set by the check_statics target in CMakeLists.txt:
#   NM       an nm that reads this platform's object format
#   OBJECTS  the plugin target's object files, separated by "|"
#   ALLOWED  expected symbol names, separated by ","

foreach(var NM OBJECTS ALLOWED)
    if(NOT DEFINED ${var} OR "${${var}}" STREQUAL "")
        message(FATAL_ERROR "check_statics: ${var} is not set")
    endif()
endforeach()

string(REPLACE "|" ";" objects "${OBJECTS}")
string(REPLACE "," ";" allowed "${ALLOWED}")

set(object "")
foreach(candidate IN LISTS objects)
    if(candidate MATCHES "gui_libs\\.c\\.(o|obj)$")
        set(object "${candidate}")
    endif()
endforeach()
if(NOT object)
    message(FATAL_ERROR "check_statics: no gui_libs object among: ${objects}")
endif()

execute_process(
    COMMAND "${NM}" --defined-only "${object}"
    OUTPUT_VARIABLE nm_out
    ERROR_VARIABLE nm_err
    RESULT_VARIABLE nm_result)
if(NOT nm_result EQUAL 0)
    message(FATAL_ERROR "check_statics: ${NM} failed on ${object}:\n${nm_err}")
endif()

string(REPLACE "\n" ";" lines "${nm_out}")
set(found "")
set(unexpected "")
foreach(line IN LISTS lines)
    # "<address> <type> <name>": b/B .bss, d/D .data, C common.
    # On Mach-O, nm reports other __DATA sections (Objective-C metadata) as
    # s/S; the check ignores those.
    if(line MATCHES "^[0-9a-fA-F]* *([bBdDC]) (.+)$")
        set(name "${CMAKE_MATCH_2}")
        # Mach-O prefixes C symbol names with "_".
        string(REGEX REPLACE "^_" "" bare "${name}")
        if(name IN_LIST allowed OR bare IN_LIST allowed)
            list(APPEND found "${bare}")
        else()
            list(APPEND unexpected "${name}")
        endif()
    endif()
endforeach()

if(unexpected)
    list(JOIN unexpected "\n  " unexpected_lines)
    message(FATAL_ERROR
        "check_statics: ${object} has writable statics that gui_libs does not "
        "redirect:\n  ${unexpected_lines}\n"
        "Every plugin instance shares these. If one belongs to sokol, redirect "
        "it in src/gui_libs.c the same way as _sg. If it is harmless (for "
        "example a C runtime detail), add it to ALLOWED in CMakeLists.txt.")
endif()

list(JOIN found ", " found_list)
message(STATUS "check_statics: ok, only ${found_list}")
