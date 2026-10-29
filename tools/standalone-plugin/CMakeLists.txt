# =============================================================================
#  OLLVM 魔改 pass 集合（基座 Aethereux/Hikari-LLVM19，实际 LLVM 20.1.5 new PM）
#  模式一（产品线，NDK 唯一可行）：拷进 llvm/lib/Transforms/Obfuscation/ 由树内 CMake 收编
#  模式二（PC 调试）：cmake -DOBF_STANDALONE=ON -DLLVM_DIR=.../lib/cmake/llvm
# =============================================================================
cmake_minimum_required(VERSION 3.20)
project(ObfMagic LANGUAGES CXX C)

option(OBF_STANDALONE "Build as a standalone clang pass plugin" OFF)

find_package(LLVM REQUIRED CONFIG HINTS ${LLVM_DIR})
message(STATUS "ObfMagic: LLVM ${LLVM_PACKAGE_VERSION} @ ${LLVM_DIR}")

separate_arguments(LLVM_DEFINITIONS_LIST NATIVE_COMMAND ${LLVM_DEFINITIONS})
add_definitions(${LLVM_DEFINITIONS_LIST})

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

set(OBF_SOURCES
  PassPlugin.cpp
  JunkCode.cpp
  StackIndirectJump.cpp
  StringEncrypt.cpp
)

if(OBF_STANDALONE)
  # ---- 模式 2：clang -fpass-plugin=./libObfMagic.so ----
  add_library(ObfMagic MODULE ${OBF_SOURCES})
  target_include_directories(ObfMagic PRIVATE
    ${LLVM_INCLUDE_DIRS}
    ${CMAKE_CURRENT_SOURCE_DIR}
  )
  target_link_libraries(ObfMagic PRIVATE LLVM)
  set_target_properties(ObfMagic PROPERTIES PREFIX "lib")
  message(STATUS "ObfMagic: standalone plugin -> libObfMagic.so")
else()
  # ---- 模式 1：树内静态库 ----
  add_library(ObfMagic STATIC ${OBF_SOURCES})
  target_include_directories(ObfMagic PRIVATE
    ${LLVM_INCLUDE_DIRS}
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_CURRENT_SOURCE_DIR}/../..
  )
  target_link_libraries(ObfMagic PRIVATE LLVMCore LLVMSupport LLVMAnalysis
                                        LLVMTransformUtils LLVMipo)
endif()

# 让 C++ 侧能 #include "ObfCommon.h" 而不写相对路径
target_include_directories(ObfMagic PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
