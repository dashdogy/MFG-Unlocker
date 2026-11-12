cmake_minimum_required(VERSION 3.24)
project(MfgEmbeddedPayloadTests LANGUAGES C CXX RC)
# The packed payload store (V1.7.1 slim.1): three fixture files packed by the module's own packer, then
# embedded_payload::Find returns their exact bytes; a flipped byte, a truncated stream, a wrong packed or raw size, a
# wrong SHA-256 and a missing resource each refuse the whole group (every id empty, on later calls too) and log it once.
set(NATIVE_SOURCE "${CMAKE_CURRENT_SOURCE_DIR}/../../source/native" CACHE PATH "Release native source")
include("${NATIVE_SOURCE}/embedded_payload.cmake")
set(GENERATED "${CMAKE_CURRENT_BINARY_DIR}/generated")
execute_process(COMMAND "${Python3_EXECUTABLE}" -I "${CMAKE_CURRENT_SOURCE_DIR}/make_fixtures.py" payloads
    --output "${GENERATED}" COMMAND_ERROR_IS_FATAL ANY)
mfg_pack_payloads("${GENERATED}/fixtures.rc" fixtures 0 3950 "${GENERATED}" FIXTURES_PACKED_RC)
execute_process(COMMAND "${Python3_EXECUTABLE}" -I "${CMAKE_CURRENT_SOURCE_DIR}/make_fixtures.py" damage
    --output "${GENERATED}" COMMAND_ERROR_IS_FATAL ANY)
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/make_fixtures.py")

add_executable(EmbeddedPayloadTests payload_tests.cpp ${MFG_EMBEDDED_PAYLOAD_SOURCES}
    "${FIXTURES_PACKED_RC}" "${GENERATED}/damaged.rc")
set_source_files_properties(payload_tests.cpp PROPERTIES COMPILE_OPTIONS "/W4;/WX")
target_compile_features(EmbeddedPayloadTests PRIVATE cxx_std_20)
target_include_directories(EmbeddedPayloadTests PRIVATE "${NATIVE_SOURCE}" "${GENERATED}")
target_compile_definitions(EmbeddedPayloadTests PRIVATE UNICODE _UNICODE NOMINMAX WIN32_LEAN_AND_MEAN)
target_link_libraries(EmbeddedPayloadTests PRIVATE bcrypt)
set_target_properties(EmbeddedPayloadTests PROPERTIES MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")

enable_testing()
add_test(NAME store COMMAND EmbeddedPayloadTests)
