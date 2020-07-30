// Copyright (C) 2013-2019 Altera Corporation, San Jose, California, USA. All rights reserved.
// Permission is hereby granted, free of charge, to any person obtaining a copy of this
// software and associated documentation files (the "Software"), to deal in the Software
// without restriction, including without limitation the rights to use, copy, modify, merge,
// publish, distribute, sublicense, and/or sell copies of the Software, and to permit persons to
// whom the Software is furnished to do so, subject to the following conditions:
// The above copyright notice and this permission notice shall be included in all copies or
// substantial portions of the Software.
// 
// THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,
// EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES
// OF MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND
// NONINFRINGEMENT. IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT
// HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY,
// WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
// FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR
// OTHER DEALINGS IN THE SOFTWARE.
// 
// This agreement shall be governed in all respects by the laws of the State of California and
// by the laws of the United States of America.

/* This is the top-level device source file for the fft1d example. The code is
 * written as an OpenCL single work-item kernel. This coding style allows the 
 * compiler to extract loop-level parallelism from the source code and 
 * instantiate a hardware pipeline capable of executing concurrently a large 
 * number of loop iterations. The compiler analyses loop-carried dependencies, 
 * and these translate into data transfers across concurrently executed loop 
 * iterations. 
 *
 * Careful coding ensures that all loop-carried dependencies are trivial, 
 * merely data transfers which span a single clock cycle. The FFT algorithm 
 * requires passing data forward across loop iterations. The code uses a 
 * sliding window to implement these data transfers. The advantage of using a
 * sliding window is that dependencies across consecutive loop iterations have
 * an invariant source and destination (pairs of constant offset array 
 * elements). Such transfer patterns can be implemented efficiently by the 
 * FPGA hardware. All this ensures an overall processing a throughput of one 
 * loop iteration per clock cycle.
 *
 * The size of the FFT transform can be customized via an argument to the FFT 
 * engine. This argument has to be a compile time constant to ensure that the 
 * compiler can propagate it throughout the function body and generate 
 * efficient hardware.
 */

// Include source code for an engine that produces 8 points each step
#ifdef INTEL_FPGA
#include "fft_8.cl"
#else
#include "fft_8_xilinx.cl"
#endif

#include "parameters.h"

#ifdef INTEL_FPGA

#endif

#define min(a,b) (a<b?a:b)

#define LOGN            LOG_FFT_SIZE

#define LOGPOINTS       3
#define POINTS          (1 << LOGPOINTS)

// Need some depth to our channels to accommodate their bursty filling.
#ifdef INTEL_FPGA
#pragma OPENCL EXTENSION cl_intel_channels : enable

channel float2 chanin[POINTS] __attribute__((depth(POINTS)));
channel float2 chanout[POINTS] __attribute__((depth(POINTS)));
#endif
#ifdef XILINX_FPGA
pipe float2x8 chanin __attribute__((xcl_reqd_pipe_depth(POINTS)));
pipe float2x8 chanout __attribute__((xcl_reqd_pipe_depth(POINTS)));
// Buffer replication that is used to achieve double buffering in the fetch kernel
#define BUFFER_REPLICATION 4
#endif

uint bit_reversed(uint x, uint bits) {
  uint y = 0;
__attribute__((opencl_unroll_hint()))
  for (uint i = 0; i < bits; i++) {
    y <<= 1;
    y |= x & 1;
    x >>= 1;
  }
  y &= ((1 << bits) - 1);
  return y;
}

__kernel
__attribute__ ((max_global_work_dim(0), reqd_work_group_size(1,1,1)))
void fetch(__global float2 * restrict src, int iter) {

  const int N = (1 << LOGN);

#ifdef INTEL_FPGA
  // SWI fetch kernel for Intel fft1d written by Arjun Ramaswami, Paderborn University, PC2
  // Source can be found under https://git.uni-paderborn.de/arjunr/fft1d-fpga/-/blob/master/fft1d/device/fft1d.cl

  for(unsigned k = 0; k < iter; k++){ 

    float2 buf[N];
    #pragma unroll POINTS
    for(int i = 0; i < N; i++){
      buf[i & ((1<<LOGN)-1)] = src[(k << LOGN) + i];    
    }

    for(unsigned j = 0; j < (N / POINTS); j++){
      write_channel_intel(chanin[0], buf[j]);               // 0
      write_channel_intel(chanin[1], buf[4 * N / 8 + j]);   // 32
      write_channel_intel(chanin[2], buf[2 * N / 8 + j]);   // 16
      write_channel_intel(chanin[3], buf[6 * N / 8 + j]);   // 48
      write_channel_intel(chanin[4], buf[N / 8 + j]);       // 8
      write_channel_intel(chanin[5], buf[5 * N / 8 + j]);   // 40
      write_channel_intel(chanin[6], buf[3 * N / 8 + j]);   // 24
      write_channel_intel(chanin[7], buf[7 * N / 8 + j]);   // 54
    }
  }
#endif
#ifdef XILINX_FPGA

  // Duplicated input buffer. One will be used to write and buffer data from global memory
  // The other will be used to read and forward the data over the channels.
  // Read and write buffers will be swapped in every iteration
  float2 buf[BUFFER_REPLICATION][POINTS][N / POINTS] __attribute__((xcl_array_partition(cyclic, 2, 1), xcl_array_partition(complete, 2), xcl_array_partition(cyclic, POINTS, 3)));

  // for iter iterations and one additional iteration to empty the last buffer
  __attribute__((xcl_loop_tripcount(2*(N / POINTS),5000*(N / POINTS),100*(N / POINTS))))
  for(unsigned k = 0; k < (iter + 1) * (N / POINTS); k++){ 

    // Read the next 8 values from global memory
    // in the last iteration just read garbage, but the data will not be forwarded over the pipes.
    // This allows the use of memory bursts here.
    __attribute__((opencl_unroll_hint(POINTS)))
    for(int j = 0; j < POINTS; j++){
      unsigned local_i = ((k << LOGPOINTS) + j) & (N - 1);
      buf[(k >> (LOGN - LOGPOINTS)) & (BUFFER_REPLICATION - 1)][local_i >> (LOGN - LOGPOINTS)][local_i & ((1 << (LOGN - LOGPOINTS)) - 1)] = src[(k << LOGPOINTS) + j];
    }

    // Start in the second iteration to forward the buffered data over the pipe
    if (k >= (N / POINTS)) {
      float2x8 buf2x8;
      buf2x8.i0 = buf[((k >> (LOGN - LOGPOINTS)) - 1) & (BUFFER_REPLICATION - 1)][0][k & ((1 << (LOGN - LOGPOINTS)) - 1)];          
      buf2x8.i1 = buf[((k >> (LOGN - LOGPOINTS)) - 1) & (BUFFER_REPLICATION - 1)][4][k & ((1 << (LOGN - LOGPOINTS)) - 1)];  
      buf2x8.i2 = buf[((k >> (LOGN - LOGPOINTS)) - 1) & (BUFFER_REPLICATION - 1)][2][k & ((1 << (LOGN - LOGPOINTS)) - 1)];  
      buf2x8.i3 = buf[((k >> (LOGN - LOGPOINTS)) - 1) & (BUFFER_REPLICATION - 1)][6][k & ((1 << (LOGN - LOGPOINTS)) - 1)]; 
      buf2x8.i4 = buf[((k >> (LOGN - LOGPOINTS)) - 1) & (BUFFER_REPLICATION - 1)][1][k & ((1 << (LOGN - LOGPOINTS)) - 1)]; 
      buf2x8.i5 = buf[((k >> (LOGN - LOGPOINTS)) - 1) & (BUFFER_REPLICATION - 1)][5][k & ((1 << (LOGN - LOGPOINTS)) - 1)];
      buf2x8.i6 = buf[((k >> (LOGN - LOGPOINTS)) - 1) & (BUFFER_REPLICATION - 1)][3][k & ((1 << (LOGN - LOGPOINTS)) - 1)];
      buf2x8.i7 = buf[((k >> (LOGN - LOGPOINTS)) - 1) & (BUFFER_REPLICATION - 1)][7][k & ((1 << (LOGN - LOGPOINTS)) - 1)];

      write_pipe_block(chanin, &buf2x8);
    }
  }
#endif
}



/* Attaching the attribute 'task' to the top level kernel to indicate 
 * that the host enqueues a task (a single work-item kernel)
 *
 * 'src' and 'dest' point to the input and output buffers in global memory; 
 * using restrict pointers as there are no dependencies between the buffers
 * 'count' represents the number of 4k sets to process
 * 'inverse' toggles between the direct and the inverse transform
 */

__attribute__ ((max_global_work_dim(0)))
__attribute__((reqd_work_group_size(1,1,1)))
kernel void fft1d(int count, int inverse) {

  const int N = (1 << LOGN);

  /* The FFT engine requires a sliding window array for data reordering; data 
   * stored in this array is carried across loop iterations and shifted by one 
   * element every iteration; all loop dependencies derived from the uses of 
   * this array are simple transfers between adjacent array elements
   */

  float2 fft_delay_elements[N + POINTS * (LOGN - 2)] __attribute__((xcl_array_partition(complete, 0)));

  /* This is the main loop. It runs 'count' back-to-back FFT transforms
   * In addition to the 'count * (N / 8)' iterations, it runs 'N / 8 - 1'
   * additional iterations to drain the last outputs 
   * (see comments attached to the FFT engine)
   *
   * The compiler leverages pipeline parallelism by overlapping the 
   * iterations of this loop - launching one iteration every clock cycle
   */
   __attribute__((xcl_pipeline_loop(1)))
  for (unsigned i = 0; i < count * (N / POINTS) + N / POINTS - 1; i++) {

    /* As required by the FFT engine, gather input data from 8 distinct 
     * segments of the input buffer; for simplicity, this implementation 
     * does not attempt to coalesce memory accesses and this leads to 
     * higher resource utilization (see the fft2d example for advanced 
     * memory access techniques)
     */

    int base = (i / (N / POINTS)) * N;
    int offset = i % (N / POINTS);

    float2x8 data;
    // Perform memory transfers only when reading data in range
    if (i < count * (N / POINTS)) {
      #ifdef INTEL_FPGA
      data.i0 = read_channel_intel(chanin[0]);
      data.i1 = read_channel_intel(chanin[1]);
      data.i2 = read_channel_intel(chanin[2]);
      data.i3 = read_channel_intel(chanin[3]);
      data.i4 = read_channel_intel(chanin[4]);
      data.i5 = read_channel_intel(chanin[5]);
      data.i6 = read_channel_intel(chanin[6]);
      data.i7 = read_channel_intel(chanin[7]);
      #else
      read_pipe_block(chanin, &data);
      #endif
    } else {
      data.i0 = data.i1 = data.i2 = data.i3 = 
                data.i4 = data.i5 = data.i6 = data.i7 = 0;
    }

    // Perform one step of the FFT engine
    data = fft_step(data, i % (N / POINTS), fft_delay_elements, inverse, LOGN);

    /* Store data back to memory. FFT engine outputs are delayed by 
     * N / 8 - 1 steps, hence gate writes accordingly
     */

    if (i >= N / POINTS - 1) {
#ifdef INTEL_FPGA
      write_channel_intel(chanout[0], data.i0);               // 0
      write_channel_intel(chanout[1], data.i1);   // 32
      write_channel_intel(chanout[2], data.i2);   // 16
      write_channel_intel(chanout[3], data.i3);   // 48
      write_channel_intel(chanout[4], data.i4);       // 8
      write_channel_intel(chanout[5], data.i5);   // 40
      write_channel_intel(chanout[6], data.i6);   // 24
      write_channel_intel(chanout[7], data.i7);   // 54
#endif
#ifdef XILINX_FPGA
      write_pipe_block(chanout, &data);
#endif
    }
  }
}

__kernel
__attribute__ ((max_global_work_dim(0), reqd_work_group_size(1,1,1)))
void store(__global float2 * restrict dest, int iter) {

  const int N = (1 << LOGN);

#ifdef INTEL_FPGA
  for(unsigned k = 0; k < iter; k++){ 

    float2 buf[N];
    for(unsigned j = 0; j < (N / POINTS); j++){
      // buf[j]             = read_channel_intel(chanout[0]);               // 0
      // buf[4 * N / 8 + j] = read_channel_intel(chanout[1]);   // 32
      // buf[2 * N / 8 + j] = read_channel_intel(chanout[2]);   // 16
      // buf[6 * N / 8 + j] = read_channel_intel(chanout[3]);   // 48
      // buf[N / 8 + j]     = read_channel_intel(chanout[4]);       // 8
      // buf[5 * N / 8 + j] = read_channel_intel(chanout[5]);   // 40
      // buf[3 * N / 8 + j] = read_channel_intel(chanout[6]);   // 24
      // buf[7 * N / 8 + j] = read_channel_intel(chanout[7]);   // 54
      dest[(k << LOGN) + j * POINTS]             = read_channel_intel(chanout[0]);               // 0
      dest[(k << LOGN) + j * POINTS + 1] = read_channel_intel(chanout[1]);   // 32
      dest[(k << LOGN) + j * POINTS + 2] = read_channel_intel(chanout[2]);   // 16
      dest[(k << LOGN) + j * POINTS + 3] = read_channel_intel(chanout[3]);   // 48
      dest[(k << LOGN) + j * POINTS + 4]     = read_channel_intel(chanout[4]);       // 8
      dest[(k << LOGN) + j * POINTS + 5] = read_channel_intel(chanout[5]);   // 40
      dest[(k << LOGN) + j * POINTS + 6] = read_channel_intel(chanout[6]);   // 24
      dest[(k << LOGN) + j * POINTS + 7] = read_channel_intel(chanout[7]);   // 54
    }
    // #pragma unroll POINTS
    // for(int i = 0; i < N; i++){
    //   dest[(k << LOGN) + i] = buf[i & ((1<<LOGN)-1)];    
    // }
  }
#endif
#ifdef XILINX_FPGA

  // Duplicated input buffer. One will be used to write and buffer data from global memory
  // The other will be used to read and forward the data over the channels.
  // Read and write buffers will be swapped in every iteration
  float2 buf[BUFFER_REPLICATION][POINTS][N / POINTS] __attribute__((xcl_array_partition(cyclic, 2, 1), xcl_array_partition(complete, 2), xcl_array_partition(cyclic, POINTS, 3)));

  // for iter iterations and one additional iteration to empty the last buffer
  __attribute__((xcl_loop_tripcount(2*(N / POINTS),5000*(N / POINTS),100*(N / POINTS))))
  for(unsigned k = 0; k < (iter + 1) * (N / POINTS); k++){ 

    // Except in last iteration read new results from channel and bit reverse the order
    if (k < (iter) * (N / POINTS)) {
      float2x8 buf2x8;
      write_pipe_block(chanout, &buf2x8);

      buf2x8.i0 = buf[((k >> (LOGN - LOGPOINTS)) - 1) & (BUFFER_REPLICATION - 1)][0][k & ((1 << (LOGN - LOGPOINTS)) - 1)];          
      buf2x8.i1 = buf[((k >> (LOGN - LOGPOINTS)) - 1) & (BUFFER_REPLICATION - 1)][4][k & ((1 << (LOGN - LOGPOINTS)) - 1)];  
      buf2x8.i2 = buf[((k >> (LOGN - LOGPOINTS)) - 1) & (BUFFER_REPLICATION - 1)][2][k & ((1 << (LOGN - LOGPOINTS)) - 1)];  
      buf2x8.i3 = buf[((k >> (LOGN - LOGPOINTS)) - 1) & (BUFFER_REPLICATION - 1)][6][k & ((1 << (LOGN - LOGPOINTS)) - 1)]; 
      buf2x8.i4 = buf[((k >> (LOGN - LOGPOINTS)) - 1) & (BUFFER_REPLICATION - 1)][1][k & ((1 << (LOGN - LOGPOINTS)) - 1)]; 
      buf2x8.i5 = buf[((k >> (LOGN - LOGPOINTS)) - 1) & (BUFFER_REPLICATION - 1)][5][k & ((1 << (LOGN - LOGPOINTS)) - 1)];
      buf2x8.i6 = buf[((k >> (LOGN - LOGPOINTS)) - 1) & (BUFFER_REPLICATION - 1)][3][k & ((1 << (LOGN - LOGPOINTS)) - 1)];
      buf2x8.i7 = buf[((k >> (LOGN - LOGPOINTS)) - 1) & (BUFFER_REPLICATION - 1)][7][k & ((1 << (LOGN - LOGPOINTS)) - 1)];

    }

    // Store the next 8 values to global memory
    // in the last iteration just write garbage and hope that there is no other write buffer. This anables memory bursts on Xilinx devices
    __attribute__((opencl_unroll_hint(POINTS)))
    for(int j = 0; j < POINTS; j++){
      unsigned local_i = ((k << LOGPOINTS) + j) & (N - 1);
      dest[(k << LOGPOINTS) + j] = buf[(k >> (LOGN - LOGPOINTS)) & (BUFFER_REPLICATION - 1)][local_i >> (LOGN - LOGPOINTS)][local_i & ((1 << (LOGN - LOGPOINTS)) - 1)];
    }
  }
#endif
}

