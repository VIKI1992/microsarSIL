# SilOsEcu.cmake - helpers to build an ECU executable on top of SilOs.
#
#   silos_add_ecu(<target>
#       ARXML <os ecuc arxml> [more arxml...]
#       SOURCES <sources...>
#       [GEN_OPTS <extra gen_os_cfg.py options...>]
#       [CAN_GEN_OPTS <extra gen_can_cfg.py options...>]
#       [PERIPH_GEN_OPTS <extra gen_periph_cfg.py options...>]
#       [INCLUDE_DIRS <dirs...>])
#
# Generates the OS configuration, the virtual CAN configuration (SilCan_Cfg.c, empty when the arxml
# has no Can module) and the MCAL symbolic names into ${CMAKE_CURRENT_BINARY_DIR}/gen_<target>,
# compiles them together with SOURCES and links the SIL libraries.

function(silos_add_ecu target)
  cmake_parse_arguments(ECU "" "" "ARXML;SOURCES;GEN_OPTS;CAN_GEN_OPTS;PERIPH_GEN_OPTS;INCLUDE_DIRS" ${ARGN})
  if(NOT ECU_ARXML)
    message(FATAL_ERROR "silos_add_ecu(${target}): ARXML is required")
  endif()
  set(gendir ${CMAKE_CURRENT_BINARY_DIR}/gen_${target})
  set(genfiles
      ${gendir}/Os_Types_Lcfg.h ${gendir}/Os_Cfg.h ${gendir}/Os_Hal_Cfg.h ${gendir}/Os_Lcfg.h
      ${gendir}/Os_Lcfg.c ${gendir}/Os_Ioc_Lcfg.h ${gendir}/Os_Ioc_Lcfg.c ${gendir}/Os_Stack_Cfg.h
      ${gendir}/Os_Compiler_Cfg.h ${gendir}/Os_Hook_Cfg.h ${gendir}/SilCan_Cfg.c ${gendir}/SilMcal_Cfg.c ${gendir}/mcal_cfg.stamp)
  add_custom_command(
    OUTPUT ${genfiles}
    COMMAND ${SIL_PYTHON} ${SIL_ROOT}/tools/gen_os_cfg.py -o ${gendir} --quiet ${ECU_GEN_OPTS} ${ECU_ARXML}
    COMMAND ${SIL_PYTHON} ${SIL_ROOT}/tools/gen_can_cfg.py -o ${gendir} --quiet --allow-empty ${ECU_CAN_GEN_OPTS} ${ECU_ARXML}
    COMMAND ${SIL_PYTHON} ${SIL_ROOT}/tools/gen_periph_cfg.py -o ${gendir} --quiet --allow-empty ${ECU_PERIPH_GEN_OPTS} ${ECU_ARXML}
    COMMAND ${SIL_PYTHON} ${SIL_ROOT}/tools/gen_mcal_cfg.py -o ${gendir} --quiet ${ECU_ARXML}
    COMMAND ${CMAKE_COMMAND} -E touch ${gendir}/mcal_cfg.stamp
    DEPENDS ${ECU_ARXML} ${SIL_ROOT}/tools/gen_os_cfg.py ${SIL_ROOT}/tools/gen_can_cfg.py ${SIL_ROOT}/tools/gen_periph_cfg.py ${SIL_ROOT}/tools/gen_mcal_cfg.py
            ${SIL_ROOT}/tools/ecuc.py
    COMMENT "Generating SIL configuration for ${target}"
    VERBATIM)
  add_executable(${target} ${ECU_SOURCES} ${gendir}/Os_Lcfg.c ${gendir}/Os_Ioc_Lcfg.c ${gendir}/SilCan_Cfg.c ${gendir}/SilMcal_Cfg.c ${gendir}/mcal_cfg.stamp)
  set_source_files_properties(${gendir}/mcal_cfg.stamp PROPERTIES HEADER_FILE_ONLY ON)
  target_include_directories(${target} PRIVATE ${gendir} ${ECU_INCLUDE_DIRS})
  target_link_libraries(${target} PRIVATE silruntime silmcal silos siltrace silport)
  if(NOT MSVC)
    target_compile_options(${target} PRIVATE -Wall -Wextra -Wno-unused-parameter)
  endif()
endfunction()
