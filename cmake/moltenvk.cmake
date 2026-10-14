# MoltenVK on macOS and iOS, linked in rather than loaded: iOS has no Vulkan
# loader to find, and on macOS it is one thing less to install. SDL finds
# vkGetInstanceProcAddr in the executable itself (SDL_Vulkan_LoadLibrary with
# no path), and the runtime calls MoltenVK's exports directly.
#
# MW2_MOLTENVK names the MoltenVK folder of the MoltenVK-all release
# (https://github.com/KhronosGroup/MoltenVK/releases): the one holding
# include/ and static/MoltenVK.xcframework.
set(MW2_MOLTENVK "${CMAKE_SOURCE_DIR}/../deps/moltenvk/MoltenVK/MoltenVK" CACHE PATH
    "MoltenVK-all's MoltenVK folder (include/, static/MoltenVK.xcframework)")
set(_mvk_framework "${MW2_MOLTENVK}/static/MoltenVK.xcframework")
if(NOT EXISTS "${_mvk_framework}")
    message(FATAL_ERROR "MoltenVK not found at ${MW2_MOLTENVK}: extract MoltenVK-all.tar and set MW2_MOLTENVK")
endif()
if(IOS AND CMAKE_OSX_SYSROOT MATCHES "[Ss]imulator")
    set(_mvk_slice ios-arm64_x86_64-simulator)
elseif(IOS)
    set(_mvk_slice ios-arm64)
else()
    set(_mvk_slice macos-arm64_x86_64)
endif()
add_library(moltenvk STATIC IMPORTED GLOBAL)
set_target_properties(moltenvk PROPERTIES
    IMPORTED_LOCATION "${_mvk_framework}/${_mvk_slice}/libMoltenVK.a"
    INTERFACE_INCLUDE_DIRECTORIES "${MW2_MOLTENVK}/include")
set(_mvk_frameworks Metal IOSurface QuartzCore Foundation CoreGraphics)
if(IOS)
    list(APPEND _mvk_frameworks UIKit)
else()
    list(APPEND _mvk_frameworks AppKit IOKit)
endif()
foreach(framework ${_mvk_frameworks})
    target_link_libraries(moltenvk INTERFACE "-framework ${framework}")
endforeach()
target_link_libraries(mw2 PRIVATE moltenvk)
target_compile_definitions(mw2 PRIVATE MW2_HAVE_VULKAN=1)
message(STATUS "Vulkan: MoltenVK (${_mvk_slice}), linked statically")
