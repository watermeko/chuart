cmake_minimum_required(VERSION 3.16)
project(chuart C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

if(NOT CMAKE_BUILD_TYPE)
	set(CMAKE_BUILD_TYPE RelWithDebInfo)
endif()

add_compile_options(-Wall -Wextra -Wshadow -Wpointer-arith -Wstrict-prototypes)

find_package(PkgConfig REQUIRED)
find_package(Threads REQUIRED)

pkg_check_modules(FUSE3 REQUIRED fuse3)
pkg_check_modules(GLIB REQUIRED glib-2.0 gio-2.0 gobject-2.0)

# libCH9140lib.so lives in the source tree; libbluetooth provides the UUID
# types used by CH9140lib.h.
set(CH9140LIB_DIR "${CMAKE_CURRENT_SOURCE_DIR}/src/lib")
find_library(BLUETOOTH_LIB bluetooth REQUIRED)

add_executable(chuart
	src/main.c
	src/log.c
	src/ring.c
	src/ble.c
	src/cuse_tty.c
)

target_include_directories(chuart PRIVATE
	src
	${FUSE3_INCLUDE_DIRS}
	${GLIB_INCLUDE_DIRS}
)

# The vendor header is not -Wall clean (e.g. `int f();`), so keep it out of our
# warning set.
target_include_directories(chuart SYSTEM PRIVATE ${CH9140LIB_DIR})

target_link_directories(chuart PRIVATE ${CH9140LIB_DIR})
target_link_libraries(chuart PRIVATE
	CH9140lib
	${FUSE3_LIBRARIES}
	${GLIB_LIBRARIES}
	${BLUETOOTH_LIB}
	Threads::Threads
)

# Run straight from the build tree without LD_LIBRARY_PATH.
set_target_properties(chuart PROPERTIES
	BUILD_RPATH "${CH9140LIB_DIR}"
	INSTALL_RPATH "$ORIGIN/../lib"
	INSTALL_RPATH_USE_LINK_PATH TRUE
)

install(TARGETS chuart RUNTIME DESTINATION bin)
install(FILES src/lib/libCH9140lib.so DESTINATION lib)
