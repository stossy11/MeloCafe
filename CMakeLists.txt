cmake_minimum_required(VERSION 3.21.1)

option(ENABLE_VCPKG "Enable the vcpkg package manager" ON)
option(MACOS_BUNDLE "The executable when built on macOS will be created as an application bundle" OFF)
option(ALLOW_PORTABLE "Allow Cemu to be run in portable mode" ON)

# used by CI script to set version:
set(EMULATOR_VERSION_MAJOR "0" CACHE STRING "")
set(EMULATOR_VERSION_MINOR "0" CACHE STRING "")
set(EMULATOR_VERSION_PATCH "0" CACHE STRING "")

set(THREADS_PREFER_PTHREAD_FLAG true) # needs to be set before vcpkg

if(CMAKE_SYSTEM_NAME STREQUAL "iOS")
	set(CMAKE_OSX_DEPLOYMENT_TARGET "15.0" CACHE STRING "Minimum iOS deployment target" FORCE)
	set(CMAKE_XCODE_ATTRIBUTE_IPHONEOS_DEPLOYMENT_TARGET "15.0" CACHE STRING "Minimum iOS deployment target" FORCE)
	list(PREPEND VCPKG_OVERLAY_TRIPLETS "${CMAKE_CURRENT_LIST_DIR}/triplets")
	list(REMOVE_DUPLICATES VCPKG_OVERLAY_TRIPLETS)
	set(VCPKG_OVERLAY_TRIPLETS "${VCPKG_OVERLAY_TRIPLETS}" CACHE STRING "Project vcpkg overlay triplets" FORCE)

if(CMAKE_OSX_SYSROOT STREQUAL "iphonesimulator")
	set(VCPKG_TARGET_TRIPLET "arm64-ios-simulator" CACHE STRING "vcpkg target triplet" FORCE)
else()
	set(VCPKG_TARGET_TRIPLET "arm64-ios" CACHE STRING "vcpkg target triplet" FORCE)
endif()

endif()


execute_process(
		COMMAND git log --format=%h -1
		WORKING_DIRECTORY ${CMAKE_CURRENT_LIST_DIR}
		OUTPUT_VARIABLE GIT_HASH
		OUTPUT_STRIP_TRAILING_WHITESPACE
)
add_definitions(-DEMULATOR_HASH=${GIT_HASH})

if (ENABLE_VCPKG)
	# check if vcpkg is shallow and unshallow it if necessary
	execute_process(
	COMMAND git rev-parse --is-shallow-repository
	WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}/dependencies/vcpkg
	OUTPUT_VARIABLE is_vcpkg_shallow
	OUTPUT_STRIP_TRAILING_WHITESPACE
    )

	if(is_vcpkg_shallow STREQUAL "true")
		message(STATUS "vcpkg is shallow. Unshallowing it now...")
		execute_process(
			COMMAND git fetch --unshallow
			WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}/dependencies/vcpkg"
			RESULT_VARIABLE result
			OUTPUT_VARIABLE output
		)
	endif()

	if(UNIX AND NOT APPLE)
		set(VCPKG_OVERLAY_PORTS "${CMAKE_CURRENT_LIST_DIR}/dependencies/vcpkg_overlay_ports_linux;${CMAKE_CURRENT_LIST_DIR}/dependencies/vcpkg_overlay_ports")
	elseif(APPLE)
		set(VCPKG_OVERLAY_PORTS "${CMAKE_CURRENT_LIST_DIR}/dependencies/vcpkg_overlay_ports_mac;${CMAKE_CURRENT_LIST_DIR}/dependencies/vcpkg_overlay_ports")
	else()
		set(VCPKG_OVERLAY_PORTS "${CMAKE_CURRENT_LIST_DIR}/dependencies/vcpkg_overlay_ports_win;${CMAKE_CURRENT_LIST_DIR}/dependencies/vcpkg_overlay_ports")
	endif()
	set(CMAKE_TOOLCHAIN_FILE "${CMAKE_CURRENT_SOURCE_DIR}/dependencies/vcpkg/scripts/buildsystems/vcpkg.cmake"
		CACHE STRING "Vcpkg toolchain file")
	# Set this so that all the various find_package() calls don't need an explicit
	# CONFIG option
	set(CMAKE_FIND_PACKAGE_PREFER_CONFIG TRUE)
	if (WIN32)
		set(VCPKG_TARGET_TRIPLET "x64-windows-static" CACHE STRING "")
	endif()
endif()

project(Cemu VERSION 2.0.0)

list(APPEND CMAKE_MODULE_PATH "${PROJECT_SOURCE_DIR}/cmake")

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

add_compile_definitions($<$<CONFIG:Debug>:CEMU_DEBUG_ASSERT>) # if build type is debug, set CEMU_DEBUG_ASSERT

add_definitions(-DEMULATOR_VERSION_MAJOR=${EMULATOR_VERSION_MAJOR})
add_definitions(-DEMULATOR_VERSION_MINOR=${EMULATOR_VERSION_MINOR})
add_definitions(-DEMULATOR_VERSION_PATCH=${EMULATOR_VERSION_PATCH})

set_property(GLOBAL PROPERTY USE_FOLDERS ON)

# enable link time optimization for release builds
set(CMAKE_INTERPROCEDURAL_OPTIMIZATION_RELEASE ON)
set(CMAKE_INTERPROCEDURAL_OPTIMIZATION_RELWITHDEBINFO ON)

if (MSVC)
	set_property(DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR} PROPERTY VS_STARTUP_PROJECT CemuBin)
	# floating point model: precise, fiber safe optimizations
	add_compile_options(/EHsc /fp:precise)
	if (CMAKE_CXX_COMPILER_ID STREQUAL "Clang")
		# Speeds up static linking (especially helpful in incremental compilation)
		if((CMAKE_LINKER MATCHES ".*lld-link.*") AND (CMAKE_AR MATCHES ".*llvm-lib.*"))
			set_property(DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR} PROPERTY STATIC_LIBRARY_OPTIONS /llvmlibthin)
		endif()
  	else()
	  add_compile_options(/GT)
	endif()
	# enable additional optimization flags for release builds
	add_compile_options($<$<CONFIG:Release,RelWithDebInfo>:/Oi>) # enable intrinsic functions
	add_compile_options($<$<CONFIG:Release,RelWithDebInfo>:/Ot>) # favor speed
endif()

if(CMAKE_GENERATOR STREQUAL "Xcode")
	set_property(DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR} PROPERTY XCODE_STARTUP_PROJECT CemuBin)
	# YES selects monolithic LTO in Xcode; YES_THIN selects ThinLTO.
	set(CMAKE_XCODE_ATTRIBUTE_LLVM_LTO "$<IF:$<CONFIG:Release,RelWithDebInfo>,YES,NO>")
endif()

if (APPLE)
    enable_language(OBJC OBJCXX)

    # CMake defaults to ThinLTO for Clang. Use monolithic LTO for IPO builds.
    foreach(lang C CXX OBJC OBJCXX)
        if(CMAKE_${lang}_COMPILER_ID MATCHES "^(AppleClang|Clang)$")
            set(CMAKE_${lang}_COMPILE_OPTIONS_IPO "-flto=full")
        endif()
    endforeach()

    if(CMAKE_SYSTEM_NAME STREQUAL "iOS")
        set(IOS TRUE)
        set(CMAKE_OSX_DEPLOYMENT_TARGET "15.0")
        set(CMAKE_XCODE_ATTRIBUTE_IPHONEOS_DEPLOYMENT_TARGET "15.0")
    else()
        set(IOS FALSE)
        set(CMAKE_OSX_DEPLOYMENT_TARGET "13.4")
    endif()
endif()

if (UNIX AND NOT APPLE)
	option(ENABLE_WAYLAND "Build with Wayland support" ON)
	option(ENABLE_FERAL_GAMEMODE "Enables Feral Interactive GameMode Support" ON)
	option(ENABLE_BLUEZ "Build with Bluez support" ON)
endif()

if (APPLE)
    set(ENABLE_METAL_DEFAULT ON)
	set(ENABLE_OPENGL_DEFAULT OFF)
else()
    set(ENABLE_METAL_DEFAULT OFF)
	set(ENABLE_OPENGL_DEFAULT ON)
endif()

option(ENABLE_OPENGL "Enables the OpenGL backend" ${ENABLE_OPENGL_DEFAULT})
option(ENABLE_VULKAN "Enables the Vulkan backend" ON)
option(ENABLE_METAL "Enables the Metal backend" ${ENABLE_METAL_DEFAULT})
option(ENABLE_DISCORD_RPC "Enables the Discord Rich Presence feature" ON)
option(ENABLE_APPSTORE "Causes all Private API usages to be subbed out." OFF)

if (IOS)
	set(ENABLE_OPENGL OFF CACHE BOOL "OpenGL is not supported on iOS" FORCE)
	set(ENABLE_VULKAN ON CACHE BOOL "Vulkan is enabled for MeloCafe on iOS" FORCE)
	set(ENABLE_METAL ON CACHE BOOL "Metal is required on iOS" FORCE)
	set(ENABLE_DISCORD_RPC OFF CACHE BOOL "Discord RPC is not available on iOS" FORCE)
	set(ENABLE_HIDAPI OFF CACHE BOOL "HIDAPI is not available on iOS" FORCE)
	set(ENABLE_LIBUSB OFF CACHE BOOL "libusb is not available on iOS" FORCE)
	set(BUILD_HEADLESS_DYLIB ON CACHE BOOL "Build CemuBin as a framework on iOS" FORCE)
endif()

if (ENABLE_METAL AND NOT APPLE)
    message(FATAL_ERROR "Metal backend is only supported on Apple platforms")
endif()

if (ENABLE_APPSTORE AND NOT APPLE) 
	set(ENABLE_APPSTORE OFF CACHE BOOL "This is only needed for Apple platforms." FORCE)
endif()


# input backends
if (WIN32)
	option(ENABLE_XINPUT "Enables the usage of XInput" ON)
	option(ENABLE_DIRECTINPUT "Enables the usage of DirectInput" ON)
	add_compile_definitions(HAS_DIRECTINPUT)
endif()

option(ENABLE_SDL "Enables the SDLController backend" ON)
option(ENABLE_HIDAPI "Build with HIDAPI" ON)
option(ENABLE_LIBUSB "Build with libusb support" ON)

if (ENABLE_LIBUSB)
	add_compile_definitions(HAS_LIBUSB)
endif()

# audio backends
if (WIN32)
	option(ENABLE_DIRECTAUDIO "Enables the directaudio backend" ON)
	option(ENABLE_XAUDIO "Enables the xaudio backend" ON)
endif()
if (IOS)
	set(ENABLE_CUBEB_DEFAULT OFF)
	set(ENABLE_WXWIDGETS_DEFAULT OFF)
else()
	set(ENABLE_CUBEB_DEFAULT ON)
	set(ENABLE_WXWIDGETS_DEFAULT ON)
endif()

option(ENABLE_CUBEB "Enabled cubeb backend" ${ENABLE_CUBEB_DEFAULT})

option(ENABLE_WXWIDGETS "Build with wxWidgets UI" ${ENABLE_WXWIDGETS_DEFAULT})

find_package(Threads REQUIRED)

if(CMAKE_GENERATOR STREQUAL "Xcode")
	set(Z_VCPKG_OPENSSL_USE_SINGLE_CONFIG 1)
endif()

find_package(CURL REQUIRED)
find_package(pugixml REQUIRED)
find_package(RapidJSON REQUIRED)
find_package(Boost COMPONENTS program_options filesystem nowide context REQUIRED)
find_package(libzip REQUIRED)
find_package(glslang REQUIRED)
find_package(ZLIB REQUIRED)
find_package(zstd MODULE REQUIRED) # MODULE so that zstd::zstd is available
find_package(OpenSSL COMPONENTS Crypto SSL REQUIRED)
find_package(glm REQUIRED)
find_package(fmt 12.1 REQUIRED)
find_package(PNG REQUIRED)

if(ENABLE_SDL)
	find_package(SDL3 REQUIRED CONFIG REQUIRED COMPONENTS SDL3)
	add_compile_definitions(HAS_SDL)
endif()

# glslang versions older than 11.11.0 define targets without a namespace
if (NOT TARGET glslang::SPIRV AND TARGET SPIRV)
	add_library(glslang::SPIRV ALIAS SPIRV)
endif()

if (UNIX AND NOT APPLE)
	find_package(X11 REQUIRED)
	if (ENABLE_WAYLAND)
		find_package(Wayland REQUIRED Client)
		find_package(WaylandScanner REQUIRED)
		find_package(WaylandProtocols 1.15 REQUIRED)

		ecm_add_wayland_client_protocol(WAYLAND_PROTOCOL_SRCS
			PROTOCOL "${WaylandProtocols_DATADIR}/stable/viewporter/viewporter.xml"
			BASENAME viewporter)
		add_library(CemuWaylandProtocols STATIC ${WAYLAND_PROTOCOL_SRCS})
		target_include_directories(CemuWaylandProtocols PUBLIC "${CMAKE_CURRENT_BINARY_DIR}")
		target_include_directories(CemuWaylandProtocols PRIVATE ${Wayland_INCLUDE_DIRS})
		add_compile_definitions(HAS_WAYLAND)
	endif()
	find_package(GTK3 REQUIRED)

	if(ENABLE_BLUEZ)
		find_package(bluez REQUIRED)
		set(SUPPORTS_WIIMOTE ON)
		add_compile_definitions(HAS_BLUEZ)
	endif()

endif()

if (ENABLE_VULKAN)
	include_directories("dependencies/Vulkan-Headers/include")
	add_compile_definitions(ENABLE_VULKAN)
	add_compile_definitions(VK_NO_PROTOTYPES)
endif()

if (ENABLE_OPENGL)
	find_package(OpenGL REQUIRED)
	add_compile_definitions(ENABLE_OPENGL)
endif()

if (ENABLE_METAL)
    include_directories(${CMAKE_SOURCE_DIR}/dependencies/metal-cpp)

    add_compile_definitions(ENABLE_METAL)
endif()

if (ENABLE_APPSTORE)
	# forces stuff like disabling debugger checking, entitlement checking, etc
	add_compile_definitions(DIST_APPSTORE)
endif()


if (IOS AND ENABLE_IOS_NATIVE_RECORDING)
	add_compile_definitions(ENABLE_IOS_NATIVE_RECORDING)
endif()

if (ENABLE_DISCORD_RPC)
	add_compile_definitions(ENABLE_DISCORD_RPC)
endif()

if (ENABLE_HIDAPI)
	set(SUPPORTS_WIIMOTE ON)
	add_compile_definitions(HAS_HIDAPI)
endif ()

if(UNIX AND NOT APPLE)
	if(ENABLE_FERAL_GAMEMODE)
		add_compile_definitions(ENABLE_FERAL_GAMEMODE)
		add_subdirectory(dependencies/gamemode EXCLUDE_FROM_ALL)
		target_include_directories(gamemode INTERFACE ./dependencies/gamemode/lib)
	endif()
endif()

if (ENABLE_WXWIDGETS)
	find_package(wxWidgets 3.3 REQUIRED COMPONENTS base core gl propgrid xrc)
	add_compile_definitions(ENABLE_WXWIDGETS)
endif()

if (ENABLE_CUBEB)
	if (NOT ENABLE_VCPKG)
	    find_package(cubeb)
	endif()
	if (NOT cubeb_FOUND)
		option(BUILD_TESTS "" OFF)
		option(BUILD_TOOLS "" OFF)
		option(BUNDLE_SPEEX "" OFF)
		set(USE_WINMM OFF CACHE BOOL "")
		add_subdirectory("dependencies/cubeb" EXCLUDE_FROM_ALL SYSTEM)
		set_property(TARGET cubeb PROPERTY MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
		add_library(cubeb::cubeb ALIAS cubeb)
	endif()
	add_compile_definitions("HAS_CUBEB=1")
endif()

add_subdirectory("dependencies/ih264d" EXCLUDE_FROM_ALL)

if (CMAKE_OSX_ARCHITECTURES)
	set(CEMU_ARCHITECTURE ${CMAKE_OSX_ARCHITECTURES})
else()
	set(CEMU_ARCHITECTURE ${CMAKE_SYSTEM_PROCESSOR})
endif()
if(CEMU_ARCHITECTURE MATCHES "(aarch64)|(AARCH64)|(arm64)|(ARM64)")
	add_subdirectory("dependencies/xbyak_aarch64" EXCLUDE_FROM_ALL)
endif()

find_package(ZArchive QUIET)
if (NOT ZArchive_FOUND)
	add_subdirectory("dependencies/ZArchive" EXCLUDE_FROM_ALL)
endif()

# Profile guided optimization (clang only): CEMU_PGO_PROFILE=/path/to/cemu.profdata optimizes with a merged profile.
set(CEMU_PGO_PROFILE "" CACHE FILEPATH "Merged .profdata file to optimize with")
if (CEMU_PGO_PROFILE)
	if (NOT CMAKE_CXX_COMPILER_ID MATCHES "Clang")
		message(FATAL_ERROR "PGO options need clang")
	endif()
	if (NOT EXISTS "${CEMU_PGO_PROFILE}")
		message(FATAL_ERROR "CEMU_PGO_PROFILE ${CEMU_PGO_PROFILE} does not exist")
	endif()
	add_compile_options("-fprofile-instr-use=${CEMU_PGO_PROFILE}" -Wno-profile-instr-unprofiled -Wno-profile-instr-out-of-date)
endif()

add_subdirectory(src)
