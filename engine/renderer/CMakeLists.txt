# bbport GPU library: shadPS4 video core + shader recompiler + Gnm/VideoOut
# drivers, adapted to the native_probe runtime through shims in shim/.
cmake_minimum_required(VERSION 3.24)
project(bbgpu LANGUAGES C CXX)
if (NOT WIN32)
    message(FATAL_ERROR "Techmo targets Windows only. Use MSYS2 CLANG64 to build the engine.")
endif()
set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)
if (NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE RelWithDebInfo)
endif()
# Frame pointers keep the loader's thread dumps (watchdog, SIGUSR2 sampling) walkable.
add_compile_options(-fno-omit-frame-pointer)

# bbport: link-time optimization (BB_LTO, default on) and profile-guided optimization
# (BB_PGO: off, generate = instrumented build writing into BB_PGO_DIR, use = build with that
# profile). Covers bbgpu and the static libraries linked into it. No -march: the same build
# runs on compatible Windows PCs.
option(BB_LTO "Link-time optimization of bbgpu" ON)
set(BB_PGO "off" CACHE STRING "Profile-guided optimization: off, generate, use")
set(BB_PGO_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../../pgo" CACHE PATH "Profile directory")
if (BB_LTO)
    include(CheckIPOSupported)
    check_ipo_supported(RESULT bb_ipo_ok OUTPUT bb_ipo_error)
    if (bb_ipo_ok)
        set(CMAKE_INTERPROCEDURAL_OPTIMIZATION ON)
    else()
        message(WARNING "LTO unavailable: ${bb_ipo_error}")
    endif()
endif()
if (BB_PGO STREQUAL "generate")
    # Atomic counters: the GPU, recording, preparation and copy threads run the same code.
    add_compile_options(-fprofile-generate=${BB_PGO_DIR} -fprofile-update=atomic)
    add_link_options(-fprofile-generate=${BB_PGO_DIR})
    add_compile_definitions(BB_PGO_GENERATE)
elseif (BB_PGO STREQUAL "use")
    # Partial training keeps unprofiled code optimized normally; counters from concurrent
    # threads can be slightly inconsistent (correction). Functions changed since the profile
    # (or built differently for it, bbgpu_init) are compiled without it.
    add_compile_options(-fprofile-use=${BB_PGO_DIR} -fprofile-partial-training
                        -fprofile-correction -Wno-missing-profile -Wno-coverage-mismatch)
endif()

find_package(Vulkan REQUIRED)
find_package(fmt REQUIRED)
find_package(Boost REQUIRED)
find_package(tsl-robin-map REQUIRED)
find_package(PkgConfig REQUIRED)
pkg_check_modules(XXHASH REQUIRED IMPORTED_TARGET libxxhash)
pkg_check_modules(SDL3 REQUIRED IMPORTED_TARGET sdl3)
find_package(Zydis REQUIRED)
if (WIN32)
    # bbport: Windows (MSYS2 MINGW64). bbgpu is a static library linked into bb-probe.exe
    # (a DLL cannot leave the runtime_* symbols undefined). Packages MSYS2 does not ship
    # (magic_enum, miniz, xbyak) are fetched; VMA is header-only there.
    include(FetchContent)
    find_package(magic_enum QUIET)
    if (NOT TARGET magic_enum::magic_enum)
        FetchContent_Declare(magic_enum GIT_REPOSITORY https://github.com/Neargye/magic_enum.git
            GIT_TAG v0.9.7 GIT_SHALLOW TRUE)
        FetchContent_MakeAvailable(magic_enum)
    endif()
    find_package(miniz QUIET)
    if (NOT TARGET miniz::miniz)
        set(BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
        set(BUILD_TESTS OFF CACHE BOOL "" FORCE)
        set(BUILD_FUZZERS OFF CACHE BOOL "" FORCE)
        set(BUILD_NO_STDIO OFF CACHE BOOL "" FORCE)
        FetchContent_Declare(miniz GIT_REPOSITORY https://github.com/richgel999/miniz.git
            GIT_TAG 3.1.0 GIT_SHALLOW TRUE)
        FetchContent_MakeAvailable(miniz)
        if (NOT TARGET miniz::miniz)
            add_library(miniz::miniz ALIAS miniz)
        endif()
    endif()
    find_package(xbyak QUIET)
    if (NOT TARGET xbyak::xbyak)
        FetchContent_Declare(xbyak GIT_REPOSITORY https://github.com/herumi/xbyak.git
            GIT_TAG v7.24.2 GIT_SHALLOW TRUE)
        FetchContent_MakeAvailable(xbyak)
    endif()
    find_package(VulkanMemoryAllocator CONFIG QUIET)
    if (NOT TARGET GPUOpen::VulkanMemoryAllocator)
        find_path(VMA_INCLUDE_DIR vk_mem_alloc.h PATH_SUFFIXES vma REQUIRED)
        add_library(GPUOpen::VulkanMemoryAllocator INTERFACE IMPORTED)
        set_target_properties(GPUOpen::VulkanMemoryAllocator PROPERTIES
            INTERFACE_INCLUDE_DIRECTORIES ${VMA_INCLUDE_DIR})
    endif()
endif()
find_program(GLSLANG_COMPILER NAMES glslang glslangValidator REQUIRED)

set(SIRIT_USE_SYSTEM_SPIRV_HEADERS OFF CACHE BOOL "" FORCE)
add_subdirectory(third_party/sirit EXCLUDE_FROM_ALL)

# bbport: FSR 3.1 temporal upscaling / frame generation, native Vulkan (MIT, FireBurn/FSR-Vulkan).
set(FFX_VK_PORTABLE_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(FFX_VK_PORTABLE_BUILD_PROBE OFF CACHE BOOL "" FORCE)
add_subdirectory(third_party/fsr-vulkan EXCLUDE_FROM_ALL)
if (WIN32)
    # AMD's FidelityFX sources rely on MSVC's transitive includes (memset, floor, placement new,
    # swprintf_s) on _WIN32. Include stdlib.h explicitly for the CRT's _countof.
    foreach(ffx_target IN ITEMS ffx_vulkan_portable ffx_vulkan_fsr3_vk_backend_1_1_4
            ffx_vulkan_fsr3_host_1_1_4 ffx_vulkan_fsr4_v07_vulkan ffx_vulkan_fsr4_v07_assets)
        if (TARGET ${ffx_target})
            target_compile_options(${ffx_target} PRIVATE "$<$<COMPILE_LANGUAGE:CXX>:SHELL:-include stdlib.h>"
                "$<$<COMPILE_LANGUAGE:CXX>:SHELL:-include cstdio>"
                "$<$<COMPILE_LANGUAGE:CXX>:SHELL:-include cwchar>" "$<$<COMPILE_LANGUAGE:CXX>:SHELL:-include cstring>"
                "$<$<COMPILE_LANGUAGE:CXX>:SHELL:-include cmath>" "$<$<COMPILE_LANGUAGE:CXX>:SHELL:-include new>"
                "$<$<COMPILE_LANGUAGE:CXX>:SHELL:-include mutex>")
        endif()
    endforeach()
endif()

# Host shaders -> headers (same scheme as shadPS4's host_shaders/CMakeLists.txt).
set(SHADER_SRC ${CMAKE_CURRENT_SOURCE_DIR}/shadps4/video_core/host_shaders)
set(SHADER_INCLUDE ${CMAKE_CURRENT_BINARY_DIR}/include)
set(SHADER_DIR ${SHADER_INCLUDE}/video_core/host_shaders)
set(SHADER_HEADERS "")
function(add_host_shader FILENAME SHADER_NAME)
    set(OUT ${SHADER_DIR}/${SHADER_NAME}.h)
    string(TOUPPER ${SHADER_NAME} SYMBOL)
    set(DEFINES ${ARGN})
    list(TRANSFORM DEFINES PREPEND "-D")
    add_custom_command(OUTPUT ${OUT}
        COMMAND ${CMAKE_COMMAND} -E make_directory ${SHADER_DIR}
        COMMAND ${GLSLANG_COMPILER} --target-env vulkan1.3 -o ${OUT} --vn ${SYMBOL} ${DEFINES} ${SHADER_SRC}/${FILENAME}
        COMMAND ${CMAKE_COMMAND} "-DHEADER_FILE=${OUT}" -P ${SHADER_SRC}/AddShaderHeaderInclude.cmake
        DEPENDS ${SHADER_SRC}/${FILENAME} VERBATIM)
    set(SHADER_HEADERS ${SHADER_HEADERS} ${OUT} PARENT_SCOPE)
endfunction()
add_host_shader(color_to_ms_depth.frag color_to_ms_depth_frag)
add_host_shader(ms_image_blit.frag ms_image_blit_frag)
add_host_shader(ms_image_blit.frag ms_image_blit_msaa_frag SRC_MSAA=1)
add_host_shader(fault_buffer_process.comp fault_buffer_process_comp)
add_host_shader(fs_tri.vert fs_tri_vert)
add_host_shader(fsr.comp fsr_easu_comp SAMPLE_EASU=1)
add_host_shader(fsr.comp fsr_rcas_comp SAMPLE_RCAS=1)
add_host_shader(post_process.frag post_process_frag)
add_host_shader(camera_motion.comp camera_motion_comp)
add_host_shader(upscale_merge.comp upscale_merge_comp)
add_host_shader(upscale_reactive.comp upscale_reactive_comp)
add_host_shader(taa.comp taa_comp)
add_host_shader(taa_sharpen.comp taa_sharpen_comp)
add_host_shader(taa_sharpen.comp taa_sharpen_ldr_comp LDR_OUTPUT=1)
add_host_shader(fsr4_color.comp fsr4_decode_comp)
add_host_shader(fsr4_color.comp fsr4_encode_comp ENCODE=1)
add_host_shader(fsr4_reactive.comp fsr4_reactive_comp)
add_host_shader(buffer_multi_copy.comp buffer_multi_copy_comp)
add_host_shader(depth_resample.frag depth_resample_frag)
add_host_shader(depth_resample.frag depth_stencil_resample_frag HAS_STENCIL=1)
add_host_shader(depth_resample.frag depth_stencil_bits_frag STENCIL_BITS=1)
add_host_shader(camera_motion.comp camera_motion_debug_comp DEBUG_OVERLAY=1)
foreach(BPP IN ITEMS 8 16 32 64 96 128)
    add_host_shader(tiling.comp tiling_micro_${BPP}_comp BITS_PER_PIXEL=${BPP} IS_MACRO_TILED=0)
    add_host_shader(tiling.comp tiling_macro_${BPP}_comp BITS_PER_PIXEL=${BPP} IS_MACRO_TILED=1)
endforeach()
add_custom_target(host_shaders DEPENDS ${SHADER_HEADERS})

file(GLOB_RECURSE VIDEO_CORE CONFIGURE_DEPENDS shadps4/video_core/*.cpp)
file(GLOB_RECURSE RECOMPILER CONFIGURE_DEPENDS shadps4/shader_recompiler/*.cpp)
list(FILTER VIDEO_CORE EXCLUDE REGEX "renderdoc\\.cpp$")
set(COMMON
    shadps4/common/decoder.cpp shadps4/common/error.cpp
    shadps4/common/io_file.cpp shadps4/common/native_clock.cpp shadps4/common/number_utils.cpp
    shadps4/common/path_util.cpp shadps4/common/rdtsc.cpp shadps4/common/spin_lock.cpp shadps4/common/recursive_lock.cpp
    shadps4/common/string_util.cpp shadps4/common/thread.cpp shadps4/common/signal_context.cpp
    shim/common/logging/log.cpp)
file(GLOB AVPLAYER CONFIGURE_DEPENDS shadps4/core/libraries/avplayer/*.cpp)
pkg_check_modules(FFMPEG REQUIRED IMPORTED_TARGET libavformat libavcodec libavutil libswscale libswresample)
file(GLOB SHIM_SOURCES CONFIGURE_DEPENDS shim/*.cpp shim/*/*.cpp)
set(BBGPU_LIBRARY_TYPE STATIC)
add_library(bbgpu ${BBGPU_LIBRARY_TYPE} ${VIDEO_CORE} ${RECOMPILER} ${COMMON} ${SHIM_SOURCES}
    shadps4/core/libraries/gnmdriver/gnmdriver.cpp
    shadps4/core/libraries/videoout/driver.cpp shadps4/core/libraries/videoout/video_out.cpp
    shadps4/core/libraries/kernel/equeue.cpp ${AVPLAYER}
    shadps4/core/libraries/videodec/video_utils.cpp)
add_dependencies(bbgpu host_shaders)
add_subdirectory(bbnet)
target_link_libraries(bbgpu PUBLIC bbnet)
# In-game settings menu: Dear ImGui (third_party/imgui, MIT) with its Vulkan backend, and an
# embedded Cyrillic font (third_party/fonts).
target_sources(bbgpu PRIVATE third_party/imgui/imgui.cpp third_party/imgui/imgui_draw.cpp
    third_party/imgui/imgui_tables.cpp third_party/imgui/imgui_widgets.cpp
    third_party/imgui/backends/imgui_impl_vulkan.cpp)
target_include_directories(bbgpu PRIVATE third_party/imgui third_party/imgui/backends)
target_compile_definitions(bbgpu PRIVATE IMGUI_IMPL_VULKAN_NO_PROTOTYPES
    BB_FONT_PATH="${CMAKE_CURRENT_SOURCE_DIR}/third_party/fonts/DejaVuSans.ttf")
set_source_files_properties(shim/bbport_overlay.cpp PROPERTIES
    OBJECT_DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/third_party/fonts/DejaVuSans.ttf)
target_include_directories(bbgpu PRIVATE shim shadps4 ${SHADER_INCLUDE} third_party/gcn/include third_party/half/include)
target_compile_definitions(bbgpu PRIVATE BOOST_ASIO_STANDALONE NDEBUG)
target_compile_options(bbgpu PRIVATE -Wno-deprecated-declarations -Wno-missing-field-initializers)
target_link_libraries(bbgpu PRIVATE ffx-vulkan::portable ffx-vulkan::fsr4-v07-vulkan)
target_link_libraries(bbgpu PUBLIC Vulkan::Headers fmt::fmt Boost::headers magic_enum::magic_enum
    tsl::robin_map GPUOpen::VulkanMemoryAllocator PkgConfig::XXHASH PkgConfig::SDL3 PkgConfig::FFMPEG miniz::miniz Zydis::Zydis sirit)
if (WIN32)
    # Windows headers: no min/max macros, no winsock 1 clash; Windows 10+ APIs (SetThreadDescription).
    target_compile_definitions(bbgpu PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN _WIN32_WINNT=0x0A00)
    target_link_libraries(bbgpu PUBLIC xbyak::xbyak pdh)
    # bb-probe.exe links libbbgpu.a with these (out/gpu/bbgpu_link.txt, one argument per line,
    # in link order): the static library's transitive dependencies.
    include(cmake/LinkClosure.cmake)
    bb_write_link_closure(bbgpu ${CMAKE_CURRENT_BINARY_DIR}/bbgpu_link.txt)
endif()

# Explicit test targets never launch the game. record-data-test also has a GPU readback mode.
add_executable(image-hash-test EXCLUDE_FROM_ALL ../../tests/test_image_hash.cpp ../../tests/test_gpu_runtime_stubs.cpp)
target_include_directories(image-hash-test PRIVATE shim shadps4 third_party/gcn/include)
target_link_libraries(image-hash-test PRIVATE bbgpu winmm ws2_32 psapi onecore)
add_executable(stable-arena-test EXCLUDE_FROM_ALL ../../tests/test_stable_arena.cpp ../../tests/test_gpu_runtime_stubs.cpp)
target_include_directories(stable-arena-test PRIVATE shim shadps4 third_party/gcn/include)
target_link_libraries(stable-arena-test PRIVATE bbgpu winmm ws2_32 psapi onecore)
add_executable(gc-policy-test EXCLUDE_FROM_ALL ../../tests/test_gc_policy.cpp)
add_executable(hdr-output-test EXCLUDE_FROM_ALL ../../tests/test_hdr_output.cpp)
target_include_directories(hdr-output-test PRIVATE shim)
add_executable(hdr-shader-test EXCLUDE_FROM_ALL ../../tests/test_hdr_shader.cpp ../../tests/test_gpu_runtime_stubs.cpp)
target_include_directories(hdr-shader-test PRIVATE shim shadps4 ${SHADER_INCLUDE} third_party/gcn/include)
target_compile_options(hdr-shader-test PRIVATE -fPIC)
set_source_files_properties(../../tests/test_hdr_shader.cpp PROPERTIES COMPILE_OPTIONS "-fvisibility=hidden;-fvisibility-inlines-hidden")
target_link_libraries(hdr-shader-test PRIVATE bbgpu)
if(WIN32)
    target_link_libraries(hdr-shader-test PRIVATE winmm ws2_32 psapi onecore)
endif()
add_dependencies(hdr-shader-test host_shaders)
target_include_directories(gc-policy-test PRIVATE shadps4)
add_executable(tiled-gc-test EXCLUDE_FROM_ALL ../../tests/test_tiled_gc.cpp)
target_include_directories(tiled-gc-test PRIVATE shim shadps4 third_party/gcn/include)
target_link_libraries(tiled-gc-test PRIVATE bbgpu winmm ws2_32 psapi onecore)
add_executable(tiled-writeback-test EXCLUDE_FROM_ALL ../../tests/test_tiled_writeback.cpp
    ../../tests/test_gpu_runtime_stubs.cpp)
target_include_directories(tiled-writeback-test PRIVATE shim shadps4 third_party/gcn/include)
target_link_libraries(tiled-writeback-test PRIVATE bbgpu winmm ws2_32 psapi onecore)
add_executable(shader-correctness-test EXCLUDE_FROM_ALL ../../tests/test_shader_correctness.cpp
    ../../tests/test_gpu_runtime_stubs.cpp)
target_include_directories(shader-correctness-test PRIVATE shim shadps4 third_party/gcn/include)
target_link_libraries(shader-correctness-test PRIVATE bbgpu winmm ws2_32 psapi)
add_executable(shader-memory-test EXCLUDE_FROM_ALL ../../tests/test_shader_memory.cpp ../../tests/test_gpu_runtime_stubs.cpp)
target_include_directories(shader-memory-test PRIVATE shim shadps4 third_party/gcn/include)
target_link_libraries(shader-memory-test PRIVATE bbgpu winmm ws2_32 psapi onecore)
target_compile_options(shader-memory-test PRIVATE -UNDEBUG)
set_source_files_properties(../../tests/test_shader_memory.cpp PROPERTIES COMPILE_OPTIONS "-fvisibility=hidden;-fvisibility-inlines-hidden")
add_executable(render-semantics-test EXCLUDE_FROM_ALL ../../tests/test_render_semantics.cpp ../../tests/test_gpu_runtime_stubs.cpp)
target_include_directories(render-semantics-test PRIVATE shim shadps4 third_party/gcn/include)
target_link_libraries(render-semantics-test PRIVATE bbgpu winmm ws2_32 psapi onecore)
target_compile_options(render-semantics-test PRIVATE -UNDEBUG)
set_source_files_properties(../../tests/test_render_semantics.cpp PROPERTIES COMPILE_OPTIONS "-fvisibility=hidden;-fvisibility-inlines-hidden")
add_executable(image-layout-cache-test EXCLUDE_FROM_ALL ../../tests/test_image_layout_cache.cpp ../../tests/test_gpu_runtime_stubs.cpp)
target_include_directories(image-layout-cache-test PRIVATE shim shadps4 third_party/gcn/include)
target_link_libraries(image-layout-cache-test PRIVATE bbgpu winmm ws2_32 psapi onecore)
target_compile_options(image-layout-cache-test PRIVATE -UNDEBUG)
set_source_files_properties(../../tests/test_image_layout_cache.cpp PROPERTIES COMPILE_OPTIONS "-fvisibility=hidden;-fvisibility-inlines-hidden")
add_executable(lru-cache-test EXCLUDE_FROM_ALL ../../tests/test_lru_cache.cpp)
target_include_directories(lru-cache-test PRIVATE shadps4)
target_compile_options(lru-cache-test PRIVATE -UNDEBUG -Wall -Wextra -Werror)
add_executable(frame-pacing-test EXCLUDE_FROM_ALL ../../tests/test_frame_pacing.cpp shim/frame_pacing.cpp)
target_compile_options(frame-pacing-test PRIVATE -UNDEBUG -Wall -Wextra -Werror)
target_include_directories(frame-pacing-test PRIVATE shim shadps4)
add_executable(ngx-lifetime-test EXCLUDE_FROM_ALL ../../tests/test_ngx_lifetime.cpp
    ../../tests/test_gpu_runtime_stubs.cpp)
target_compile_options(ngx-lifetime-test PRIVATE -UNDEBUG)
target_include_directories(ngx-lifetime-test PRIVATE shim shadps4 third_party/gcn/include)
target_link_libraries(ngx-lifetime-test PRIVATE bbgpu winmm ws2_32 psapi onecore)
set_source_files_properties(../../tests/test_ngx_lifetime.cpp PROPERTIES
    COMPILE_OPTIONS "-fvisibility=hidden;-fvisibility-inlines-hidden")
add_executable(record-data-test EXCLUDE_FROM_ALL ../../tests/test_record_data.cpp
    ../../tests/test_gpu_runtime_stubs.cpp)
target_compile_options(record-data-test PRIVATE -UNDEBUG)
target_include_directories(record-data-test PRIVATE shim shadps4 third_party/gcn/include)
target_link_libraries(record-data-test PRIVATE bbgpu winmm ws2_32 psapi onecore)
set_source_files_properties(../../tests/test_record_data.cpp PROPERTIES
    COMPILE_OPTIONS "-fvisibility=hidden;-fvisibility-inlines-hidden")
add_executable(memory-notifications-test EXCLUDE_FROM_ALL ../../tests/test_memory_notifications.c
    ../runtime/win32_compat.c ../runtime/win32_memory.c)
target_compile_options(memory-notifications-test PRIVATE -UNDEBUG -Wall -Wextra -Werror)
target_compile_definitions(memory-notifications-test PRIVATE _FILE_OFFSET_BITS=64 _WIN32_WINNT=0x0A00)
target_link_libraries(memory-notifications-test PRIVATE ws2_32 onecore)
add_executable(memory-pressure-test EXCLUDE_FROM_ALL ../../tests/test_memory_pressure.cpp)
target_compile_options(memory-pressure-test PRIVATE -UNDEBUG -Wall -Wextra -Werror)
target_include_directories(memory-pressure-test PRIVATE shadps4)
add_executable(memory-residency-test EXCLUDE_FROM_ALL ../../tests/test_memory_residency.cpp ../../tests/test_gpu_runtime_stubs.cpp)
target_compile_options(memory-residency-test PRIVATE -UNDEBUG)
target_include_directories(memory-residency-test PRIVATE shim shadps4 third_party/gcn/include)
target_link_libraries(memory-residency-test PRIVATE bbgpu)
if (WIN32)
    target_link_libraries(memory-residency-test PRIVATE winmm ws2_32 psapi onecore)
endif()
set_source_files_properties(../../tests/test_memory_residency.cpp PROPERTIES
    COMPILE_OPTIONS "-fvisibility=hidden;-fvisibility-inlines-hidden")
add_executable(image-buffer-sync-test EXCLUDE_FROM_ALL ../../tests/test_image_buffer_sync.cpp ../../tests/test_gpu_runtime_stubs.cpp)
target_compile_options(image-buffer-sync-test PRIVATE -UNDEBUG)
target_include_directories(image-buffer-sync-test PRIVATE shim shadps4 third_party/gcn/include)
target_link_libraries(image-buffer-sync-test PRIVATE bbgpu)
if (WIN32)
    target_link_libraries(image-buffer-sync-test PRIVATE winmm ws2_32 psapi onecore)
endif()
set_source_files_properties(../../tests/test_image_buffer_sync.cpp PROPERTIES
    COMPILE_OPTIONS "-fvisibility=hidden;-fvisibility-inlines-hidden")
add_executable(buffer-budget-test EXCLUDE_FROM_ALL ../../tests/test_buffer_budget.cpp)
add_custom_command(OUTPUT ${CMAKE_CURRENT_BINARY_DIR}/test_shaders/memory_safety_comp.h
    COMMAND ${CMAKE_COMMAND} -E make_directory ${CMAKE_CURRENT_BINARY_DIR}/test_shaders
    COMMAND ${GLSLANG_COMPILER} --target-env vulkan1.3 --vn MEMORY_SAFETY_COMP
        -o ${CMAKE_CURRENT_BINARY_DIR}/test_shaders/memory_safety_comp.h
        ${CMAKE_CURRENT_SOURCE_DIR}/../../tests/shaders/memory_safety.comp
    DEPENDS ${GLSLANG_COMPILER} ../../tests/shaders/memory_safety.comp VERBATIM)
target_sources(buffer-budget-test PRIVATE ${CMAKE_CURRENT_BINARY_DIR}/test_shaders/memory_safety_comp.h)
add_custom_command(OUTPUT ${CMAKE_CURRENT_BINARY_DIR}/test_shaders/null_index_vert.h
    COMMAND ${CMAKE_COMMAND} -E make_directory ${CMAKE_CURRENT_BINARY_DIR}/test_shaders
    COMMAND ${GLSLANG_COMPILER} --target-env vulkan1.3 --vn NULL_INDEX_VERT
        -o ${CMAKE_CURRENT_BINARY_DIR}/test_shaders/null_index_vert.h
        ${CMAKE_CURRENT_SOURCE_DIR}/../../tests/shaders/null_index.vert
    DEPENDS ${GLSLANG_COMPILER} ../../tests/shaders/null_index.vert VERBATIM)
target_sources(buffer-budget-test PRIVATE ${CMAKE_CURRENT_BINARY_DIR}/test_shaders/null_index_vert.h)
target_include_directories(buffer-budget-test PRIVATE ${CMAKE_CURRENT_BINARY_DIR}/test_shaders)
target_compile_options(buffer-budget-test PRIVATE -UNDEBUG)
target_include_directories(buffer-budget-test PRIVATE shim shadps4 third_party/gcn/include)
target_link_libraries(buffer-budget-test PRIVATE bbgpu winmm ws2_32 psapi onecore)
set_source_files_properties(../../tests/test_buffer_budget.cpp PROPERTIES
    COMPILE_OPTIONS "-fvisibility=hidden;-fvisibility-inlines-hidden")
add_executable(index-buffer-range-test EXCLUDE_FROM_ALL ../../tests/test_index_buffer_range.cpp)
target_compile_options(index-buffer-range-test PRIVATE -UNDEBUG -Wall -Wextra -Werror)
target_include_directories(index-buffer-range-test PRIVATE shadps4)
add_executable(liverpool-tasks-test EXCLUDE_FROM_ALL ../../tests/test_liverpool_tasks.cpp
    ../../tests/test_gpu_runtime_stubs.cpp)
target_compile_options(liverpool-tasks-test PRIVATE -UNDEBUG)
target_include_directories(liverpool-tasks-test PRIVATE shim shadps4 third_party/gcn/include)
target_link_libraries(liverpool-tasks-test PRIVATE bbgpu winmm ws2_32 psapi onecore)
set_source_files_properties(../../tests/test_liverpool_tasks.cpp PROPERTIES
    COMPILE_OPTIONS "-fvisibility=hidden;-fvisibility-inlines-hidden")
add_executable(pipeline-cache-blob-test EXCLUDE_FROM_ALL ../../tests/test_pipeline_cache_blob.cpp)
add_executable(pipeline-precompiler-test EXCLUDE_FROM_ALL ../../tests/test_pipeline_precompiler.cpp)
add_executable(pipeline-key-test EXCLUDE_FROM_ALL ../../tests/test_pipeline_key.cpp)
target_compile_options(pipeline-key-test PRIVATE -UNDEBUG -Wall -Wextra -Werror)
target_include_directories(pipeline-key-test PRIVATE shim)
target_include_directories(pipeline-key-test SYSTEM PRIVATE shadps4 third_party/gcn/include)
target_link_libraries(pipeline-key-test PRIVATE xxhash)

target_compile_options(pipeline-precompiler-test PRIVATE -UNDEBUG -Wall -Wextra -Werror)
target_include_directories(pipeline-precompiler-test PRIVATE shadps4)
foreach(stage IN ITEMS vert frag comp)
    string(TOUPPER ${stage} upper_stage)
    add_custom_command(OUTPUT ${CMAKE_CURRENT_BINARY_DIR}/test_shaders/pipeline_prep_${stage}.h
        COMMAND ${CMAKE_COMMAND} -E make_directory ${CMAKE_CURRENT_BINARY_DIR}/test_shaders
        COMMAND ${GLSLANG_COMPILER} --target-env vulkan1.3 --vn PIPELINE_PREP_${upper_stage}
            -o ${CMAKE_CURRENT_BINARY_DIR}/test_shaders/pipeline_prep_${stage}.h
            ${CMAKE_CURRENT_SOURCE_DIR}/../../tests/shaders/pipeline_prep.${stage}
        DEPENDS ${GLSLANG_COMPILER} ../../tests/shaders/pipeline_prep.${stage} VERBATIM)
endforeach()
add_custom_command(OUTPUT ${CMAKE_CURRENT_BINARY_DIR}/test_shaders/pipeline_prep_quad.h
    COMMAND ${CMAKE_COMMAND} -E make_directory ${CMAKE_CURRENT_BINARY_DIR}/test_shaders
    COMMAND ${GLSLANG_COMPILER} --target-env vulkan1.3 --vn PIPELINE_PREP_QUAD
        -o ${CMAKE_CURRENT_BINARY_DIR}/test_shaders/pipeline_prep_quad.h
        ${CMAKE_CURRENT_SOURCE_DIR}/../../tests/shaders/pipeline_prep_quad.vert
    DEPENDS ${GLSLANG_COMPILER} ../../tests/shaders/pipeline_prep_quad.vert VERBATIM)
add_executable(pipeline-precompile-gpu-test EXCLUDE_FROM_ALL ../../tests/test_pipeline_precompile_gpu.cpp
    ../../tests/test_gpu_runtime_stubs.cpp
    ${CMAKE_CURRENT_BINARY_DIR}/test_shaders/pipeline_prep_vert.h
    ${CMAKE_CURRENT_BINARY_DIR}/test_shaders/pipeline_prep_frag.h
    ${CMAKE_CURRENT_BINARY_DIR}/test_shaders/pipeline_prep_comp.h
    ${CMAKE_CURRENT_BINARY_DIR}/test_shaders/pipeline_prep_quad.h)
target_include_directories(pipeline-precompile-gpu-test PRIVATE shim shadps4
    third_party/gcn/include ${CMAKE_CURRENT_BINARY_DIR}/test_shaders)
target_compile_options(pipeline-precompile-gpu-test PRIVATE -UNDEBUG)
target_link_libraries(pipeline-precompile-gpu-test PRIVATE bbgpu winmm ws2_32 psapi onecore)
set_source_files_properties(../../tests/test_pipeline_precompile_gpu.cpp PROPERTIES
    COMPILE_OPTIONS "-fvisibility=hidden;-fvisibility-inlines-hidden")
add_custom_command(OUTPUT ${CMAKE_CURRENT_BINARY_DIR}/test_shaders/buffer_access_vert.h
    COMMAND ${CMAKE_COMMAND} -E make_directory ${CMAKE_CURRENT_BINARY_DIR}/test_shaders
    COMMAND ${GLSLANG_COMPILER} --target-env vulkan1.3 --vn BUFFER_ACCESS_VERT
        -o ${CMAKE_CURRENT_BINARY_DIR}/test_shaders/buffer_access_vert.h
        ${CMAKE_CURRENT_SOURCE_DIR}/../../tests/shaders/buffer_access.vert
    DEPENDS ${GLSLANG_COMPILER} ../../tests/shaders/buffer_access.vert VERBATIM)
add_executable(buffer-access-test EXCLUDE_FROM_ALL ../../tests/test_buffer_access.cpp ../../tests/test_gpu_runtime_stubs.cpp
    ${CMAKE_CURRENT_BINARY_DIR}/test_shaders/buffer_access_vert.h ${CMAKE_CURRENT_BINARY_DIR}/test_shaders/pipeline_prep_comp.h)
target_include_directories(buffer-access-test PRIVATE shim shadps4 third_party/gcn/include ${CMAKE_CURRENT_BINARY_DIR}/test_shaders)
target_link_libraries(buffer-access-test PRIVATE bbgpu winmm ws2_32 psapi onecore)
target_compile_options(buffer-access-test PRIVATE -UNDEBUG)
set_source_files_properties(../../tests/test_buffer_access.cpp PROPERTIES COMPILE_OPTIONS "-fvisibility=hidden;-fvisibility-inlines-hidden")
target_compile_options(pipeline-cache-blob-test PRIVATE -UNDEBUG -Wall -Wextra -Werror)
target_include_directories(pipeline-cache-blob-test PRIVATE ../..)
add_executable(image-overlap-test EXCLUDE_FROM_ALL ../../tests/test_image_overlap.cpp ../../tests/test_gpu_runtime_stubs.cpp)
target_compile_options(image-overlap-test PRIVATE -UNDEBUG)
target_include_directories(image-overlap-test PRIVATE shim shadps4 third_party/gcn/include)
target_link_libraries(image-overlap-test PRIVATE bbgpu)
if (WIN32)
    target_link_libraries(image-overlap-test PRIVATE winmm ws2_32 psapi onecore)
endif()
set_source_files_properties(../../tests/test_image_overlap.cpp PROPERTIES
    COMPILE_OPTIONS "-fvisibility=hidden;-fvisibility-inlines-hidden")
add_executable(pipeline-archive-test EXCLUDE_FROM_ALL ../../tests/test_pipeline_archive.cpp)
add_executable(gpu-profiler-test EXCLUDE_FROM_ALL ../../tests/test_gpu_profiler.cpp ../../tests/test_gpu_runtime_stubs.cpp)
target_compile_options(gpu-profiler-test PRIVATE -UNDEBUG)
target_include_directories(gpu-profiler-test PRIVATE shim shadps4 third_party/gcn/include)
target_link_libraries(gpu-profiler-test PRIVATE bbgpu)
if (WIN32)
    target_link_libraries(gpu-profiler-test PRIVATE winmm ws2_32 psapi onecore)
endif()
set_source_files_properties(../../tests/test_gpu_profiler.cpp PROPERTIES
    COMPILE_OPTIONS "-fvisibility=hidden;-fvisibility-inlines-hidden")
target_compile_options(pipeline-archive-test PRIVATE -UNDEBUG)
target_include_directories(pipeline-archive-test PRIVATE shadps4)
target_link_libraries(pipeline-archive-test PRIVATE fmt::fmt)
add_executable(motion-history-test EXCLUDE_FROM_ALL ../../tests/test_motion_history.cpp)
target_compile_options(motion-history-test PRIVATE -UNDEBUG -Wall -Wextra -Werror)
target_include_directories(motion-history-test PRIVATE ../..)
add_executable(motion-shader-test EXCLUDE_FROM_ALL ../../tests/test_motion_shaders.cpp ../../tests/test_gpu_runtime_stubs.cpp)
target_compile_options(motion-shader-test PRIVATE -UNDEBUG)
target_include_directories(motion-shader-test PRIVATE shim shadps4 third_party/gcn/include)
target_link_libraries(motion-shader-test PRIVATE bbgpu)
if (WIN32)
    target_link_libraries(motion-shader-test PRIVATE winmm ws2_32 psapi onecore)
endif()
add_executable(ui-composition-test EXCLUDE_FROM_ALL ../../tests/test_ui_composition.cpp)
target_compile_options(ui-composition-test PRIVATE -UNDEBUG -Wall -Wextra -Werror)
target_include_directories(ui-composition-test PRIVATE ../..)
add_executable(settings-test EXCLUDE_FROM_ALL ../../tests/test_settings.cpp shim/bbport_settings.cpp)
add_executable(performance-monitor-test EXCLUDE_FROM_ALL ../../tests/test_performance_monitor.cpp shim/performance_monitor.cpp)
target_include_directories(performance-monitor-test PRIVATE shim)
target_compile_options(performance-monitor-test PRIVATE -Wall -Wextra -Werror)
target_compile_definitions(performance-monitor-test PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN _WIN32_WINNT=0x0A00)
target_link_libraries(performance-monitor-test PRIVATE pdh)
add_executable(performance-monitor-gpu-test EXCLUDE_FROM_ALL ../../tests/test_performance_monitor_gpu.cpp ../../tests/test_gpu_runtime_stubs.cpp)
target_include_directories(performance-monitor-gpu-test PRIVATE shim shadps4 third_party/gcn/include)
target_compile_options(performance-monitor-gpu-test PRIVATE -fPIC)
set_source_files_properties(../../tests/test_performance_monitor_gpu.cpp PROPERTIES COMPILE_OPTIONS "-fvisibility=hidden;-fvisibility-inlines-hidden")
target_link_libraries(performance-monitor-gpu-test PRIVATE bbgpu winmm ws2_32 psapi onecore)
target_compile_options(settings-test PRIVATE -UNDEBUG -Wall -Wextra -Werror)
target_include_directories(settings-test PRIVATE ../.. shim)

add_executable(scene-resolution-test EXCLUDE_FROM_ALL ../../tests/test_scene_resolution.cpp ../../tests/test_gpu_runtime_stubs.cpp)
target_include_directories(scene-resolution-test PRIVATE shim shadps4 third_party/gcn/include)
# The test's Vulkan-Hpp inline methods have a different set of platform extensions
# from vk_platform.cpp. Keep them local and use an explicit dispatcher in the test.
target_compile_options(scene-resolution-test PRIVATE -UNDEBUG -fPIC)
set_source_files_properties(../../tests/test_scene_resolution.cpp PROPERTIES
    COMPILE_OPTIONS "-fvisibility=hidden;-fvisibility-inlines-hidden")
target_link_libraries(scene-resolution-test PRIVATE bbgpu)
if (WIN32)
    target_link_libraries(scene-resolution-test PRIVATE winmm ws2_32 psapi onecore)
endif()
add_executable(taa-shader-test EXCLUDE_FROM_ALL ../../tests/test_taa_shader.cpp ../../tests/test_gpu_runtime_stubs.cpp)
target_include_directories(taa-shader-test PRIVATE shim shadps4 ${SHADER_INCLUDE} third_party/gcn/include)
target_compile_options(taa-shader-test PRIVATE -UNDEBUG -fPIC)
set_source_files_properties(../../tests/test_taa_shader.cpp PROPERTIES
    COMPILE_OPTIONS "-fvisibility=hidden;-fvisibility-inlines-hidden")
target_link_libraries(taa-shader-test PRIVATE bbgpu)
add_dependencies(taa-shader-test host_shaders)

add_executable(camera-motion-test EXCLUDE_FROM_ALL ../../tests/test_camera_motion.cpp ../../tests/test_gpu_runtime_stubs.cpp)
target_include_directories(camera-motion-test PRIVATE shim shadps4 ${SHADER_INCLUDE} third_party/gcn/include)
target_compile_options(camera-motion-test PRIVATE -UNDEBUG -fPIC)
target_link_libraries(camera-motion-test PRIVATE bbgpu)
add_dependencies(camera-motion-test host_shaders)

# bbport: FSR 4 benchmark outside the game (tools/fsr4_bench.cpp): ninja -C out/gpu fsr4-bench.
add_executable(upscaler-support-test EXCLUDE_FROM_ALL ../../tests/test_upscaler_support.cpp
    shim/bbport_settings.cpp)
target_include_directories(upscaler-support-test PRIVATE shim)
target_compile_options(upscaler-support-test PRIVATE -UNDEBUG)

add_executable(fsr4-bench EXCLUDE_FROM_ALL tools/fsr4_bench.cpp
    shadps4/video_core/renderer_vulkan/fsr411/fsr411.cpp)
target_include_directories(fsr4-bench PRIVATE shadps4/video_core/renderer_vulkan/fsr411)
target_link_libraries(fsr4-bench PRIVATE ffx-vulkan::fsr4-v07-vulkan ffx-vulkan::fsr4-v07-assets Vulkan::Vulkan)

add_executable(engine-hooks-test EXCLUDE_FROM_ALL ../../tests/test_engine_hooks.cpp shim/engine/engine_hooks.cpp)
target_compile_options(engine-hooks-test PRIVATE -UNDEBUG -Wall -Wextra -Werror)
target_include_directories(engine-hooks-test PRIVATE ../..)
# Local game-derived native menu assets. No game data is embedded in this tool.
find_package(ZLIB REQUIRED)
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
    ${CMAKE_CURRENT_SOURCE_DIR}/../../tools/engine/pc_option_messages.tsv)
file(READ ${CMAKE_CURRENT_SOURCE_DIR}/../../tools/engine/pc_option_messages.tsv BB_ENGINE_MESSAGES)
file(WRITE ${CMAKE_CURRENT_BINARY_DIR}/include/pc_option_messages.inc "R\"tsv(${BB_ENGINE_MESSAGES})tsv\"\n")
add_executable(bb-engine-assets ../../tools/engine/menu_assets.cpp ../../tools/engine/esd.cpp ../../tools/engine/rebirth_script.cpp ../../tools/engine/sha256.cpp)
target_include_directories(bb-engine-assets PRIVATE ${CMAKE_CURRENT_BINARY_DIR}/include)
target_link_libraries(bb-engine-assets PRIVATE ZLIB::ZLIB)
if (WIN32)
    target_link_libraries(bb-engine-assets PRIVATE shell32)
endif()
add_executable(engine-menu-memory-test EXCLUDE_FROM_ALL ../../tests/test_engine_menu_memory.cpp shim/engine/menu_memory.cpp)
target_compile_options(engine-menu-memory-test PRIVATE -UNDEBUG -Wall -Wextra -Werror)
target_include_directories(engine-menu-memory-test PRIVATE ../..)
add_executable(rebirth-refund-test EXCLUDE_FROM_ALL ../../tests/test_rebirth_refund.cpp)
target_compile_options(rebirth-refund-test PRIVATE -UNDEBUG -Wall -Wextra -Werror)
target_include_directories(rebirth-refund-test PRIVATE ../..)
add_executable(pc-input-test EXCLUDE_FROM_ALL ../../tests/test_pc_input.cpp shim/input/bindings.cpp)
target_include_directories(pc-input-test PRIVATE shim)
target_link_libraries(pc-input-test PRIVATE PkgConfig::SDL3)
target_compile_options(pc-input-test PRIVATE -UNDEBUG -Wall -Wextra -Werror)
add_executable(pc-input-bridge-test EXCLUDE_FROM_ALL ../../tests/test_pc_input_bridge.cpp shim/input/input.cpp shim/input/bindings.cpp)
target_include_directories(pc-input-bridge-test PRIVATE shim shadps4)
target_link_libraries(pc-input-bridge-test PRIVATE PkgConfig::SDL3 Vulkan::Headers)
target_compile_options(pc-input-bridge-test PRIVATE -UNDEBUG)
add_executable(widescreen-test EXCLUDE_FROM_ALL ../../tests/test_widescreen.cpp
    shim/engine/widescreen.cpp shim/engine/engine_hooks.cpp shim/bbport_settings.cpp)
target_include_directories(widescreen-test PRIVATE shim)
target_compile_options(widescreen-test PRIVATE -UNDEBUG -Wall -Wextra -Werror)
