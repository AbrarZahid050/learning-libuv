add_executable(cppuv_loop_tests loop_tests.cpp)
target_link_libraries(cppuv_loop_tests PRIVATE cppuv::cppuv)

add_test(NAME cppuv_loop_tests COMMAND cppuv_loop_tests)

set_tests_properties(cppuv_loop_tests PROPERTIES TIMEOUT 20)
