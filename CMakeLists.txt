cmake_minimum_required(VERSION 3.24)

project(nylon LANGUAGES CXX)

if(NOT WIN32)
	message(FATAL_ERROR "Nylon is a Windows-only program.")
endif()

add_executable(nylon
	src/main.cpp
	src/platform/win32.cpp
	src/database/database.cpp
	src/image/image.cpp
	src/registry/hive.cpp
	src/registry/registry.cpp
)

target_compile_features(nylon PRIVATE cxx_std_20)

target_compile_definitions(nylon PRIVATE
	UNICODE
	_UNICODE
	WIN32_LEAN_AND_MEAN
	NOMINMAX
	_WIN32_WINNT=0x0A00
)

target_include_directories(nylon PRIVATE src)

target_link_libraries(nylon PRIVATE winsqlite3)

if(MSVC)
	target_compile_options(nylon PRIVATE
		/W4
		/WX
		/permissive-
		/utf-8
		/sdl
		/EHsc
		/Zc:preprocessor
		/Zc:__cplusplus
		/Zc:inline
		/Zc:throwingNew
		/guard:cf
	)

	target_link_options(nylon PRIVATE
		/guard:cf
		/DYNAMICBASE
		/NXCOMPAT
	)
endif()

set_target_properties(nylon PROPERTIES
	RUNTIME_OUTPUT_DIRECTORY "${CMAKE_SOURCE_DIR}/bin"
	RUNTIME_OUTPUT_DIRECTORY_DEBUG "${CMAKE_SOURCE_DIR}/bin"
	RUNTIME_OUTPUT_DIRECTORY_RELEASE "${CMAKE_SOURCE_DIR}/bin"
)
