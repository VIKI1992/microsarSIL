# Warning policy: strict for SIL code, relaxed for vendor/SIP code (set per target).
function(sil_strict_warnings target)
  if(MSVC)
    target_compile_options(${target} PRIVATE /W4 /permissive- /Zc:preprocessor)
  else()
    target_compile_options(${target} PRIVATE
      -Wall -Wextra -Wshadow -Wundef -Wpointer-arith -Wcast-align -Wno-unused-parameter
      $<$<COMPILE_LANGUAGE:C>:-Wstrict-prototypes>
      $<$<COMPILE_LANGUAGE:C>:-Wmissing-prototypes>
      $<$<COMPILE_LANGUAGE:CXX>:-Wnon-virtual-dtor>)
  endif()
endfunction()

function(sil_vendor_warnings target)
  if(MSVC)
    target_compile_options(${target} PRIVATE /W3)
  else()
    target_compile_options(${target} PRIVATE -Wall -Wno-unused-parameter -Wno-unused-variable
      -Werror=pointer-to-int-cast -Werror=int-to-pointer-cast)
  endif()
endfunction()
