cmake_minimum_required(VERSION 3.16)
project(chuart C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

if(NOT CMAKE_BUILD_TYPE)
	set(CMAKE_BUILD_TYPE RelWithDebInfo)
endif()

add_compile_options(-Wall -Wextra -Wshadow -Wpointer-arith -Wstrict-prototypes)

# GNUInstallDirs gives us the distribution's own idea of bin/lib directories
# (lib vs lib64 vs lib/<multiarch>), so installs are not Debian-specific.
include(GNUInstallDirs)
find_package(Threads REQUIRED)

# --------------------------------------------------------------- dependencies --
# Prefer pkg-config, but fall back to plain searches so that a minimal system
# without pkg-config can still build.
find_package(PkgConfig QUIET)
if(PkgConfig_FOUND)
	pkg_check_modules(FUSE3 QUIET fuse3)
	pkg_check_modules(GLIB QUIET glib-2.0 gio-2.0 gobject-2.0)
endif()

if(NOT FUSE3_FOUND)
	find_path(FUSE3_INCLUDE_DIRS fuse_lowlevel.h PATH_SUFFIXES fuse3)
	find_library(FUSE3_LIBRARIES NAMES fuse3)
endif()

if(NOT GLIB_FOUND)
	find_path(GLIB_INCLUDE_DIRS glib.h PATH_SUFFIXES glib-2.0)
	# glibconfig.h is architecture dependent and can live outside the usual
	# include directory: /usr/lib/<multiarch>/glib-2.0/include on Debian,
	# /usr/lib64/glib-2.0/include on Fedora.
	find_path(GLIB_CONFIG_INCLUDE_DIRS glibconfig.h
		PATH_SUFFIXES glib-2.0/include
			      lib/glib-2.0/include
			      lib64/glib-2.0/include
			      lib/${CMAKE_LIBRARY_ARCHITECTURE}/glib-2.0/include)
	if(GLIB_CONFIG_INCLUDE_DIRS)
		list(APPEND GLIB_INCLUDE_DIRS ${GLIB_CONFIG_INCLUDE_DIRS})
	endif()
	set(GLIB_LIBRARIES "")
	foreach(_glib_lib glib-2.0 gobject-2.0 gio-2.0)
		find_library(GLIB_${_glib_lib}_LIBRARY NAMES ${_glib_lib})
		list(APPEND GLIB_LIBRARIES ${GLIB_${_glib_lib}_LIBRARY})
	endforeach()
endif()

find_library(BLUETOOTH_LIBRARIES NAMES bluetooth)
find_path(BLUETOOTH_INCLUDE_DIRS bluetooth/bluetooth.h)

set(_missing "")
foreach(_item FUSE3_INCLUDE_DIRS FUSE3_LIBRARIES GLIB_INCLUDE_DIRS
		GLIB_LIBRARIES BLUETOOTH_LIBRARIES BLUETOOTH_INCLUDE_DIRS)
	if(NOT ${_item})
		list(APPEND _missing ${_item})
	endif()
endforeach()
if(NOT GLIB_FOUND AND NOT GLIB_CONFIG_INCLUDE_DIRS)
	list(APPEND _missing "glibconfig.h (libglib2.0-dev, glib2-devel, ...)")
endif()
if(_missing)
	message(FATAL_ERROR
		"missing dependencies: ${_missing}\n"
		"Install them first, for example:\n"
		"  Debian/Ubuntu : apt install libfuse3-dev libglib2.0-dev libbluetooth-dev\n"
		"  Fedora/RHEL   : dnf install fuse3-devel glib2-devel bluez-libs-devel\n"
		"  Arch          : pacman -S fuse3 glib2 bluez-libs\n"
		"  openSUSE      : zypper install fuse3-devel glib2-devel bluez-devel")
endif()

# The vendor library ships next to the sources and is linked by name.
set(CH9140LIB_DIR "${CMAKE_CURRENT_SOURCE_DIR}/src/lib")

add_executable(chuart
	src/main.c
	src/devices.c
	src/log.c
	src/ring.c
	src/ble.c
	src/cuse_tty.c
	src/tty_params.c
	src/ui.c
)

target_include_directories(chuart PRIVATE
	src
	${FUSE3_INCLUDE_DIRS}
	${GLIB_INCLUDE_DIRS}
	${BLUETOOTH_INCLUDE_DIRS}
)
# The vendor header is not -Wall clean, so keep it out of our warning set.
target_include_directories(chuart SYSTEM PRIVATE ${CH9140LIB_DIR})

target_link_directories(chuart PRIVATE ${CH9140LIB_DIR})
target_link_libraries(chuart PRIVATE
	CH9140lib
	${FUSE3_LIBRARIES}
	${GLIB_LIBRARIES}
	${BLUETOOTH_LIBRARIES}
	Threads::Threads
)

# Run straight from the build tree without LD_LIBRARY_PATH.
set_target_properties(chuart PROPERTIES BUILD_RPATH "${CH9140LIB_DIR}")

# Installed layout: <prefix>/bin/chuart plus libCH9140lib.so in the
# distribution's library directory.  The relative RPATH works whichever
# convention that directory follows; an absolute library dir overrides it.
if(IS_ABSOLUTE "${CMAKE_INSTALL_LIBDIR}")
	set(_install_rpath "${CMAKE_INSTALL_LIBDIR}")
else()
	set(_install_rpath "$ORIGIN/../${CMAKE_INSTALL_LIBDIR}")
endif()
set_target_properties(chuart PROPERTIES
	INSTALL_RPATH "${_install_rpath}"
	INSTALL_RPATH_USE_LINK_PATH TRUE
)

install(TARGETS chuart RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
install(FILES src/lib/libCH9140lib.so DESTINATION ${CMAKE_INSTALL_LIBDIR})
