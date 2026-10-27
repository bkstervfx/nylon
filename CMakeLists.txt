cmake_minimum_required(VERSION 3.25)

project(nylon LANGUAGES CXX)

if(NOT WIN32)
	message(FATAL_ERROR "Nylon is Windows-only.")
endif()

add_executable(nylon
	src/main.cpp
	src/appx/appx.cpp
	src/com/com.cpp
	src/database/database.cpp
	src/filesystem/filesystem.cpp
	src/image/image.cpp
	src/pe/pe.cpp
	src/registry/registry.cpp
	src/servicing/servicing.cpp
	src/services/services.cpp
	src/shell/shell.cpp
	src/tasks/tasks.cpp
	src/win32/delta.cpp
	src/win32/dism.cpp
	src/xml/xml.cpp
)

target_compile_features(nylon PRIVATE cxx_std_20)

target_compile_definitions(nylon PRIVATE
	UNICODE
	_UNICODE
	WIN32_LEAN_AND_MEAN
	NOMINMAX
	_WIN32_WINNT=0x0A00
)

target_compile_options(nylon PRIVATE
	$<$<CXX_COMPILER_ID:MSVC>:/W4>
	$<$<CXX_COMPILER_ID:MSVC>:/WX>
	$<$<CXX_COMPILER_ID:MSVC>:/permissive->
	$<$<CXX_COMPILER_ID:MSVC>:/sdl>
	$<$<CXX_COMPILER_ID:MSVC>:/Zc:__cplusplus>
	$<$<CXX_COMPILER_ID:MSVC>:/utf-8>
)

target_link_libraries(nylon PRIVATE
	advapi32
	shlwapi
	xmllite
	winsqlite3
)

set_target_properties(nylon PROPERTIES
	RUNTIME_OUTPUT_DIRECTORY "${CMAKE_SOURCE_DIR}/bin"
	RUNTIME_OUTPUT_DIRECTORY_DEBUG "${CMAKE_SOURCE_DIR}/bin"
	RUNTIME_OUTPUT_DIRECTORY_RELEASE "${CMAKE_SOURCE_DIR}/bin"
)
