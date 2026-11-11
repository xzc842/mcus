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

add_executable(test_arm_isa test_arm_isa.cpp)
target_link_libraries(test_arm_isa PRIVATE mcus_core mcus_isa_arm)
target_compile_definitions(test_arm_isa PRIVATE
    MCUS_TEST_ARM_MODEL_PATH="${CMAKE_SOURCE_DIR}/models/stm32f103c8t6.json")
add_test(NAME arm_isa COMMAND test_arm_isa)

add_executable(test_generic_model test_generic_model.cpp)
target_link_libraries(test_generic_model PRIVATE mcus_core)
target_compile_definitions(test_generic_model PRIVATE
    MCUS_TEST_GENERIC_MODEL_PATH="${CMAKE_SOURCE_DIR}/models/generic.json")
add_test(NAME generic_model COMMAND test_generic_model)

add_executable(test_interrupt test_interrupt.cpp)
target_link_libraries(test_interrupt PRIVATE mcus_core)
target_compile_definitions(test_interrupt PRIVATE
    MCUS_TEST_MODEL_PATH="${CMAKE_SOURCE_DIR}/models/dummy.json")
add_test(NAME interrupt COMMAND test_interrupt)

add_executable(test_peripherals test_peripherals.cpp)
target_link_libraries(test_peripherals PRIVATE mcus_core mcus_isa_8051)
target_compile_definitions(test_peripherals PRIVATE
    MCUS_TEST_CH552G_MODEL_PATH="${CMAKE_SOURCE_DIR}/models/ch552g.json")
add_test(NAME peripherals COMMAND test_peripherals)
add_test(NAME bench_minimal COMMAND mcus_bench
    --model ${CMAKE_SOURCE_DIR}/models/dummy.json
    --steps 20000 --warmup 100 --repeat 1)
set_tests_properties(bench_minimal PROPERTIES LABELS bench)

add_test(NAME bench_8051 COMMAND mcus_bench
    --model ${CMAKE_SOURCE_DIR}/models/ch552g.json
    --isa ${CMAKE_BINARY_DIR}/libmcus_isa_8051.so
    --steps 20000 --warmup 100 --repeat 1)
set_tests_properties(bench_8051 PROPERTIES LABELS bench)

add_test(NAME bench_arm COMMAND mcus_bench
    --model ${CMAKE_SOURCE_DIR}/models/stm32f103c8t6.json
    --isa ${CMAKE_BINARY_DIR}/libmcus_isa_arm.so
    --steps 20000 --warmup 100 --repeat 1)
set_tests_properties(bench_arm PROPERTIES LABELS bench)

add_executable(test_stc90c51 test_stc90c51.cpp)
target_link_libraries(test_stc90c51 PRIVATE mcus_core mcus_isa_8051)
target_compile_definitions(test_stc90c51 PRIVATE
    MCUS_TEST_STC90C51_MODEL_PATH="${CMAKE_SOURCE_DIR}/models/stc90c51.json")
add_test(NAME stc90c51 COMMAND test_stc90c51)

add_executable(test_interfaces test_interfaces.cpp)
target_link_libraries(test_interfaces PRIVATE mcus_core)
target_compile_definitions(test_interfaces PRIVATE
    MCUS_TEST_INTERFACE_MODEL_PATH="${CMAKE_SOURCE_DIR}/models/interface_test.json")
add_test(NAME interfaces COMMAND test_interfaces)

add_executable(test_simulator_timer test_simulator_timer.cpp)
target_link_libraries(test_simulator_timer PRIVATE mcus_core)
target_compile_definitions(test_simulator_timer PRIVATE
    MCUS_TEST_STC90C51_MODEL_PATH="${CMAKE_SOURCE_DIR}/models/stc90c51.json"
    MCUS_BIN_PATH="$<TARGET_FILE:mcus>"
    MCUS_ISA_PATH="$<TARGET_FILE:mcus_isa_8051>"
    MCUS_JIT_PATH="$<TARGET_FILE:mcus_jit_8051>")
add_test(NAME simulator_timer COMMAND test_simulator_timer)
add_test(NAME simulator_timer_batch COMMAND test_simulator_timer 64)
add_test(NAME simulator_timer1 COMMAND test_simulator_timer 1 1)
add_test(NAME simulator_timer2 COMMAND test_simulator_timer 1 2)

add_executable(test_stm32_peripherals test_stm32_peripherals.cpp)
target_link_libraries(test_stm32_peripherals PRIVATE mcus_core mcus_isa_arm)
target_compile_definitions(test_stm32_peripherals PRIVATE
    MCUS_TEST_ARM_MODEL_PATH="${CMAKE_SOURCE_DIR}/models/stm32f103c8t6.json")
add_test(NAME stm32_peripherals COMMAND test_stm32_peripherals)

add_executable(test_timer_engine test_timer_engine.cpp)
target_link_libraries(test_timer_engine PRIVATE mcus_core mcus_isa_8051)
target_compile_definitions(test_timer_engine PRIVATE
    MCUS_TEST
    MCUS_TEST_CH552G_MODEL_PATH="${CMAKE_SOURCE_DIR}/models/ch552g.json")
add_test(NAME timer_engine COMMAND test_timer_engine)

add_executable(test_timer_engine_arm test_timer_engine.cpp)
target_link_libraries(test_timer_engine_arm PRIVATE mcus_core mcus_isa_arm)
target_compile_definitions(test_timer_engine_arm PRIVATE
    MCUS_TEST
    MCUS_TEST_ARM
    MCUS_TEST_ARM_MODEL_PATH="${CMAKE_SOURCE_DIR}/models/stm32f103c8t6.json")
add_test(NAME timer_engine_arm COMMAND test_timer_engine_arm)

add_executable(test_jit test_jit.cpp)
target_link_libraries(test_jit PRIVATE mcus_jit)
target_compile_definitions(test_jit PRIVATE
    MCUS_TEST_CH552G_MODEL_PATH="${CMAKE_SOURCE_DIR}/models/ch552g.json"
    MCUS_TEST_JIT8051_PATH="${CMAKE_BINARY_DIR}/libmcus_jit_8051.so")
add_dependencies(test_jit mcus_jit_8051)
add_test(NAME jit COMMAND test_jit)

add_executable(test_jit_diff test_jit_diff.cpp)
target_link_libraries(test_jit_diff PRIVATE mcus_core mcus_isa_8051)
target_compile_definitions(test_jit_diff PRIVATE
    MCUS_TEST_CH552G_MODEL_PATH="${CMAKE_SOURCE_DIR}/models/ch552g.json"
    MCUS_TEST_JIT8051_PATH="${CMAKE_BINARY_DIR}/libmcus_jit_8051.so")
add_dependencies(test_jit_diff mcus_jit_8051)
add_test(NAME jit_diff COMMAND test_jit_diff)

add_test(NAME simulator_timer_jit
         COMMAND test_simulator_timer 64 0 ${CMAKE_BINARY_DIR}/libmcus_jit_8051.so)
add_test(NAME simulator_timer_jit_exact
         COMMAND test_simulator_timer 1 0 ${CMAKE_BINARY_DIR}/libmcus_jit_8051.so)
add_dependencies(test_simulator_timer mcus_jit_8051)

add_executable(test_jit_timing test_jit_timing.cpp)
target_link_libraries(test_jit_timing PRIVATE mcus_core mcus_isa_8051)
target_compile_definitions(test_jit_timing PRIVATE
    MCUS_TEST_CH552G_MODEL_PATH="${CMAKE_SOURCE_DIR}/models/ch552g.json"
    MCUS_TEST_JIT8051_PATH="${CMAKE_BINARY_DIR}/libmcus_jit_8051.so")
add_dependencies(test_jit_timing mcus_jit_8051)
add_test(NAME jit_timing COMMAND test_jit_timing)

add_executable(test_jit_event_timing test_jit_event_timing.cpp)
target_link_libraries(test_jit_event_timing PRIVATE mcus_core mcus_isa_8051)
target_compile_definitions(test_jit_event_timing PRIVATE
    MCUS_TEST_CH552G_MODEL_PATH="${CMAKE_SOURCE_DIR}/models/ch552g.json"
    MCUS_TEST_JIT8051_PATH="${CMAKE_BINARY_DIR}/libmcus_jit_8051.so")
add_dependencies(test_jit_event_timing mcus_jit_8051)
add_test(NAME jit_event_timing COMMAND test_jit_event_timing)

add_executable(test_jit_peripherals_timing test_jit_peripherals_timing.cpp)
target_link_libraries(test_jit_peripherals_timing PRIVATE mcus_core mcus_isa_8051)
target_compile_definitions(test_jit_peripherals_timing PRIVATE
    MCUS_TEST_CH552G_MODEL_PATH="${CMAKE_SOURCE_DIR}/models/ch552g.json"
    MCUS_TEST_JIT8051_PATH="${CMAKE_BINARY_DIR}/libmcus_jit_8051.so")
add_dependencies(test_jit_peripherals_timing mcus_jit_8051)
add_test(NAME jit_peripherals_timing COMMAND test_jit_peripherals_timing)
