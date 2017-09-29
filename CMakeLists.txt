cmake_minimum_required(VERSION 3.6.2)
project(kaleidoscope)

set(CMAKE_CXX_STANDARD 11)
set(CMAKE_CXX_COMPILER /usr/local/opt/llvm/bin/clang)

find_package(LLVM REQUIRED CONFIG)

message(STATUS "Found LLVM ${LLVM_PACKAGE_VERSION}")
message(STATUS "Using LLVMConfig.cmake in: ${LLVM_DIR}")

set(SOURCE_FILES main.cpp KaleidoscopeJIT.h)
add_executable(kaleidoscope ${SOURCE_FILES})

include_directories(${LLVM_INCLUDE_DIRS})
add_definitions(${LLVM_DEFINITIONS})

# Find the libraries that correspond to the LLVM components
# that we wish to use
llvm_map_components_to_libnames(llvm_libs support core irreader codegen native mcjit )



set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -fno-rtti -lSystem -lc++" )

# Link against LLVM libraries
target_link_libraries(kaleidoscope ${llvm_libs})



