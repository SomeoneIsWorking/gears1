# Correctness tests for the instruction implementations added to our
# XenonRecomp fork. These need only the generated ppc_context.h and simde.
add_executable(test_vmx_instructions test_vmx_instructions.cpp)
target_include_directories(test_vmx_instructions PRIVATE
    "${GEARS_PPC_DIR}"
    "${THIRDPARTY_ROOT}/simde")
add_test(NAME vmx_instructions COMMAND test_vmx_instructions)
