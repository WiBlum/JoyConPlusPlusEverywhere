# CMakeList.txt : CMake project for JoyCon++, include source and define
# project specific logic here.
#
cmake_minimum_required (VERSION 3.8)

# Enable Hot Reload for MSVC compilers if supported.
if (POLICY CMP0141)
  cmake_policy(SET CMP0141 NEW)
  set(CMAKE_MSVC_DEBUG_INFORMATION_FORMAT "$<IF:$<AND:$<C_COMPILER_ID:MSVC>,$<CXX_COMPILER_ID:MSVC>>,$<$<CONFIG:Debug,RelWithDebInfo>:EditAndContinue>,$<$<CONFIG:Debug,RelWithDebInfo>:ProgramDatabase>>")
endif()

project ("JoyCon++")

find_package(hidapi CONFIG REQUIRED)


# Add source to this project's executable.

add_executable(JoyCon++
  "JoyCon++.cpp"
  "JoyCon++.h"
 "src/bluetooth.cpp"
"src/bluetooth.h"
 "src/joycon.cpp"
"src/joycon.h"
"src/constants.h"
 )

target_link_libraries(JoyCon++ PRIVATE
  Bthprops.lib      # Windows Bluetooth Classic
  windowsapp.lib    # WinRT core (for BLE via C++/WinRT)
  hidapi::hidapi
)

if (CMAKE_VERSION VERSION_GREATER 3.12)
  set_property(TARGET JoyCon++ PROPERTY CXX_STANDARD 20)
endif()

# TODO: Add tests and install targets if needed.
