#include "hip/hip_runtime.h"

#ifndef ARCH_DEVICE_CUDA_H
#define ARCH_DEVICE_CUDA_H

/* Include required headers */
#include "hip/hip_runtime.h"
#include "hip/hip_runtime.h"
#include "hipcub/hipcub.hpp"

/* Define architecture-specific macros */
#define ARCH_LOOP_LAMBDA [=] __host__ __device__
#define ARCH_INNER_BODY2(i, j, aggregate)
#define ARCH_INNER_BODY3(i, j, k, aggregate)
#define ARCH_INNER_BODY4(i, j, k, l, aggregate)

/* Set CUDA blocksize used for reductions */
#define ARCH_BLOCKSIZE_R 256

/* Define the CUDA error checking macro */
#define CUDA_ERR(err) (cuda_error(err, __FILE__, __LINE__))
  inline static void cuda_error(hipError_t err, const char *file, int line) {
  	if (err != hipSuccess) {
  		printf("\n\n%s in %s at line %d\n", hipGetErrorString(err), file, line);
  		exit(1);
  	}
}

/* Namespace for architecture-specific functions */
namespace arch{

/* Create auxiliary max atomic function for double types */
__device__ __forceinline__ static void atomicMax(double *address, double val2) {
  unsigned long long ret = __double_as_longlong(*address);
  while(val2 > __longlong_as_double(ret)) {
    unsigned long long old = ret;
    if((ret = atomicCAS((unsigned long long *)address, old, __double_as_longlong(val2))) == old)
    break;
  }
}

/* Create auxiliary min atomic function for double types */
__device__ __forceinline__ static void atomicMin(double *address, double val2) {
  unsigned long long ret = __double_as_longlong(*address);
  while(val2 < __longlong_as_double(ret)) {
    unsigned long long old = ret;
    if((ret = atomicCAS((unsigned long long *)address, old, __double_as_longlong(val2))) == old)
    break;
  }
}

/* Device function for memory allocation */
__host__ __forceinline__ static void* allocate(size_t bytes) {
  void* ptr;
  CUDA_ERR(hipMallocManaged(&ptr, bytes));
  return ptr;
}

/* Device function for memory deallocation */
__host__ __forceinline__ static void free(void* ptr) {
  CUDA_ERR(hipFree(ptr));
}


/* Specializations for lambda calls depending on the templated dimension */
template <typename Lambda, typename T>
__device__ __forceinline__ static void lambda_eval(const uint (&idx)[1], T * __restrict__ thread_data, Lambda loop_body) { loop_body(idx[0], thread_data); }

template <typename Lambda, typename T>
__device__ __forceinline__ static void lambda_eval(const uint (&idx)[2], T * __restrict__ thread_data, Lambda loop_body) { loop_body(idx[0], idx[1], thread_data); }

template <typename Lambda, typename T>
__device__ __forceinline__ static void lambda_eval(const uint (&idx)[3], T * __restrict__ thread_data, Lambda loop_body) { loop_body(idx[0], idx[1], idx[2], thread_data); }

template <typename Lambda, typename T>
__device__ __forceinline__ static void lambda_eval(const uint (&idx)[4], T * __restrict__ thread_data, Lambda loop_body) { loop_body(idx[0], idx[1], idx[2], idx[3], thread_data); }

/* Get the index for the underlying dimension, and call the respective lambda wrapper */
template <uint NDim, typename Lambda, typename T>
__device__ __forceinline__ static void loop_eval(const uint idx_glob, const uint * __restrict__ lims, T * __restrict__ thread_data, Lambda loop_body) { 
  uint idx[NDim];
  switch (NDim)
  {
    case 4:
      idx[3] = (idx_glob / (lims[0] * lims[1] * lims[2])) % lims[3];
    case 3:
      idx[2] = (idx_glob / (lims[0] * lims[1])) % lims[2];
    case 2:
      idx[1] = (idx_glob / lims[0]) % lims[1];
    case 1:
      idx[0] = idx_glob % lims[0];  
  }
  lambda_eval(idx, thread_data, loop_body);
}

/* A general device kernel for reductions */
template <reduce_op Op, uint NDim, uint NReduStatic, typename Lambda, typename T>
__global__ static void __launch_bounds__(ARCH_BLOCKSIZE_R)
reduction_kernel(Lambda loop_body, const T * __restrict__ init_val, T * __restrict__ rslt, const uint * __restrict__ lims, const uint n_total, const uint n_redu_dynamic, T *thread_data_dynamic)
{
  /* Specialize BlockReduce for a 1D block of ARCH_BLOCKSIZE_R threads of type `T` */
  typedef hipcub::BlockReduce<T, ARCH_BLOCKSIZE_R, hipcub::BLOCK_REDUCE_RAKING_COMMUTATIVE_ONLY, 1, 1> BlockReduce;

  /* Dynamic shared memory declaration */
  extern __shared__ char temp_storage_dynamic[];

  /* Static shared memory declaration */
  constexpr uint size = NReduStatic ? NReduStatic : 1;
  __shared__ typename BlockReduce::TempStorage temp_storage_static[size];

  /* Assign a pointer to the shared memory (dynamic or static case) */
  typename BlockReduce::TempStorage *temp_storage = NReduStatic ? temp_storage_static : (typename BlockReduce::TempStorage*) temp_storage_dynamic;

  /* Get the global 1D thread index*/
  const uint idx_glob = blockIdx.x * blockDim.x + threadIdx.x;

  /* Static thread data declaration */
  T thread_data_static[size];
  
  /* Assign a pointer to the thread data (dynamic or static case)*/
  T *thread_data = NReduStatic ? thread_data_static : &thread_data_dynamic[n_redu_dynamic * idx_glob];

  /* Get the number of reductions (may be known at compile time or not) */
  const uint n_reductions = NReduStatic ? NReduStatic : n_redu_dynamic;  
  
  /* Set initial values */
  for(uint i = 0; i < n_reductions; i++){
    if (Op == reduce_op::sum)
      thread_data[i] = 0;
    else
      thread_data[i] = init_val[i];
  }

  /* Check the loop limits and evaluate the loop body */
  if (idx_glob < n_total)
    loop_eval<NDim>(idx_glob, lims, thread_data, loop_body);
  
  /* Perform reductions */
  for(uint i = 0; i < n_reductions; i++){
    /* Compute the block-wide sum for thread 0 which stores it */
    if(Op == reduce_op::sum){
      T aggregate = BlockReduce(temp_storage[i]).Sum(thread_data[i]);
      /* The first thread of each block stores the block-wide aggregate atomically */
      if(threadIdx.x == 0) 
        atomicAdd(&rslt[i], aggregate);
    }
    else if(Op == reduce_op::max){
      T aggregate = BlockReduce(temp_storage[i]).Reduce(thread_data[i], hipcub::Max()); 
      if(threadIdx.x == 0) 
        atomicMax(&rslt[i], aggregate);
    }
    else if(Op == reduce_op::min){
      T aggregate = BlockReduce(temp_storage[i]).Reduce(thread_data[i], hipcub::Min());
      if(threadIdx.x == 0) 
        atomicMin(&rslt[i], aggregate);
    }
    else
      /* Other reduction operations are not supported - print an error message */
      if(threadIdx.x == 0) 
        printf("ERROR at %s:%d: Invalid reduction identifier \"Op\".", __FILE__, __LINE__);
  }
}
  


/* Parallel reduce driver function for the CUDA reductions */
template <reduce_op Op, uint NReduStatic, uint NDim, typename Lambda, typename T>
__forceinline__ static void parallel_reduce_driver(const uint (&limits)[NDim], Lambda loop_body, T *sum, const uint n_redu_dynamic) {

  /* Get the number of reductions (may be known at compile time or not) */
  const uint n_reductions = NReduStatic ? NReduStatic : n_redu_dynamic;  

  /* Calculate the required size for the 1D kernel */
  uint n_total = 1;
  for(uint i = 0; i < NDim; i++)
    n_total *= limits[i];

  /* Set the kernel dimensions */
  const uint blocksize = ARCH_BLOCKSIZE_R;
  const uint gridsize = (n_total - 1 + blocksize) / blocksize;

  /* Create a device buffer for the reduction results */
  T* d_buf;
  CUDA_ERR(hipMalloc(&d_buf, n_reductions*sizeof(T)));
  CUDA_ERR(hipMemcpy(d_buf, sum, n_reductions*sizeof(T), hipMemcpyHostToDevice));
  
  /* Create a device buffer to transfer the initial values to device */
  T* d_const_buf;
  CUDA_ERR(hipMalloc(&d_const_buf, n_reductions*sizeof(T)));
  CUDA_ERR(hipMemcpy(d_const_buf, d_buf, n_reductions*sizeof(T), hipMemcpyDeviceToDevice));

  /* Create a device buffer to transfer the loop limits of each dimension to device */
  uint* d_limits;
  CUDA_ERR(hipMalloc(&d_limits, NDim*sizeof(uint)));
  CUDA_ERR(hipMemcpy(d_limits, limits, NDim*sizeof(uint), hipMemcpyHostToDevice));

  /* Call the reduction kernel with different arguments depending 
   * on if the number of reductions is known at the compile time 
   */
  T* d_thread_data_dynamic;
  if(NReduStatic == 0) {
    /* Get the cub temp storage size for the dynamic shared memory kernel argument */
    constexpr auto cub_temp_storage_type_size = sizeof(typename hipcub::BlockReduce<T, ARCH_BLOCKSIZE_R, hipcub::BLOCK_REDUCE_RAKING_COMMUTATIVE_ONLY, 1, 1>::TempStorage);
    /* Allocate memory for the thread data values */
    CUDA_ERR(hipMalloc(&d_thread_data_dynamic, n_reductions * blocksize * gridsize * sizeof(T)));
    /* Call the kernel (the number of reductions not known at compile time) */
    hipLaunchKernelGGL(HIP_KERNEL_NAME(reduction_kernel<Op, NDim, 0>), gridsize, blocksize, n_reductions * cub_temp_storage_type_size, 0, loop_body, d_const_buf, d_buf, d_limits, n_total, n_reductions, d_thread_data_dynamic);
    /* Synchronize and free the thread data allocation */
    CUDA_ERR(hipStreamSynchronize(0));
    CUDA_ERR(hipFree(d_thread_data_dynamic));
  }
  else{
    /* Call the kernel (the number of reductions known at compile time) */
    hipLaunchKernelGGL(HIP_KERNEL_NAME(reduction_kernel<Op, NDim, NReduStatic>), gridsize, blocksize, 0, 0, loop_body, d_const_buf, d_buf, d_limits, n_total, n_reductions, d_thread_data_dynamic);
    /* Synchronize after kernel call */
    CUDA_ERR(hipStreamSynchronize(0));
  }
  /* Copy the results back to host and free the allocated memory back to pool*/
  CUDA_ERR(hipMemcpy(sum, d_buf, n_reductions*sizeof(T), hipMemcpyDeviceToHost));
  CUDA_ERR(hipFree(d_buf));
  CUDA_ERR(hipFree(d_const_buf));
  CUDA_ERR(hipFree(d_limits));
}
}
#endif // !ARCH_DEVICE_CUDA_H
