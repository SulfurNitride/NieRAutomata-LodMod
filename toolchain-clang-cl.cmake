set(CMAKE_SYSTEM_NAME Windows)
set(CMAKE_SYSTEM_PROCESSOR x86_64)

set(XWIN_DIR "$ENV{HOME}/.xwin")

set(CMAKE_C_COMPILER clang-cl)
set(CMAKE_CXX_COMPILER clang-cl)
set(CMAKE_LINKER lld-link)
set(CMAKE_RC_COMPILER llvm-rc)

# Target x64 Windows, static CRT (/MT)
set(CMAKE_C_FLAGS_INIT "--target=x86_64-pc-windows-msvc /EHsc /MT")
set(CMAKE_CXX_FLAGS_INIT "--target=x86_64-pc-windows-msvc /EHsc /std:c++20 /MT")

# Include paths: MSVC CRT + Windows SDK
set(XWIN_INCLUDES
    "-imsvc${XWIN_DIR}/crt/include"
    "-imsvc${XWIN_DIR}/sdk/include/ucrt"
    "-imsvc${XWIN_DIR}/sdk/include/shared"
    "-imsvc${XWIN_DIR}/sdk/include/um"
    "-imsvc${XWIN_DIR}/sdk/include/winrt"
)
string(JOIN " " XWIN_INCLUDES_STR ${XWIN_INCLUDES})
set(CMAKE_C_FLAGS_INIT "${CMAKE_C_FLAGS_INIT} ${XWIN_INCLUDES_STR}")
set(CMAKE_CXX_FLAGS_INIT "${CMAKE_CXX_FLAGS_INIT} ${XWIN_INCLUDES_STR}")

# Library paths
set(CMAKE_EXE_LINKER_FLAGS_INIT "/libpath:${XWIN_DIR}/crt/lib/x86_64 /libpath:${XWIN_DIR}/sdk/lib/um/x86_64 /libpath:${XWIN_DIR}/sdk/lib/ucrt/x86_64")
set(CMAKE_SHARED_LINKER_FLAGS_INIT "/libpath:${XWIN_DIR}/crt/lib/x86_64 /libpath:${XWIN_DIR}/sdk/lib/um/x86_64 /libpath:${XWIN_DIR}/sdk/lib/ucrt/x86_64")

# Force release-style runtime for all build types
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded" CACHE STRING "" FORCE)
