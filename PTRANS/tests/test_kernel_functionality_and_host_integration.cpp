//
// Created by Marius Meyer on 04.12.19.
//
#include <memory>

#include "gtest/gtest.h"
#include "../src/host/execution.h"
#include "../src/host/transpose_functionality.hpp"
#include "parameters.h"
#include "testing/test_program_settings.h"


struct OpenCLKernelTest : testing::Test {
    std::string kernelFileName;
    HOST_DATA_TYPE *A;
    HOST_DATA_TYPE *B;
    HOST_DATA_TYPE *A_out;
    std::shared_ptr<bm_execution::ExecutionConfiguration> config;
    cl_uint matrix_size;
    cl::Program program;
    cl::Context context;
    std::vector<cl::Device> device;

    OpenCLKernelTest() {
        kernelFileName = programSettings->kernelFileName;
        matrix_size = BLOCK_SIZE;
        posix_memalign(reinterpret_cast<void **>(&A), 64,
                       sizeof(HOST_DATA_TYPE) * matrix_size * matrix_size);
        posix_memalign(reinterpret_cast<void **>(&B), 64,
                       sizeof(HOST_DATA_TYPE) * matrix_size * matrix_size);
        posix_memalign(reinterpret_cast<void **>(&A_out), 64,
                       sizeof(HOST_DATA_TYPE) * matrix_size * matrix_size);
        for (int i = 0; i < matrix_size; i++) {
            for (int j = 0; j < matrix_size; j++) {
                A[i * matrix_size + j] = 0.0;
                B[i * matrix_size + j] = 0.0;
                A_out[i * matrix_size + j] = 0.0;
            }
        }
        setupFPGA();
    }

    void setupFPGA() {
        if (!config.get()) {
            // TODO: Workaround. File bug report to XRT?
            // This is done because of a bug in Xilix XRT that does not allow
            // to reprogram an FPGA twice which will crash with CL_OUT_OF_RESOURCES
            device = fpga_setup::selectFPGADevice(programSettings->defaultPlatform, programSettings->defaultDevice);
            context = cl::Context(device[0]);
            program = fpga_setup::fpgaSetup(&context, device, &kernelFileName);
        }

        config = std::make_shared<bm_execution::ExecutionConfiguration>(
                bm_execution::ExecutionConfiguration{
                        context, device[0], program,
                        KERNEL_NAME,
                        1,
                        matrix_size,
                        BLOCK_SIZE,
                        false
                });
        generateInputData(matrix_size, A, B);
    }

    ~OpenCLKernelTest() override {
        free(A);
        free(B);
        free(A_out);
    }
};


/**
 * Tests if B will not be transposed
 */
TEST_F(OpenCLKernelTest, FPGACorrectBStaysTheSame) {
    for (int i = 0; i < matrix_size; i++) {
        for (int j = 0; j < matrix_size; j++) {
            A[i * matrix_size + j] = 0.0;
            B[i * matrix_size + j] = i * matrix_size + j;
        }
    }
    auto result = bm_execution::calculate(config, A, B, A_out);
    for (int i = 0; i < matrix_size; i++) {
        for (int j = 0; j < matrix_size; j++) {
            EXPECT_FLOAT_EQ(A_out[i * matrix_size + j], B[i * matrix_size + j]);
        }
    }
}

/**
 * Tests if a block of A will be correctly transposed
 */
TEST_F(OpenCLKernelTest, FPGAABlockIsTransposed) {
    for (int i = 0; i < matrix_size; i++) {
        for (int j = 0; j < matrix_size; j++) {
            A[i * matrix_size + j] = i * matrix_size + j;
            B[i * matrix_size + j] = 0.0;
        }
    }
    auto result = bm_execution::calculate(config, A, B, A_out);
    for (int i = 0; i < matrix_size; i++) {
        for (int j = 0; j < matrix_size; j++) {
            EXPECT_FLOAT_EQ(A_out[i * matrix_size + j], A[j * matrix_size + i]);
        }
    }
}

/**
 * Tests if A will be transposed when it is bigger than one block
 */
TEST_F(OpenCLKernelTest, FPGAAIsTransposed) {
    // delete memory allocated in constructor
    free(A);
    free(B);
    free(A_out);

    // allocate more memory for test with multiple blocks
    matrix_size = 2 * BLOCK_SIZE;
    posix_memalign(reinterpret_cast<void **>(&A), 64,
                   sizeof(HOST_DATA_TYPE) * matrix_size * matrix_size);
    posix_memalign(reinterpret_cast<void **>(&B), 64,
                   sizeof(HOST_DATA_TYPE) * matrix_size * matrix_size);
    posix_memalign(reinterpret_cast<void **>(&A_out), 64,
                   sizeof(HOST_DATA_TYPE) * matrix_size * matrix_size);

    setupFPGA();

    // Do actual test

    for (int i = 0; i < matrix_size; i++) {
        for (int j = 0; j < matrix_size; j++) {
            A[i * matrix_size + j] = i * matrix_size + j;
            B[i * matrix_size + j] = 0.0;
        }
    }
    auto result = bm_execution::calculate(config, A, B, A_out);
    for (int i = 0; i < matrix_size; i++) {
        for (int j = 0; j < matrix_size; j++) {
            EXPECT_FLOAT_EQ(A_out[i * matrix_size + j], A[j * matrix_size + i]);
        }
    }
}

/**
 * Tests if matrix A and B will be summed up in the result
 */
TEST_F(OpenCLKernelTest, FPGAAAndBAreSummedUp) {
    for (int i = 0; i < matrix_size; i++) {
        for (int j = 0; j < matrix_size; j++) {
            A[i * matrix_size + j] = 1.0;
            B[i * matrix_size + j] = i * matrix_size + j;
        }
    }
    auto result = bm_execution::calculate(config, A, B, A_out);
    for (int i = 0; i < matrix_size; i++) {
        for (int j = 0; j < matrix_size; j++) {
            EXPECT_FLOAT_EQ(A_out[i * matrix_size + j], B[i * matrix_size + j] + 1.0);
        }
    }
}


/**
 * Checks the size and values of the timing measurements that are retured by calculate.
 */
TEST_F(OpenCLKernelTest, FPGATimingsMeasuredForEveryIteration) {
    config->repetitons = 10;
    auto result = bm_execution::calculate(config, A, B, A_out);
    EXPECT_EQ(result->calculationTimings.size(), 10);
    EXPECT_EQ(result->transferTimings.size(), 10);
    for (int t = 0; t < 10; t++) {
        EXPECT_GE(result->transferTimings[t], 0.0);
        EXPECT_GE(result->calculationTimings[t], 0.0);
    }
}

/**
 * Check if the generated input data is in the specified range
 */
TEST(ExecutionDefault, GenerateInputDataRange) {
    HOST_DATA_TYPE *A = new HOST_DATA_TYPE[25];
    HOST_DATA_TYPE *B = new HOST_DATA_TYPE[25];
    generateInputData(5, A, B);
    for (int i = 0; i < 5; i++) {
        for (int j = 0; j < 5; j++) {
            EXPECT_LT(A[i * 5 + j], 100);
            EXPECT_GT(A[i * 5 + j], -100);
            EXPECT_LT(B[i * 5 + j], 101);
            EXPECT_GT(B[i * 5 + j], -99);
        }
    }
}

/**
 * Check if the input data is generated correctly
 */
TEST(ExecutionDefault, GenerateInputDataCorrectness) {
    HOST_DATA_TYPE *A = new HOST_DATA_TYPE[25];
    HOST_DATA_TYPE *B = new HOST_DATA_TYPE[25];
    HOST_DATA_TYPE *result = new HOST_DATA_TYPE[25];
    generateInputData(5, A, B);
    transposeReference(A, B, result, 5);
    for (int i = 0; i < 5; i++) {
        for (int j = 0; j < 5; j++) {
            EXPECT_NEAR(result[i * 5 + j], 1.0, std::numeric_limits<HOST_DATA_TYPE>::epsilon());
        }
    }
}
