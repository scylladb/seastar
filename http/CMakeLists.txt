seastar_ragel_generate (http_response_parser.rl SEASTAR_HTTP_RESPONSE_PARSER_HEADER)
add_custom_target (seastar-http-response-parser SOURCES ${SEASTAR_HTTP_RESPONSE_PARSER_HEADER})

seastar_ragel_generate (request_parser.rl SEASTAR_HTTP_REQUEST_PARSER_HEADER)
add_custom_target (seastar-http-request-parser SOURCES ${SEASTAR_HTTP_REQUEST_PARSER_HEADER})
