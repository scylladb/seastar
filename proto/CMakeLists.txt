protobuf_generate_cpp (sources headers metrics2.proto)

add_library (seastar-protobuf OBJECT
  ${sources}
  ${headers})

target_include_directories (seastar-protobuf PUBLIC ${PROTOBUF_INCLUDE_DIRS})
