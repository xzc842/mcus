# Copyright 2026 XueZicheng <xuezicheng842@outlook.com>
#
# Licensed under the Apache License, Version 2.0 (the "License");
# you may not use this file except in compliance with the License.
# You may obtain a copy of the License at
#
#     http://www.apache.org/licenses/LICENSE-2.0
#
# Unless required by applicable law or agreed to in writing, software
# distributed under the License is distributed on an "AS IS" BASIS,
# WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
# See the License for the specific language governing permissions and
# limitations under the License.

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

add_executable(test_breakpoint test_breakpoint.cpp)
target_link_libraries(test_breakpoint PRIVATE mcus_core)
target_compile_definitions(test_breakpoint PRIVATE
    MCUS_TEST_MODEL_PATH="${CMAKE_SOURCE_DIR}/models/dummy.json")
add_test(NAME breakpoint COMMAND test_breakpoint)

add_executable(test_firmware test_firmware.cpp)
target_link_libraries(test_firmware PRIVATE mcus_core)
add_test(NAME firmware COMMAND test_firmware)