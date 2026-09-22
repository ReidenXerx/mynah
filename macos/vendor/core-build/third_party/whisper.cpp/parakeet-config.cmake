set(PARAKEET_VERSION      0.14.0)
set(PARAKEET_BUILD_COMMIT 306c88f4)
set(PARAKEET_BUILD_NUMBER 4872)
set(PARAKEET_SHARED_LIB   OFF)


####### Expanded from @PACKAGE_INIT@ by configure_package_config_file() #######
####### Any changes to this file will be overwritten by the next CMake run ####
####### The input file was parakeet-config.cmake.in                            ########

get_filename_component(PACKAGE_PREFIX_DIR "${CMAKE_CURRENT_LIST_DIR}/../../../" ABSOLUTE)

macro(set_and_check _var _file)
  set(${_var} "${_file}")
  if(NOT EXISTS "${_file}")
    message(FATAL_ERROR "File or directory ${_file} referenced by variable ${_var} does not exist !")
  endif()
endmacro()

macro(check_required_components _NAME)
  foreach(comp ${${_NAME}_FIND_COMPONENTS})
    if(NOT ${_NAME}_${comp}_FOUND)
      if(${_NAME}_FIND_REQUIRED_${comp})
        set(${_NAME}_FOUND FALSE)
      endif()
    endif()
  endforeach()
endmacro()

####################################################################################

set_and_check(PARAKEET_INCLUDE_DIR "${PACKAGE_PREFIX_DIR}/include")
set_and_check(PARAKEET_LIB_DIR     "${PACKAGE_PREFIX_DIR}/lib")
set_and_check(PARAKEET_BIN_DIR     "${PACKAGE_PREFIX_DIR}/bin")

find_package(ggml REQUIRED HINTS ${PARAKEET_LIB_DIR}/cmake)

find_library(parakeet_LIBRARY parakeet
    REQUIRED
    HINTS ${PARAKEET_LIB_DIR}
    NO_CMAKE_FIND_ROOT_PATH
)

add_library(parakeet UNKNOWN IMPORTED)
set_target_properties(parakeet
    PROPERTIES
    INTERFACE_INCLUDE_DIRECTORIES "${PARAKEET_INCLUDE_DIR}"
        INTERFACE_LINK_LIBRARIES "ggml::ggml;ggml::ggml-base;"
        IMPORTED_LINK_INTERFACE_LANGUAGES "CXX"
        IMPORTED_LOCATION "${parakeet_LIBRARY}"
        INTERFACE_COMPILE_FEATURES cxx_std_11
        POSITION_INDEPENDENT_CODE ON)

check_required_components(parakeet)
