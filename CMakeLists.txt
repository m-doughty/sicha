cmake_minimum_required(VERSION 3.16)

project(sicha
	VERSION 0.1.0
	DESCRIPTION "LLM chat-completion client with retries and fallback routing"
	LANGUAGES C)

option(SICHA_BUILD_TESTS "Build the sicha test suite" ON)
option(SICHA_BUILD_BENCH "Build the benchmark / counter-gate binary" OFF)
option(SICHA_WITH_CURL "Build the built-in libcurl transport" ON)
option(SICHA_FUZZ "Build the libFuzzer harnesses (requires Clang)" OFF)
option(SICHA_WERROR "Treat warnings as errors" OFF)
option(SICHA_SANITIZE "Build with AddressSanitizer + UBSanitizer" OFF)
option(SICHA_TSAN "Build with ThreadSanitizer" OFF)

if(SICHA_SANITIZE AND SICHA_TSAN)
	message(FATAL_ERROR "SICHA_SANITIZE and SICHA_TSAN are mutually exclusive")
endif()
if(SICHA_FUZZ AND NOT CMAKE_C_COMPILER_ID MATCHES "Clang")
	message(FATAL_ERROR "SICHA_FUZZ requires Clang (libFuzzer)")
endif()

set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# Strict -std=c17 (C_EXTENSIONS OFF) makes glibc hide POSIX interfaces
# (clock_gettime, CLOCK_*, nanosleep, pthreads extras) unless a
# feature-test macro asks for them; _DEFAULT_SOURCE / _DARWIN_C_SOURCE
# keep the glibc / macOS default namespaces intact alongside it.  On
# Windows, hush MSVC's CRT deprecation layer (fopen, getenv, ...).
if(WIN32)
	add_compile_definitions(_CRT_SECURE_NO_WARNINGS)
else()
	add_compile_definitions(_POSIX_C_SOURCE=200809L _DEFAULT_SOURCE
		_DARWIN_C_SOURCE)
endif()

if(MSVC)
	set(SICHA_C_WARNINGS /W4)
	if(SICHA_WERROR)
		list(APPEND SICHA_C_WARNINGS /WX)
	endif()
else()
	set(SICHA_C_WARNINGS -Wall -Wextra -Wshadow -Wvla)
	if(SICHA_WERROR)
		list(APPEND SICHA_C_WARNINGS -Werror)
	endif()
endif()

set(SICHA_SAN_FLAGS "")
if(SICHA_SANITIZE)
	if(MSVC)
		message(FATAL_ERROR "SICHA_SANITIZE is not supported with MSVC")
	endif()
	set(SICHA_SAN_FLAGS -fsanitize=address,undefined -fno-sanitize-recover=all)
endif()
if(SICHA_TSAN)
	if(MSVC)
		message(FATAL_ERROR "SICHA_TSAN is not supported with MSVC")
	endif()
	set(SICHA_SAN_FLAGS -fsanitize=thread)
endif()
if(SICHA_FUZZ)
	# the library needs matching instrumentation for the fuzz targets
	set(SICHA_SAN_FLAGS -fsanitize=fuzzer-no-link,address,undefined
		-fno-sanitize-recover=all)
	set(SICHA_SAN_LINK_FLAGS -fsanitize=address,undefined)
else()
	set(SICHA_SAN_LINK_FLAGS ${SICHA_SAN_FLAGS})
endif()

# One set of objects feeds two targets: the real library (`sicha`,
# static or shared per BUILD_SHARED_LIBS, hidden visibility) and a
# static archive (`sicha_whitebox`) that tests and fuzzers link so
# they can reach internal modules even when the real target is a
# shared library that only exports the public API.
set(SICHA_SOURCES
	src/status.c
	src/buf.c
	src/sse.c
	src/classify.c
	src/response.c
	src/cancel.c
	src/transport_script.c
	src/client.c
	src/request.c
	src/yyjson.c)
if(SICHA_WITH_CURL)
	find_package(CURL REQUIRED)
	list(APPEND SICHA_SOURCES src/transport_curl.c)
endif()

add_library(sicha_obj OBJECT ${SICHA_SOURCES})

set_target_properties(sicha_obj PROPERTIES
	C_STANDARD 17
	C_STANDARD_REQUIRED ON
	C_EXTENSIONS OFF
	C_VISIBILITY_PRESET hidden
	POSITION_INDEPENDENT_CODE ON)
target_include_directories(sicha_obj PUBLIC
	$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>)
# dllexport annotations only make sense when actually building a DLL;
# in static builds they leak into the archives and confuse MSVC test
# links (__imp_ aliases, .exp side-products)
if(BUILD_SHARED_LIBS)
	target_compile_definitions(sicha_obj PRIVATE SICHA_BUILD)
else()
	target_compile_definitions(sicha_obj PRIVATE SICHA_STATIC)
endif()
if(SICHA_WITH_CURL)
	target_compile_definitions(sicha_obj PRIVATE SICHA_WITH_CURL)
	target_link_libraries(sicha_obj PRIVATE CURL::libcurl)
endif()
target_compile_options(sicha_obj PRIVATE ${SICHA_C_WARNINGS}
	${SICHA_SAN_FLAGS})
if(MSVC)
	# vendored yyjson is built at /W3; sicha's own code stays at /W4
	set_source_files_properties(src/yyjson.c PROPERTIES
		COMPILE_OPTIONS /W3)
endif()

add_library(sicha $<TARGET_OBJECTS:sicha_obj>)
add_library(sicha_whitebox STATIC $<TARGET_OBJECTS:sicha_obj>)

foreach(lib sicha sicha_whitebox)
	target_include_directories(${lib} PUBLIC
		$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
		$<INSTALL_INTERFACE:include>)
	if(NOT WIN32)
		set(THREADS_PREFER_PTHREAD_FLAG ON)
		find_package(Threads REQUIRED)
		target_link_libraries(${lib} PUBLIC Threads::Threads)
	endif()
	if(SICHA_WITH_CURL)
		target_link_libraries(${lib} PUBLIC CURL::libcurl)
	endif()
	if(SICHA_SAN_FLAGS)
		target_compile_options(${lib} INTERFACE ${SICHA_SAN_FLAGS})
		target_link_options(${lib} PUBLIC ${SICHA_SAN_LINK_FLAGS})
	endif()
endforeach()

# consumers of the whitebox archive always link statically
target_compile_definitions(sicha_whitebox INTERFACE SICHA_STATIC)
if(NOT BUILD_SHARED_LIBS)
	target_compile_definitions(sicha PUBLIC SICHA_STATIC)
endif()

include(GNUInstallDirs)
install(TARGETS sicha
	ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
	LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
	RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
install(FILES include/sicha.h DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})

if(SICHA_BUILD_TESTS)
	enable_testing()

	add_library(sicha_test_support STATIC
		tests/support.c)
	target_include_directories(sicha_test_support PUBLIC tests
		${CMAKE_CURRENT_SOURCE_DIR}/include)
	# support links no sicha target (each test picks real vs whitebox),
	# so pin the header to static linkage: without this, MSVC compiles
	# its sicha_cancel_* calls as dllimport (__imp_*) references that
	# only resolve when a test happens to pull cancel.obj itself
	target_compile_definitions(sicha_test_support PRIVATE SICHA_STATIC)
	set_target_properties(sicha_test_support PROPERTIES
		C_STANDARD 17 C_STANDARD_REQUIRED ON C_EXTENSIONS OFF)
	target_compile_options(sicha_test_support PRIVATE
		${SICHA_C_WARNINGS} ${SICHA_SAN_FLAGS})

	# test_status exercises the real target through the public API
	# only — in a BUILD_SHARED_LIBS build it doubles as the check
	# that the shared library exports everything FFI users need.
	set(SICHA_PUBLIC_TESTS status)
	# whitebox suites reach internal modules via src/
	set(SICHA_WHITEBOX_TESTS buf sse classify request response script backoff routing cancel determinism threads)
	if(SICHA_WITH_CURL)
		list(APPEND SICHA_WHITEBOX_TESTS curl)
	endif()

	foreach(t ${SICHA_PUBLIC_TESTS} ${SICHA_WHITEBOX_TESTS})
		if(t STREQUAL "curl")
			add_executable(test_${t} tests/test_${t}.c
				tests/http_server.c)
			if(WIN32)
				target_link_libraries(test_${t} PRIVATE ws2_32)
			endif()
		else()
			add_executable(test_${t} tests/test_${t}.c)
		endif()
		target_link_libraries(test_${t} PRIVATE sicha_test_support)
		if(t IN_LIST SICHA_PUBLIC_TESTS)
			target_link_libraries(test_${t} PRIVATE sicha)
		else()
			target_link_libraries(test_${t} PRIVATE sicha_whitebox)
			target_include_directories(test_${t} PRIVATE src)
		endif()
		set_target_properties(test_${t} PROPERTIES
			C_STANDARD 17 C_STANDARD_REQUIRED ON C_EXTENSIONS OFF)
		target_compile_options(test_${t} PRIVATE ${SICHA_C_WARNINGS})
		add_test(NAME ${t} COMMAND test_${t})
	endforeach()

	# Opt-in live-endpoint integration test: point it at any running
	# OAI-compatible endpoint via SICHA_LIVE_BASE_URL /
	# SICHA_LIVE_MODEL (+ optional SICHA_LIVE_API_KEY,
	# SICHA_LIVE_KOBOLD_ASSIST=1).  Unconfigured it exits 77 and
	# ctest reports ***Skipped — visible, never a silent pass.
	if(SICHA_WITH_CURL)
		add_executable(test_live tests/test_live.c)
		target_link_libraries(test_live PRIVATE sicha_test_support
			sicha)
		set_target_properties(test_live PROPERTIES
			C_STANDARD 17 C_STANDARD_REQUIRED ON C_EXTENSIONS OFF)
		target_compile_options(test_live PRIVATE ${SICHA_C_WARNINGS})
		add_test(NAME live COMMAND test_live)
		set_tests_properties(live PROPERTIES SKIP_RETURN_CODE 77)
	endif()
endif()

if(SICHA_BUILD_BENCH)
	enable_testing()
	add_executable(sicha_bench bench/bench.c)
	target_link_libraries(sicha_bench PRIVATE sicha_whitebox)
	target_include_directories(sicha_bench PRIVATE src)
	set_target_properties(sicha_bench PROPERTIES
		C_STANDARD 17 C_STANDARD_REQUIRED ON C_EXTENSIONS OFF)
	target_compile_options(sicha_bench PRIVATE ${SICHA_C_WARNINGS})
	add_test(NAME bench_counters COMMAND sicha_bench --check)
endif()

option(SICHA_BUILD_DEMO "Build the notcurses demos" OFF)
if(SICHA_BUILD_DEMO)
	find_package(PkgConfig QUIET)
	if(PKG_CONFIG_FOUND)
		pkg_check_modules(NOTCURSES QUIET IMPORTED_TARGET notcurses-core)
	endif()
	if(NOT NOTCURSES_FOUND AND SICHA_NOTCURSES_INCLUDE AND SICHA_NOTCURSES_LIB)
		if(NOT EXISTS ${SICHA_NOTCURSES_INCLUDE}/notcurses/version.h)
			# version.h is generated by notcurses' own build; a
			# source checkout lacks it, so synthesize one
			file(WRITE ${CMAKE_BINARY_DIR}/nc-compat/notcurses/version.h
				"#define NOTCURSES_VERSION_MAJOR 3\n"
				"#define NOTCURSES_VERSION_MINOR 0\n"
				"#define NOTCURSES_VERSION_PATCH 17\n"
				"#define NOTCURSES_VERSION_TWEAK 0\n"
				"#define NOTCURSES_VERSION \"3.0.17\"\n")
		endif()
		find_library(SICHA_NOTCURSES_CORE_LIB notcurses-core
			PATHS ${SICHA_NOTCURSES_LIB} NO_DEFAULT_PATH)
		if(NOT SICHA_NOTCURSES_CORE_LIB)
			message(FATAL_ERROR "libnotcurses-core not found in ${SICHA_NOTCURSES_LIB}")
		endif()
		if(APPLE)
			# vendored dylibs use @loader_path install names; point
			# each executable's reference at the absolute location
			execute_process(COMMAND otool -D ${SICHA_NOTCURSES_CORE_LIB}
				OUTPUT_VARIABLE _nc_id_out)
			string(REGEX REPLACE ".*\n(.+)\n?$" "\\1" _nc_id "${_nc_id_out}")
			string(STRIP "${_nc_id}" _nc_id)
			get_filename_component(_nc_real ${SICHA_NOTCURSES_CORE_LIB} REALPATH)
		endif()
	elseif(NOT NOTCURSES_FOUND)
		message(FATAL_ERROR "notcurses not found: install it (pkg-config"
			" notcurses-core) or pass -DSICHA_NOTCURSES_INCLUDE=..."
			" -DSICHA_NOTCURSES_LIB=...")
	endif()

	foreach(demo chat fallback)
		add_executable(sicha_${demo} demo/${demo}.c)
		target_link_libraries(sicha_${demo} PRIVATE sicha)
		set_target_properties(sicha_${demo} PROPERTIES
			C_STANDARD 17 C_STANDARD_REQUIRED ON C_EXTENSIONS OFF)
		target_compile_options(sicha_${demo} PRIVATE ${SICHA_C_WARNINGS})
		if(NOTCURSES_FOUND)
			target_link_libraries(sicha_${demo} PRIVATE PkgConfig::NOTCURSES)
		else()
			target_include_directories(sicha_${demo} PRIVATE
				${SICHA_NOTCURSES_INCLUDE})
			if(NOT EXISTS ${SICHA_NOTCURSES_INCLUDE}/notcurses/version.h)
				target_include_directories(sicha_${demo} PRIVATE
					${CMAKE_BINARY_DIR}/nc-compat)
			endif()
			target_link_libraries(sicha_${demo} PRIVATE
				${SICHA_NOTCURSES_CORE_LIB})
			if(APPLE)
				add_custom_command(TARGET sicha_${demo} POST_BUILD
					COMMAND install_name_tool -change "${_nc_id}"
						"${_nc_real}" $<TARGET_FILE:sicha_${demo}>)
			endif()
		endif()
	endforeach()
endif()

if(SICHA_FUZZ)
	foreach(f sse request response client)
		add_executable(fuzz_${f} fuzz/fuzz_${f}.c)
		target_link_libraries(fuzz_${f} PRIVATE sicha_whitebox)
		target_include_directories(fuzz_${f} PRIVATE src)
		set_target_properties(fuzz_${f} PROPERTIES
			C_STANDARD 17 C_STANDARD_REQUIRED ON C_EXTENSIONS OFF)
		target_compile_options(fuzz_${f} PRIVATE
			${SICHA_C_WARNINGS} -fsanitize=fuzzer,address,undefined
			-fno-sanitize-recover=all)
		target_link_options(fuzz_${f} PRIVATE
			-fsanitize=fuzzer,address,undefined)
	endforeach()
endif()
