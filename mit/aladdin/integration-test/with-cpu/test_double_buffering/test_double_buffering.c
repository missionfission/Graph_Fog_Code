/* An example of how to do double buffering in gem5-aladdin.
 *
 * Key ideas:
 *   - Double buffering is essentially a way to block computation. This means
 *     that any loop accessing arrays mapped to scratchpads must be careful not
 *     to exceed the array bounds of the smaller working buffers (e.g. idx
 *     should always be less than BUFFER_SIZE or BUFFER_SIZE*2, see below).
 *   - All the input data neeed for the first iteration of the primary outer
 *     loop should be dmaLoad'ed BEFORE the loop starts.
 *   - DMA operations only obey memory dependences with respect to ordinary
 *     loads and stores, not other DMA loads and stores, so DMA fences must be
 *     used to enforce ordering between DMA operations as necessary.
 */

#include <assert.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "gem5_harness.h"

// This is the total size of the input data.
#define ARRAY_SIZE 256

// This is HALF of the size of the accelerator's local scratchpad for the input
// data. The scratchpad is logically divided into two parts, each part
// BUFFER_SIZE elements large. One part is used for computation and the other
// is used to prefetch the next set of data to compute on.
//
// ARRAY_SIZE should be a multiple of BUFFER_SIZE * 2.
#define BUFFER_SIZE 64

// Arrays that are DMAed need to be cacheline aligned.
#define CACHELINE_SIZE 32

void kernel_loop(int input[BUFFER_SIZE*2], int output[BUFFER_SIZE*2]) {
#ifdef DMA_MODE
  // Useful constants to help distinguish where data is coming from/going to.
  const size_t DMA_SIZE = BUFFER_SIZE*sizeof(int);
  const size_t FIRST_HALF = 0;
  const size_t SECOND_HALF = DMA_SIZE;

  // Load all the data that is needed for the first half of the first loop
  // iteration.
  //
  // "Source" is the CPU (where the data is coming from).
  // "Dest" is where the data is going to (the local scratchpad).
  dmaLoad(&input[FIRST_HALF], &input[FIRST_HALF], DMA_SIZE);

  // Load all the data that is needed for the second half of the first loop
  // iteration.
  //
  // IMPORTANT: These two DMA loads cannot be combined into a single call. By
  // dividing them in half, this is how Aladdin knows that the computation for
  // the first half of the data can proceed even while the second half has not
  // finished loading, because the memory dependences are tracked on the
  // granularity of dmaLoad calls.
  dmaLoad(&input[BUFFER_SIZE], &input[BUFFER_SIZE], DMA_SIZE);
#endif

  // Primary outer loop.
outer_loop: for (int it = 0; it < ARRAY_SIZE/BUFFER_SIZE; it+=2) {

    // Process using the first half of the input/output buffers. Note that the
    // array indices go from 0 to BUFFER_SIZE.
loop0: for (int i = 0; i < BUFFER_SIZE; i++) {
      output[i] = input[i];
    }

#ifdef DMA_MODE
    // Store the first half processed data.
    //
    // "Source" is the first half of the scratchpad.
    // "Dest" is the appropriate half of the entire array in main memory.
    dmaStore(&output[it*BUFFER_SIZE], &output[FIRST_HALF], DMA_SIZE);

    // Don't load any more data if we're already on the last iteration!
    if (it != ARRAY_SIZE/BUFFER_SIZE - 2) {
      // Issue a DMA fence. This orders the subsequent DMA load with the
      // precending DMA store and thus prevents DMA load from overwriting input
      // data for the first half until the processing is done.
      dmaFence();

      // Prefetch the next iteration's first half data.
      dmaLoad(&input[FIRST_HALF], &input[(it+2)*BUFFER_SIZE], DMA_SIZE);
    }
#endif

    // Process using the second half of the input/output buffers. Note that the
    // array indices now go from BUFFER_SIZE to BUFFER_SIZE*2!
loop1: for (int i = BUFFER_SIZE; i < BUFFER_SIZE*2; i++) {
      output[i] = input[i];
    }

#ifdef DMA_MODE
    // Store the second half processed data.
    dmaStore(&output[(it+1)*BUFFER_SIZE], &output[BUFFER_SIZE], DMA_SIZE);
    if (it != ARRAY_SIZE/BUFFER_SIZE - 2) {
      dmaFence();
      // Prefetch the next iteration's second half data.
      dmaLoad(&input[BUFFER_SIZE], &input[(it+3)*BUFFER_SIZE], DMA_SIZE);
    }
#endif
  }
}

int main() {
  int* input;
  int* output;
  posix_memalign((void**)&input, CACHELINE_SIZE, ARRAY_SIZE * sizeof(int));
  posix_memalign((void**)&output, CACHELINE_SIZE, ARRAY_SIZE * sizeof(int));

  // Data initialization.
  memset((void*)output, -1, ARRAY_SIZE*sizeof(int));
  for (int i = 0; i < ARRAY_SIZE; i++) {
    input[i] = i;
  }

#ifdef GEM5_HARNESS
  // We only need to map the actual physical size of the scratchpads (which is
  // BUFFER_SIZE*2, not ARRAY_SIZE!).
  mapArrayToAccelerator(
      INTEGRATION_TEST, "input", input, BUFFER_SIZE * 2 * sizeof(int));
  mapArrayToAccelerator(
      INTEGRATION_TEST, "output", output, BUFFER_SIZE * 2 * sizeof(int));

  fprintf(stdout, "Invoking accelerator!\n");
  invokeAcceleratorAndBlock(INTEGRATION_TEST);
  fprintf(stdout, "Accelerator finished!\n");
#else
  kernel_loop(input, output);
#endif

  bool pass = true;
  for (int i = 0; i < ARRAY_SIZE; i++) {
    if (input[i] != output[i]) {
      pass = false;
      printf("output[%d] : Expected %d, got %d\n", i, input[i], output[i]);
    }
  }

  if (pass)
    printf("Test passed!\n");
  else
    printf("Test failed!\n");

  free(input);
  free(output);

  return 0;
}
