find_program (Seastar_DOXYGEN_EXECUTABLE doxygen)

configure_file (
  ${CMAKE_CURRENT_SOURCE_DIR}/Doxyfile.in
  ${CMAKE_CURRENT_BINARY_DIR}/Doxyfile
  @ONLY)

configure_file (
  ${CMAKE_CURRENT_SOURCE_DIR}/DoxygenLayout.xml
  ${CMAKE_CURRENT_BINARY_DIR}/DoxygenLayout.xml
  COPYONLY)

add_custom_target (doc_api
  COMMAND ${Seastar_DOXYGEN_EXECUTABLE} ${CMAKE_CURRENT_BINARY_DIR}/Doxyfile)

add_custom_command (
  OUTPUT ${CMAKE_CURRENT_BINARY_DIR}/tutorial.html
  DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/tutorial.md
  COMMAND
    ${CMAKE_CURRENT_SOURCE_DIR}/md2html
    ${CMAKE_CURRENT_SOURCE_DIR}/tutorial.md
    ${CMAKE_CURRENT_BINARY_DIR}/tutorial.html)

add_custom_target (doc_tutorial_html
  DEPENDS ${CMAKE_CURRENT_BINARY_DIR}/tutorial.html)

configure_file (
  ${CMAKE_CURRENT_SOURCE_DIR}/htmlsplit.py
  ${CMAKE_CURRENT_BINARY_DIR}/htmlsplit.py
  COPYONLY)

add_custom_command (
  OUTPUT ${CMAKE_CURRENT_BINARY_DIR}/split
  DEPENDS
    doc_tutorial_html
    ${CMAKE_CURRENT_BINARY_DIR}/htmlsplit.py
  COMMAND ${CMAKE_COMMAND} -E make_directory ${CMAKE_CURRENT_BINARY_DIR}/split
  COMMAND ${CMAKE_CURRENT_BINARY_DIR}/htmlsplit.py)

add_custom_target (doc_tutorial_html_split
  DEPENDS ${CMAKE_CURRENT_BINARY_DIR}/split)

add_custom_command (
  OUTPUT ${CMAKE_CURRENT_BINARY_DIR}/tutorial.pdf
  DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/tutorial.md
  COMMAND
    ${CMAKE_CURRENT_SOURCE_DIR}/md2pdf
    ${CMAKE_CURRENT_SOURCE_DIR}/tutorial.md
    ${CMAKE_CURRENT_BINARY_DIR}/tutorial.pdf)

add_custom_target (doc_tutorial_pdf
  DEPENDS ${CMAKE_CURRENT_BINARY_DIR}/tutorial.pdf)

# Logical target for all documentation.
add_custom_target (docs
  DEPENDS
    doc_api
    doc_tutorial_html
    doc_tutorial_html_split
    doc_tutorial_pdf)
