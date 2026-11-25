cmake_minimum_required(VERSION 3.20)

if(POLICY CMP0091)
    cmake_policy(SET CMP0091 NEW)
endif()

set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")

project(DriverLoom VERSION 1.2.0 LANGUAGES CXX RC)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

include(FetchContent)

FetchContent_Declare(
    imgui
    URL                        https://github.com/ocornut/imgui/archive/d613c9f597eb75a6779d667dfaedad8085ee2693.zip
    URL_HASH                   SHA256=DBD9613615626994E9386833A0694EA0C56F6977966F61BAB44292C19CA1E6F6
    DOWNLOAD_EXTRACT_TIMESTAMP TRUE
)
FetchContent_MakeAvailable(imgui)

add_library(imgui_lib STATIC
    "${imgui_SOURCE_DIR}/imgui.cpp"
    "${imgui_SOURCE_DIR}/imgui_draw.cpp"
    "${imgui_SOURCE_DIR}/imgui_tables.cpp"
    "${imgui_SOURCE_DIR}/imgui_widgets.cpp"
    "${imgui_SOURCE_DIR}/backends/imgui_impl_win32.cpp"
    "${imgui_SOURCE_DIR}/backends/imgui_impl_dx11.cpp"
)

target_include_directories(imgui_lib PUBLIC
    "${imgui_SOURCE_DIR}"
    "${imgui_SOURCE_DIR}/backends"
)

target_compile_definitions(imgui_lib PUBLIC
    UNICODE
    _UNICODE
    NOMINMAX
    WIN32_LEAN_AND_MEAN
)

if(MSVC)
    target_compile_options(imgui_lib PRIVATE /utf-8 /W3 /guard:cf)
elseif(MINGW)
    target_compile_options(imgui_lib PRIVATE -finput-charset=UTF-8 -fexec-charset=UTF-8)
endif()

add_executable(DriverLoom WIN32
    src/main.cpp
    src/app/CliWorker.cpp
    src/ui/AppWidgets.cpp
    src/ui/DriverLoomApp.cpp
    src/ui/TabOverview.cpp
    src/ui/TabDevices.cpp
    src/ui/TabComponents.cpp
    src/ui/TabWindowsUpdate.cpp
    src/ui/TabLocalInstaller.cpp
    src/ui/TabDiagnostics.cpp
    src/core/DeviceScanner.cpp
    src/core/WindowsUpdateManager.cpp
    src/core/LocalDriverInstaller.cpp
    src/core/SystemRestore.cpp
    src/core/HttpClient.cpp
    src/core/PackageSecurity.cpp
    src/core/OnlineCatalogClient.cpp
    src/core/SystemComponentsScanner.cpp
    src/core/SystemDiagnostics.cpp
    resources/version.rc
)

target_include_directories(DriverLoom PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/src"
)

target_compile_definitions(DriverLoom PRIVATE
    UNICODE
    _UNICODE
    NOMINMAX
    WIN32_LEAN_AND_MEAN
    _WIN32_WINNT=0x0A00
    WINVER=0x0A00
)

target_link_libraries(DriverLoom PRIVATE
    imgui_lib
    d3d11
    d3dcompiler
    dxgi
    dwmapi
    setupapi
    newdev
    ole32
    oleaut32
    cfgmgr32
    winhttp
    wintrust
    crypt32
    bcrypt
    version
    uuid
    shell32
    advapi32
    user32
    gdi32
)

if(MSVC)
    target_compile_options(DriverLoom PRIVATE /utf-8 /W4 /permissive- /guard:cf)
    target_link_libraries(DriverLoom PRIVATE wuguid)
    target_link_options(DriverLoom PRIVATE
        /MANIFEST:NO
        /DEPENDENTLOADFLAG:0x800
        /GUARD:CF
        /DYNAMICBASE
        /NXCOMPAT
        /HIGHENTROPYVA
        /CETCOMPAT
    )
elseif(MINGW)
    set(NO_DEFAULT_MANIFEST_SPECS "${CMAKE_CURRENT_BINARY_DIR}/no_default_manifest.specs")
    file(WRITE "${NO_DEFAULT_MANIFEST_SPECS}"
        "*endfile:\n"
        "%{mdaz-ftz:crtfastmath.o%s;Ofast|ffast-math|funsafe-math-optimizations:%{!shared:%{!mno-daz-ftz:crtfastmath.o%s}}} %{fvtable-verify=none:%s;fvtable-verify=preinit:vtv_end.o%s;fvtable-verify=std:vtv_end.o%s} crtend.o%s\n"
    )
    target_compile_options(DriverLoom PRIVATE
        -finput-charset=UTF-8
        -fexec-charset=UTF-8
        -Wall
        -Wextra
        -Wpedantic
    )
    target_link_options(DriverLoom PRIVATE
        "-specs=${NO_DEFAULT_MANIFEST_SPECS}"
        -static
        -static-libgcc
        -static-libstdc++
    )
endif()
