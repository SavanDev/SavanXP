# A SavanXP program built with CMake against the SDK sysroot. build.sh next to
# it configures this with the SDK's toolchain file:
#
#   cmake -S sdk/cmakesmoke -B build/external/cmakesmoke -G Ninja \
#       -DCMAKE_TOOLCHAIN_FILE=build/sdk/2.0/share/sdk/savanxp-toolchain.cmake
cmake_minimum_required(VERSION 3.20)
project(cmakesmoke C)

add_executable(cmakesmoke main.c)
# The runtime links on its own; libm is a group the program names.
target_link_libraries(cmakesmoke PRIVATE m)
