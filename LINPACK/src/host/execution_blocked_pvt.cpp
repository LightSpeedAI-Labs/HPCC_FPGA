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

/* Related header files */
#include "execution.h"

/* C++ standard library headers */
#include <chrono>
#include <fstream>
#include <memory>
#include <vector>

/* External library headers */
#include "CL/cl.hpp"
#if QUARTUS_MAJOR_VERSION > 18
#include "CL/cl_ext_intelfpga.h"
#endif

/* Project's headers */
#include "setup/fpga_setup.hpp"
#include "linpack_functionality.hpp"

namespace bm_execution {

/*
 Prepare kernels and execute benchmark

 @copydoc bm_execution::calculate()
*/
std::shared_ptr<ExecutionTimings>
calculate(std::shared_ptr<ExecutionConfiguration> config,
          HOST_DATA_TYPE* A,
          HOST_DATA_TYPE* b,
          cl_int* ipvt) {

    int err;

    // Create Command queue
    cl::CommandQueue compute_queue(config->context, config->device);

    // Create Buffers for input and output
    cl::Buffer Buffer_a(config->context, CL_MEM_READ_WRITE,
                                        sizeof(HOST_DATA_TYPE)*config->matrixSize*config->matrixSize);
    cl::Buffer Buffer_pivot(config->context, CL_MEM_READ_WRITE,
                                        sizeof(cl_int)*config->matrixSize);

    // create the kernels
    cl::Kernel gefakernel(config->program, "gefa",
                                    &err);
    ASSERT_CL(err);


    // prepare kernels
    err = gefakernel.setArg(0, Buffer_a);
    ASSERT_CL(err);
    err = gefakernel.setArg(1, Buffer_pivot);
    ASSERT_CL(err);
    err = gefakernel.setArg(2, static_cast<uint>(config->matrixSize >> LOCAL_MEM_BLOCK_LOG));
    ASSERT_CL(err);

    /* --- Execute actual benchmark kernels --- */

    double t;
    std::vector<double> executionTimes;
    for (int i = 0; i < config->repetitions; i++) {
        compute_queue.enqueueWriteBuffer(Buffer_a, CL_TRUE, 0,
                                    sizeof(HOST_DATA_TYPE)*config->matrixSize*config->matrixSize, A);
        compute_queue.finish();
        auto t1 = std::chrono::high_resolution_clock::now();
        compute_queue.enqueueTask(gefakernel);
        compute_queue.finish();
        auto t2 = std::chrono::high_resolution_clock::now();
        std::chrono::duration<double> timespan =
            std::chrono::duration_cast<std::chrono::duration<double>>
                                                                (t2 - t1);
        executionTimes.push_back(timespan.count());
    }

    /* --- Read back results from Device --- */

    compute_queue.enqueueReadBuffer(Buffer_a, CL_TRUE, 0,
                                     sizeof(HOST_DATA_TYPE)*config->matrixSize*config->matrixSize, A);
    compute_queue.enqueueReadBuffer(Buffer_pivot, CL_TRUE, 0,
                                     sizeof(cl_int)*config->matrixSize, ipvt);

    // Solve linear equations on CPU
    // TODO: This has to be done on FPGA
    gesl_ref(A, b, ipvt, config->matrixSize, config->matrixSize);

    std::shared_ptr<bm_execution::ExecutionTimings> results(
                    new ExecutionTimings{executionTimes});
    return results;
}

}  // namespace bm_execution