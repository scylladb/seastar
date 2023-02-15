# seastar_check_self_contained() checks if the headers listed as the source of
# a target are self-contained.
#
# Header files should be self-contained. In general, the source files should
# not have to adhere to special conditions to include them. For instance,
# they don't need to include other header files for using a header file, or
# to define certain macro(s) for using it. But the macros are allowed to be
# used to its behavior though.
#
# seastar_check_self_contained() is created to perform a minimal check on the
# specified set of header files by compiling each of them.
#
# Please note, there are chances that a symbol declaration could be included
# indirectly by an (indirectly) included header file even if that symbol is not
# a part of the public interface of that header file, so this dependency is
# a little bit fragile. seastar_check_self_contained()" does not warn at seeing
# the indirect dependency, it just check if the preprocessed header file
# *contains* the declarations of the symbols so that any source file including
# it can be compiled as well. The check performed by the CMake function allows
# the indirect inclusion of the used symbols. For instance, if "juno.h"
# references a symbol named "Jupiter", which is declared in "jupiter.h". So,
# strictly speaking, "juno.h" should include "jupiter.h" for accessing this
# symbol. But "juno.h" happens to include "solar.h", which in turn includes
# "jupiter.h". So "seastar_check_self_contained()" accepts "juno.h", while a
# tool like iwyu would complain at seeing it.
#
# You can also use CMAKE_CXX_INCLUDE_WHAT_YOU_USE for using an external tool
# for performing a similar check. see
# https://cmake.org/cmake/help/latest/prop_tgt/LANG_INCLUDE_WHAT_YOU_USE.html

function (seastar_check_self_contained library)
  cmake_parse_arguments (
    parsed_args
    ""
    ""
    "EXCLUDE;INCLUDE"
    ${ARGN})

  get_target_property (sources ${library} SOURCES)
  list (FILTER sources INCLUDE REGEX "${parsed_args_INCLUDE}")
  list (FILTER sources EXCLUDE REGEX "${parsed_args_EXCLUDE}")
  foreach (fn ${sources})
    get_filename_component (file_ext ${fn} EXT)
    if (NOT file_ext STREQUAL ".hh")
      message (SEND_ERROR "Only headers are checked if they are self-contained, while ${fn} is not a header.")
    elseif (IS_ABSOLUTE ${fn})
      # the header specified with absolute path is likely to be generated, this
      # is not our focus at this moment.
      continue ()
    endif ()
    get_filename_component (file_dir ${fn} DIRECTORY)
    list (APPEND includes "${file_dir}")
    set (src_dir "${CMAKE_BINARY_DIR}/checkheaders/${file_dir}")
    file (MAKE_DIRECTORY "${src_dir}")
    get_filename_component (file_name ${fn} NAME)
    set (src "${src_dir}/${file_name}.cc")
    # CMake refuses to compile .hh files, so we need to rename them first.
    add_custom_command (
      OUTPUT ${src}
      DEPENDS ${fn}
      # silence "-Wpragma-once-outside-header"
      COMMAND sed
            -e "s/^#pragma once//"
            "${fn}" > "${src}"
      WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}"
      VERBATIM)
    list (APPEND srcs "${src}")
  endforeach ()

  set (check_lib "checkheaders-${library}")
  add_library (${check_lib} EXCLUDE_FROM_ALL)
  target_sources (${check_lib}
    PRIVATE ${srcs})
  # use ${library} as an interface library by consuming all of its
  # compile time options
  get_target_property (libraries ${library} LINK_LIBRARIES)
  if (libraries)
    target_link_libraries (${check_lib}
      PRIVATE ${libraries})
  endif ()

  # if header includes other header files with relative path,
  # we should satisfy it.
  list (REMOVE_DUPLICATES includes)
  target_include_directories (${check_lib}
    PRIVATE ${includes})
  get_target_property (includes ${library} INCLUDE_DIRECTORIES)
  if (includes)
    target_include_directories (${check_lib}
      PRIVATE ${includes})
  endif ()

  # symbols in header file should always be referenced, but these
  # are just pure headers, so unused variables should be tolerated.
  target_compile_options (${check_lib}
    PRIVATE
      -Wno-error=unused-const-variable
      -Wno-error=unused-function
      -Wno-error=unused-variable)
  get_target_property (compile_options ${library} COMPILE_OPTIONS)
  if (compile_options)
    target_compile_options (${check_lib}
      PRIVATE ${compile_options})
  endif ()

  get_target_property (compile_definitions ${library} COMPILE_DEFINITIONS)
  if (compile_definitions)
    target_compile_definitions (${check_lib}
      PRIVATE ${compile_definitions})
  endif ()

  add_dependencies (checkheaders ${check_lib})
endfunction ()
