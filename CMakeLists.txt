cmake_minimum_required(VERSION 3.24)

project(nylon LANGUAGES C CXX)

if(NOT WIN32)
	message(FATAL_ERROR "Nylon is a Windows-only program.")
endif()

if(NOT MINGW OR NOT CMAKE_CXX_COMPILER_ID MATCHES "Clang")
	message(FATAL_ERROR "Nylon uses LLVM-MinGW. Run build.cmd for the portable build.")
endif()

set(SQLITE_DIR "${CMAKE_SOURCE_DIR}/tools/sqlite")
if(NOT EXISTS "${SQLITE_DIR}/sqlite3.c" OR NOT EXISTS "${SQLITE_DIR}/sqlite3.h")
	message(FATAL_ERROR "SQLite is not bootstrapped. Run build.cmd once first.")
endif()

file(GLOB_RECURSE NYLON_CPP CONFIGURE_DEPENDS
	"${CMAKE_SOURCE_DIR}/src/*.cpp"
)

add_executable(nylon
	${NYLON_CPP}
	"${SQLITE_DIR}/sqlite3.c"
)

target_compile_features(nylon PRIVATE cxx_std_20)

target_compile_definitions(nylon PRIVATE
	UNICODE
	_UNICODE
	WIN32_LEAN_AND_MEAN
	NOMINMAX
	_WIN32_WINNT=0x0A00
	SQLITE_OMIT_LOAD_EXTENSION
	SQLITE_DQS=0
	SQLITE_DEFAULT_MEMSTATUS=0
)

target_include_directories(nylon PRIVATE
	src
	"${SQLITE_DIR}"
)

target_compile_options(nylon PRIVATE
	$<$<COMPILE_LANGUAGE:CXX>:-Wall;-Wextra;-Wpedantic;-Werror;-fstack-protector-strong;-ffunction-sections;-fdata-sections;-mguard=cf>
	$<$<COMPILE_LANGUAGE:C>:-fstack-protector-strong;-ffunction-sections;-fdata-sections;-mguard=cf>
)

target_link_libraries(nylon PRIVATE bcrypt)

target_link_options(nylon PRIVATE
	-static
	-fuse-ld=lld
	-mguard=cf
	-Wl,--gc-sections
	-Wl,--dynamicbase
	-Wl,--nxcompat
	-Wl,--high-entropy-va
)

set_target_properties(nylon PROPERTIES
	RUNTIME_OUTPUT_DIRECTORY "${CMAKE_SOURCE_DIR}/bin"
)
