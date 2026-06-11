
if (NOT FREERTOS_KERNEL_PATH AND NOT DEFINED ENV{FREERTOS_KERNEL_PATH})
    message("Skipping FreeRTOS examples as FREERTOS_KERNEL_PATH not defined")
    return()
endif()

include(FreeRTOS_Kernel_import.cmake)

# Example running FreeRTOS on 1 core
add_executable(pico_emb
    main.c
    gesture.cpp
    )

target_include_directories(pico_emb PRIVATE
    ${CMAKE_CURRENT_LIST_DIR}
    )

# ─────────────── Edge Impulse: modelo de IA on-device ───────────────
# O classificador roda na mesma Pico que o controlador. A camada de porting
# já existe no SDK (porting/raspberry, feita para pico+FreeRTOS) e é ativada
# pelos defines EI_PORTING_RASPBERRY=1 e FREERTOS_ENABLED.
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

# Obs.: NÃO definimos FREERTOS_ENABLED de propósito. Com ele, a porting do EI
# define seu próprio pvPortCalloc, que colide com o pvPortCalloc já presente
# nesta versão do FreeRTOS-Kernel (heap_4.c). Sem ele, o EI usa malloc/calloc
# do newlib — usados só pela task de inferência (uso single-thread, seguro).
target_compile_definitions(pico_emb PRIVATE
    EI_PORTING_RASPBERRY=1
    TF_LITE_STATIC_MEMORY
    TF_LITE_DISABLE_X86_NEON
    )

# Linking to FreeRTOS-Kernel-Heap4 means we use a dynamic heap for allocations
target_link_libraries(pico_emb PRIVATE
    pico_async_context_freertos
    FreeRTOS-Kernel-Heap4
    hardware_i2c
    hardware_pwm
    Fusion
    pico_stdlib
    )
# Set the nunber of cores to 1.
# This defaults to 2 in FreeRTOSConfig_examples_common.h if not defined in here
target_compile_definitions(pico_emb PRIVATE
    configNUMBER_OF_CORES=1
    )
if(PICO_CYW43_SUPPORTED)
    # For led support on pico_w
    target_link_libraries(pico_emb PRIVATE
        pico_cyw43_arch_none
        )
endif()
pico_add_extra_outputs(pico_emb)

set_target_properties(pico_emb PROPERTIES RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR})
