# Writes templates/*.bin as constexpr byte arrays in a C++ header, so the
# template data is compiled like any other source: it lands in .rodata of an
# object that carries the compiler's .note.gnu.property (CET IBT/SHSTK) and
# .note.GNU-stack. An `ld -r -b binary` object carries neither, and one such
# input makes the linker drop IBT and SHSTK from the whole executable.
#
# Each array is named after its file (default_icc_template.bin ->
# default_icc_template), 16 bytes to a line.
#
# include() it and call jdvrif_write_template_header(), or run it as a script:
#   cmake -DSOURCE_DIR=<dir> -DOUTPUT=<header> "-DTEMPLATES=<a.bin;b.bin>" -P embed_templates.cmake

function(jdvrif_write_template_header source_dir output)
  set(content "// Generated from the template files at build time. Do not edit.\n")
  string(APPEND content "#pragma once\n\nnamespace jdvrif_templates {\n")
  foreach(template IN LISTS ARGN)
    get_filename_component(name "${template}" NAME_WE)
    file(READ "${source_dir}/${template}" hex HEX)
    if(hex STREQUAL "")
      message(FATAL_ERROR "Template file is empty: ${template}")
    endif()
    string(REGEX REPLACE "([0-9a-f][0-9a-f])" "0x\\1," bytes "${hex}")
    string(REGEX REPLACE "((0x[0-9a-f][0-9a-f],)(0x[0-9a-f][0-9a-f],)(0x[0-9a-f][0-9a-f],)(0x[0-9a-f][0-9a-f],)(0x[0-9a-f][0-9a-f],)(0x[0-9a-f][0-9a-f],)(0x[0-9a-f][0-9a-f],)(0x[0-9a-f][0-9a-f],)(0x[0-9a-f][0-9a-f],)(0x[0-9a-f][0-9a-f],)(0x[0-9a-f][0-9a-f],)(0x[0-9a-f][0-9a-f],)(0x[0-9a-f][0-9a-f],)(0x[0-9a-f][0-9a-f],)(0x[0-9a-f][0-9a-f],)(0x[0-9a-f][0-9a-f],))"
      "    \\1\n" bytes "${bytes}")
    # A final short line has no indent or newline yet.
    string(REGEX REPLACE "\n(0x[^\n]+)$" "\n    \\1\n" bytes "${bytes}")
    if(NOT bytes MATCHES "^    ")
      set(bytes "    ${bytes}\n")
    endif()
    string(APPEND content "inline constexpr unsigned char ${name}[] = {\n${bytes}};\n")
  endforeach()
  string(APPEND content "} // namespace jdvrif_templates\n")
  # Rewritten only when the bytes change, so builds stay incremental.
  file(CONFIGURE OUTPUT "${output}" CONTENT "${content}" @ONLY)
endfunction()

if(CMAKE_SCRIPT_MODE_FILE STREQUAL CMAKE_CURRENT_LIST_FILE)
  jdvrif_write_template_header("${SOURCE_DIR}" "${OUTPUT}" ${TEMPLATES})
endif()
