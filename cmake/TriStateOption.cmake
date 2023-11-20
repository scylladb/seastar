# the "option()" defined by CMake represents a boolean. but somtimes, we want
# to enable/disable it depending on the CMAKE_BUILD_TYPE, if user leaves the
# option unset.
function (tri_state_option option)
  cmake_parse_arguments (
    parsed_args
    ""
    "CONDITION"
    "DEFAULT_BUILD_TYPES"
    ${ARGN})

  get_property(is_multi_config GLOBAL PROPERTY GENERATOR_IS_MULTI_CONFIG)
  if (is_multi_config)
    set (all_build_types ${CMAKE_CONFIGURATION_TYPES})
  else ()
    set (all_build_types ${CMAKE_BUILD_TYPE})
  endif ()

  # generic boolean values passed as string, potentially from configure.py
  set (True_STRING_VALUES "ON" "yes" "Yes" "YES" "true" "True" "TRUE")
  set (Default_STRING_VALUES "DEFAULT" "default" "Default")

  if ("${option}" IN_LIST True_STRING_VALUES)
    set (enabled_types ${all_build_types})
  elseif ("${option}" IN_LIST Default_STRING_VALUES)
    set (enabled_types ${parsed_args_DEFAULT_BUILD_TYPES})
  else ()
    set (enabled_types "")
  endif ()

  if (is_multi_config)
    set (${parsed_args_CONDITION} "$<IN_LIST:$<CONFIG>,${enabled_types}>" PARENT_SCOPE)
  elseif (CMAKE_BUILD_TYPE IN_LIST enabled_types)
    set (${parsed_args_CONDITION} 1 PARENT_SCOPE)
  else ()
    set (${parsed_args_CONDITION} 0 PARENT_SCOPE)
  endif ()
endfunction ()
