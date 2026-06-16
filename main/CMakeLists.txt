
if (NOT FREERTOS_KERNEL_PATH AND NOT DEFINED ENV{FREERTOS_KERNEL_PATH})
    message("Skipping FreeRTOS examples as FREERTOS_KERNEL_PATH not defined")
    return()
endif()

include(FreeRTOS_Kernel_import.cmake)

# Example running FreeRTOS on 1 core
add_executable(pico_emb
    main.c
    # gesture.cpp  # IA (Edge Impulse) removida do build temporariamente
    )

# O protocolo (printf/getchar do main.c) sai pela USB nativa da Pico como
# porta serial virtual (CDC). Antes o stdio ia para a UART (GP0/GP1) e só
# aparecia como COM através do debug probe; com USB CDC, a própria USB da
# Pico vira a porta COM e o controle funciona plugado direto, sem probe.
pico_enable_stdio_usb(pico_emb 1)
pico_enable_stdio_uart(pico_emb 0)

target_include_directories(pico_emb PRIVATE
    ${CMAKE_CURRENT_LIST_DIR}
    )

# ─────────────── Edge Impulse: REMOVIDO do build (temporário) ───────────────
# A IA (gesture.cpp + Edge Impulse) foi tirada do binário porque seus
# construtores estáticos de C++ rodavam antes do main() e travavam o boot,
# impedindo a USB CDC de subir. Para reativar, descomente este bloco E o
# gesture.cpp no add_executable acima.
#[==[
set(EI_ROOT ${CMAKE_CURRENT_LIST_DIR}/../ei-model)
include(${EI_ROOT}/edge-impulse-sdk/cmake/utils.cmake)

target_include_directories(pico_emb PRIVATE
    ${EI_ROOT}
    ${EI_ROOT}/edge-impulse-sdk
    ${EI_ROOT}/model-parameters
    ${EI_ROOT}/tflite-model
    ${EI_ROOT}/edge-impulse-sdk/third_party/flatbuffers/include
    ${EI_ROOT}/edge-impulse-sdk/third_party/gemmlowp
    ${EI_ROOT}/edge-impulse-sdk/third_party/ruy
    ${EI_ROOT}/edge-impulse-sdk/CMSIS/Core/Include
    ${EI_ROOT}/edge-impulse-sdk/CMSIS/DSP/Include
    ${EI_ROOT}/edge-impulse-sdk/CMSIS/DSP/PrivateInclude
    ${EI_ROOT}/edge-impulse-sdk/CMSIS/NN/Include
    )

# tflite-micro + CMSIS-NN + modelo EON-compilado + porting pico
RECURSIVE_FIND_FILE(EI_TF_CC  "${EI_ROOT}/edge-impulse-sdk/tensorflow" "*.cc")
RECURSIVE_FIND_FILE(EI_TF_CPP "${EI_ROOT}/edge-impulse-sdk/tensorflow" "*.cpp")
RECURSIVE_FIND_FILE(EI_TF_C   "${EI_ROOT}/edge-impulse-sdk/tensorflow" "*.c")
RECURSIVE_FIND_FILE(EI_NN_C   "${EI_ROOT}/edge-impulse-sdk/CMSIS/NN/Source" "*.c")
RECURSIVE_FIND_FILE(EI_MODEL  "${EI_ROOT}/tflite-model" "*.cpp")
target_sources(pico_emb PRIVATE
    ${EI_TF_CC} ${EI_TF_CPP} ${EI_TF_C}
    ${EI_NN_C}
    ${EI_MODEL}
    ${EI_ROOT}/edge-impulse-sdk/porting/raspberry/ei_classifier_porting.cpp
    ${EI_ROOT}/edge-impulse-sdk/dsp/dct/fast-dct-fft.cpp
    )

# DSP do EI (memory + kissfft) e subconjunto curado do CMSIS-DSP
include(${EI_ROOT}/edge-impulse-sdk/cmake/add_source.cmake)
apply_predefined_sources(pico_emb USE_DSP)

target_compile_definitions(pico_emb PRIVATE
    EI_PORTING_RASPBERRY=1
    TF_LITE_STATIC_MEMORY
    TF_LITE_DISABLE_X86_NEON
    )
#]==]

# Linking to FreeRTOS-Kernel-Heap4 means we use a dynamic heap for allocations
target_link_libraries(pico_emb PRIVATE
    pico_async_context_freertos
    FreeRTOS-Kernel-Heap4
    hardware_i2c
    hardware_pwm
    Fusion
    pico_stdlib
    )
# SMP: roda o FreeRTOS nos 2 cores do RP2350. A afinidade de cada task é
# fixada em main.c via vTaskCoreAffinitySet (configUSE_CORE_AFFINITY é ligado
# automaticamente pela FreeRTOSConfig.h quando há mais de 1 core).
target_compile_definitions(pico_emb PRIVATE
    configNUMBER_OF_CORES=2
    )
if(PICO_CYW43_SUPPORTED)
    # For led support on pico_w
    target_link_libraries(pico_emb PRIVATE
        pico_cyw43_arch_none
        )
endif()
pico_add_extra_outputs(pico_emb)

set_target_properties(pico_emb PROPERTIES RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR})
