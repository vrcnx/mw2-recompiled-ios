# iOS/iPadOS packaging (included from the top-level CMakeLists.txt when IOS is
# set). CMake builds the game and everything it links as static archives, then
# merges them into one, libmw2_ios.a. The app bundle, signing and install are
# Xcode's: ios/project.yml (XcodeGen) links that archive into a thin app
# target, which keeps automatic provisioning and on-device debugging.

# Walks a target's link interface and returns every static archive it pulls in.
# Anything that is not a target -- frameworks, system libraries -- goes to
# <extras_var>.
function(mw2_collect_static_archives target archives_var extras_var)
    set(_pending ${target})
    set(_seen)
    set(_archives)
    set(_extras)
    while(_pending)
        list(POP_FRONT _pending _item)
        if(_item MATCHES "^\\$<LINK_ONLY:(.+)>$")
            set(_item "${CMAKE_MATCH_1}")
        endif()
        if(_item MATCHES "^\\$<" OR _item IN_LIST _seen)
            continue()
        endif()
        list(APPEND _seen ${_item})
        if(NOT TARGET ${_item})
            list(APPEND _extras ${_item})
            continue()
        endif()
        get_target_property(_aliased ${_item} ALIASED_TARGET)
        if(_aliased)
            list(APPEND _pending ${_aliased})
            continue()
        endif()
        get_target_property(_type ${_item} TYPE)
        get_target_property(_imported ${_item} IMPORTED)
        if(_type STREQUAL "STATIC_LIBRARY")
            if(_imported)
                get_target_property(_location ${_item} IMPORTED_LOCATION)
                list(APPEND _archives "${_location}")
            else()
                list(APPEND _archives "$<TARGET_FILE:${_item}>")
            endif()
        endif()
        foreach(_property LINK_LIBRARIES INTERFACE_LINK_LIBRARIES)
            if(_property STREQUAL "LINK_LIBRARIES" AND (_imported OR _type STREQUAL "INTERFACE_LIBRARY"))
                continue()
            endif()
            get_target_property(_deps ${_item} ${_property})
            if(_deps)
                list(APPEND _pending ${_deps})
            endif()
        endforeach()
    endwhile()
    list(REMOVE_DUPLICATES _archives)
    list(REMOVE_DUPLICATES _extras)
    set(${archives_var} ${_archives} PARENT_SCOPE)
    set(${extras_var} ${_extras} PARENT_SCOPE)
endfunction()

if(CMAKE_OSX_SYSROOT MATCHES "[Ss]imulator")
    set(MW2_IOS_SDK_NAME iphonesimulator)
else()
    set(MW2_IOS_SDK_NAME iphoneos)
endif()
set(MW2_IOS_OUTPUT_DIR "${CMAKE_BINARY_DIR}/ios")
set(MW2_IOS_ARCHIVE "${MW2_IOS_OUTPUT_DIR}/libmw2_ios.a")

# Collected at the end of configure, once everything has added its link
# dependencies.
cmake_language(DEFER DIRECTORY ${CMAKE_SOURCE_DIR} CALL mw2_ios_define_archive)
function(mw2_ios_define_archive)
    mw2_collect_static_archives(mw2 _archives _extras)
    file(GENERATE OUTPUT "${MW2_IOS_OUTPUT_DIR}/archives.txt" CONTENT "$<JOIN:${_archives},\n>\n")
    file(WRITE "${MW2_IOS_OUTPUT_DIR}/link-extras.txt" "${_extras}\n")
    add_custom_command(
        OUTPUT "${MW2_IOS_ARCHIVE}"
        COMMAND ${CMAKE_COMMAND} -E rm -f "${MW2_IOS_ARCHIVE}"
        COMMAND xcrun -sdk ${MW2_IOS_SDK_NAME} libtool -static -no_warning_for_no_symbols
            -o "${MW2_IOS_ARCHIVE}" ${_archives}
        DEPENDS ${_archives}
        COMMENT "Merging static archives into libmw2_ios.a"
        COMMAND_EXPAND_LISTS
        VERBATIM)
    add_custom_target(mw2_ios ALL DEPENDS "${MW2_IOS_ARCHIVE}")
    add_dependencies(mw2_ios mw2)
endfunction()
