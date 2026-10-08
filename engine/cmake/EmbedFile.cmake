# Usage: cmake -DINPUT=<spv> -DOUTPUT=<header> -DNAME=<identifier> -P EmbedFile.cmake
# Converts a SPIR-V binary into a C++ header holding a uint32_t array.
file(READ "${INPUT}" hex HEX)
string(LENGTH "${hex}" hexLen)
math(EXPR byteLen "${hexLen} / 2")
string(REGEX MATCHALL "[0-9a-f][0-9a-f][0-9a-f][0-9a-f][0-9a-f][0-9a-f][0-9a-f][0-9a-f]" words "${hex}")
set(body "")
set(n 0)
foreach(w IN LISTS words)
    # SPIR-V words are little-endian in the file.
    string(REGEX REPLACE "(..)(..)(..)(..)" "0x\\4\\3\\2\\1," word "${w}")
    string(APPEND body "${word}")
    math(EXPR n "${n} + 1")
    math(EXPR m "${n} % 8")
    if(m EQUAL 0)
        string(APPEND body "\n")
    endif()
endforeach()
file(WRITE "${OUTPUT}"
"#pragma once
#include <cstddef>
#include <cstdint>
namespace av::shaders {
inline const uint32_t ${NAME}[] = {
${body}
};
inline constexpr size_t ${NAME}Size = ${byteLen};
}  // namespace av::shaders
")
