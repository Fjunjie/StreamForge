# Enforces the StreamForge toolchain constraint (requirement 6.1):
# Clang 20 for both C and C++ compilation. Configuration aborts otherwise.
function(sf_check_toolchain)
    foreach(_lang C CXX)
        set(_id "${CMAKE_${_lang}_COMPILER_ID}")
        set(_ver "${CMAKE_${_lang}_COMPILER_VERSION}")
        if(NOT _id STREQUAL "Clang")
            message(FATAL_ERROR
                "StreamForge requires the Clang 20 toolchain, but the ${_lang} compiler is '${_id}' "
                "(${CMAKE_${_lang}_COMPILER}). Reconfigure with CC=clang-20 CXX=clang++-20.")
        endif()
        if(NOT _ver VERSION_GREATER_EQUAL 20.0.0 OR _ver VERSION_GREATER_EQUAL 21.0.0)
            message(FATAL_ERROR
                "StreamForge requires Clang major version 20 for ${lang_placeholder}compilation; "
                "found Clang ${_ver}. Reconfigure with CC=clang-20 CXX=clang++-20.")
        endif()
    endforeach()
    message(STATUS "StreamForge toolchain: Clang ${CMAKE_C_COMPILER_VERSION} (C) / "
        "${CMAKE_CXX_COMPILER_VERSION} (C++)")
endfunction()
