/*
 * This copyright notice applies to this header file only:
 *
 * Copyright (c) 2016
 *
 * Permission is hereby granted, free of charge, to any person
 * obtaining a copy of this software and associated documentation
 * files (the "Software"), to deal in the Software without
 * restriction, including without limitation the rights to use,
 * copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the software, and to permit persons to whom the
 * software is furnished to do so, subject to the following
 * conditions:
 *
 * The above copyright notice and this permission notice shall be
 * included in all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,
 * EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES
 * OF MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND
 * NONINFRINGEMENT. IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT
 * HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY,
 * WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
 * FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR
 * OTHER DEALINGS IN THE SOFTWARE.
 */

#pragma once

#include <stddef.h>

#define CUDA_VERSION 7050

#if defined(_WIN32) || defined(__CYGWIN__)
#define CUDAAPI __stdcall
#else
#define CUDAAPI
#endif

#define CU_CTX_SCHED_BLOCKING_SYNC 4

typedef int CUdevice;
typedef void *CUarray;
typedef void *CUcontext;
typedef void *CUstream;
#if defined(__x86_64) || defined(AMD64) || defined(_M_AMD64)
typedef unsigned long long CUdeviceptr;
#else
typedef unsigned int CUdeviceptr;
#endif

typedef enum cudaError_enum {
    CUDA_SUCCESS = 0,
	CUDA_ERROR_INVALID_VALUE = 1
} CUresult;

typedef enum CUmemorytype_enum {
    CU_MEMORYTYPE_HOST = 1,
    CU_MEMORYTYPE_DEVICE = 2
} CUmemorytype;

typedef struct CUDA_MEMCPY2D_st {
    size_t srcXInBytes;
    size_t srcY;
    CUmemorytype srcMemoryType;
    const void *srcHost;
    CUdeviceptr srcDevice;
    CUarray srcArray;
    size_t srcPitch;

    size_t dstXInBytes;
    size_t dstY;
    CUmemorytype dstMemoryType;
    void *dstHost;
    CUdeviceptr dstDevice;
    CUarray dstArray;
    size_t dstPitch;

    size_t WidthInBytes;
    size_t Height;
} CUDA_MEMCPY2D;

typedef CUresult CUDAAPI cuInit_t(unsigned int Flags);
typedef CUresult CUDAAPI cuDeviceGetCount_t(int *count);
typedef CUresult CUDAAPI cuDeviceGet_t(CUdevice *device, int ordinal);
typedef CUresult CUDAAPI cuDeviceGetName_t(char *name, int len, CUdevice dev);
typedef CUresult CUDAAPI cuDeviceComputeCapability_t(int *major, int *minor, CUdevice dev);
typedef CUresult CUDAAPI cuCtxCreate_v2_t(CUcontext *pctx, unsigned int flags, CUdevice dev);
typedef CUresult CUDAAPI cuCtxPushCurrent_v2_t(CUcontext pctx);
typedef CUresult CUDAAPI cuCtxPopCurrent_v2_t(CUcontext *pctx);
typedef CUresult CUDAAPI cuCtxDestroy_v2_t(CUcontext ctx);
typedef CUresult CUDAAPI cuMemAlloc_v2_t(CUdeviceptr *dptr, size_t bytesize);
typedef CUresult CUDAAPI cuMemFree_v2_t(CUdeviceptr dptr);
typedef CUresult CUDAAPI cuMemcpy2D_v2_t(const CUDA_MEMCPY2D *pcopy);
typedef CUresult CUDAAPI cuGetErrorName_t(CUresult error, const char **pstr);
typedef CUresult CUDAAPI cuGetErrorString_t(CUresult error, const char **pstr);

