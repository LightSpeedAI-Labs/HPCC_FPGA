/*
Copyright (c) 2019 Marius Meyer

Permission is hereby granted, free of charge, to any person obtaining a copy of
this software and associated documentation files (the "Software"), to deal in
the Software without restriction, including without limitation the rights to
use, copy, modify, merge, publish, distribute, sublicense, and/or sell copies
of the Software, and to permit persons to whom the Software is furnished to do
so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in all
copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
SOFTWARE.
*/
#ifndef SRC_HOST_CPU_EXECUTION_H_
#define SRC_HOST_CPU_EXECUTION_H_

#ifdef MKL_FOUND

/* C++ standard library headers */
#include <memory>
#include <vector>
#include <chrono>

/* External library headers */
#include "mpi.h"
#include "mkl_trans.h"

/* Project's headers */
#include "data_handlers/handler.hpp"

namespace transpose
{
    namespace fpga_execution
    {
        namespace cpu
        {

            /**
 * @brief Transpose and add the matrices using MKL routines
 * 
 * @param config The progrma configuration
 * @param data data object that contains all required data for the execution
 * @return std::unique_ptr<transpose::TransposeExecutionTimings> The measured execution times 
 */
            static std::unique_ptr<transpose::TransposeExecutionTimings>
            calculate(const hpcc_base::ExecutionSettings<transpose::TransposeProgramSettings> &config, transpose::TransposeData &data, transpose::data_handler::TransposeDataHandler &handler)
            {
                int err;

                std::vector<double> transferTimings;
                std::vector<double> calculationTimings;

                if (data.blockSize != BLOCK_SIZE) {
                    throw std::runtime_error("Block size for CPU hardcoded to " + std::to_string(BLOCK_SIZE) + ". Recompile to use different block sizes!");
                }

                ulong local_matrix_width = std::sqrt(data.numBlocks);

                for (int repetition = 0; repetition < config.programSettings->numRepetitions; repetition++)
                {

                    std::chrono::duration<double> transferTime(0);

                    MPI_Barrier(MPI_COMM_WORLD);

                    auto startCalculation = std::chrono::high_resolution_clock::now();

                    // Exchange A data via PCIe and MPI
                    handler.exchangeData(data);


                    switch (config.programSettings->dataHandlerIdentifier) {
                        case transpose::data_handler::DataHandlerType::diagonal: 
                                #pragma omp parallel for 
                                for (ulong offset=0; offset < data.numBlocks * BLOCK_SIZE * BLOCK_SIZE; offset += BLOCK_SIZE * BLOCK_SIZE) {
                                    mkl_somatadd('R', 'T', 'N', BLOCK_SIZE, BLOCK_SIZE, 1.0, &data.A[offset], BLOCK_SIZE, 1.0, &data.B[offset], BLOCK_SIZE, &data.result[offset], BLOCK_SIZE);
                                }
                                break;
                        case transpose::data_handler::DataHandlerType::pq: 
                                #pragma omp parallel for 
                                for (ulong yoffset=0; yoffset < BLOCK_SIZE * local_matrix_width; yoffset += BLOCK_SIZE) {
                                    for (ulong xoffset=0; xoffset < BLOCK_SIZE * local_matrix_width; xoffset += BLOCK_SIZE) {
                                        ulong toffset = xoffset * BLOCK_SIZE * local_matrix_width + yoffset;
                                        ulong offset = yoffset * BLOCK_SIZE * local_matrix_width + xoffset;
                                        mkl_somatadd('R', 'T', 'N', BLOCK_SIZE, BLOCK_SIZE, 1.0, &data.A[toffset], BLOCK_SIZE * local_matrix_width, 1.0, &data.B[offset], BLOCK_SIZE * local_matrix_width, &data.result[offset], BLOCK_SIZE * local_matrix_width);
                                    }
                                }
                                break;
                        default: throw std::runtime_error("Given data handler is not supported by CPU implementation: " + transpose::data_handler::handlerToString(config.programSettings->dataHandlerIdentifier));
                    }


                    auto endCalculation = std::chrono::high_resolution_clock::now();
#ifndef NDEBUG
                    int mpi_rank;
                    MPI_Comm_rank(MPI_COMM_WORLD, &mpi_rank);
                    std::cout << "Rank " << mpi_rank << ": "
                          << "Done i=" << repetition << std::endl;
#endif
                    std::chrono::duration<double> calculationTime =
                        std::chrono::duration_cast<std::chrono::duration<double>>(endCalculation - startCalculation);
                    calculationTimings.push_back(calculationTime.count());

                    // Transfer back data for next repetition!
                    handler.exchangeData(data);

                    transferTimings.push_back(transferTime.count());
                }

                std::unique_ptr<transpose::TransposeExecutionTimings> result(new transpose::TransposeExecutionTimings{
                    transferTimings,
                    calculationTimings});
                return result;
            }

        } // namespace bm_execution
    }
}
#endif // MKL_FOUND
#endif // SRC_HOST_CPU_EXECUTION_H_
