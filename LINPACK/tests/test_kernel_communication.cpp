#include "gtest/gtest.h"
#include "parameters.h"
#include "test_program_settings.h"
#include "linpack_benchmark.hpp"


#define BLOCK_SIZE (1 << LOCAL_MEM_BLOCK_LOG)
#define CHUNK (1 << REGISTER_BLOCK_LOG)


class LinpackKernelCommunicationTest : public testing::Test {

public:
    std::unique_ptr<linpack::LinpackBenchmark> bm;
    std::unique_ptr<linpack::LinpackData> data;
    const unsigned numberOfChannels = 4;
    const std::string channelOutName = "kernel_output_ch";
    const std::string channelInName = "kernel_input_ch";

    void SetUp() override {
        bm = std::unique_ptr<linpack::LinpackBenchmark>(new linpack::LinpackBenchmark(global_argc, global_argv));
        bm->getExecutionSettings().programSettings->isDiagonallyDominant = true;
        bm->getExecutionSettings().programSettings->matrixSize = BLOCK_SIZE;
        data = bm->generateInputData();
        setupExternalChannelFiles();
    }

    /**
     * @brief Setup the external channels files for the execution of a benchmark kernel
     * 
     */
    void
    setupExternalChannelFiles() {
        for (int i=0; i < numberOfChannels; i++) {
            std::string fname = channelOutName + std::to_string(i);
            std::remove(fname.c_str());
            std::ofstream fs;
            fs.open(fname, std::ofstream::out | std::ofstream::trunc);
            fs.close();
        }
    }

    /**
     * @brief Get the Data sent over an external channel.
     * 
     * @param channel_id Id of the external channel. It is assumed to be conntected in the order 0->Top,1->Right,2->Bottom,3->Left.
     *                  So channel 0 will be connected to the channel 2 of the FPGA above the current FPGA in the 2D Torus. 
     * @param output_channel Boolean, if true , the output channel is read otherwise the input channel
     * @return std::vector<HOST_DATA_TYPE> The data that is contained in the output file of the channel
     */
    std::vector<HOST_DATA_TYPE>
    getDataFromExternalChannel(uint channel_id, bool output_channel) {
        std::string fname = ( output_channel ? channelOutName : channelInName) + std::to_string(channel_id);
        std::ifstream fs;
        fs.open(fname, std::ifstream::binary | std::ifstream::in);
        HOST_DATA_TYPE value;
        std::vector<HOST_DATA_TYPE> values;
        while (fs.read(reinterpret_cast<char*>(&value), sizeof(HOST_DATA_TYPE))) {
            values.push_back(value);
        }
        return values;
    }

};


class LinpackKernelCommunicationTestLU : public LinpackKernelCommunicationTest {

    void SetUp() override {
        LinpackKernelCommunicationTest::SetUp();
        executeKernel();
    }

    void executeKernel() {
        int err;
        cl::CommandQueue compute_queue(*bm->getExecutionSettings().context, *bm->getExecutionSettings().device, 0, &err);
        cl::CommandQueue network_queue(*bm->getExecutionSettings().context, *bm->getExecutionSettings().device, 0, &err);
        cl::Buffer buffer(*(bm->getExecutionSettings().context), CL_MEM_READ_WRITE,
                                            sizeof(HOST_DATA_TYPE)*bm->getExecutionSettings().programSettings->matrixSize*bm->getExecutionSettings().programSettings->matrixSize);
        cl::Kernel kernel(*bm->getExecutionSettings().program, "lu", &err);

        err = kernel.setArg(0, buffer);

        // Start network layer kernel
        cl::Kernel network(*bm->getExecutionSettings().program, "network_layer", &err);
        err = network.setArg(0, static_cast<cl_uint>(3));
        err = network.setArg(1, CL_TRUE);
        network_queue.enqueueTask(network);

        compute_queue.enqueueWriteBuffer(buffer, CL_TRUE, 0, sizeof(HOST_DATA_TYPE)*bm->getExecutionSettings().programSettings->matrixSize*bm->getExecutionSettings().programSettings->matrixSize, data->A);
        compute_queue.enqueueTask(kernel);
        compute_queue.finish();
        compute_queue.enqueueReadBuffer(buffer, CL_TRUE, 0, sizeof(HOST_DATA_TYPE)*bm->getExecutionSettings().programSettings->matrixSize*bm->getExecutionSettings().programSettings->matrixSize, data->A);

        network_queue.finish();
    }
};

class LinpackKernelCommunicationTestTop : public LinpackKernelCommunicationTest {

    void SetUp() override {
        LinpackKernelCommunicationTest::SetUp();
        // Generate uniformy distributed data
        bm->getExecutionSettings().programSettings->isDiagonallyDominant = false;
        data = bm->generateInputData();
        bm->getExecutionSettings().programSettings->isDiagonallyDominant = true;
        setupInputChannels();
        executeKernel();
    }

    void setupInputChannels() {
        auto gefa_data = bm->generateInputData();
        linpack::gefa_ref_nopvt(gefa_data->A, bm->getExecutionSettings().programSettings->matrixSize,bm->getExecutionSettings().programSettings->matrixSize);
        // Fill all input channels with the correct number of 1.0s
        for (int i=0; i < numberOfChannels; i++) {
            std::string fname = channelInName + std::to_string(i);
            std::remove(fname.c_str());
            std::ofstream fs;
            fs.open(fname, std::ofstream::out | std::ofstream::trunc | std::ofstream::binary);
            for (int ii = 0; ii < BLOCK_SIZE; ii++ ) {
                for (int jj = (ii / CHUNK) * CHUNK; jj < BLOCK_SIZE; jj++ ) {
                    fs.write(reinterpret_cast<const char*>(&gefa_data->A[jj * bm->getExecutionSettings().programSettings->matrixSize + ii]), sizeof(HOST_DATA_TYPE));
                }
            }
            fs.close();
        }
    }

    void executeKernel() {
        int err;
        cl::CommandQueue compute_queue(*bm->getExecutionSettings().context, *bm->getExecutionSettings().device, 0, &err);
        cl::CommandQueue network_queue(*bm->getExecutionSettings().context, *bm->getExecutionSettings().device, 0, &err);
        cl::Buffer buffer(*(bm->getExecutionSettings().context), CL_MEM_READ_WRITE,
                                            sizeof(HOST_DATA_TYPE)*bm->getExecutionSettings().programSettings->matrixSize*bm->getExecutionSettings().programSettings->matrixSize);
        cl::Kernel kernel(*bm->getExecutionSettings().program, "top_update", &err);

        err = kernel.setArg(0, buffer);

        // Start network layer kernel
        cl::Kernel network(*bm->getExecutionSettings().program, "network_layer", &err);
        err = network.setArg(0, static_cast<cl_uint>(2));
        err = network.setArg(1, CL_TRUE);
        network_queue.enqueueTask(network);

        compute_queue.enqueueWriteBuffer(buffer, CL_TRUE, 0, sizeof(HOST_DATA_TYPE)*bm->getExecutionSettings().programSettings->matrixSize*bm->getExecutionSettings().programSettings->matrixSize, data->A);
        compute_queue.enqueueTask(kernel);
        compute_queue.finish();
        compute_queue.enqueueReadBuffer(buffer, CL_TRUE, 0, sizeof(HOST_DATA_TYPE)*bm->getExecutionSettings().programSettings->matrixSize*bm->getExecutionSettings().programSettings->matrixSize, data->A);

        network_queue.finish();
    }
};

TEST_F(LinpackKernelCommunicationTestTop, TopBlockExternalResultisCorrect) {
    uint matrix_size = bm->getExecutionSettings().programSettings->matrixSize;
    auto gefa_data = bm->generateInputData();

    // generate uniformly distributed block as top block
    bm->getExecutionSettings().programSettings->isDiagonallyDominant = false;
    auto ref_data = bm->generateInputData();
    bm->getExecutionSettings().programSettings->isDiagonallyDominant = true;
    linpack::gefa_ref_nopvt(gefa_data->A, matrix_size,matrix_size);

	// std::cout << "Host:" << std::endl;
	// for (int i = 0; i < BLOCK_SIZE; i++) {
	// 	for (int j=0; j < BLOCK_SIZE; j++) {
	// 		std::cout << ref_data->A[i * matrix_size + j] << ",";
	// 	}
	// 	std::cout<< std::endl;
	// }
	// std::cout << std::endl;

    // std::cout << "Kernel:" << std::endl;
	// for (int i = 0; i < BLOCK_SIZE; i++) {
	// 	for (int j=0; j < BLOCK_SIZE; j++) {
	// 		std::cout << (data->A[i * matrix_size + j] - ref_data->A[i * matrix_size + j]) << ",";
	// 	}
	// 	std::cout<< std::endl;
	// }
	// std::cout << std::endl;

    // For each diagnonal element
    for (int k = 0; k < matrix_size; k++) {
        // For each element below it scale the current row
        for (int i = 0; i < matrix_size; i++) {
            ref_data->A[k * matrix_size + i] *= gefa_data->A[k * matrix_size + k];
        }
        // For each row below the current row
        for (int j = k + 1; j < matrix_size; j++) {
            // multiply current column to current row and add it up
            for (int i = 0; i < matrix_size; i++) {
                ref_data->A[j * matrix_size + i] += ref_data->A[k * matrix_size + i] * gefa_data->A[j * matrix_size + k];
            }
        }
    }
    double total_error = 0.0;
    for (int i = 0; i < bm->getExecutionSettings().programSettings->matrixSize; i++) {
        for (int j = 0; j < bm->getExecutionSettings().programSettings->matrixSize; j++) {
            total_error += std::abs(ref_data->A[i * bm->getExecutionSettings().programSettings->matrixSize + j] - data->A[i * bm->getExecutionSettings().programSettings->matrixSize + j]);
        }
    }
    EXPECT_FLOAT_EQ(total_error, 0.0);
}

TEST_F(LinpackKernelCommunicationTestTop, TopBlockExternalChannelOutputToRightCorrectAmountOfData) {
    // data that was sent to left kernels
    auto data_left = getDataFromExternalChannel(1, true);

    size_t number_values = 0;
    for (int i = 0; i < BLOCK_SIZE; i++ ) {
        number_values += (BLOCK_SIZE - (i / CHUNK) * CHUNK);
    }
    EXPECT_EQ(data_left.size(), number_values);
}

TEST_F(LinpackKernelCommunicationTestTop, TopBlockExternalChannelOutputToLeftCorrectAmountOfData) {
    // data that was sent to left kernels
    auto data_left = getDataFromExternalChannel(3, true);

    EXPECT_EQ(data_left.size(), 0);
}

TEST_F(LinpackKernelCommunicationTestTop, TopBlockExternalChannelOutputToTopCorrectAmountOfData) {
    // data that was sent to left kernels
    auto data_left = getDataFromExternalChannel(0, true);

    EXPECT_EQ(data_left.size(), 0);
}

TEST_F(LinpackKernelCommunicationTestTop, TopBlockExternalChannelOutputToBottomCorrectAmountOfData) {
    // data that was sent to top kernels
    auto data_top = getDataFromExternalChannel(2, true);

    size_t number_values = BLOCK_SIZE * BLOCK_SIZE;
    EXPECT_EQ(data_top.size(), number_values);
}

TEST_F(LinpackKernelCommunicationTestTop, TopBlockExternalChannelOutputToRightCorrect) {
    // data that was sent to next top kernels
    auto data_left = getDataFromExternalChannel(1, true);
    // data that was sent from LU kernel
    auto data_lu = getDataFromExternalChannel(3, false);

    size_t number_values = 0;
    for (int i = 0; i < BLOCK_SIZE; i++ ) {
        number_values += (BLOCK_SIZE - (i / CHUNK) * CHUNK);
    }
    EXPECT_EQ(data_left.size(), number_values);
    if (data_left.size() == number_values) {

        HOST_DATA_TYPE total_error = 0.0;

        size_t offset = 0;
        // for every row of a block
        for (int i = 0; i < BLOCK_SIZE; i++ ) {
            // for every column of a block
            for (int j = (i / CHUNK) * CHUNK; j < BLOCK_SIZE; j++) {
                total_error += std::abs(data_lu[offset + (j - (i / CHUNK) * CHUNK)] - data_left[offset + (j - (i / CHUNK) * CHUNK)]);
            }
            offset += BLOCK_SIZE - (i / CHUNK) * CHUNK;
        }
        EXPECT_FLOAT_EQ(total_error, 0.0);
    }
}

TEST_F(LinpackKernelCommunicationTestTop, TopBlockExternalChannelOutputToBottomCorrect) {
    // data that was sent to kernels below
    auto data_top = getDataFromExternalChannel(2, true);

    size_t number_values = BLOCK_SIZE * BLOCK_SIZE;
    EXPECT_EQ(data_top.size(), number_values);
    if (data_top.size() == number_values) {

        HOST_DATA_TYPE total_error = 0.0;

        size_t offset = 0;
        // for every column of a block
        for (int i = 0; i < BLOCK_SIZE; i++ ) {
            // for every row of a block
            for (int j = (i / CHUNK) * CHUNK; j < BLOCK_SIZE; j++) {
                total_error += std::abs(data->A[j + i * BLOCK_SIZE] - data_top[i*BLOCK_SIZE + j]);
            }
        }
        EXPECT_FLOAT_EQ(total_error, 0.0);
    }
}

TEST_F(LinpackKernelCommunicationTestLU, LUBlockExternalResultisSameAsRef) {
    auto data2 = bm->generateInputData();
    linpack::gefa_ref_nopvt(data2->A, bm->getExecutionSettings().programSettings->matrixSize,bm->getExecutionSettings().programSettings->matrixSize);
    double total_error = 0.0;
    for (int i = 0; i < bm->getExecutionSettings().programSettings->matrixSize; i++) {
        for (int j = 0; j < bm->getExecutionSettings().programSettings->matrixSize; j++) {
            total_error += std::abs(data2->A[i * bm->getExecutionSettings().programSettings->matrixSize + j] - data->A[i * bm->getExecutionSettings().programSettings->matrixSize + j]);
        }
    }
    EXPECT_FLOAT_EQ(total_error, 0.0);
}


TEST_F(LinpackKernelCommunicationTestLU, LUBlockExternalResultisCorrect) {
    linpack::gesl_ref_nopvt(data->A, data->b, bm->getExecutionSettings().programSettings->matrixSize,bm->getExecutionSettings().programSettings->matrixSize);
    EXPECT_TRUE(bm->validateOutputAndPrintError(*data));

}

TEST_F(LinpackKernelCommunicationTestLU, LUBlockExternalChannelOutputToRightCorrectAmountOfData) {
    // data that was sent to left kernels
    auto data_left = getDataFromExternalChannel(1, true);

    size_t number_values = 0;
    for (int i = 0; i < BLOCK_SIZE; i++ ) {
        number_values += (BLOCK_SIZE - (i / CHUNK) * CHUNK);
    }
    EXPECT_EQ(data_left.size(), number_values);
}

TEST_F(LinpackKernelCommunicationTestLU, LUBlockExternalChannelOutputToLeftCorrectAmountOfData) {
    // data that was sent to left kernels
    auto data_left = getDataFromExternalChannel(3, true);

    EXPECT_EQ(data_left.size(), 0);
}

TEST_F(LinpackKernelCommunicationTestLU, LUBlockExternalChannelOutputToTopCorrectAmountOfData) {
    // data that was sent to left kernels
    auto data_left = getDataFromExternalChannel(0, true);

    EXPECT_EQ(data_left.size(), 0);
}

TEST_F(LinpackKernelCommunicationTestLU, LUBlockExternalChannelOutputToBottomCorrectAmountOfData) {
    // data that was sent to top kernels
    auto data_top = getDataFromExternalChannel(2, true);

    size_t number_values = 0;
    for (int i = 0; i < BLOCK_SIZE; i++ ) {
        number_values += (BLOCK_SIZE - (i / CHUNK) * CHUNK);
    }
    EXPECT_EQ(data_top.size(), number_values);
}

TEST_F(LinpackKernelCommunicationTestLU, LUBlockExternalChannelOutputToRightCorrect) {
    // data that was sent to top kernels
    auto data_left = getDataFromExternalChannel(1, true);

    size_t number_values = 0;
    for (int i = 0; i < BLOCK_SIZE; i++ ) {
        number_values += (BLOCK_SIZE - (i / CHUNK) * CHUNK);
    }
    EXPECT_EQ(data_left.size(), number_values);
    if (data_left.size() == number_values) {

        HOST_DATA_TYPE total_error = 0.0;

        size_t offset = 0;
        // for every row of a block
        for (int i = 0; i < BLOCK_SIZE; i++ ) {
            // for every column of a block
            for (int j = (i / CHUNK) * CHUNK; j < BLOCK_SIZE; j++) {
                total_error += std::abs(data->A[j + i * BLOCK_SIZE] - data_left[offset + (j - (i / CHUNK) * CHUNK)]);
            }
            offset += BLOCK_SIZE - (i / CHUNK) * CHUNK;
        }
        EXPECT_FLOAT_EQ(total_error, 0.0);
    }
}

TEST_F(LinpackKernelCommunicationTestLU, LUBlockExternalChannelOutputToBottomCorrect) {
    // data that was sent to top kernels
    auto data_top = getDataFromExternalChannel(2, true);

    size_t number_values = 0;
    for (int i = 0; i < BLOCK_SIZE; i++ ) {
        number_values += (BLOCK_SIZE - (i / CHUNK) * CHUNK);
    }
    EXPECT_EQ(data_top.size(), number_values);
    if (data_top.size() == number_values) {

        HOST_DATA_TYPE total_error = 0.0;

        size_t offset = 0;
        // for every column of a block
        for (int i = 0; i < BLOCK_SIZE; i++ ) {
            // for every row of a block
            for (int j = (i / CHUNK) * CHUNK; j < BLOCK_SIZE; j++) {
                total_error += std::abs(data->A[i + j * BLOCK_SIZE] - data_top[offset + (j - (i / CHUNK) * CHUNK)]);
            }
            offset += BLOCK_SIZE - (i / CHUNK) * CHUNK;
        }
        EXPECT_FLOAT_EQ(total_error, 0.0);
    }
}

// TODO implement tests for other kernels
