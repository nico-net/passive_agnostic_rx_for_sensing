# SPDX-License-Identifier: LicenseRef-CSSL-1.0
# CUDA must already be enabled. Only generic numeric architectures are portable:
# architecture-specific a/f variants do not have the forward PTX contract used here.
# Architectures the found nvcc cannot target are dropped with a warning (the highest kept one is also emitted as PTX);
# only an empty remainder is fatal.
function(nr_gpu_architectures input output)
  if("${input}" STREQUAL "")
    message(FATAL_ERROR "GPU architecture list must not be empty")
  endif()
  execute_process(COMMAND "${CMAKE_CUDA_COMPILER}" --list-gpu-code
                  OUTPUT_VARIABLE supported RESULT_VARIABLE result)
  if(NOT result EQUAL 0)
    message(FATAL_ERROR "Cannot query nvcc supported GPU architectures")
  endif()
  set(normalized "")
  set(highest 0)
  foreach(arch IN LISTS input)
    if(NOT arch MATCHES "^([1-9][0-9]*)(-real|-virtual)?$")
      message(FATAL_ERROR "Invalid GPU architecture '${arch}': use a numeric list such as 89;121")
    endif()
    set(number "${CMAKE_MATCH_1}")
    if(NOT supported MATCHES "sm_${number}([ \r\n]|$)")
      get_property(warned GLOBAL PROPERTY NR_GPU_ARCH_DROPPED_${number}) # warn once per architecture, however often the list is filtered
      if(NOT warned)
        message(WARNING "GPU architecture ${number} is unsupported by ${CMAKE_CUDA_COMPILER} (CUDA ${CMAKE_CUDA_COMPILER_VERSION}); dropped from the build")
        set_property(GLOBAL PROPERTY NR_GPU_ARCH_DROPPED_${number} TRUE)
      endif()
      continue()
    endif()
    if(number GREATER highest)
      set(highest "${number}")
    endif()
    if(arch MATCHES "-virtual$")
      list(APPEND normalized "${number}-virtual")
    else()
      list(APPEND normalized "${number}-real")
    endif()
  endforeach()
  if(NOT normalized)
    message(FATAL_ERROR "None of the GPU architectures '${input}' is supported by ${CMAKE_CUDA_COMPILER}; select a supported list or upgrade CUDA")
  endif()
  list(APPEND normalized "${highest}-virtual")
  list(REMOVE_DUPLICATES normalized)
  set(${output} "${normalized}" PARENT_SCOPE)
endfunction()

# enable_language(CUDA) compiles a test program for CMAKE_CUDA_ARCHITECTURES, which fails when nvcc cannot target one of them
# (the default 89;121 below CUDA 12.9): detect the compiler without architectures, then leave the requested list filtered by
# nr_gpu_architectures (unsupported entries dropped, warned once) as CMAKE_CUDA_ARCHITECTURES.
macro(nr_gpu_enable_cuda)
  set(_nr_gpu_requested_archs "${CMAKE_CUDA_ARCHITECTURES}")
  set(CMAKE_CUDA_ARCHITECTURES OFF)
  enable_language(CUDA)
  nr_gpu_architectures("${_nr_gpu_requested_archs}" _nr_gpu_filtered_archs)
  set(CMAKE_CUDA_ARCHITECTURES "${_nr_gpu_filtered_archs}")
endmacro()
