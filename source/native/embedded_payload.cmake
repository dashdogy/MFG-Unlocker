# A generated payload list (.rc lines `<id> RCDATA "<file>"`) as one payload group: every payload stays its own
# uncompressed resource (V1.7.2 nolzma.1; V1.7.1 slim.1 had packed each group as one LZMA stream).
# pack_embedded_payloads.py writes <name>.packed.rc (the payload list) and <name>.packed.h (the group's table and
# SHA-256); a target that links the .rc compiles MFG_EMBEDDED_PAYLOAD_SOURCES to read it.
include_guard(DIRECTORY)
find_package(Python3 3.10 REQUIRED COMPONENTS Interpreter)
set(MFG_EMBEDDED_PAYLOAD_SOURCES "${CMAKE_CURRENT_LIST_DIR}/embedded_payload.cpp")
set(MFG_EMBEDDED_PAYLOAD_PACKER "${CMAKE_CURRENT_LIST_DIR}/pack_embedded_payloads.py")
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${MFG_EMBEDDED_PAYLOAD_PACKER}")

# Lists <list> as group <name> (slot unique in the module) and sets <out> to the group's .rc.
function(mfg_pack_payloads list name slot output out)
    execute_process(COMMAND "${Python3_EXECUTABLE}" "${MFG_EMBEDDED_PAYLOAD_PACKER}"
        --rc "${list}" --name ${name} --slot ${slot} --output "${output}"
        COMMAND_ERROR_IS_FATAL ANY)
    set(${out} "${output}/${name}.packed.rc" PARENT_SCOPE)
endfunction()
