cmake_minimum_required(VERSION 3.28...3.30)

include("${CMAKE_CURRENT_SOURCE_DIR}/cmake/common/bootstrap.cmake" NO_POLICY_SCOPE)

project(${_name} VERSION ${_version})

option(ENABLE_FRONTEND_API "Use obs-frontend-api for UI functionality" ON)
option(ENABLE_QT "Use Qt functionality" ON)
option(BUILD_TESTS "Build test suite" OFF)
option(BUILD_EVAL "Build speech evaluation tool" OFF)

include(compilerconfig)
include(defaults)
include(helpers)

add_library(${CMAKE_PROJECT_NAME} MODULE)

find_package(libobs REQUIRED)
target_link_libraries(${CMAKE_PROJECT_NAME} PRIVATE OBS::libobs)

if(ENABLE_FRONTEND_API)
  find_package(obs-frontend-api REQUIRED)
  target_link_libraries(${CMAKE_PROJECT_NAME} PRIVATE OBS::obs-frontend-api)
endif()

if(ENABLE_QT)
  find_package(Qt6 REQUIRED COMPONENTS Widgets Core Gui)
  target_link_libraries(${CMAKE_PROJECT_NAME} PRIVATE Qt6::Core Qt6::Gui Qt6::Widgets)
  target_compile_options(
    ${CMAKE_PROJECT_NAME}
    PRIVATE $<$<C_COMPILER_ID:Clang,AppleClang>:-Wno-quoted-include-in-framework-header -Wno-comma>
  )
  set_target_properties(
    ${CMAKE_PROJECT_NAME}
    PROPERTIES AUTOMOC ON AUTOUIC ON AUTORCC ON
  )
endif()

include(FetchContent)
set(BUILD_SHARED_LIBS OFF CACHE BOOL "" FORCE)
set(CMAKE_POSITION_INDEPENDENT_CODE ON CACHE BOOL "" FORCE)
set(WHISPER_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(WHISPER_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
set(WHISPER_BUILD_SERVER OFF CACHE BOOL "" FORCE)
set(GGML_CUDA OFF CACHE BOOL "" FORCE)
set(GGML_BLAS OFF CACHE BOOL "" FORCE)
if(APPLE)
  set(GGML_METAL_EMBED_LIBRARY ON CACHE BOOL "" FORCE)
endif()

FetchContent_Declare(
  whisper
  GIT_REPOSITORY https://github.com/ggerganov/whisper.cpp.git
  GIT_TAG v1.7.6
  GIT_SHALLOW TRUE
  SYSTEM
)
set(_PREV_WARNING_AS_ERROR ${CMAKE_COMPILE_WARNING_AS_ERROR})
set(_PREV_XCODE_TREAT_ERRORS ${CMAKE_XCODE_ATTRIBUTE_GCC_TREAT_WARNINGS_AS_ERRORS})
set(_PREV_XCODE_WARN_64_32 ${CMAKE_XCODE_ATTRIBUTE_GCC_WARN_64_TO_32_BIT_CONVERSION})
set(_PREV_XCODE_ARC ${CMAKE_XCODE_ATTRIBUTE_CLANG_ENABLE_OBJC_ARC})
set(CMAKE_COMPILE_WARNING_AS_ERROR OFF)
set(CMAKE_XCODE_ATTRIBUTE_GCC_TREAT_WARNINGS_AS_ERRORS NO)
set(CMAKE_XCODE_ATTRIBUTE_GCC_WARN_64_TO_32_BIT_CONVERSION NO)
set(CMAKE_XCODE_ATTRIBUTE_CLANG_ENABLE_OBJC_ARC NO)

FetchContent_MakeAvailable(whisper)

set(CMAKE_COMPILE_WARNING_AS_ERROR ${_PREV_WARNING_AS_ERROR})
set(CMAKE_XCODE_ATTRIBUTE_GCC_TREAT_WARNINGS_AS_ERRORS ${_PREV_XCODE_TREAT_ERRORS})
set(CMAKE_XCODE_ATTRIBUTE_GCC_WARN_64_TO_32_BIT_CONVERSION ${_PREV_XCODE_WARN_64_32})
set(CMAKE_XCODE_ATTRIBUTE_CLANG_ENABLE_OBJC_ARC ${_PREV_XCODE_ARC})

foreach(
  _t
  whisper
  ggml
  ggml-base
  ggml-cpu
  ggml-metal
)
  if(TARGET ${_t})
    set_target_properties(
      ${_t}
      PROPERTIES
        POSITION_INDEPENDENT_CODE ON
        COMPILE_WARNING_AS_ERROR OFF
        XCODE_ATTRIBUTE_CLANG_ENABLE_OBJC_ARC "NO"
        XCODE_ATTRIBUTE_GCC_TREAT_WARNINGS_AS_ERRORS "NO"
        XCODE_ATTRIBUTE_GCC_WARN_64_TO_32_BIT_CONVERSION "NO"
        XCODE_ATTRIBUTE_WARNING_CFLAGS "-w"
    )
    target_compile_options(
      ${_t}
      PRIVATE
        $<$<C_COMPILER_ID:MSVC>:/W0>
        $<$<CXX_COMPILER_ID:MSVC>:/W0>
        $<$<NOT:$<C_COMPILER_ID:MSVC>>:-w>
        $<$<NOT:$<CXX_COMPILER_ID:MSVC>>:-w>
    )
  endif()
endforeach()

if(TARGET ggml-metal)
  set_target_properties(ggml-metal PROPERTIES XCODE_ATTRIBUTE_CLANG_ENABLE_OBJC_ARC "NO")
  target_compile_options(ggml-metal PRIVATE -fno-objc-arc -w)
endif()

if(TARGET whisper)
  target_link_libraries(${CMAKE_PROJECT_NAME} PRIVATE whisper)
endif()

set(MODEL_URL "https://huggingface.co/ggerganov/whisper.cpp/resolve/main/ggml-tiny.en.bin")
set(MODEL_DIR "${CMAKE_CURRENT_SOURCE_DIR}/data/models")
set(MODEL_FILE "${MODEL_DIR}/ggml-tiny.en.bin")
file(MAKE_DIRECTORY "${MODEL_DIR}")
if(NOT EXISTS "${MODEL_FILE}")
  message(STATUS "Downloading whisper model ggml-tiny.en.bin for installer packaging...")
  file(DOWNLOAD "${MODEL_URL}" "${MODEL_FILE}" SHOW_PROGRESS STATUS DL_STATUS TIMEOUT 60)
  list(GET DL_STATUS 0 DL_ERR)
  if(NOT DL_ERR EQUAL 0)
    message(WARNING "Could not pre-download model during CMake (${DL_STATUS}). Model will be downloaded on first run.")
    file(REMOVE "${MODEL_FILE}")
  endif()
endif()

target_sources(
  ${CMAKE_PROJECT_NAME}
  PRIVATE
    src/plugin-main.cpp
    src/detection-core.cpp
    src/detection-core.hpp
    src/mini-thumbnail.cpp
    src/mini-thumbnail.hpp
    src/audio-engine.cpp
    src/audio-engine.hpp
    src/bible.cpp
    src/bible.hpp
    src/display-state.hpp
    src/dock.cpp
    src/dock.hpp
    src/lower-third.cpp
    src/lower-third.hpp
    src/lyrics.cpp
    src/lyrics.hpp
    src/lyrics-dock.cpp
    src/lyrics-dock.hpp
    src/worship-word-dock.cpp
    src/worship-word-dock.hpp
    data/icons/icons.qrc
)
set_target_properties(${CMAKE_PROJECT_NAME} PROPERTIES CXX_STANDARD 17 CXX_STANDARD_REQUIRED ON)

set_target_properties_plugin(${CMAKE_PROJECT_NAME} PROPERTIES OUTPUT_NAME ${_name})

if(BUILD_TESTS)
  enable_testing()
  add_executable(lyrics-tests tests/lyrics-tests.cpp src/lyrics.cpp src/lyrics.hpp)
  target_link_libraries(lyrics-tests PRIVATE Qt6::Core)
  set_target_properties(
    lyrics-tests
    PROPERTIES AUTOMOC ON CXX_STANDARD 17 CXX_STANDARD_REQUIRED ON
  )
  add_test(NAME lyrics-tests COMMAND lyrics-tests)
endif()

if(BUILD_EVAL OR BUILD_TESTS)
  add_executable(
    ldww-eval
    tools/ldww-eval/main.cpp
    src/detection-core.cpp
    src/detection-core.hpp
    src/lyrics.cpp
    src/lyrics.hpp
    src/bible.cpp
    src/bible.hpp
  )
  target_link_libraries(ldww-eval PRIVATE Qt6::Core Qt6::Gui whisper)
  target_include_directories(ldww-eval PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
  set_target_properties(
    ldww-eval
    PROPERTIES AUTOMOC ON CXX_STANDARD 17 CXX_STANDARD_REQUIRED ON
  )
  if(BUILD_TESTS)
    add_test(
      NAME ldww-eval-synthetic
      COMMAND
        ldww-eval --wav ${CMAKE_CURRENT_SOURCE_DIR}/tests/eval-synthetic/synthetic_eval.wav --truth
        ${CMAKE_CURRENT_SOURCE_DIR}/tests/eval-synthetic/ground_truth.csv --songs-dir
        ${CMAKE_CURRENT_SOURCE_DIR}/data/songs-samples
    )
  endif()
endif()
