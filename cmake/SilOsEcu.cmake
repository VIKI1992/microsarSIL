# SilOsEcu.cmake - helpers to build an ECU executable on top of SilOs.
#
#   silos_add_ecu(<target>
#       ARXML <os ecuc arxml> [more arxml...]
#       SOURCES <sources...>
#       [GEN_OPTS <extra gen_os_cfg.py options...>]
#       [INCLUDE_DIRS <dirs...>])
#
# Generates the OS configuration into ${CMAKE_CURRENT_BINARY_DIR}/gen_<target>, compiles it together
# with SOURCES and links the SIL libraries.

function(silos_add_ecu target)
  cmake_parse_arguments(ECU "" "" "ARXML;SOURCES;GEN_OPTS;INCLUDE_DIRS" ${ARGN})
  if(NOT ECU_ARXML)
    message(FATAL_ERROR "silos_add_ecu(${target}): ARXML is required")
  endif()
  set(gendir ${CMAKE_CURRENT_BINARY_DIR}/gen_${target})
  set(genfiles
      ${gendir}/Os_Types_Lcfg.h ${gendir}/Os_Cfg.h ${gendir}/Os_Hal_Cfg.h ${gendir}/Os_Lcfg.h
      ${gendir}/Os_Lcfg.c ${gendir}/Os_Ioc_Lcfg.h ${gendir}/Os_Ioc_Lcfg.c ${gendir}/Os_Stack_Cfg.h
      ${gendir}/Os_Compiler_Cfg.h ${gendir}/Os_Hook_Cfg.h)
  add_custom_command(
    OUTPUT ${genfiles}
    COMMAND ${SIL_PYTHON} ${SIL_ROOT}/tools/gen_os_cfg.py -o ${gendir} --quiet ${ECU_GEN_OPTS} ${ECU_ARXML}
    DEPENDS ${ECU_ARXML} ${SIL_ROOT}/tools/gen_os_cfg.py ${SIL_ROOT}/tools/ecuc.py
    COMMENT "Generating SilOs configuration for ${target}"
    VERBATIM)
  add_executable(${target} ${ECU_SOURCES} ${gendir}/Os_Lcfg.c ${gendir}/Os_Ioc_Lcfg.c)
  target_include_directories(${target} PRIVATE ${gendir} ${ECU_INCLUDE_DIRS})
  target_link_libraries(${target} PRIVATE silruntime silos siltrace silport)
  if(NOT MSVC)
    target_compile_options(${target} PRIVATE -Wall -Wextra -Wno-unused-parameter)
  endif()
endfunction()
