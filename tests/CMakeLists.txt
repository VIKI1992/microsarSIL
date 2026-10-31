# OS semantics tests run as an ECU built on the demo configuration with test task bodies.
silos_add_ecu(test_os
  ARXML ${SIL_ROOT}/demo/ecuc/DemoEcu.arxml ${SIL_ROOT}/demo/ecuc/DemoEcu_Can.arxml ${SIL_ROOT}/demo/ecuc/DemoEcu_Mcal.arxml
  SOURCES unit/test_os.c)
add_test(NAME os_semantics COMMAND test_os --quiet)
set_tests_properties(os_semantics PROPERTIES TIMEOUT 120)

add_test(NAME demo_runs_2s COMMAND demo_ecu --duration=2 --quiet --perfetto=${CMAKE_CURRENT_BINARY_DIR}/demo.json --btf=${CMAKE_CURRENT_BINARY_DIR}/demo.btf)
set_tests_properties(demo_runs_2s PROPERTIES TIMEOUT 120)

add_test(NAME demo_realtime_1s COMMAND demo_ecu --time=realtime:4 --duration=1 --quiet)
set_tests_properties(demo_realtime_1s PROPERTIES TIMEOUT 120)

add_test(NAME tools_pytest COMMAND ${SIL_PYTHON} -m pytest -q ${SIL_ROOT}/tests/tools)
set_tests_properties(tools_pytest PROPERTIES TIMEOUT 300)

# control server, virtual CAN and the python package against the demo ECU
add_test(NAME sil_pytest COMMAND ${SIL_PYTHON} -m pytest -q ${SIL_ROOT}/tests/sil)
set_tests_properties(sil_pytest PROPERTIES TIMEOUT 600 ENVIRONMENT "MICROSARSIL_DEMO_EXE=$<TARGET_FILE:demo_ecu>")
