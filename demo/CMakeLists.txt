silos_add_ecu(demo_ecu
  ARXML ${CMAKE_CURRENT_SOURCE_DIR}/ecuc/DemoEcu.arxml ${CMAKE_CURRENT_SOURCE_DIR}/ecuc/DemoEcu_Can.arxml ${CMAKE_CURRENT_SOURCE_DIR}/ecuc/DemoEcu_Mcal.arxml
  SOURCES app/demo_ecu.c)
