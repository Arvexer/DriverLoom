if(NOT DEFINED SRC_DIR OR SRC_DIR STREQUAL "")
    message(FATAL_ERROR "CheckHygiene.cmake requires -DSRC_DIR=<path>")
endif()

# ---------------------------------------------------------------------------
# 1. 4-way Version Synchronization Check
# ---------------------------------------------------------------------------
file(READ "${SRC_DIR}/CMakeLists.txt" cmake_txt)
if(NOT cmake_txt MATCHES "project\\(DriverLoom VERSION ([0-9]+\\.[0-9]+\\.[0-9]+)")
    message(FATAL_ERROR "Failed to parse project(DriverLoom VERSION X.Y.Z) in CMakeLists.txt")
endif()
set(ver "${CMAKE_MATCH_1}")
string(REPLACE "." "," ver_comma "${ver}")

file(READ "${SRC_DIR}/src/common/AppCommon.hpp" app_common_txt)
if(NOT app_common_txt MATCHES "kAppVersion = \"${ver}\"")
    message(FATAL_ERROR "Version mismatch in src/common/AppCommon.hpp (expected ${ver})")
endif()

file(READ "${SRC_DIR}/resources/version.rc" ver_rc_txt)
if(NOT ver_rc_txt MATCHES "FILEVERSION[ \t]+${ver_comma},0" OR
   NOT ver_rc_txt MATCHES "PRODUCTVERSION[ \t]+${ver_comma},0" OR
   NOT ver_rc_txt MATCHES "\"FileVersion\",[ \t]*\"${ver}\\.0\"" OR
   NOT ver_rc_txt MATCHES "\"ProductVersion\",[ \t]*\"${ver}\\.0\"")
    message(FATAL_ERROR "Version mismatch in resources/version.rc (expected ${ver}.0 / ${ver_comma},0)")
endif()

file(READ "${SRC_DIR}/resources/app.manifest" manifest_txt)
if(NOT manifest_txt MATCHES "version=\"${ver}\\.0\"")
    message(FATAL_ERROR "Version mismatch in resources/app.manifest (expected ${ver}.0)")
endif()

# ---------------------------------------------------------------------------
# 2. Core Header Self-Containment Check (AppCommon.hpp)
# ---------------------------------------------------------------------------
foreach(req_hdr IN ITEMS "<algorithm>" "<cstddef>" "<cstdint>")
    string(FIND "${app_common_txt}" "#include ${req_hdr}" hdr_pos)
    if(hdr_pos EQUAL -1)
        message(FATAL_ERROR "src/common/AppCommon.hpp is missing #include ${req_hdr}")
    endif()
endforeach()

# ---------------------------------------------------------------------------
# 3. Cross-Compiler Header Self-Containment & Security Regression Shield
# ---------------------------------------------------------------------------
file(GLOB_RECURSE all_src_files
    "${SRC_DIR}/src/*.hpp"
    "${SRC_DIR}/src/*.cpp"
    "${SRC_DIR}/tests/*.cpp"
)

foreach(f IN LISTS all_src_files)
    file(READ "${f}" content)
    string(REGEX REPLACE "(^|[ \t])//[^\r\n]*" "\\1" code "${content}")

    if(code MATCHES "std::(min|max|clamp|sort|replace|find_if|transform)([^A-Za-z0-9_]|$)")
        if(NOT content MATCHES "#include[ \t]*<algorithm>")
            message(FATAL_ERROR "${f}: uses std:: algorithm helper without #include <algorithm>")
        endif()
    endif()
    if(code MATCHES "std::(round|floor|ceil|fabs|sin|cos|sqrt)([^A-Za-z0-9_]|$)")
        if(NOT content MATCHES "#include[ \t]*<cmath>")
            message(FATAL_ERROR "${f}: uses std:: cmath helper without #include <cmath>")
        endif()
    endif()
    if(code MATCHES "std::(memcpy|memset|memcmp|strlen)([^A-Za-z0-9_]|$)")
        if(NOT content MATCHES "#include[ \t]*<cstring>")
            message(FATAL_ERROR "${f}: uses std:: cstring helper without #include <cstring>")
        endif()
    endif()
    if(code MATCHES "std::numeric_limits([^A-Za-z0-9_]|$)")
        if(NOT content MATCHES "#include[ \t]*<limits>")
            message(FATAL_ERROR "${f}: uses std::numeric_limits without #include <limits>")
        endif()
    endif()

    get_filename_component(f_ext "${f}" EXT)
    if(f_ext STREQUAL ".hpp" AND NOT content MATCHES "#include[ \t]*\"common/AppCommon\\.hpp\"")
        if(code MATCHES "(^|[^A-Za-z0-9_])u?int(8|16|32|64)_t([^A-Za-z0-9_]|$)")
            if(NOT content MATCHES "#include[ \t]*<cstdint>")
                message(FATAL_ERROR "${f}: standalone header uses fixed-width integer without #include <cstdint>")
            endif()
        endif()
    endif()

    # Security & UI regressions only apply to src/ files (tests may mention helper names)
    string(FIND "${f}" "/tests/" is_test_pos)
    if(is_test_pos EQUAL -1)
        if(code MATCHES "(^|[^A-Za-z0-9_])(LoadLibrary[AW]|LoadLibraryExA)[ \t]*\\(")
            message(FATAL_ERROR "${f}: forbidden LoadLibraryA/W/ExA — must use LoadLibraryExW(..., LOAD_LIBRARY_SEARCH_SYSTEM32)")
        endif()
        if(code MATCHES "lpVerb[ \t]*=[^\r\n]*L\"open\"")
            message(FATAL_ERROR "${f}: forbidden ShellExecuteExW lpVerb = L\"open\" — use CreateProcessW when elevated to prevent HKCU class hijacking")
        endif()
        if(code MATCHES "KF_FLAG_DEFAULT([^A-Za-z0-9_]|$)")
            message(FATAL_ERROR "${f}: forbidden KF_FLAG_DEFAULT — must use KF_FLAG_DEFAULT_PATH to block HKCU folder redirection")
        endif()
        if(code MATCHES "\"azureedge\\.net\"")
            message(FATAL_ERROR "${f}: forbidden bare multi-tenant azureedge.net suffix — pin a specific vendor hostname")
        endif()
        if(code MATCHES "\"msecnd\\.net\"")
            message(FATAL_ERROR "${f}: forbidden bare multi-tenant msecnd.net suffix — pin a specific vendor hostname")
        endif()
        get_filename_component(f_name "${f}" NAME)
        if(NOT f_name STREQUAL "AppWidgets.cpp")
            if(code MATCHES "ShellExecuteW[ \t]*\\([^;]*L\"open\"")
                message(FATAL_ERROR "${f}: forbidden ShellExecuteW(..., L\"open\", ...) — use LaunchSystem32Tool / LaunchDeviceManager / CreateProcessW")
            endif()
        endif()
        if(code MATCHES "(^|[^A-Za-z0-9_])DiUninstallDriverW([^A-Za-z0-9_]|$)")
            message(FATAL_ERROR "${f}: forbidden DiUninstallDriverW — must use SetupUninstallOEMInfW")
        endif()
        if(code MATCHES "(^|[^A-Za-z0-9_])SUOI_FORCEDELETE([^A-Za-z0-9_]|$)")
            message(FATAL_ERROR "${f}: forbidden SUOI_FORCEDELETE flag")
        endif()
        if(code MATCHES "ExtractZipViaShellCom")
            message(FATAL_ERROR "${f}: forbidden Shell COM ZIP extractor reference")
        endif()
        if(code MATCHES "GetFontSize\\([ \t]*\\)[ \t]*/[ \t]*16")
            message(FATAL_ERROR "${f}: forbidden GetFontSize()/16 scaling — must use EffectiveUiScale()")
        endif()
        string(TOLOWER "${code}" code_lower)
        if(code_lower MATCHES "(^|[^a-z0-9_])(cmd\\.exe|powershell\\.exe|pnputil\\.exe|sc\\.exe)([^a-z0-9_]|$)")
            message(FATAL_ERROR "${f}: forbidden external CLI binary (cmd/powershell/pnputil/sc)")
        endif()
    endif()
endforeach()

message(STATUS "CodeHygieneChecks: ALL PASSED (version ${ver}, headers, security invariants)")
