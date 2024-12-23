# seastar_check_include_style() enforces that all source and header files under
# specified directories include the headers with predefined list of prefixes
# with angle brackets instead of quotes.

find_package (Python3 COMPONENTS Interpreter)

function (seastar_check_include_style target library)
  get_target_property (sources ${library} SOURCES)
  set (check-target "${target}-${library}")
  add_custom_target("${check-target}"
    COMMAND Python3::Interpreter ${CMAKE_CURRENT_LIST_DIR}/cmake/check-seastar-include-style.py ${sources}
    WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}"
    COMMENT "Checking include directive styles for ${library} source files"
    USES_TERMINAL)
  add_dependencies (${target} ${check-target})
endfunction ()
