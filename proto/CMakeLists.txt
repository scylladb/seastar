find_package(Protobuf REQUIRED)
include_directories(${PROTOBUF_INCLUDE_DIRS})
include_directories(${CMAKE_CURRENT_BINARY_DIR})
protobuf_generate_cpp(METRICS2_PROTO_SRCS METRICS2_PROTO_HDRS metrics2.proto)

add_library(metrics2_proto OBJECT
        ${METRICS2_PROTO_SRCS}
        ${METRICS2_PROTO_HDRS})

#target_link_libraries(metrics2_proto ${PROTOBUF_LIBRARIES})