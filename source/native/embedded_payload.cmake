# V1.7.1 slim.1: a generated payload list (.rc lines `<id> RCDATA "<file>"`) ships as one LZMA-compressed
# resource. pack_embedded_payloads.py writes <name>.packed.rc and <name>.packed.h; a target that links the
# packed .rc compiles MFG_EMBEDDED_PAYLOAD_SOURCES (embedded_payload.cpp and LZMA SDK's decoder) to read it.
include_guard(DIRECTORY)
find_package(Python3 3.10 REQUIRED COMPONENTS Interpreter)
set(MFG_EMBEDDED_PAYLOAD_SOURCES
    "${CMAKE_CURRENT_LIST_DIR}/embedded_payload.cpp" "${CMAKE_CURRENT_LIST_DIR}/third_party/lzma/LzmaDec.c")
set(MFG_EMBEDDED_PAYLOAD_PACKER "${CMAKE_CURRENT_LIST_DIR}/pack_embedded_payloads.py")
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${MFG_EMBEDDED_PAYLOAD_PACKER}")

# Packs <list> as group <name> (slot and resource id unique in the module) and sets <out> to the packed .rc.
function(mfg_pack_payloads list name slot resource output out)
    execute_process(COMMAND "${Python3_EXECUTABLE}" "${MFG_EMBEDDED_PAYLOAD_PACKER}"
        --rc "${list}" --name ${name} --slot ${slot} --resource ${resource} --output "${output}"
        COMMAND_ERROR_IS_FATAL ANY)
    set(${out} "${output}/${name}.packed.rc" PARENT_SCOPE)
endfunction()
