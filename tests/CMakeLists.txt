add_executable(test_event_stream test_event_stream.cpp)
target_link_libraries(test_event_stream PRIVATE mcus_core)
add_test(NAME event_stream COMMAND test_event_stream)

add_executable(test_shared_memory test_shared_memory.cpp)
target_link_libraries(test_shared_memory PRIVATE mcus_core)
target_compile_definitions(test_shared_memory PRIVATE
    MCUS_TEST_MODEL_PATH="${CMAKE_SOURCE_DIR}/models/dummy.json")
add_test(NAME shared_memory COMMAND test_shared_memory)

add_executable(test_cpu_minimal test_cpu_minimal.cpp)
target_link_libraries(test_cpu_minimal PRIVATE mcus_core)
target_compile_definitions(test_cpu_minimal PRIVATE
    MCUS_TEST_MODEL_PATH="${CMAKE_SOURCE_DIR}/models/dummy.json")
add_test(NAME cpu_minimal COMMAND test_cpu_minimal)

add_executable(test_timer test_timer.cpp)
target_link_libraries(test_timer PRIVATE mcus_core)
target_compile_definitions(test_timer PRIVATE
    MCUS_TEST_MODEL_PATH="${CMAKE_SOURCE_DIR}/models/dummy.json")
add_test(NAME timer COMMAND test_timer)

add_executable(test_ch552g test_ch552g.cpp)
target_link_libraries(test_ch552g PRIVATE mcus_core)
target_compile_definitions(test_ch552g PRIVATE
    MCUS_TEST_CH552G_MODEL_PATH="${CMAKE_SOURCE_DIR}/models/ch552g.json")
add_test(NAME ch552g COMMAND test_ch552g)