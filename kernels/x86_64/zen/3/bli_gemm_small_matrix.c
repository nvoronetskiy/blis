/*

BLIS
An object-based framework for developing high-performance BLAS-like
libraries.

Copyright (C) 2017, Advanced Micro Devices, Inc.

Redistribution and use in source and binary forms, with or without
modification, are permitted provided that the following conditions are
met:
- Redistributions of source code must retain the above copyright
notice, this list of conditions and the following disclaimer.
- Redistributions in binary form must reproduce the above copyright
notice, this list of conditions and the following disclaimer in the
documentation and/or other materials provided with the distribution.
- Neither the name of The University of Texas at Austin nor the names
of its contributors may be used to endorse or promote products
derived from this software without specific prior written permission.

THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
"AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT
HOLDER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT
LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
THEORY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
(INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.

*/

#include "blis.h"
#include "immintrin.h"
#include "xmmintrin.h"

#define MR 32
#define D_MR (MR >> 1)
#define NR 3

//The scratch buffer allocated for performing transpose.
#define F_SCRATCH_DIM 1*1024
#define BLIS_ENABLE_PREFETCH
static float temp_scratch_buf[F_SCRATCH_DIM] __attribute__((aligned(64)));
static float A_pack[BLIS_SMALL_MATRIX_THRES * BLIS_SMALL_MATRIX_THRES]  __attribute__((aligned(64)));
#define D_BLIS_SMALL_MATRIX_THRES (BLIS_SMALL_MATRIX_THRES / 2 )
#define D_BLIS_SMALL_M_RECT_MATRIX_THRES (BLIS_SMALL_M_RECT_MATRIX_THRES / 2)
#define D_BLIS_SMALL_K_RECT_MATRIX_THRES (BLIS_SMALL_K_RECT_MATRIX_THRES / 2)
static double D_A_pack[D_BLIS_SMALL_MATRIX_THRES * D_BLIS_SMALL_MATRIX_THRES]  __attribute__((aligned(64)));

static gint_t bli_sgemm_small_matrix
(
    obj_t*  alpha,
    obj_t*  a,
    obj_t*  b,
    obj_t*  beta,
    obj_t*  c,
    cntx_t* cntx,
    cntl_t* cntl
);

static gint_t bli_dgemm_small_matrix
(
    obj_t*  alpha,
    obj_t*  a,
    obj_t*  b,
    obj_t*  beta,
    obj_t*  c,
    cntx_t* cntx,
    cntl_t* cntl
);
/*
* The bli_gemm_small_matrix function will use the
* custom MRxNR kernels, to perform the computation.
* The custom kernels are used if the [M * N] < 240 * 240
*/
gint_t bli_gemm_small_matrix
(
    obj_t*  alpha,
    obj_t*  a,
    obj_t*  b,
    obj_t*  beta,
    obj_t*  c,
    cntx_t* cntx,
    cntl_t* cntl
    )
{

    // If alpha is zero, scale by beta and return.
    if (bli_obj_equals(alpha, &BLIS_ZERO))
    {
        return BLIS_NOT_YET_IMPLEMENTED;
    }

    if (bli_obj_has_trans(*a))
    {
        return BLIS_NOT_YET_IMPLEMENTED;
    }

    // if row major format return.
    if ((bli_obj_row_stride(*a) != 1) ||
        (bli_obj_row_stride(*b) != 1) ||
        (bli_obj_row_stride(*c) != 1))
    {
        return BLIS_INVALID_ROW_STRIDE;
    }
    // The custom kernels are implemented only for float datatype.
    num_t dt = ((*c).info & (0x7 << 0));

    if (dt == BLIS_DOUBLE)
    {
        return bli_dgemm_small_matrix(alpha, a, b, beta, c, cntx, cntl);

    }

    if (dt == BLIS_FLOAT)
    {
        return bli_sgemm_small_matrix(alpha, a, b, beta, c, cntx, cntl);
    }

    return BLIS_NOT_YET_IMPLEMENTED;

};


gint_t bli_sgemm_small_matrix
(
    obj_t*  alpha,
    obj_t*  a,
    obj_t*  b,
    obj_t*  beta,
    obj_t*  c,
    cntx_t* cntx,
    cntl_t* cntl
    )
{

    int M = bli_obj_length(*c); // number of rows of Matrix C
    int N = bli_obj_width(*c);  // number of columns of Matrix C
    int K = bli_obj_width(*a);  // number of columns of OP(A), will be updated if OP(A) is Transpose(A) .

    //   printf("alpha_cast = %f beta_cast = %f [ Trans = %d %d], [stride = %d %d %d] [m,n,k = %d %d %d]\n",*alpha_cast,*beta_cast, bli_obj_has_trans(*a), bli_obj_has_trans(*b), lda, ldb,ldc, M,N,K);
    if (((M * N) < (BLIS_SMALL_MATRIX_THRES * BLIS_SMALL_MATRIX_THRES))
        || ((M  < BLIS_SMALL_M_RECT_MATRIX_THRES) && (K < BLIS_SMALL_K_RECT_MATRIX_THRES)))
    {

        int lda = bli_obj_col_stride(*a); // column stride of matrix OP(A), where OP(A) is Transpose(A) if transA enabled.
        int ldb = bli_obj_col_stride(*b); // column stride of matrix OP(B), where OP(B) is Transpose(B) if transB enabled.
        int ldc = bli_obj_col_stride(*c); // column stride of matrix C
        int row_idx, col_idx, k;
        float *A = a->buffer; // pointer to elements of Matrix A
        float *B = b->buffer; // pointer to elements of Matrix B
        float *C = c->buffer; // pointer to elements of Matrix C

        float *tA = A, *tB = B, *tC = C;//, *tA_pack;
        float *tA_packed; // temprorary pointer to hold packed A memory pointer
        int row_idx_packed; //packed A memory row index
        int lda_packed; //lda of packed A
        int col_idx_start; //starting index after A matrix is packed.
        dim_t tb_inc_row = 1; // row stride of matrix B
        dim_t tb_inc_col = ldb; // column stride of matrix B
        __m256 ymm4, ymm5, ymm6, ymm7;
        __m256 ymm8, ymm9, ymm10, ymm11;
        __m256 ymm12, ymm13, ymm14, ymm15;
        __m256 ymm0, ymm1, ymm2, ymm3;

        int n_remainder; // If the N is non multiple of 3.(N%3)
        int m_remainder; // If the M is non multiple of 32.(M%32)

        float *alpha_cast, *beta_cast; // alpha, beta multiples
        alpha_cast = (alpha->buffer);
        beta_cast = (beta->buffer);
        int required_packing_A = 1;

        // when N is equal to 1 call GEMV instead of GEMM
        if (N == 1)
        {
            bli_gemv
                (
                    alpha,
                    a,
                    b,
                    beta,
                    c
                    );
            return BLIS_SUCCESS;
        }

        // When MAtrix A requires transpose perform it using
        // scratch buffer also update the K dimension.
        // This code needs optimization, and probably won't be
        // used in the future.
        if (bli_obj_has_trans(*a))
        {
            K = bli_obj_length(*a);

            //if the scratch buffer cannot accomodate matrix A, return.
            if ((M * K) > F_SCRATCH_DIM)
            {
                return BLIS_FAILURE;
            }

            for (row_idx = 0; row_idx < M; row_idx += 1)
            {
                for (col_idx = 0; col_idx < K; col_idx += 1)
                {
                    temp_scratch_buf[row_idx + col_idx * M] =
                        A[col_idx + row_idx * lda];
                }
            }
            A = temp_scratch_buf;
            lda = M;

        }

        //update the pointer math if matrix B needs to be transposed.
        if (bli_obj_has_trans(*b))
        {
            tb_inc_col = 1; //switch row and column strides
            tb_inc_row = ldb;
        }

        if (N <= 3)
        {
            required_packing_A = 0;
        }
        /*
        * The computation loop runs for MRxN columns of C matrix, thus
        * accessing the MRxK A matrix data and KxNR B matrix data.
        * The computation is organized as inner loops of dimension MRxNR.
        */
        // Process MR rows of C matrix at a time.
        for (row_idx = 0; (row_idx + (MR - 1)) < M; row_idx += MR)
        {

            col_idx_start = 0;
            tA_packed = A;
            row_idx_packed = row_idx;
            lda_packed = lda;

            // This is the part of the pack and compute optimization.
            // During the first column iteration, we store the accessed A matrix into
            // contiguous static memory. This helps to keep te A matrix in Cache and
            // aviods the TLB misses.
            if (required_packing_A)
            {
                col_idx = 0;

                //pointer math to point to proper memory
                tC = C + ldc * col_idx + row_idx;
                tB = B + tb_inc_col * col_idx;
                tA = A + row_idx;
                tA_packed = A_pack;

#ifdef BLIS_ENABLE_PREFETCH
                _mm_prefetch((char*)(tC + 0), _MM_HINT_T0);
                _mm_prefetch((char*)(tC + 16), _MM_HINT_T0);
                _mm_prefetch((char*)(tC + ldc), _MM_HINT_T0);
                _mm_prefetch((char*)(tC + ldc + 16), _MM_HINT_T0);
                _mm_prefetch((char*)(tC + 2 * ldc), _MM_HINT_T0);
                _mm_prefetch((char*)(tC + 2 * ldc + 16), _MM_HINT_T0);
#endif
                // clear scratch registers.
                ymm4 = _mm256_setzero_ps();
                ymm5 = _mm256_setzero_ps();
                ymm6 = _mm256_setzero_ps();
                ymm7 = _mm256_setzero_ps();
                ymm8 = _mm256_setzero_ps();
                ymm9 = _mm256_setzero_ps();
                ymm10 = _mm256_setzero_ps();
                ymm11 = _mm256_setzero_ps();
                ymm12 = _mm256_setzero_ps();
                ymm13 = _mm256_setzero_ps();
                ymm14 = _mm256_setzero_ps();
                ymm15 = _mm256_setzero_ps();

                for (k = 0; k < K; ++k)
                {
                    // The inner loop broadcasts the B matrix data and
                    // multiplies it with the A matrix.
                    // This loop is processing MR x K
                    ymm0 = _mm256_broadcast_ss(tB + tb_inc_col * 0);
                    ymm1 = _mm256_broadcast_ss(tB + tb_inc_col * 1);
                    ymm2 = _mm256_broadcast_ss(tB + tb_inc_col * 2);
                    tB += tb_inc_row;

                    //broadcasted matrix B elements are multiplied
                    //with matrix A columns.
                    ymm3 = _mm256_loadu_ps(tA);
                    _mm256_storeu_ps(tA_packed, ymm3); // the packing of matrix A
                    //                   ymm4 += ymm0 * ymm3;
                    ymm4 = _mm256_fmadd_ps(ymm0, ymm3, ymm4);
                    //                    ymm8 += ymm1 * ymm3;
                    ymm8 = _mm256_fmadd_ps(ymm1, ymm3, ymm8);
                    //                    ymm12 += ymm2 * ymm3;
                    ymm12 = _mm256_fmadd_ps(ymm2, ymm3, ymm12);

                    ymm3 = _mm256_loadu_ps(tA + 8);
                    _mm256_storeu_ps(tA_packed + 8, ymm3); // the packing of matrix A
                    //                    ymm5 += ymm0 * ymm3;
                    ymm5 = _mm256_fmadd_ps(ymm0, ymm3, ymm5);
                    //                    ymm9 += ymm1 * ymm3;
                    ymm9 = _mm256_fmadd_ps(ymm1, ymm3, ymm9);
                    //                    ymm13 += ymm2 * ymm3;
                    ymm13 = _mm256_fmadd_ps(ymm2, ymm3, ymm13);

                    ymm3 = _mm256_loadu_ps(tA + 16);
                    _mm256_storeu_ps(tA_packed + 16, ymm3); // the packing of matrix A
                    //                   ymm6 += ymm0 * ymm3;
                    ymm6 = _mm256_fmadd_ps(ymm0, ymm3, ymm6);
                    //                    ymm10 += ymm1 * ymm3;
                    ymm10 = _mm256_fmadd_ps(ymm1, ymm3, ymm10);
                    //                    ymm14 += ymm2 * ymm3;
                    ymm14 = _mm256_fmadd_ps(ymm2, ymm3, ymm14);

                    ymm3 = _mm256_loadu_ps(tA + 24);
                    _mm256_storeu_ps(tA_packed + 24, ymm3); // the packing of matrix A
                    //                    ymm7 += ymm0 * ymm3;
                    ymm7 = _mm256_fmadd_ps(ymm0, ymm3, ymm7);
                    //                    ymm11 += ymm1 * ymm3;
                    ymm11 = _mm256_fmadd_ps(ymm1, ymm3, ymm11);
                    //                   ymm15 += ymm2 * ymm3;
                    ymm15 = _mm256_fmadd_ps(ymm2, ymm3, ymm15);

                    tA += lda;
                    tA_packed += MR;
                }
                // alpha, beta multiplication.
                ymm0 = _mm256_broadcast_ss(alpha_cast);
                ymm1 = _mm256_broadcast_ss(beta_cast);

                //multiply A*B by alpha.
                ymm4 = _mm256_mul_ps(ymm4, ymm0);
                ymm5 = _mm256_mul_ps(ymm5, ymm0);
                ymm6 = _mm256_mul_ps(ymm6, ymm0);
                ymm7 = _mm256_mul_ps(ymm7, ymm0);
                ymm8 = _mm256_mul_ps(ymm8, ymm0);
                ymm9 = _mm256_mul_ps(ymm9, ymm0);
                ymm10 = _mm256_mul_ps(ymm10, ymm0);
                ymm11 = _mm256_mul_ps(ymm11, ymm0);
                ymm12 = _mm256_mul_ps(ymm12, ymm0);
                ymm13 = _mm256_mul_ps(ymm13, ymm0);
                ymm14 = _mm256_mul_ps(ymm14, ymm0);
                ymm15 = _mm256_mul_ps(ymm15, ymm0);

                // multiply C by beta and accumulate col 1.
                ymm2 = _mm256_loadu_ps(tC);
                ymm4 = _mm256_fmadd_ps(ymm2, ymm1, ymm4);
                ymm2 = _mm256_loadu_ps(tC + 8);
                ymm5 = _mm256_fmadd_ps(ymm2, ymm1, ymm5);
                ymm2 = _mm256_loadu_ps(tC + 16);
                ymm6 = _mm256_fmadd_ps(ymm2, ymm1, ymm6);
                ymm2 = _mm256_loadu_ps(tC + 24);
                ymm7 = _mm256_fmadd_ps(ymm2, ymm1, ymm7);
                _mm256_storeu_ps(tC, ymm4);
                _mm256_storeu_ps(tC + 8, ymm5);
                _mm256_storeu_ps(tC + 16, ymm6);
                _mm256_storeu_ps(tC + 24, ymm7);

                // multiply C by beta and accumulate, col 2.
                tC += ldc;
                ymm2 = _mm256_loadu_ps(tC);
                ymm8 = _mm256_fmadd_ps(ymm2, ymm1, ymm8);
                ymm2 = _mm256_loadu_ps(tC + 8);
                ymm9 = _mm256_fmadd_ps(ymm2, ymm1, ymm9);
                ymm2 = _mm256_loadu_ps(tC + 16);
                ymm10 = _mm256_fmadd_ps(ymm2, ymm1, ymm10);
                ymm2 = _mm256_loadu_ps(tC + 24);
                ymm11 = _mm256_fmadd_ps(ymm2, ymm1, ymm11);
                _mm256_storeu_ps(tC, ymm8);
                _mm256_storeu_ps(tC + 8, ymm9);
                _mm256_storeu_ps(tC + 16, ymm10);
                _mm256_storeu_ps(tC + 24, ymm11);

                // multiply C by beta and accumulate, col 3.
                tC += ldc;
                ymm2 = _mm256_loadu_ps(tC);
                ymm12 = _mm256_fmadd_ps(ymm2, ymm1, ymm12);
                ymm2 = _mm256_loadu_ps(tC + 8);
                ymm13 = _mm256_fmadd_ps(ymm2, ymm1, ymm13);
                ymm2 = _mm256_loadu_ps(tC + 16);
                ymm14 = _mm256_fmadd_ps(ymm2, ymm1, ymm14);
                ymm2 = _mm256_loadu_ps(tC + 24);
                ymm15 = _mm256_fmadd_ps(ymm2, ymm1, ymm15);
                _mm256_storeu_ps(tC, ymm12);
                _mm256_storeu_ps(tC + 8, ymm13);
                _mm256_storeu_ps(tC + 16, ymm14);
                _mm256_storeu_ps(tC + 24, ymm15);

                // modify the pointer arithematic to use packed A matrix.
                col_idx_start = NR;
                tA_packed = A_pack;
                row_idx_packed = 0;
                lda_packed = MR;
            }
            // Process NR columns of C matrix at a time.
            for (col_idx = col_idx_start; (col_idx + (NR - 1)) < N; col_idx += NR)
            {
                //pointer math to point to proper memory
                tC = C + ldc * col_idx + row_idx;
                tB = B + tb_inc_col * col_idx;
                tA = tA_packed + row_idx_packed;

#ifdef BLIS_ENABLE_PREFETCH
                _mm_prefetch((char*)(tC + 0), _MM_HINT_T0);
                _mm_prefetch((char*)(tC + 16), _MM_HINT_T0);
                _mm_prefetch((char*)(tC + ldc), _MM_HINT_T0);
                _mm_prefetch((char*)(tC + ldc + 16), _MM_HINT_T0);
                _mm_prefetch((char*)(tC + 2 * ldc), _MM_HINT_T0);
                _mm_prefetch((char*)(tC + 2 * ldc + 16), _MM_HINT_T0);
#endif
                // clear scratch registers.
                ymm4 = _mm256_setzero_ps();
                ymm5 = _mm256_setzero_ps();
                ymm6 = _mm256_setzero_ps();
                ymm7 = _mm256_setzero_ps();
                ymm8 = _mm256_setzero_ps();
                ymm9 = _mm256_setzero_ps();
                ymm10 = _mm256_setzero_ps();
                ymm11 = _mm256_setzero_ps();
                ymm12 = _mm256_setzero_ps();
                ymm13 = _mm256_setzero_ps();
                ymm14 = _mm256_setzero_ps();
                ymm15 = _mm256_setzero_ps();

                for (k = 0; k < K; ++k)
                {
                    // The inner loop broadcasts the B matrix data and
                    // multiplies it with the A matrix.
                    // This loop is processing MR x K
                    ymm0 = _mm256_broadcast_ss(tB + tb_inc_col * 0);
                    ymm1 = _mm256_broadcast_ss(tB + tb_inc_col * 1);
                    ymm2 = _mm256_broadcast_ss(tB + tb_inc_col * 2);
                    tB += tb_inc_row;

                    //broadcasted matrix B elements are multiplied
                    //with matrix A columns.
                    ymm3 = _mm256_loadu_ps(tA);
                    //                   ymm4 += ymm0 * ymm3;
                    ymm4 = _mm256_fmadd_ps(ymm0, ymm3, ymm4);
                    //                    ymm8 += ymm1 * ymm3;
                    ymm8 = _mm256_fmadd_ps(ymm1, ymm3, ymm8);
                    //                    ymm12 += ymm2 * ymm3;
                    ymm12 = _mm256_fmadd_ps(ymm2, ymm3, ymm12);

                    ymm3 = _mm256_loadu_ps(tA + 8);
                    //                    ymm5 += ymm0 * ymm3;
                    ymm5 = _mm256_fmadd_ps(ymm0, ymm3, ymm5);
                    //                    ymm9 += ymm1 * ymm3;
                    ymm9 = _mm256_fmadd_ps(ymm1, ymm3, ymm9);
                    //                    ymm13 += ymm2 * ymm3;
                    ymm13 = _mm256_fmadd_ps(ymm2, ymm3, ymm13);

                    ymm3 = _mm256_loadu_ps(tA + 16);
                    //                   ymm6 += ymm0 * ymm3;
                    ymm6 = _mm256_fmadd_ps(ymm0, ymm3, ymm6);
                    //                    ymm10 += ymm1 * ymm3;
                    ymm10 = _mm256_fmadd_ps(ymm1, ymm3, ymm10);
                    //                    ymm14 += ymm2 * ymm3;
                    ymm14 = _mm256_fmadd_ps(ymm2, ymm3, ymm14);

                    ymm3 = _mm256_loadu_ps(tA + 24);
                    //                    ymm7 += ymm0 * ymm3;
                    ymm7 = _mm256_fmadd_ps(ymm0, ymm3, ymm7);
                    //                    ymm11 += ymm1 * ymm3;
                    ymm11 = _mm256_fmadd_ps(ymm1, ymm3, ymm11);
                    //                   ymm15 += ymm2 * ymm3;
                    ymm15 = _mm256_fmadd_ps(ymm2, ymm3, ymm15);

                    tA += lda_packed;
                }
                // alpha, beta multiplication.
                ymm0 = _mm256_broadcast_ss(alpha_cast);
                ymm1 = _mm256_broadcast_ss(beta_cast);

                //multiply A*B by alpha.
                ymm4 = _mm256_mul_ps(ymm4, ymm0);
                ymm5 = _mm256_mul_ps(ymm5, ymm0);
                ymm6 = _mm256_mul_ps(ymm6, ymm0);
                ymm7 = _mm256_mul_ps(ymm7, ymm0);
                ymm8 = _mm256_mul_ps(ymm8, ymm0);
                ymm9 = _mm256_mul_ps(ymm9, ymm0);
                ymm10 = _mm256_mul_ps(ymm10, ymm0);
                ymm11 = _mm256_mul_ps(ymm11, ymm0);
                ymm12 = _mm256_mul_ps(ymm12, ymm0);
                ymm13 = _mm256_mul_ps(ymm13, ymm0);
                ymm14 = _mm256_mul_ps(ymm14, ymm0);
                ymm15 = _mm256_mul_ps(ymm15, ymm0);

                // multiply C by beta and accumulate col 1.
                ymm2 = _mm256_loadu_ps(tC);
                ymm4 = _mm256_fmadd_ps(ymm2, ymm1, ymm4);
                ymm2 = _mm256_loadu_ps(tC + 8);
                ymm5 = _mm256_fmadd_ps(ymm2, ymm1, ymm5);
                ymm2 = _mm256_loadu_ps(tC + 16);
                ymm6 = _mm256_fmadd_ps(ymm2, ymm1, ymm6);
                ymm2 = _mm256_loadu_ps(tC + 24);
                ymm7 = _mm256_fmadd_ps(ymm2, ymm1, ymm7);
                _mm256_storeu_ps(tC, ymm4);
                _mm256_storeu_ps(tC + 8, ymm5);
                _mm256_storeu_ps(tC + 16, ymm6);
                _mm256_storeu_ps(tC + 24, ymm7);

                // multiply C by beta and accumulate, col 2.
                tC += ldc;
                ymm2 = _mm256_loadu_ps(tC);
                ymm8 = _mm256_fmadd_ps(ymm2, ymm1, ymm8);
                ymm2 = _mm256_loadu_ps(tC + 8);
                ymm9 = _mm256_fmadd_ps(ymm2, ymm1, ymm9);
                ymm2 = _mm256_loadu_ps(tC + 16);
                ymm10 = _mm256_fmadd_ps(ymm2, ymm1, ymm10);
                ymm2 = _mm256_loadu_ps(tC + 24);
                ymm11 = _mm256_fmadd_ps(ymm2, ymm1, ymm11);
                _mm256_storeu_ps(tC, ymm8);
                _mm256_storeu_ps(tC + 8, ymm9);
                _mm256_storeu_ps(tC + 16, ymm10);
                _mm256_storeu_ps(tC + 24, ymm11);

                // multiply C by beta and accumulate, col 3.
                tC += ldc;
                ymm2 = _mm256_loadu_ps(tC);
                ymm12 = _mm256_fmadd_ps(ymm2, ymm1, ymm12);
                ymm2 = _mm256_loadu_ps(tC + 8);
                ymm13 = _mm256_fmadd_ps(ymm2, ymm1, ymm13);
                ymm2 = _mm256_loadu_ps(tC + 16);
                ymm14 = _mm256_fmadd_ps(ymm2, ymm1, ymm14);
                ymm2 = _mm256_loadu_ps(tC + 24);
                ymm15 = _mm256_fmadd_ps(ymm2, ymm1, ymm15);
                _mm256_storeu_ps(tC, ymm12);
                _mm256_storeu_ps(tC + 8, ymm13);
                _mm256_storeu_ps(tC + 16, ymm14);
                _mm256_storeu_ps(tC + 24, ymm15);

            }
            n_remainder = N - col_idx;

            // if the N is not multiple of 3.
            // handling edge case.
            if (n_remainder == 2)
            {
                //pointer math to point to proper memory
                tC = C + ldc * col_idx + row_idx;
                tB = B + tb_inc_col * col_idx;
                tA = A + row_idx;

                // clear scratch registers.
                ymm8 = _mm256_setzero_ps();
                ymm9 = _mm256_setzero_ps();
                ymm10 = _mm256_setzero_ps();
                ymm11 = _mm256_setzero_ps();
                ymm12 = _mm256_setzero_ps();
                ymm13 = _mm256_setzero_ps();
                ymm14 = _mm256_setzero_ps();
                ymm15 = _mm256_setzero_ps();

                for (k = 0; k < K; ++k)
                {
                    // The inner loop broadcasts the B matrix data and
                    // multiplies it with the A matrix.
                    ymm0 = _mm256_broadcast_ss(tB + tb_inc_col * 0);
                    ymm1 = _mm256_broadcast_ss(tB + tb_inc_col * 1);
                    tB += tb_inc_row;

                    //broadcasted matrix B elements are multiplied
                    //with matrix A columns.
                    ymm3 = _mm256_loadu_ps(tA);
                    ymm8 = _mm256_fmadd_ps(ymm0, ymm3, ymm8);
                    ymm12 = _mm256_fmadd_ps(ymm1, ymm3, ymm12);

                    ymm3 = _mm256_loadu_ps(tA + 8);
                    ymm9 = _mm256_fmadd_ps(ymm0, ymm3, ymm9);
                    ymm13 = _mm256_fmadd_ps(ymm1, ymm3, ymm13);

                    ymm3 = _mm256_loadu_ps(tA + 16);
                    ymm10 = _mm256_fmadd_ps(ymm0, ymm3, ymm10);
                    ymm14 = _mm256_fmadd_ps(ymm1, ymm3, ymm14);

                    ymm3 = _mm256_loadu_ps(tA + 24);
                    ymm11 = _mm256_fmadd_ps(ymm0, ymm3, ymm11);
                    ymm15 = _mm256_fmadd_ps(ymm1, ymm3, ymm15);

                    tA += lda;

                }
                // alpha, beta multiplication.
                ymm0 = _mm256_broadcast_ss(alpha_cast);
                ymm1 = _mm256_broadcast_ss(beta_cast);

                //multiply A*B by alpha.
                ymm8 = _mm256_mul_ps(ymm8, ymm0);
                ymm9 = _mm256_mul_ps(ymm9, ymm0);
                ymm10 = _mm256_mul_ps(ymm10, ymm0);
                ymm11 = _mm256_mul_ps(ymm11, ymm0);
                ymm12 = _mm256_mul_ps(ymm12, ymm0);
                ymm13 = _mm256_mul_ps(ymm13, ymm0);
                ymm14 = _mm256_mul_ps(ymm14, ymm0);
                ymm15 = _mm256_mul_ps(ymm15, ymm0);

                // multiply C by beta and accumulate, col 1.
                ymm2 = _mm256_loadu_ps(tC + 0);
                ymm8 = _mm256_fmadd_ps(ymm2, ymm1, ymm8);
                ymm2 = _mm256_loadu_ps(tC + 8);
                ymm9 = _mm256_fmadd_ps(ymm2, ymm1, ymm9);
                ymm2 = _mm256_loadu_ps(tC + 16);
                ymm10 = _mm256_fmadd_ps(ymm2, ymm1, ymm10);
                ymm2 = _mm256_loadu_ps(tC + 24);
                ymm11 = _mm256_fmadd_ps(ymm2, ymm1, ymm11);
                _mm256_storeu_ps(tC + 0, ymm8);
                _mm256_storeu_ps(tC + 8, ymm9);
                _mm256_storeu_ps(tC + 16, ymm10);
                _mm256_storeu_ps(tC + 24, ymm11);

                // multiply C by beta and accumulate, col 2.
                tC += ldc;
                ymm2 = _mm256_loadu_ps(tC);
                ymm12 = _mm256_fmadd_ps(ymm2, ymm1, ymm12);
                ymm2 = _mm256_loadu_ps(tC + 8);
                ymm13 = _mm256_fmadd_ps(ymm2, ymm1, ymm13);
                ymm2 = _mm256_loadu_ps(tC + 16);
                ymm14 = _mm256_fmadd_ps(ymm2, ymm1, ymm14);
                ymm2 = _mm256_loadu_ps(tC + 24);
                ymm15 = _mm256_fmadd_ps(ymm2, ymm1, ymm15);
                _mm256_storeu_ps(tC, ymm12);
                _mm256_storeu_ps(tC + 8, ymm13);
                _mm256_storeu_ps(tC + 16, ymm14);
                _mm256_storeu_ps(tC + 24, ymm15);

                col_idx += 2;
            }
            // if the N is not multiple of 3.
            // handling edge case.
            if (n_remainder == 1)
            {
                //pointer math to point to proper memory
                tC = C + ldc * col_idx + row_idx;
                tB = B + tb_inc_col * col_idx;
                tA = A + row_idx;

                // clear scratch registers.
                ymm12 = _mm256_setzero_ps();
                ymm13 = _mm256_setzero_ps();
                ymm14 = _mm256_setzero_ps();
                ymm15 = _mm256_setzero_ps();

                for (k = 0; k < K; ++k)
                {
                    // The inner loop broadcasts the B matrix data and
                    // multiplies it with the A matrix.
                    ymm0 = _mm256_broadcast_ss(tB + tb_inc_col * 0);
                    tB += tb_inc_row;

                    //broadcasted matrix B elements are multiplied
                    //with matrix A columns.
                    ymm3 = _mm256_loadu_ps(tA);
                    ymm12 = _mm256_fmadd_ps(ymm0, ymm3, ymm12);

                    ymm3 = _mm256_loadu_ps(tA + 8);
                    ymm13 = _mm256_fmadd_ps(ymm0, ymm3, ymm13);

                    ymm3 = _mm256_loadu_ps(tA + 16);
                    ymm14 = _mm256_fmadd_ps(ymm0, ymm3, ymm14);

                    ymm3 = _mm256_loadu_ps(tA + 24);
                    ymm15 = _mm256_fmadd_ps(ymm0, ymm3, ymm15);

                    tA += lda;

                }
                // alpha, beta multiplication.
                ymm0 = _mm256_broadcast_ss(alpha_cast);
                ymm1 = _mm256_broadcast_ss(beta_cast);

                //multiply A*B by alpha.
                ymm12 = _mm256_mul_ps(ymm12, ymm0);
                ymm13 = _mm256_mul_ps(ymm13, ymm0);
                ymm14 = _mm256_mul_ps(ymm14, ymm0);
                ymm15 = _mm256_mul_ps(ymm15, ymm0);

                // multiply C by beta and accumulate.
                ymm2 = _mm256_loadu_ps(tC + 0);
                ymm12 = _mm256_fmadd_ps(ymm2, ymm1, ymm12);
                ymm2 = _mm256_loadu_ps(tC + 8);
                ymm13 = _mm256_fmadd_ps(ymm2, ymm1, ymm13);
                ymm2 = _mm256_loadu_ps(tC + 16);
                ymm14 = _mm256_fmadd_ps(ymm2, ymm1, ymm14);
                ymm2 = _mm256_loadu_ps(tC + 24);
                ymm15 = _mm256_fmadd_ps(ymm2, ymm1, ymm15);

                _mm256_storeu_ps(tC + 0, ymm12);
                _mm256_storeu_ps(tC + 8, ymm13);
                _mm256_storeu_ps(tC + 16, ymm14);
                _mm256_storeu_ps(tC + 24, ymm15);
            }
        }

        m_remainder = M - row_idx;

        if (m_remainder >= 24)
        {
            m_remainder -= 24;

            for (col_idx = 0; (col_idx + 2) < N; col_idx += 3)
            {
                //pointer math to point to proper memory
                tC = C + ldc * col_idx + row_idx;
                tB = B + tb_inc_col * col_idx;
                tA = A + row_idx;

                // clear scratch registers.
                ymm4 = _mm256_setzero_ps();
                ymm5 = _mm256_setzero_ps();
                ymm6 = _mm256_setzero_ps();
                ymm8 = _mm256_setzero_ps();
                ymm9 = _mm256_setzero_ps();
                ymm10 = _mm256_setzero_ps();
                ymm12 = _mm256_setzero_ps();
                ymm13 = _mm256_setzero_ps();
                ymm14 = _mm256_setzero_ps();

                for (k = 0; k < K; ++k)
                {
                    // The inner loop broadcasts the B matrix data and
                    // multiplies it with the A matrix.
                    ymm0 = _mm256_broadcast_ss(tB + tb_inc_col * 0);
                    ymm1 = _mm256_broadcast_ss(tB + tb_inc_col * 1);
                    ymm2 = _mm256_broadcast_ss(tB + tb_inc_col * 2);
                    tB += tb_inc_row;

                    //broadcasted matrix B elements are multiplied
                    //with matrix A columns.
                    ymm3 = _mm256_loadu_ps(tA);
                    //                   ymm4 += ymm0 * ymm3;
                    ymm4 = _mm256_fmadd_ps(ymm0, ymm3, ymm4);
                    //                    ymm8 += ymm1 * ymm3;
                    ymm8 = _mm256_fmadd_ps(ymm1, ymm3, ymm8);
                    //                    ymm12 += ymm2 * ymm3;
                    ymm12 = _mm256_fmadd_ps(ymm2, ymm3, ymm12);

                    ymm3 = _mm256_loadu_ps(tA + 8);
                    //                    ymm5 += ymm0 * ymm3;
                    ymm5 = _mm256_fmadd_ps(ymm0, ymm3, ymm5);
                    //                    ymm9 += ymm1 * ymm3;
                    ymm9 = _mm256_fmadd_ps(ymm1, ymm3, ymm9);
                    //                    ymm13 += ymm2 * ymm3;
                    ymm13 = _mm256_fmadd_ps(ymm2, ymm3, ymm13);

                    ymm3 = _mm256_loadu_ps(tA + 16);
                    //                   ymm6 += ymm0 * ymm3;
                    ymm6 = _mm256_fmadd_ps(ymm0, ymm3, ymm6);
                    //                    ymm10 += ymm1 * ymm3;
                    ymm10 = _mm256_fmadd_ps(ymm1, ymm3, ymm10);
                    //                    ymm14 += ymm2 * ymm3;
                    ymm14 = _mm256_fmadd_ps(ymm2, ymm3, ymm14);

                    tA += lda;
                }
                // alpha, beta multiplication.
                ymm0 = _mm256_broadcast_ss(alpha_cast);
                ymm1 = _mm256_broadcast_ss(beta_cast);

                //multiply A*B by alpha.
                ymm4 = _mm256_mul_ps(ymm4, ymm0);
                ymm5 = _mm256_mul_ps(ymm5, ymm0);
                ymm6 = _mm256_mul_ps(ymm6, ymm0);
                ymm8 = _mm256_mul_ps(ymm8, ymm0);
                ymm9 = _mm256_mul_ps(ymm9, ymm0);
                ymm10 = _mm256_mul_ps(ymm10, ymm0);
                ymm12 = _mm256_mul_ps(ymm12, ymm0);
                ymm13 = _mm256_mul_ps(ymm13, ymm0);
                ymm14 = _mm256_mul_ps(ymm14, ymm0);

                // multiply C by beta and accumulate.
                ymm2 = _mm256_loadu_ps(tC);
                ymm4 = _mm256_fmadd_ps(ymm2, ymm1, ymm4);
                ymm2 = _mm256_loadu_ps(tC + 8);
                ymm5 = _mm256_fmadd_ps(ymm2, ymm1, ymm5);
                ymm2 = _mm256_loadu_ps(tC + 16);
                ymm6 = _mm256_fmadd_ps(ymm2, ymm1, ymm6);
                _mm256_storeu_ps(tC, ymm4);
                _mm256_storeu_ps(tC + 8, ymm5);
                _mm256_storeu_ps(tC + 16, ymm6);

                // multiply C by beta and accumulate.
                tC += ldc;
                ymm2 = _mm256_loadu_ps(tC);
                ymm8 = _mm256_fmadd_ps(ymm2, ymm1, ymm8);
                ymm2 = _mm256_loadu_ps(tC + 8);
                ymm9 = _mm256_fmadd_ps(ymm2, ymm1, ymm9);
                ymm2 = _mm256_loadu_ps(tC + 16);
                ymm10 = _mm256_fmadd_ps(ymm2, ymm1, ymm10);
                _mm256_storeu_ps(tC, ymm8);
                _mm256_storeu_ps(tC + 8, ymm9);
                _mm256_storeu_ps(tC + 16, ymm10);

                // multiply C by beta and accumulate.
                tC += ldc;
                ymm2 = _mm256_loadu_ps(tC);
                ymm12 = _mm256_fmadd_ps(ymm2, ymm1, ymm12);
                ymm2 = _mm256_loadu_ps(tC + 8);
                ymm13 = _mm256_fmadd_ps(ymm2, ymm1, ymm13);
                ymm2 = _mm256_loadu_ps(tC + 16);
                ymm14 = _mm256_fmadd_ps(ymm2, ymm1, ymm14);
                _mm256_storeu_ps(tC, ymm12);
                _mm256_storeu_ps(tC + 8, ymm13);
                _mm256_storeu_ps(tC + 16, ymm14);

            }
            n_remainder = N - col_idx;
            // if the N is not multiple of 3.
            // handling edge case.
            if (n_remainder == 2)
            {
                //pointer math to point to proper memory
                tC = C + ldc * col_idx + row_idx;
                tB = B + tb_inc_col * col_idx;
                tA = A + row_idx;

                // clear scratch registers.
                ymm8 = _mm256_setzero_ps();
                ymm9 = _mm256_setzero_ps();
                ymm10 = _mm256_setzero_ps();
                ymm12 = _mm256_setzero_ps();
                ymm13 = _mm256_setzero_ps();
                ymm14 = _mm256_setzero_ps();

                for (k = 0; k < K; ++k)
                {
                    // The inner loop broadcasts the B matrix data and
                    // multiplies it with the A matrix.
                    ymm0 = _mm256_broadcast_ss(tB + tb_inc_col * 0);
                    ymm1 = _mm256_broadcast_ss(tB + tb_inc_col * 1);
                    tB += tb_inc_row;

                    //broadcasted matrix B elements are multiplied
                    //with matrix A columns.
                    ymm3 = _mm256_loadu_ps(tA);
                    ymm8 = _mm256_fmadd_ps(ymm0, ymm3, ymm8);
                    ymm12 = _mm256_fmadd_ps(ymm1, ymm3, ymm12);

                    ymm3 = _mm256_loadu_ps(tA + 8);
                    ymm9 = _mm256_fmadd_ps(ymm0, ymm3, ymm9);
                    ymm13 = _mm256_fmadd_ps(ymm1, ymm3, ymm13);

                    ymm3 = _mm256_loadu_ps(tA + 16);
                    ymm10 = _mm256_fmadd_ps(ymm0, ymm3, ymm10);
                    ymm14 = _mm256_fmadd_ps(ymm1, ymm3, ymm14);

                    tA += lda;

                }
                // alpha, beta multiplication.
                ymm0 = _mm256_broadcast_ss(alpha_cast);
                ymm1 = _mm256_broadcast_ss(beta_cast);

                //multiply A*B by alpha.
                ymm8 = _mm256_mul_ps(ymm8, ymm0);
                ymm9 = _mm256_mul_ps(ymm9, ymm0);
                ymm10 = _mm256_mul_ps(ymm10, ymm0);
                ymm12 = _mm256_mul_ps(ymm12, ymm0);
                ymm13 = _mm256_mul_ps(ymm13, ymm0);
                ymm14 = _mm256_mul_ps(ymm14, ymm0);

                // multiply C by beta and accumulate.
                ymm2 = _mm256_loadu_ps(tC + 0);
                ymm8 = _mm256_fmadd_ps(ymm2, ymm1, ymm8);
                ymm2 = _mm256_loadu_ps(tC + 8);
                ymm9 = _mm256_fmadd_ps(ymm2, ymm1, ymm9);
                ymm2 = _mm256_loadu_ps(tC + 16);
                ymm10 = _mm256_fmadd_ps(ymm2, ymm1, ymm10);
                _mm256_storeu_ps(tC + 0, ymm8);
                _mm256_storeu_ps(tC + 8, ymm9);
                _mm256_storeu_ps(tC + 16, ymm10);

                // multiply C by beta and accumulate.
                tC += ldc;
                ymm2 = _mm256_loadu_ps(tC);
                ymm12 = _mm256_fmadd_ps(ymm2, ymm1, ymm12);
                ymm2 = _mm256_loadu_ps(tC + 8);
                ymm13 = _mm256_fmadd_ps(ymm2, ymm1, ymm13);
                ymm2 = _mm256_loadu_ps(tC + 16);
                ymm14 = _mm256_fmadd_ps(ymm2, ymm1, ymm14);
                _mm256_storeu_ps(tC, ymm12);
                _mm256_storeu_ps(tC + 8, ymm13);
                _mm256_storeu_ps(tC + 16, ymm14);

                col_idx += 2;
            }
            // if the N is not multiple of 3.
            // handling edge case.
            if (n_remainder == 1)
            {
                //pointer math to point to proper memory
                tC = C + ldc * col_idx + row_idx;
                tB = B + tb_inc_col * col_idx;
                tA = A + row_idx;

                // clear scratch registers.
                ymm12 = _mm256_setzero_ps();
                ymm13 = _mm256_setzero_ps();
                ymm14 = _mm256_setzero_ps();

                for (k = 0; k < K; ++k)
                {
                    // The inner loop broadcasts the B matrix data and
                    // multiplies it with the A matrix.
                    ymm0 = _mm256_broadcast_ss(tB + tb_inc_col * 0);
                    tB += tb_inc_row;

                    //broadcasted matrix B elements are multiplied
                    //with matrix A columns.
                    ymm3 = _mm256_loadu_ps(tA);
                    ymm12 = _mm256_fmadd_ps(ymm0, ymm3, ymm12);

                    ymm3 = _mm256_loadu_ps(tA + 8);
                    ymm13 = _mm256_fmadd_ps(ymm0, ymm3, ymm13);

                    ymm3 = _mm256_loadu_ps(tA + 16);
                    ymm14 = _mm256_fmadd_ps(ymm0, ymm3, ymm14);

                    tA += lda;

                }
                // alpha, beta multiplication.
                ymm0 = _mm256_broadcast_ss(alpha_cast);
                ymm1 = _mm256_broadcast_ss(beta_cast);

                //multiply A*B by alpha.
                ymm12 = _mm256_mul_ps(ymm12, ymm0);
                ymm13 = _mm256_mul_ps(ymm13, ymm0);
                ymm14 = _mm256_mul_ps(ymm14, ymm0);

                // multiply C by beta and accumulate.
                ymm2 = _mm256_loadu_ps(tC + 0);
                ymm12 = _mm256_fmadd_ps(ymm2, ymm1, ymm12);
                ymm2 = _mm256_loadu_ps(tC + 8);
                ymm13 = _mm256_fmadd_ps(ymm2, ymm1, ymm13);
                ymm2 = _mm256_loadu_ps(tC + 16);
                ymm14 = _mm256_fmadd_ps(ymm2, ymm1, ymm14);

                _mm256_storeu_ps(tC + 0, ymm12);
                _mm256_storeu_ps(tC + 8, ymm13);
                _mm256_storeu_ps(tC + 16, ymm14);
            }

            row_idx += 24;
        }

        if (m_remainder >= 16)
        {
            m_remainder -= 16;

            for (col_idx = 0; (col_idx + 2) < N; col_idx += 3)
            {
                //pointer math to point to proper memory
                tC = C + ldc * col_idx + row_idx;
                tB = B + tb_inc_col * col_idx;
                tA = A + row_idx;

                // clear scratch registers.
                ymm4 = _mm256_setzero_ps();
                ymm5 = _mm256_setzero_ps();
                ymm6 = _mm256_setzero_ps();
                ymm7 = _mm256_setzero_ps();
                ymm8 = _mm256_setzero_ps();
                ymm9 = _mm256_setzero_ps();

                for (k = 0; k < K; ++k)
                {
                    // The inner loop broadcasts the B matrix data and
                    // multiplies it with the A matrix.
                    ymm0 = _mm256_broadcast_ss(tB + tb_inc_col * 0);
                    ymm1 = _mm256_broadcast_ss(tB + tb_inc_col * 1);
                    ymm2 = _mm256_broadcast_ss(tB + tb_inc_col * 2);
                    tB += tb_inc_row;

                    //broadcasted matrix B elements are multiplied
                    //with matrix A columns.
                    ymm3 = _mm256_loadu_ps(tA);
                    ymm4 = _mm256_fmadd_ps(ymm0, ymm3, ymm4);
                    ymm6 = _mm256_fmadd_ps(ymm1, ymm3, ymm6);
                    ymm8 = _mm256_fmadd_ps(ymm2, ymm3, ymm8);

                    ymm3 = _mm256_loadu_ps(tA + 8);
                    ymm5 = _mm256_fmadd_ps(ymm0, ymm3, ymm5);
                    ymm7 = _mm256_fmadd_ps(ymm1, ymm3, ymm7);
                    ymm9 = _mm256_fmadd_ps(ymm2, ymm3, ymm9);

                    tA += lda;
                }
                // alpha, beta multiplication.
                ymm0 = _mm256_broadcast_ss(alpha_cast);
                ymm1 = _mm256_broadcast_ss(beta_cast);

                //multiply A*B by alpha.
                ymm4 = _mm256_mul_ps(ymm4, ymm0);
                ymm5 = _mm256_mul_ps(ymm5, ymm0);
                ymm6 = _mm256_mul_ps(ymm6, ymm0);
                ymm7 = _mm256_mul_ps(ymm7, ymm0);
                ymm8 = _mm256_mul_ps(ymm8, ymm0);
                ymm9 = _mm256_mul_ps(ymm9, ymm0);

                // multiply C by beta and accumulate.
                ymm2 = _mm256_loadu_ps(tC);
                ymm4 = _mm256_fmadd_ps(ymm2, ymm1, ymm4);
                ymm2 = _mm256_loadu_ps(tC + 8);
                ymm5 = _mm256_fmadd_ps(ymm2, ymm1, ymm5);
                _mm256_storeu_ps(tC, ymm4);
                _mm256_storeu_ps(tC + 8, ymm5);

                // multiply C by beta and accumulate.
                tC += ldc;
                ymm2 = _mm256_loadu_ps(tC);
                ymm6 = _mm256_fmadd_ps(ymm2, ymm1, ymm6);
                ymm2 = _mm256_loadu_ps(tC + 8);
                ymm7 = _mm256_fmadd_ps(ymm2, ymm1, ymm7);
                _mm256_storeu_ps(tC, ymm6);
                _mm256_storeu_ps(tC + 8, ymm7);

                // multiply C by beta and accumulate.
                tC += ldc;
                ymm2 = _mm256_loadu_ps(tC);
                ymm8 = _mm256_fmadd_ps(ymm2, ymm1, ymm8);
                ymm2 = _mm256_loadu_ps(tC + 8);
                ymm9 = _mm256_fmadd_ps(ymm2, ymm1, ymm9);
                _mm256_storeu_ps(tC, ymm8);
                _mm256_storeu_ps(tC + 8, ymm9);

            }
            n_remainder = N - col_idx;
            // if the N is not multiple of 3.
            // handling edge case.
            if (n_remainder == 2)
            {
                //pointer math to point to proper memory
                tC = C + ldc * col_idx + row_idx;
                tB = B + tb_inc_col * col_idx;
                tA = A + row_idx;

                // clear scratch registers.
                ymm4 = _mm256_setzero_ps();
                ymm5 = _mm256_setzero_ps();
                ymm6 = _mm256_setzero_ps();
                ymm7 = _mm256_setzero_ps();

                for (k = 0; k < K; ++k)
                {
                    // The inner loop broadcasts the B matrix data and
                    // multiplies it with the A matrix.
                    ymm0 = _mm256_broadcast_ss(tB + tb_inc_col * 0);
                    ymm1 = _mm256_broadcast_ss(tB + tb_inc_col * 1);
                    tB += tb_inc_row;

                    //broadcasted matrix B elements are multiplied
                    //with matrix A columns.
                    ymm3 = _mm256_loadu_ps(tA);
                    ymm4 = _mm256_fmadd_ps(ymm0, ymm3, ymm4);
                    ymm6 = _mm256_fmadd_ps(ymm1, ymm3, ymm6);

                    ymm3 = _mm256_loadu_ps(tA + 8);
                    ymm5 = _mm256_fmadd_ps(ymm0, ymm3, ymm5);
                    ymm7 = _mm256_fmadd_ps(ymm1, ymm3, ymm7);

                    tA += lda;
                }
                // alpha, beta multiplication.
                ymm0 = _mm256_broadcast_ss(alpha_cast);
                ymm1 = _mm256_broadcast_ss(beta_cast);

                //multiply A*B by alpha.
                ymm4 = _mm256_mul_ps(ymm4, ymm0);
                ymm5 = _mm256_mul_ps(ymm5, ymm0);
                ymm6 = _mm256_mul_ps(ymm6, ymm0);
                ymm7 = _mm256_mul_ps(ymm7, ymm0);

                // multiply C by beta and accumulate.
                ymm2 = _mm256_loadu_ps(tC);
                ymm4 = _mm256_fmadd_ps(ymm2, ymm1, ymm4);
                ymm2 = _mm256_loadu_ps(tC + 8);
                ymm5 = _mm256_fmadd_ps(ymm2, ymm1, ymm5);
                _mm256_storeu_ps(tC, ymm4);
                _mm256_storeu_ps(tC + 8, ymm5);

                // multiply C by beta and accumulate.
                tC += ldc;
                ymm2 = _mm256_loadu_ps(tC);
                ymm6 = _mm256_fmadd_ps(ymm2, ymm1, ymm6);
                ymm2 = _mm256_loadu_ps(tC + 8);
                ymm7 = _mm256_fmadd_ps(ymm2, ymm1, ymm7);
                _mm256_storeu_ps(tC, ymm6);
                _mm256_storeu_ps(tC + 8, ymm7);

                col_idx += 2;

            }
            // if the N is not multiple of 3.
            // handling edge case.
            if (n_remainder == 1)
            {
                //pointer math to point to proper memory
                tC = C + ldc * col_idx + row_idx;
                tB = B + tb_inc_col * col_idx;
                tA = A + row_idx;

                ymm4 = _mm256_setzero_ps();
                ymm5 = _mm256_setzero_ps();

                for (k = 0; k < K; ++k)
                {
                    // The inner loop broadcasts the B matrix data and
                    // multiplies it with the A matrix.
                    ymm0 = _mm256_broadcast_ss(tB + tb_inc_col * 0);
                    tB += tb_inc_row;

                    //broadcasted matrix B elements are multiplied
                    //with matrix A columns.
                    ymm3 = _mm256_loadu_ps(tA);
                    ymm4 = _mm256_fmadd_ps(ymm0, ymm3, ymm4);

                    ymm3 = _mm256_loadu_ps(tA + 8);
                    ymm5 = _mm256_fmadd_ps(ymm0, ymm3, ymm5);

                    tA += lda;
                }
                // alpha, beta multiplication.
                ymm0 = _mm256_broadcast_ss(alpha_cast);
                ymm1 = _mm256_broadcast_ss(beta_cast);

                ymm4 = _mm256_mul_ps(ymm4, ymm0);
                ymm5 = _mm256_mul_ps(ymm5, ymm0);

                // multiply C by beta and accumulate.
                ymm2 = _mm256_loadu_ps(tC);
                ymm4 = _mm256_fmadd_ps(ymm2, ymm1, ymm4);
                ymm2 = _mm256_loadu_ps(tC + 8);
                ymm5 = _mm256_fmadd_ps(ymm2, ymm1, ymm5);
                _mm256_storeu_ps(tC, ymm4);
                _mm256_storeu_ps(tC + 8, ymm5);

            }

            row_idx += 16;
        }

        if (m_remainder >= 8)
        {
            m_remainder -= 8;

            for (col_idx = 0; (col_idx + 2) < N; col_idx += 3)
            {
                //pointer math to point to proper memory
                tC = C + ldc * col_idx + row_idx;
                tB = B + tb_inc_col * col_idx;
                tA = A + row_idx;

                // clear scratch registers.
                ymm4 = _mm256_setzero_ps();
                ymm5 = _mm256_setzero_ps();
                ymm6 = _mm256_setzero_ps();

                for (k = 0; k < K; ++k)
                {
                    // The inner loop broadcasts the B matrix data and
                    // multiplies it with the A matrix.
                    ymm0 = _mm256_broadcast_ss(tB + tb_inc_col * 0);
                    ymm1 = _mm256_broadcast_ss(tB + tb_inc_col * 1);
                    ymm2 = _mm256_broadcast_ss(tB + tb_inc_col * 2);
                    tB += tb_inc_row;

                    //broadcasted matrix B elements are multiplied
                    //with matrix A columns.
                    ymm3 = _mm256_loadu_ps(tA);
                    ymm4 = _mm256_fmadd_ps(ymm0, ymm3, ymm4);
                    ymm5 = _mm256_fmadd_ps(ymm1, ymm3, ymm5);
                    ymm6 = _mm256_fmadd_ps(ymm2, ymm3, ymm6);

                    tA += lda;
                }
                // alpha, beta multiplication.
                ymm0 = _mm256_broadcast_ss(alpha_cast);
                ymm1 = _mm256_broadcast_ss(beta_cast);

                //multiply A*B by alpha.
                ymm4 = _mm256_mul_ps(ymm4, ymm0);
                ymm5 = _mm256_mul_ps(ymm5, ymm0);
                ymm6 = _mm256_mul_ps(ymm6, ymm0);

                // multiply C by beta and accumulate.
                ymm2 = _mm256_loadu_ps(tC);
                ymm4 = _mm256_fmadd_ps(ymm2, ymm1, ymm4);
                _mm256_storeu_ps(tC, ymm4);

                // multiply C by beta and accumulate.
                tC += ldc;
                ymm2 = _mm256_loadu_ps(tC);
                ymm5 = _mm256_fmadd_ps(ymm2, ymm1, ymm5);
                _mm256_storeu_ps(tC, ymm5);

                // multiply C by beta and accumulate.
                tC += ldc;
                ymm2 = _mm256_loadu_ps(tC);
                ymm6 = _mm256_fmadd_ps(ymm2, ymm1, ymm6);
                _mm256_storeu_ps(tC, ymm6);
            }
            n_remainder = N - col_idx;
            // if the N is not multiple of 3.
            // handling edge case.
            if (n_remainder == 2)
            {
                //pointer math to point to proper memory
                tC = C + ldc * col_idx + row_idx;
                tB = B + tb_inc_col * col_idx;
                tA = A + row_idx;

                ymm4 = _mm256_setzero_ps();
                ymm5 = _mm256_setzero_ps();

                for (k = 0; k < K; ++k)
                {
                    // The inner loop broadcasts the B matrix data and
                    // multiplies it with the A matrix.
                    ymm0 = _mm256_broadcast_ss(tB + tb_inc_col * 0);
                    ymm1 = _mm256_broadcast_ss(tB + tb_inc_col * 1);
                    tB += tb_inc_row;

                    //broadcasted matrix B elements are multiplied
                    //with matrix A columns.
                    ymm3 = _mm256_loadu_ps(tA);
                    ymm4 = _mm256_fmadd_ps(ymm0, ymm3, ymm4);
                    ymm5 = _mm256_fmadd_ps(ymm1, ymm3, ymm5);

                    tA += lda;
                }
                // alpha, beta multiplication.
                ymm0 = _mm256_broadcast_ss(alpha_cast);
                ymm1 = _mm256_broadcast_ss(beta_cast);

                //multiply A*B by alpha.
                ymm4 = _mm256_mul_ps(ymm4, ymm0);
                ymm5 = _mm256_mul_ps(ymm5, ymm0);

                // multiply C by beta and accumulate.
                ymm2 = _mm256_loadu_ps(tC);
                ymm4 = _mm256_fmadd_ps(ymm2, ymm1, ymm4);
                _mm256_storeu_ps(tC, ymm4);

                // multiply C by beta and accumulate.
                tC += ldc;
                ymm2 = _mm256_loadu_ps(tC);
                ymm5 = _mm256_fmadd_ps(ymm2, ymm1, ymm5);
                _mm256_storeu_ps(tC, ymm5);

                col_idx += 2;

            }
            // if the N is not multiple of 3.
            // handling edge case.
            if (n_remainder == 1)
            {
                //pointer math to point to proper memory
                tC = C + ldc * col_idx + row_idx;
                tB = B + tb_inc_col * col_idx;
                tA = A + row_idx;

                ymm4 = _mm256_setzero_ps();

                for (k = 0; k < K; ++k)
                {
                    // The inner loop broadcasts the B matrix data and
                    // multiplies it with the A matrix.
                    ymm0 = _mm256_broadcast_ss(tB + tb_inc_col * 0);
                    tB += tb_inc_row;

                    //broadcasted matrix B elements are multiplied
                    //with matrix A columns.
                    ymm3 = _mm256_loadu_ps(tA);
                    ymm4 = _mm256_fmadd_ps(ymm0, ymm3, ymm4);

                    tA += lda;
                }
                // alpha, beta multiplication.
                ymm0 = _mm256_broadcast_ss(alpha_cast);
                ymm1 = _mm256_broadcast_ss(beta_cast);

                ymm4 = _mm256_mul_ps(ymm4, ymm0);

                // multiply C by beta and accumulate.
                ymm2 = _mm256_loadu_ps(tC);
                ymm4 = _mm256_fmadd_ps(ymm2, ymm1, ymm4);
                _mm256_storeu_ps(tC, ymm4);

            }

            row_idx += 8;
        }
        // M is not a multiple of 32.
        // The handling of edge case where the remainder
        // dimension is less than 8. The padding takes place
        // to handle this case.
        if ((m_remainder) && (lda > 7))
        {
            float f_temp[8];

            for (col_idx = 0; (col_idx + 2) < N; col_idx += 3)
            {
                //pointer math to point to proper memory
                tC = C + ldc * col_idx + row_idx;
                tB = B + tb_inc_col * col_idx;
                tA = A + row_idx;

                // clear scratch registers.
                ymm5 = _mm256_setzero_ps();
                ymm7 = _mm256_setzero_ps();
                ymm9 = _mm256_setzero_ps();

                for (k = 0; k < (K - 1); ++k)
                {
                    // The inner loop broadcasts the B matrix data and
                    // multiplies it with the A matrix.
                    ymm0 = _mm256_broadcast_ss(tB + tb_inc_col * 0);
                    ymm1 = _mm256_broadcast_ss(tB + tb_inc_col * 1);
                    ymm2 = _mm256_broadcast_ss(tB + tb_inc_col * 2);
                    tB += tb_inc_row;

                    //broadcasted matrix B elements are multiplied
                    //with matrix A columns.
                    ymm3 = _mm256_loadu_ps(tA);
                    ymm5 = _mm256_fmadd_ps(ymm0, ymm3, ymm5);
                    ymm7 = _mm256_fmadd_ps(ymm1, ymm3, ymm7);
                    ymm9 = _mm256_fmadd_ps(ymm2, ymm3, ymm9);

                    tA += lda;
                }
                // alpha, beta multiplication.
                ymm0 = _mm256_broadcast_ss(tB + tb_inc_col * 0);
                ymm1 = _mm256_broadcast_ss(tB + tb_inc_col * 1);
                ymm2 = _mm256_broadcast_ss(tB + tb_inc_col * 2);
                tB += tb_inc_row;

                for (int i = 0; i < m_remainder; i++)
                {
                    f_temp[i] = tA[i];
                }
                ymm3 = _mm256_loadu_ps(f_temp);
                ymm5 = _mm256_fmadd_ps(ymm0, ymm3, ymm5);
                ymm7 = _mm256_fmadd_ps(ymm1, ymm3, ymm7);
                ymm9 = _mm256_fmadd_ps(ymm2, ymm3, ymm9);

                ymm0 = _mm256_broadcast_ss(alpha_cast);
                ymm1 = _mm256_broadcast_ss(beta_cast);

                //multiply A*B by alpha.
                ymm5 = _mm256_mul_ps(ymm5, ymm0);
                ymm7 = _mm256_mul_ps(ymm7, ymm0);
                ymm9 = _mm256_mul_ps(ymm9, ymm0);


                for (int i = 0; i < m_remainder; i++)
                {
                    f_temp[i] = tC[i];
                }
                ymm2 = _mm256_loadu_ps(f_temp);
                ymm5 = _mm256_fmadd_ps(ymm2, ymm1, ymm5);
                _mm256_storeu_ps(f_temp, ymm5);
                for (int i = 0; i < m_remainder; i++)
                {
                    tC[i] = f_temp[i];
                }

                tC += ldc;
                for (int i = 0; i < m_remainder; i++)
                {
                    f_temp[i] = tC[i];
                }
                ymm2 = _mm256_loadu_ps(f_temp);
                ymm7 = _mm256_fmadd_ps(ymm2, ymm1, ymm7);
                _mm256_storeu_ps(f_temp, ymm7);
                for (int i = 0; i < m_remainder; i++)
                {
                    tC[i] = f_temp[i];
                }

                tC += ldc;
                for (int i = 0; i < m_remainder; i++)
                {
                    f_temp[i] = tC[i];
                }
                ymm2 = _mm256_loadu_ps(f_temp);
                ymm9 = _mm256_fmadd_ps(ymm2, ymm1, ymm9);
                _mm256_storeu_ps(f_temp, ymm9);
                for (int i = 0; i < m_remainder; i++)
                {
                    tC[i] = f_temp[i];
                }
            }
            n_remainder = N - col_idx;
            // if the N is not multiple of 3.
            // handling edge case.
            if (n_remainder == 2)
            {
                //pointer math to point to proper memory
                tC = C + ldc * col_idx + row_idx;
                tB = B + tb_inc_col * col_idx;
                tA = A + row_idx;

                ymm5 = _mm256_setzero_ps();
                ymm7 = _mm256_setzero_ps();

                for (k = 0; k < (K - 1); ++k)
                {
                    // The inner loop broadcasts the B matrix data and
                    // multiplies it with the A matrix.
                    ymm0 = _mm256_broadcast_ss(tB + tb_inc_col * 0);
                    ymm1 = _mm256_broadcast_ss(tB + tb_inc_col * 1);
                    tB += tb_inc_row;

                    ymm3 = _mm256_loadu_ps(tA);
                    ymm5 = _mm256_fmadd_ps(ymm0, ymm3, ymm5);
                    ymm7 = _mm256_fmadd_ps(ymm1, ymm3, ymm7);

                    tA += lda;
                }

                ymm0 = _mm256_broadcast_ss(tB + tb_inc_col * 0);
                ymm1 = _mm256_broadcast_ss(tB + tb_inc_col * 1);
                tB += tb_inc_row;

                for (int i = 0; i < m_remainder; i++)
                {
                    f_temp[i] = tA[i];
                }
                ymm3 = _mm256_loadu_ps(f_temp);
                ymm5 = _mm256_fmadd_ps(ymm0, ymm3, ymm5);
                ymm7 = _mm256_fmadd_ps(ymm1, ymm3, ymm7);

                ymm0 = _mm256_broadcast_ss(alpha_cast);
                ymm1 = _mm256_broadcast_ss(beta_cast);

                ymm5 = _mm256_mul_ps(ymm5, ymm0);
                ymm7 = _mm256_mul_ps(ymm7, ymm0);

                for (int i = 0; i < m_remainder; i++)
                {
                    f_temp[i] = tC[i];
                }
                ymm2 = _mm256_loadu_ps(f_temp);
                ymm5 = _mm256_fmadd_ps(ymm2, ymm1, ymm5);
                _mm256_storeu_ps(f_temp, ymm5);
                for (int i = 0; i < m_remainder; i++)
                {
                    tC[i] = f_temp[i];
                }

                tC += ldc;
                for (int i = 0; i < m_remainder; i++)
                {
                    f_temp[i] = tC[i];
                }
                ymm2 = _mm256_loadu_ps(f_temp);
                ymm7 = _mm256_fmadd_ps(ymm2, ymm1, ymm7);
                _mm256_storeu_ps(f_temp, ymm7);
                for (int i = 0; i < m_remainder; i++)
                {
                    tC[i] = f_temp[i];
                }
            }
            // if the N is not multiple of 3.
            // handling edge case.
            if (n_remainder == 1)
            {
                //pointer math to point to proper memory
                tC = C + ldc * col_idx + row_idx;
                tB = B + tb_inc_col * col_idx;
                tA = A + row_idx;

                ymm5 = _mm256_setzero_ps();

                for (k = 0; k < (K - 1); ++k)
                {
                    // The inner loop broadcasts the B matrix data and
                    // multiplies it with the A matrix.
                    ymm0 = _mm256_broadcast_ss(tB + tb_inc_col * 0);
                    tB += tb_inc_row;

                    ymm3 = _mm256_loadu_ps(tA);
                    ymm5 = _mm256_fmadd_ps(ymm0, ymm3, ymm5);

                    tA += lda;
                }

                ymm0 = _mm256_broadcast_ss(tB + tb_inc_col * 0);
                tB += tb_inc_row;

                for (int i = 0; i < m_remainder; i++)
                {
                    f_temp[i] = tA[i];
                }
                ymm3 = _mm256_loadu_ps(f_temp);
                ymm5 = _mm256_fmadd_ps(ymm0, ymm3, ymm5);

                ymm0 = _mm256_broadcast_ss(alpha_cast);
                ymm1 = _mm256_broadcast_ss(beta_cast);

                // multiply C by beta and accumulate.
                ymm5 = _mm256_mul_ps(ymm5, ymm0);

                for (int i = 0; i < m_remainder; i++)
                {
                    f_temp[i] = tC[i];
                }
                ymm2 = _mm256_loadu_ps(f_temp);
                ymm5 = _mm256_fmadd_ps(ymm2, ymm1, ymm5);
                _mm256_storeu_ps(f_temp, ymm5);
                for (int i = 0; i < m_remainder; i++)
                {
                    tC[i] = f_temp[i];
                }
            }
            m_remainder = 0;
        }

        if (m_remainder)
        {
            float result;
            for (; row_idx < M; row_idx += 1)
            {
                for (col_idx = 0; col_idx < N; col_idx += 1)
                {
                    //pointer math to point to proper memory
                    tC = C + ldc * col_idx + row_idx;
                    tB = B + tb_inc_col * col_idx;
                    tA = A + row_idx;

                    result = 0;
                    for (k = 0; k < K; ++k)
                    {
                        result += (*tA) * (*tB);
                        tA += lda;
                        tB += tb_inc_row;
                    }

                    result *= (*alpha_cast);
                    (*tC) = (*tC) * (*beta_cast) + result;
                }
            }
        }
        return BLIS_SUCCESS;
    }
    else
        return BLIS_NONCONFORMAL_DIMENSIONS;


};

static gint_t bli_dgemm_small_matrix
(
    obj_t*  alpha,
    obj_t*  a,
    obj_t*  b,
    obj_t*  beta,
    obj_t*  c,
    cntx_t* cntx,
    cntl_t* cntl
)
{

    int M = bli_obj_length(*c); // number of rows of Matrix C
    int N = bli_obj_width(*c);  // number of columns of Matrix C
    int K = bli_obj_width(*a);  // number of columns of OP(A), will be updated if OP(A) is Transpose(A) .

                                // If alpha is zero, scale by beta and return.
    //   printf("alpha_cast = %f beta_cast = %f [ Trans = %d %d], [stride = %d %d %d] [m,n,k = %d %d %d]\n",*alpha_cast,*beta_cast, bli_obj_has_trans(*a), bli_obj_has_trans(*b), lda, ldb,ldc, M,N,K);
    if (((M * N) < (D_BLIS_SMALL_MATRIX_THRES * D_BLIS_SMALL_MATRIX_THRES))
        || ((M  < D_BLIS_SMALL_M_RECT_MATRIX_THRES) && (K < D_BLIS_SMALL_K_RECT_MATRIX_THRES)))
    {

        int lda = bli_obj_col_stride(*a); // column stride of matrix OP(A), where OP(A) is Transpose(A) if transA enabled.
        int ldb = bli_obj_col_stride(*b); // column stride of matrix OP(B), where OP(B) is Transpose(B) if transB enabled.
        int ldc = bli_obj_col_stride(*c); // column stride of matrix C
        int row_idx, col_idx, k;
        double *A = a->buffer; // pointer to elements of Matrix A
        double *B = b->buffer; // pointer to elements of Matrix B
        double *C = c->buffer; // pointer to elements of Matrix C

        double *tA = A, *tB = B, *tC = C;//, *tA_pack;
        double *tA_packed; // temprorary pointer to hold packed A memory pointer
        int row_idx_packed; //packed A memory row index
        int lda_packed; //lda of packed A
        int col_idx_start; //starting index after A matrix is packed.
        dim_t tb_inc_row = 1; // row stride of matrix B
        dim_t tb_inc_col = ldb; // column stride of matrix B
        __m256d ymm4, ymm5, ymm6, ymm7;
        __m256d ymm8, ymm9, ymm10, ymm11;
        __m256d ymm12, ymm13, ymm14, ymm15;
        __m256d ymm0, ymm1, ymm2, ymm3;

        int n_remainder; // If the N is non multiple of 3.(N%3)
        int m_remainder; // If the M is non multiple of 16.(M%16)

        double *alpha_cast, *beta_cast; // alpha, beta multiples
        alpha_cast = (alpha->buffer);
        beta_cast = (beta->buffer);
        int required_packing_A = 1;

        // when N is equal to 1 call GEMV instead of GEMM
        if (N == 1)
        {
            bli_gemv
            (
                alpha,
                a,
                b,
                beta,
                c
            );
            return BLIS_SUCCESS;
        }

        //update the pointer math if matrix B needs to be transposed.
        if (bli_obj_has_trans(*b))
        {
            tb_inc_col = 1; //switch row and column strides
            tb_inc_row = ldb;
        }

        if (N <= 3)
        {
            required_packing_A = 0;
        }
        /*
        * The computation loop runs for D_MRxN columns of C matrix, thus
        * accessing the D_MRxK A matrix data and KxNR B matrix data.
        * The computation is organized as inner loops of dimension D_MRxNR.
        */
        // Process D_MR rows of C matrix at a time.
        for (row_idx = 0; (row_idx + (D_MR - 1)) < M; row_idx += D_MR)
        {

            col_idx_start = 0;
            tA_packed = A;
            row_idx_packed = row_idx;
            lda_packed = lda;

            // This is the part of the pack and compute optimization.
            // During the first column iteration, we store the accessed A matrix into
            // contiguous static memory. This helps to keep te A matrix in Cache and
            // aviods the TLB misses.
            if (required_packing_A)
            {
                col_idx = 0;

                //pointer math to point to proper memory
                tC = C + ldc * col_idx + row_idx;
                tB = B + tb_inc_col * col_idx;
                tA = A + row_idx;
                tA_packed = D_A_pack;

#ifdef BLIS_ENABLE_PREFETCH
                _mm_prefetch((char*)(tC + 0), _MM_HINT_T0);
                _mm_prefetch((char*)(tC + 8), _MM_HINT_T0);
                _mm_prefetch((char*)(tC + ldc), _MM_HINT_T0);
                _mm_prefetch((char*)(tC + ldc + 8), _MM_HINT_T0);
                _mm_prefetch((char*)(tC + 2 * ldc), _MM_HINT_T0);
                _mm_prefetch((char*)(tC + 2 * ldc + 8), _MM_HINT_T0);
#endif
                // clear scratch registers.
                ymm4 = _mm256_setzero_pd();
                ymm5 = _mm256_setzero_pd();
                ymm6 = _mm256_setzero_pd();
                ymm7 = _mm256_setzero_pd();
                ymm8 = _mm256_setzero_pd();
                ymm9 = _mm256_setzero_pd();
                ymm10 = _mm256_setzero_pd();
                ymm11 = _mm256_setzero_pd();
                ymm12 = _mm256_setzero_pd();
                ymm13 = _mm256_setzero_pd();
                ymm14 = _mm256_setzero_pd();
                ymm15 = _mm256_setzero_pd();

                for (k = 0; k < K; ++k)
                {
                    // The inner loop broadcasts the B matrix data and
                    // multiplies it with the A matrix.
                    // This loop is processing D_MR x K
                    ymm0 = _mm256_broadcast_sd(tB + tb_inc_col * 0);
                    ymm1 = _mm256_broadcast_sd(tB + tb_inc_col * 1);
                    ymm2 = _mm256_broadcast_sd(tB + tb_inc_col * 2);
                    tB += tb_inc_row;

                    //broadcasted matrix B elements are multiplied
                    //with matrix A columns.
                    ymm3 = _mm256_loadu_pd(tA);
                    _mm256_storeu_pd(tA_packed, ymm3); // the packing of matrix A
                    //                   ymm4 += ymm0 * ymm3;
                    ymm4 = _mm256_fmadd_pd(ymm0, ymm3, ymm4);
                    //                    ymm8 += ymm1 * ymm3;
                    ymm8 = _mm256_fmadd_pd(ymm1, ymm3, ymm8);
                    //                    ymm12 += ymm2 * ymm3;
                    ymm12 = _mm256_fmadd_pd(ymm2, ymm3, ymm12);

                    ymm3 = _mm256_loadu_pd(tA + 4);
                    _mm256_storeu_pd(tA_packed + 4, ymm3); // the packing of matrix A
                    //                    ymm5 += ymm0 * ymm3;
                    ymm5 = _mm256_fmadd_pd(ymm0, ymm3, ymm5);
                    //                    ymm9 += ymm1 * ymm3;
                    ymm9 = _mm256_fmadd_pd(ymm1, ymm3, ymm9);
                    //                    ymm13 += ymm2 * ymm3;
                    ymm13 = _mm256_fmadd_pd(ymm2, ymm3, ymm13);

                    ymm3 = _mm256_loadu_pd(tA + 8);
                    _mm256_storeu_pd(tA_packed + 8, ymm3); // the packing of matrix A
                    //                   ymm6 += ymm0 * ymm3;
                    ymm6 = _mm256_fmadd_pd(ymm0, ymm3, ymm6);
                    //                    ymm10 += ymm1 * ymm3;
                    ymm10 = _mm256_fmadd_pd(ymm1, ymm3, ymm10);
                    //                    ymm14 += ymm2 * ymm3;
                    ymm14 = _mm256_fmadd_pd(ymm2, ymm3, ymm14);

                    ymm3 = _mm256_loadu_pd(tA + 12);
                    _mm256_storeu_pd(tA_packed + 12, ymm3); // the packing of matrix A
                    //                    ymm7 += ymm0 * ymm3;
                    ymm7 = _mm256_fmadd_pd(ymm0, ymm3, ymm7);
                    //                    ymm11 += ymm1 * ymm3;
                    ymm11 = _mm256_fmadd_pd(ymm1, ymm3, ymm11);
                    //                   ymm15 += ymm2 * ymm3;
                    ymm15 = _mm256_fmadd_pd(ymm2, ymm3, ymm15);

                    tA += lda;
                    tA_packed += D_MR;
                }
                // alpha, beta multiplication.
                ymm0 = _mm256_broadcast_sd(alpha_cast);
                ymm1 = _mm256_broadcast_sd(beta_cast);

                //multiply A*B by alpha.
                ymm4 = _mm256_mul_pd(ymm4, ymm0);
                ymm5 = _mm256_mul_pd(ymm5, ymm0);
                ymm6 = _mm256_mul_pd(ymm6, ymm0);
                ymm7 = _mm256_mul_pd(ymm7, ymm0);
                ymm8 = _mm256_mul_pd(ymm8, ymm0);
                ymm9 = _mm256_mul_pd(ymm9, ymm0);
                ymm10 = _mm256_mul_pd(ymm10, ymm0);
                ymm11 = _mm256_mul_pd(ymm11, ymm0);
                ymm12 = _mm256_mul_pd(ymm12, ymm0);
                ymm13 = _mm256_mul_pd(ymm13, ymm0);
                ymm14 = _mm256_mul_pd(ymm14, ymm0);
                ymm15 = _mm256_mul_pd(ymm15, ymm0);

                // multiply C by beta and accumulate col 1.
                ymm2 = _mm256_loadu_pd(tC);
                ymm4 = _mm256_fmadd_pd(ymm2, ymm1, ymm4);
                ymm2 = _mm256_loadu_pd(tC + 4);
                ymm5 = _mm256_fmadd_pd(ymm2, ymm1, ymm5);
                ymm2 = _mm256_loadu_pd(tC + 8);
                ymm6 = _mm256_fmadd_pd(ymm2, ymm1, ymm6);
                ymm2 = _mm256_loadu_pd(tC + 12);
                ymm7 = _mm256_fmadd_pd(ymm2, ymm1, ymm7);
                _mm256_storeu_pd(tC, ymm4);
                _mm256_storeu_pd(tC + 4, ymm5);
                _mm256_storeu_pd(tC + 8, ymm6);
                _mm256_storeu_pd(tC + 12, ymm7);

                // multiply C by beta and accumulate, col 2.
                tC += ldc;
                ymm2 = _mm256_loadu_pd(tC);
                ymm8 = _mm256_fmadd_pd(ymm2, ymm1, ymm8);
                ymm2 = _mm256_loadu_pd(tC + 4);
                ymm9 = _mm256_fmadd_pd(ymm2, ymm1, ymm9);
                ymm2 = _mm256_loadu_pd(tC + 8);
                ymm10 = _mm256_fmadd_pd(ymm2, ymm1, ymm10);
                ymm2 = _mm256_loadu_pd(tC + 12);
                ymm11 = _mm256_fmadd_pd(ymm2, ymm1, ymm11);
                _mm256_storeu_pd(tC, ymm8);
                _mm256_storeu_pd(tC + 4, ymm9);
                _mm256_storeu_pd(tC + 8, ymm10);
                _mm256_storeu_pd(tC + 12, ymm11);

                // multiply C by beta and accumulate, col 3.
                tC += ldc;
                ymm2 = _mm256_loadu_pd(tC);
                ymm12 = _mm256_fmadd_pd(ymm2, ymm1, ymm12);
                ymm2 = _mm256_loadu_pd(tC + 4);
                ymm13 = _mm256_fmadd_pd(ymm2, ymm1, ymm13);
                ymm2 = _mm256_loadu_pd(tC + 8);
                ymm14 = _mm256_fmadd_pd(ymm2, ymm1, ymm14);
                ymm2 = _mm256_loadu_pd(tC + 12);
                ymm15 = _mm256_fmadd_pd(ymm2, ymm1, ymm15);
                _mm256_storeu_pd(tC, ymm12);
                _mm256_storeu_pd(tC + 4, ymm13);
                _mm256_storeu_pd(tC + 8, ymm14);
                _mm256_storeu_pd(tC + 12, ymm15);

                // modify the pointer arithematic to use packed A matrix.
                col_idx_start = NR;
                tA_packed = D_A_pack;
                row_idx_packed = 0;
                lda_packed = D_MR;
            }
            // Process NR columns of C matrix at a time.
            for (col_idx = col_idx_start; (col_idx + (NR - 1)) < N; col_idx += NR)
            {
                //pointer math to point to proper memory
                tC = C + ldc * col_idx + row_idx;
                tB = B + tb_inc_col * col_idx;
                tA = tA_packed + row_idx_packed;

#ifdef BLIS_ENABLE_PREFETCH
                _mm_prefetch((char*)(tC + 0), _MM_HINT_T0);
                _mm_prefetch((char*)(tC + 8), _MM_HINT_T0);
                _mm_prefetch((char*)(tC + ldc), _MM_HINT_T0);
                _mm_prefetch((char*)(tC + ldc + 8), _MM_HINT_T0);
                _mm_prefetch((char*)(tC + 2 * ldc), _MM_HINT_T0);
                _mm_prefetch((char*)(tC + 2 * ldc + 8), _MM_HINT_T0);
#endif
                // clear scratch registers.
                ymm4 = _mm256_setzero_pd();
                ymm5 = _mm256_setzero_pd();
                ymm6 = _mm256_setzero_pd();
                ymm7 = _mm256_setzero_pd();
                ymm8 = _mm256_setzero_pd();
                ymm9 = _mm256_setzero_pd();
                ymm10 = _mm256_setzero_pd();
                ymm11 = _mm256_setzero_pd();
                ymm12 = _mm256_setzero_pd();
                ymm13 = _mm256_setzero_pd();
                ymm14 = _mm256_setzero_pd();
                ymm15 = _mm256_setzero_pd();

                for (k = 0; k < K; ++k)
                {
                    // The inner loop broadcasts the B matrix data and
                    // multiplies it with the A matrix.
                    // This loop is processing D_MR x K
                    ymm0 = _mm256_broadcast_sd(tB + tb_inc_col * 0);
                    ymm1 = _mm256_broadcast_sd(tB + tb_inc_col * 1);
                    ymm2 = _mm256_broadcast_sd(tB + tb_inc_col * 2);
                    tB += tb_inc_row;

                    //broadcasted matrix B elements are multiplied
                    //with matrix A columns.
                    ymm3 = _mm256_loadu_pd(tA);
                    //                   ymm4 += ymm0 * ymm3;
                    ymm4 = _mm256_fmadd_pd(ymm0, ymm3, ymm4);
                    //                    ymm8 += ymm1 * ymm3;
                    ymm8 = _mm256_fmadd_pd(ymm1, ymm3, ymm8);
                    //                    ymm12 += ymm2 * ymm3;
                    ymm12 = _mm256_fmadd_pd(ymm2, ymm3, ymm12);

                    ymm3 = _mm256_loadu_pd(tA + 4);
                    //                    ymm5 += ymm0 * ymm3;
                    ymm5 = _mm256_fmadd_pd(ymm0, ymm3, ymm5);
                    //                    ymm9 += ymm1 * ymm3;
                    ymm9 = _mm256_fmadd_pd(ymm1, ymm3, ymm9);
                    //                    ymm13 += ymm2 * ymm3;
                    ymm13 = _mm256_fmadd_pd(ymm2, ymm3, ymm13);

                    ymm3 = _mm256_loadu_pd(tA + 8);
                    //                   ymm6 += ymm0 * ymm3;
                    ymm6 = _mm256_fmadd_pd(ymm0, ymm3, ymm6);
                    //                    ymm10 += ymm1 * ymm3;
                    ymm10 = _mm256_fmadd_pd(ymm1, ymm3, ymm10);
                    //                    ymm14 += ymm2 * ymm3;
                    ymm14 = _mm256_fmadd_pd(ymm2, ymm3, ymm14);

                    ymm3 = _mm256_loadu_pd(tA + 12);
                    //                    ymm7 += ymm0 * ymm3;
                    ymm7 = _mm256_fmadd_pd(ymm0, ymm3, ymm7);
                    //                    ymm11 += ymm1 * ymm3;
                    ymm11 = _mm256_fmadd_pd(ymm1, ymm3, ymm11);
                    //                   ymm15 += ymm2 * ymm3;
                    ymm15 = _mm256_fmadd_pd(ymm2, ymm3, ymm15);

                    tA += lda_packed;
                }
                // alpha, beta multiplication.
                ymm0 = _mm256_broadcast_sd(alpha_cast);
                ymm1 = _mm256_broadcast_sd(beta_cast);

                //multiply A*B by alpha.
                ymm4 = _mm256_mul_pd(ymm4, ymm0);
                ymm5 = _mm256_mul_pd(ymm5, ymm0);
                ymm6 = _mm256_mul_pd(ymm6, ymm0);
                ymm7 = _mm256_mul_pd(ymm7, ymm0);
                ymm8 = _mm256_mul_pd(ymm8, ymm0);
                ymm9 = _mm256_mul_pd(ymm9, ymm0);
                ymm10 = _mm256_mul_pd(ymm10, ymm0);
                ymm11 = _mm256_mul_pd(ymm11, ymm0);
                ymm12 = _mm256_mul_pd(ymm12, ymm0);
                ymm13 = _mm256_mul_pd(ymm13, ymm0);
                ymm14 = _mm256_mul_pd(ymm14, ymm0);
                ymm15 = _mm256_mul_pd(ymm15, ymm0);

                // multiply C by beta and accumulate col 1.
                ymm2 = _mm256_loadu_pd(tC);
                ymm4 = _mm256_fmadd_pd(ymm2, ymm1, ymm4);
                ymm2 = _mm256_loadu_pd(tC + 4);
                ymm5 = _mm256_fmadd_pd(ymm2, ymm1, ymm5);
                ymm2 = _mm256_loadu_pd(tC + 8);
                ymm6 = _mm256_fmadd_pd(ymm2, ymm1, ymm6);
                ymm2 = _mm256_loadu_pd(tC + 12);
                ymm7 = _mm256_fmadd_pd(ymm2, ymm1, ymm7);
                _mm256_storeu_pd(tC, ymm4);
                _mm256_storeu_pd(tC + 4, ymm5);
                _mm256_storeu_pd(tC + 8, ymm6);
                _mm256_storeu_pd(tC + 12, ymm7);

                // multiply C by beta and accumulate, col 2.
                tC += ldc;
                ymm2 = _mm256_loadu_pd(tC);
                ymm8 = _mm256_fmadd_pd(ymm2, ymm1, ymm8);
                ymm2 = _mm256_loadu_pd(tC + 4);
                ymm9 = _mm256_fmadd_pd(ymm2, ymm1, ymm9);
                ymm2 = _mm256_loadu_pd(tC + 8);
                ymm10 = _mm256_fmadd_pd(ymm2, ymm1, ymm10);
                ymm2 = _mm256_loadu_pd(tC + 12);
                ymm11 = _mm256_fmadd_pd(ymm2, ymm1, ymm11);
                _mm256_storeu_pd(tC, ymm8);
                _mm256_storeu_pd(tC + 4, ymm9);
                _mm256_storeu_pd(tC + 8, ymm10);
                _mm256_storeu_pd(tC + 12, ymm11);

                // multiply C by beta and accumulate, col 3.
                tC += ldc;
                ymm2 = _mm256_loadu_pd(tC);
                ymm12 = _mm256_fmadd_pd(ymm2, ymm1, ymm12);
                ymm2 = _mm256_loadu_pd(tC + 4);
                ymm13 = _mm256_fmadd_pd(ymm2, ymm1, ymm13);
                ymm2 = _mm256_loadu_pd(tC + 8);
                ymm14 = _mm256_fmadd_pd(ymm2, ymm1, ymm14);
                ymm2 = _mm256_loadu_pd(tC + 12);
                ymm15 = _mm256_fmadd_pd(ymm2, ymm1, ymm15);
                _mm256_storeu_pd(tC, ymm12);
                _mm256_storeu_pd(tC + 4, ymm13);
                _mm256_storeu_pd(tC + 8, ymm14);
                _mm256_storeu_pd(tC + 12, ymm15);

            }
            n_remainder = N - col_idx;

            // if the N is not multiple of 3.
            // handling edge case.
            if (n_remainder == 2)
            {
                //pointer math to point to proper memory
                tC = C + ldc * col_idx + row_idx;
                tB = B + tb_inc_col * col_idx;
                tA = A + row_idx;

                // clear scratch registers.
                ymm8 = _mm256_setzero_pd();
                ymm9 = _mm256_setzero_pd();
                ymm10 = _mm256_setzero_pd();
                ymm11 = _mm256_setzero_pd();
                ymm12 = _mm256_setzero_pd();
                ymm13 = _mm256_setzero_pd();
                ymm14 = _mm256_setzero_pd();
                ymm15 = _mm256_setzero_pd();

                for (k = 0; k < K; ++k)
                {
                    // The inner loop broadcasts the B matrix data and
                    // multiplies it with the A matrix.
                    ymm0 = _mm256_broadcast_sd(tB + tb_inc_col * 0);
                    ymm1 = _mm256_broadcast_sd(tB + tb_inc_col * 1);
                    tB += tb_inc_row;

                    //broadcasted matrix B elements are multiplied
                    //with matrix A columns.
                    ymm3 = _mm256_loadu_pd(tA);
                    ymm8 = _mm256_fmadd_pd(ymm0, ymm3, ymm8);
                    ymm12 = _mm256_fmadd_pd(ymm1, ymm3, ymm12);

                    ymm3 = _mm256_loadu_pd(tA + 4);
                    ymm9 = _mm256_fmadd_pd(ymm0, ymm3, ymm9);
                    ymm13 = _mm256_fmadd_pd(ymm1, ymm3, ymm13);

                    ymm3 = _mm256_loadu_pd(tA + 8);
                    ymm10 = _mm256_fmadd_pd(ymm0, ymm3, ymm10);
                    ymm14 = _mm256_fmadd_pd(ymm1, ymm3, ymm14);

                    ymm3 = _mm256_loadu_pd(tA + 12);
                    ymm11 = _mm256_fmadd_pd(ymm0, ymm3, ymm11);
                    ymm15 = _mm256_fmadd_pd(ymm1, ymm3, ymm15);

                    tA += lda;

                }
                // alpha, beta multiplication.
                ymm0 = _mm256_broadcast_sd(alpha_cast);
                ymm1 = _mm256_broadcast_sd(beta_cast);

                //multiply A*B by alpha.
                ymm8 = _mm256_mul_pd(ymm8, ymm0);
                ymm9 = _mm256_mul_pd(ymm9, ymm0);
                ymm10 = _mm256_mul_pd(ymm10, ymm0);
                ymm11 = _mm256_mul_pd(ymm11, ymm0);
                ymm12 = _mm256_mul_pd(ymm12, ymm0);
                ymm13 = _mm256_mul_pd(ymm13, ymm0);
                ymm14 = _mm256_mul_pd(ymm14, ymm0);
                ymm15 = _mm256_mul_pd(ymm15, ymm0);

                // multiply C by beta and accumulate, col 1.
                ymm2 = _mm256_loadu_pd(tC + 0);
                ymm8 = _mm256_fmadd_pd(ymm2, ymm1, ymm8);
                ymm2 = _mm256_loadu_pd(tC + 4);
                ymm9 = _mm256_fmadd_pd(ymm2, ymm1, ymm9);
                ymm2 = _mm256_loadu_pd(tC + 8);
                ymm10 = _mm256_fmadd_pd(ymm2, ymm1, ymm10);
                ymm2 = _mm256_loadu_pd(tC + 12);
                ymm11 = _mm256_fmadd_pd(ymm2, ymm1, ymm11);
                _mm256_storeu_pd(tC + 0, ymm8);
                _mm256_storeu_pd(tC + 4, ymm9);
                _mm256_storeu_pd(tC + 8, ymm10);
                _mm256_storeu_pd(tC + 12, ymm11);

                // multiply C by beta and accumulate, col 2.
                tC += ldc;
                ymm2 = _mm256_loadu_pd(tC);
                ymm12 = _mm256_fmadd_pd(ymm2, ymm1, ymm12);
                ymm2 = _mm256_loadu_pd(tC + 4);
                ymm13 = _mm256_fmadd_pd(ymm2, ymm1, ymm13);
                ymm2 = _mm256_loadu_pd(tC + 8);
                ymm14 = _mm256_fmadd_pd(ymm2, ymm1, ymm14);
                ymm2 = _mm256_loadu_pd(tC + 12);
                ymm15 = _mm256_fmadd_pd(ymm2, ymm1, ymm15);
                _mm256_storeu_pd(tC, ymm12);
                _mm256_storeu_pd(tC + 4, ymm13);
                _mm256_storeu_pd(tC + 8, ymm14);
                _mm256_storeu_pd(tC + 12, ymm15);

                col_idx += 2;
            }
            // if the N is not multiple of 3.
            // handling edge case.
            if (n_remainder == 1)
            {
                //pointer math to point to proper memory
                tC = C + ldc * col_idx + row_idx;
                tB = B + tb_inc_col * col_idx;
                tA = A + row_idx;

                // clear scratch registers.
                ymm12 = _mm256_setzero_pd();
                ymm13 = _mm256_setzero_pd();
                ymm14 = _mm256_setzero_pd();
                ymm15 = _mm256_setzero_pd();

                for (k = 0; k < K; ++k)
                {
                    // The inner loop broadcasts the B matrix data and
                    // multiplies it with the A matrix.
                    ymm0 = _mm256_broadcast_sd(tB + tb_inc_col * 0);
                    tB += tb_inc_row;

                    //broadcasted matrix B elements are multiplied
                    //with matrix A columns.
                    ymm3 = _mm256_loadu_pd(tA);
                    ymm12 = _mm256_fmadd_pd(ymm0, ymm3, ymm12);

                    ymm3 = _mm256_loadu_pd(tA + 4);
                    ymm13 = _mm256_fmadd_pd(ymm0, ymm3, ymm13);

                    ymm3 = _mm256_loadu_pd(tA + 8);
                    ymm14 = _mm256_fmadd_pd(ymm0, ymm3, ymm14);

                    ymm3 = _mm256_loadu_pd(tA + 12);
                    ymm15 = _mm256_fmadd_pd(ymm0, ymm3, ymm15);

                    tA += lda;

                }
                // alpha, beta multiplication.
                ymm0 = _mm256_broadcast_sd(alpha_cast);
                ymm1 = _mm256_broadcast_sd(beta_cast);

                //multiply A*B by alpha.
                ymm12 = _mm256_mul_pd(ymm12, ymm0);
                ymm13 = _mm256_mul_pd(ymm13, ymm0);
                ymm14 = _mm256_mul_pd(ymm14, ymm0);
                ymm15 = _mm256_mul_pd(ymm15, ymm0);

                // multiply C by beta and accumulate.
                ymm2 = _mm256_loadu_pd(tC + 0);
                ymm12 = _mm256_fmadd_pd(ymm2, ymm1, ymm12);
                ymm2 = _mm256_loadu_pd(tC + 4);
                ymm13 = _mm256_fmadd_pd(ymm2, ymm1, ymm13);
                ymm2 = _mm256_loadu_pd(tC + 8);
                ymm14 = _mm256_fmadd_pd(ymm2, ymm1, ymm14);
                ymm2 = _mm256_loadu_pd(tC + 12);
                ymm15 = _mm256_fmadd_pd(ymm2, ymm1, ymm15);

                _mm256_storeu_pd(tC + 0, ymm12);
                _mm256_storeu_pd(tC + 4, ymm13);
                _mm256_storeu_pd(tC + 8, ymm14);
                _mm256_storeu_pd(tC + 12, ymm15);
            }
        }

        m_remainder = M - row_idx;

        if (m_remainder >= 12)
        {
            m_remainder -= 12;

            for (col_idx = 0; (col_idx + 2) < N; col_idx += 3)
            {
                //pointer math to point to proper memory
                tC = C + ldc * col_idx + row_idx;
                tB = B + tb_inc_col * col_idx;
                tA = A + row_idx;

                // clear scratch registers.
                ymm4 = _mm256_setzero_pd();
                ymm5 = _mm256_setzero_pd();
                ymm6 = _mm256_setzero_pd();
                ymm8 = _mm256_setzero_pd();
                ymm9 = _mm256_setzero_pd();
                ymm10 = _mm256_setzero_pd();
                ymm12 = _mm256_setzero_pd();
                ymm13 = _mm256_setzero_pd();
                ymm14 = _mm256_setzero_pd();

                for (k = 0; k < K; ++k)
                {
                    // The inner loop broadcasts the B matrix data and
                    // multiplies it with the A matrix.
                    ymm0 = _mm256_broadcast_sd(tB + tb_inc_col * 0);
                    ymm1 = _mm256_broadcast_sd(tB + tb_inc_col * 1);
                    ymm2 = _mm256_broadcast_sd(tB + tb_inc_col * 2);
                    tB += tb_inc_row;

                    //broadcasted matrix B elements are multiplied
                    //with matrix A columns.
                    ymm3 = _mm256_loadu_pd(tA);
                    //                   ymm4 += ymm0 * ymm3;
                    ymm4 = _mm256_fmadd_pd(ymm0, ymm3, ymm4);
                    //                    ymm8 += ymm1 * ymm3;
                    ymm8 = _mm256_fmadd_pd(ymm1, ymm3, ymm8);
                    //                    ymm12 += ymm2 * ymm3;
                    ymm12 = _mm256_fmadd_pd(ymm2, ymm3, ymm12);

                    ymm3 = _mm256_loadu_pd(tA + 4);
                    //                    ymm5 += ymm0 * ymm3;
                    ymm5 = _mm256_fmadd_pd(ymm0, ymm3, ymm5);
                    //                    ymm9 += ymm1 * ymm3;
                    ymm9 = _mm256_fmadd_pd(ymm1, ymm3, ymm9);
                    //                    ymm13 += ymm2 * ymm3;
                    ymm13 = _mm256_fmadd_pd(ymm2, ymm3, ymm13);

                    ymm3 = _mm256_loadu_pd(tA + 8);
                    //                   ymm6 += ymm0 * ymm3;
                    ymm6 = _mm256_fmadd_pd(ymm0, ymm3, ymm6);
                    //                    ymm10 += ymm1 * ymm3;
                    ymm10 = _mm256_fmadd_pd(ymm1, ymm3, ymm10);
                    //                    ymm14 += ymm2 * ymm3;
                    ymm14 = _mm256_fmadd_pd(ymm2, ymm3, ymm14);

                    tA += lda;
                }
                // alpha, beta multiplication.
                ymm0 = _mm256_broadcast_sd(alpha_cast);
                ymm1 = _mm256_broadcast_sd(beta_cast);

                //multiply A*B by alpha.
                ymm4 = _mm256_mul_pd(ymm4, ymm0);
                ymm5 = _mm256_mul_pd(ymm5, ymm0);
                ymm6 = _mm256_mul_pd(ymm6, ymm0);
                ymm8 = _mm256_mul_pd(ymm8, ymm0);
                ymm9 = _mm256_mul_pd(ymm9, ymm0);
                ymm10 = _mm256_mul_pd(ymm10, ymm0);
                ymm12 = _mm256_mul_pd(ymm12, ymm0);
                ymm13 = _mm256_mul_pd(ymm13, ymm0);
                ymm14 = _mm256_mul_pd(ymm14, ymm0);

                // multiply C by beta and accumulate.
                ymm2 = _mm256_loadu_pd(tC);
                ymm4 = _mm256_fmadd_pd(ymm2, ymm1, ymm4);
                ymm2 = _mm256_loadu_pd(tC + 4);
                ymm5 = _mm256_fmadd_pd(ymm2, ymm1, ymm5);
                ymm2 = _mm256_loadu_pd(tC + 8);
                ymm6 = _mm256_fmadd_pd(ymm2, ymm1, ymm6);
                _mm256_storeu_pd(tC, ymm4);
                _mm256_storeu_pd(tC + 4, ymm5);
                _mm256_storeu_pd(tC + 8, ymm6);

                // multiply C by beta and accumulate.
                tC += ldc;
                ymm2 = _mm256_loadu_pd(tC);
                ymm8 = _mm256_fmadd_pd(ymm2, ymm1, ymm8);
                ymm2 = _mm256_loadu_pd(tC + 4);
                ymm9 = _mm256_fmadd_pd(ymm2, ymm1, ymm9);
                ymm2 = _mm256_loadu_pd(tC + 8);
                ymm10 = _mm256_fmadd_pd(ymm2, ymm1, ymm10);
                _mm256_storeu_pd(tC, ymm8);
                _mm256_storeu_pd(tC + 4, ymm9);
                _mm256_storeu_pd(tC + 8, ymm10);

                // multiply C by beta and accumulate.
                tC += ldc;
                ymm2 = _mm256_loadu_pd(tC);
                ymm12 = _mm256_fmadd_pd(ymm2, ymm1, ymm12);
                ymm2 = _mm256_loadu_pd(tC + 4);
                ymm13 = _mm256_fmadd_pd(ymm2, ymm1, ymm13);
                ymm2 = _mm256_loadu_pd(tC + 8);
                ymm14 = _mm256_fmadd_pd(ymm2, ymm1, ymm14);
                _mm256_storeu_pd(tC, ymm12);
                _mm256_storeu_pd(tC + 4, ymm13);
                _mm256_storeu_pd(tC + 8, ymm14);

            }
            n_remainder = N - col_idx;
            // if the N is not multiple of 3.
            // handling edge case.
            if (n_remainder == 2)
            {
                //pointer math to point to proper memory
                tC = C + ldc * col_idx + row_idx;
                tB = B + tb_inc_col * col_idx;
                tA = A + row_idx;

                // clear scratch registers.
                ymm8 = _mm256_setzero_pd();
                ymm9 = _mm256_setzero_pd();
                ymm10 = _mm256_setzero_pd();
                ymm12 = _mm256_setzero_pd();
                ymm13 = _mm256_setzero_pd();
                ymm14 = _mm256_setzero_pd();

                for (k = 0; k < K; ++k)
                {
                    // The inner loop broadcasts the B matrix data and
                    // multiplies it with the A matrix.
                    ymm0 = _mm256_broadcast_sd(tB + tb_inc_col * 0);
                    ymm1 = _mm256_broadcast_sd(tB + tb_inc_col * 1);
                    tB += tb_inc_row;

                    //broadcasted matrix B elements are multiplied
                    //with matrix A columns.
                    ymm3 = _mm256_loadu_pd(tA);
                    ymm8 = _mm256_fmadd_pd(ymm0, ymm3, ymm8);
                    ymm12 = _mm256_fmadd_pd(ymm1, ymm3, ymm12);

                    ymm3 = _mm256_loadu_pd(tA + 4);
                    ymm9 = _mm256_fmadd_pd(ymm0, ymm3, ymm9);
                    ymm13 = _mm256_fmadd_pd(ymm1, ymm3, ymm13);

                    ymm3 = _mm256_loadu_pd(tA + 8);
                    ymm10 = _mm256_fmadd_pd(ymm0, ymm3, ymm10);
                    ymm14 = _mm256_fmadd_pd(ymm1, ymm3, ymm14);

                    tA += lda;

                }
                // alpha, beta multiplication.
                ymm0 = _mm256_broadcast_sd(alpha_cast);
                ymm1 = _mm256_broadcast_sd(beta_cast);

                //multiply A*B by alpha.
                ymm8 = _mm256_mul_pd(ymm8, ymm0);
                ymm9 = _mm256_mul_pd(ymm9, ymm0);
                ymm10 = _mm256_mul_pd(ymm10, ymm0);
                ymm12 = _mm256_mul_pd(ymm12, ymm0);
                ymm13 = _mm256_mul_pd(ymm13, ymm0);
                ymm14 = _mm256_mul_pd(ymm14, ymm0);

                // multiply C by beta and accumulate.
                ymm2 = _mm256_loadu_pd(tC + 0);
                ymm8 = _mm256_fmadd_pd(ymm2, ymm1, ymm8);
                ymm2 = _mm256_loadu_pd(tC + 4);
                ymm9 = _mm256_fmadd_pd(ymm2, ymm1, ymm9);
                ymm2 = _mm256_loadu_pd(tC + 8);
                ymm10 = _mm256_fmadd_pd(ymm2, ymm1, ymm10);
                _mm256_storeu_pd(tC + 0, ymm8);
                _mm256_storeu_pd(tC + 4, ymm9);
                _mm256_storeu_pd(tC + 8, ymm10);

                // multiply C by beta and accumulate.
                tC += ldc;
                ymm2 = _mm256_loadu_pd(tC);
                ymm12 = _mm256_fmadd_pd(ymm2, ymm1, ymm12);
                ymm2 = _mm256_loadu_pd(tC + 4);
                ymm13 = _mm256_fmadd_pd(ymm2, ymm1, ymm13);
                ymm2 = _mm256_loadu_pd(tC + 8);
                ymm14 = _mm256_fmadd_pd(ymm2, ymm1, ymm14);
                _mm256_storeu_pd(tC, ymm12);
                _mm256_storeu_pd(tC + 4, ymm13);
                _mm256_storeu_pd(tC + 8, ymm14);

                col_idx += 2;
            }
            // if the N is not multiple of 3.
            // handling edge case.
            if (n_remainder == 1)
            {
                //pointer math to point to proper memory
                tC = C + ldc * col_idx + row_idx;
                tB = B + tb_inc_col * col_idx;
                tA = A + row_idx;

                // clear scratch registers.
                ymm12 = _mm256_setzero_pd();
                ymm13 = _mm256_setzero_pd();
                ymm14 = _mm256_setzero_pd();

                for (k = 0; k < K; ++k)
                {
                    // The inner loop broadcasts the B matrix data and
                    // multiplies it with the A matrix.
                    ymm0 = _mm256_broadcast_sd(tB + tb_inc_col * 0);
                    tB += tb_inc_row;

                    //broadcasted matrix B elements are multiplied
                    //with matrix A columns.
                    ymm3 = _mm256_loadu_pd(tA);
                    ymm12 = _mm256_fmadd_pd(ymm0, ymm3, ymm12);

                    ymm3 = _mm256_loadu_pd(tA + 4);
                    ymm13 = _mm256_fmadd_pd(ymm0, ymm3, ymm13);

                    ymm3 = _mm256_loadu_pd(tA + 8);
                    ymm14 = _mm256_fmadd_pd(ymm0, ymm3, ymm14);

                    tA += lda;

                }
                // alpha, beta multiplication.
                ymm0 = _mm256_broadcast_sd(alpha_cast);
                ymm1 = _mm256_broadcast_sd(beta_cast);

                //multiply A*B by alpha.
                ymm12 = _mm256_mul_pd(ymm12, ymm0);
                ymm13 = _mm256_mul_pd(ymm13, ymm0);
                ymm14 = _mm256_mul_pd(ymm14, ymm0);

                // multiply C by beta and accumulate.
                ymm2 = _mm256_loadu_pd(tC + 0);
                ymm12 = _mm256_fmadd_pd(ymm2, ymm1, ymm12);
                ymm2 = _mm256_loadu_pd(tC + 4);
                ymm13 = _mm256_fmadd_pd(ymm2, ymm1, ymm13);
                ymm2 = _mm256_loadu_pd(tC + 8);
                ymm14 = _mm256_fmadd_pd(ymm2, ymm1, ymm14);

                _mm256_storeu_pd(tC + 0, ymm12);
                _mm256_storeu_pd(tC + 4, ymm13);
                _mm256_storeu_pd(tC + 8, ymm14);
            }

            row_idx += 12;
        }

        if (m_remainder >= 8)
        {
            m_remainder -= 8;

            for (col_idx = 0; (col_idx + 2) < N; col_idx += 3)
            {
                //pointer math to point to proper memory
                tC = C + ldc * col_idx + row_idx;
                tB = B + tb_inc_col * col_idx;
                tA = A + row_idx;

                // clear scratch registers.
                ymm4 = _mm256_setzero_pd();
                ymm5 = _mm256_setzero_pd();
                ymm6 = _mm256_setzero_pd();
                ymm7 = _mm256_setzero_pd();
                ymm8 = _mm256_setzero_pd();
                ymm9 = _mm256_setzero_pd();

                for (k = 0; k < K; ++k)
                {
                    // The inner loop broadcasts the B matrix data and
                    // multiplies it with the A matrix.
                    ymm0 = _mm256_broadcast_sd(tB + tb_inc_col * 0);
                    ymm1 = _mm256_broadcast_sd(tB + tb_inc_col * 1);
                    ymm2 = _mm256_broadcast_sd(tB + tb_inc_col * 2);
                    tB += tb_inc_row;

                    //broadcasted matrix B elements are multiplied
                    //with matrix A columns.
                    ymm3 = _mm256_loadu_pd(tA);
                    ymm4 = _mm256_fmadd_pd(ymm0, ymm3, ymm4);
                    ymm6 = _mm256_fmadd_pd(ymm1, ymm3, ymm6);
                    ymm8 = _mm256_fmadd_pd(ymm2, ymm3, ymm8);

                    ymm3 = _mm256_loadu_pd(tA + 4);
                    ymm5 = _mm256_fmadd_pd(ymm0, ymm3, ymm5);
                    ymm7 = _mm256_fmadd_pd(ymm1, ymm3, ymm7);
                    ymm9 = _mm256_fmadd_pd(ymm2, ymm3, ymm9);

                    tA += lda;
                }
                // alpha, beta multiplication.
                ymm0 = _mm256_broadcast_sd(alpha_cast);
                ymm1 = _mm256_broadcast_sd(beta_cast);

                //multiply A*B by alpha.
                ymm4 = _mm256_mul_pd(ymm4, ymm0);
                ymm5 = _mm256_mul_pd(ymm5, ymm0);
                ymm6 = _mm256_mul_pd(ymm6, ymm0);
                ymm7 = _mm256_mul_pd(ymm7, ymm0);
                ymm8 = _mm256_mul_pd(ymm8, ymm0);
                ymm9 = _mm256_mul_pd(ymm9, ymm0);

                // multiply C by beta and accumulate.
                ymm2 = _mm256_loadu_pd(tC);
                ymm4 = _mm256_fmadd_pd(ymm2, ymm1, ymm4);
                ymm2 = _mm256_loadu_pd(tC + 4);
                ymm5 = _mm256_fmadd_pd(ymm2, ymm1, ymm5);
                _mm256_storeu_pd(tC, ymm4);
                _mm256_storeu_pd(tC + 4, ymm5);

                // multiply C by beta and accumulate.
                tC += ldc;
                ymm2 = _mm256_loadu_pd(tC);
                ymm6 = _mm256_fmadd_pd(ymm2, ymm1, ymm6);
                ymm2 = _mm256_loadu_pd(tC + 4);
                ymm7 = _mm256_fmadd_pd(ymm2, ymm1, ymm7);
                _mm256_storeu_pd(tC, ymm6);
                _mm256_storeu_pd(tC + 4, ymm7);

                // multiply C by beta and accumulate.
                tC += ldc;
                ymm2 = _mm256_loadu_pd(tC);
                ymm8 = _mm256_fmadd_pd(ymm2, ymm1, ymm8);
                ymm2 = _mm256_loadu_pd(tC + 4);
                ymm9 = _mm256_fmadd_pd(ymm2, ymm1, ymm9);
                _mm256_storeu_pd(tC, ymm8);
                _mm256_storeu_pd(tC + 4, ymm9);

            }
            n_remainder = N - col_idx;
            // if the N is not multiple of 3.
            // handling edge case.
            if (n_remainder == 2)
            {
                //pointer math to point to proper memory
                tC = C + ldc * col_idx + row_idx;
                tB = B + tb_inc_col * col_idx;
                tA = A + row_idx;

                // clear scratch registers.
                ymm4 = _mm256_setzero_pd();
                ymm5 = _mm256_setzero_pd();
                ymm6 = _mm256_setzero_pd();
                ymm7 = _mm256_setzero_pd();

                for (k = 0; k < K; ++k)
                {
                    // The inner loop broadcasts the B matrix data and
                    // multiplies it with the A matrix.
                    ymm0 = _mm256_broadcast_sd(tB + tb_inc_col * 0);
                    ymm1 = _mm256_broadcast_sd(tB + tb_inc_col * 1);
                    tB += tb_inc_row;

                    //broadcasted matrix B elements are multiplied
                    //with matrix A columns.
                    ymm3 = _mm256_loadu_pd(tA);
                    ymm4 = _mm256_fmadd_pd(ymm0, ymm3, ymm4);
                    ymm6 = _mm256_fmadd_pd(ymm1, ymm3, ymm6);

                    ymm3 = _mm256_loadu_pd(tA + 4);
                    ymm5 = _mm256_fmadd_pd(ymm0, ymm3, ymm5);
                    ymm7 = _mm256_fmadd_pd(ymm1, ymm3, ymm7);

                    tA += lda;
                }
                // alpha, beta multiplication.
                ymm0 = _mm256_broadcast_sd(alpha_cast);
                ymm1 = _mm256_broadcast_sd(beta_cast);

                //multiply A*B by alpha.
                ymm4 = _mm256_mul_pd(ymm4, ymm0);
                ymm5 = _mm256_mul_pd(ymm5, ymm0);
                ymm6 = _mm256_mul_pd(ymm6, ymm0);
                ymm7 = _mm256_mul_pd(ymm7, ymm0);

                // multiply C by beta and accumulate.
                ymm2 = _mm256_loadu_pd(tC);
                ymm4 = _mm256_fmadd_pd(ymm2, ymm1, ymm4);
                ymm2 = _mm256_loadu_pd(tC + 4);
                ymm5 = _mm256_fmadd_pd(ymm2, ymm1, ymm5);
                _mm256_storeu_pd(tC, ymm4);
                _mm256_storeu_pd(tC + 4, ymm5);

                // multiply C by beta and accumulate.
                tC += ldc;
                ymm2 = _mm256_loadu_pd(tC);
                ymm6 = _mm256_fmadd_pd(ymm2, ymm1, ymm6);
                ymm2 = _mm256_loadu_pd(tC + 4);
                ymm7 = _mm256_fmadd_pd(ymm2, ymm1, ymm7);
                _mm256_storeu_pd(tC, ymm6);
                _mm256_storeu_pd(tC + 4, ymm7);

                col_idx += 2;

            }
            // if the N is not multiple of 3.
            // handling edge case.
            if (n_remainder == 1)
            {
                //pointer math to point to proper memory
                tC = C + ldc * col_idx + row_idx;
                tB = B + tb_inc_col * col_idx;
                tA = A + row_idx;

                ymm4 = _mm256_setzero_pd();
                ymm5 = _mm256_setzero_pd();

                for (k = 0; k < K; ++k)
                {
                    // The inner loop broadcasts the B matrix data and
                    // multiplies it with the A matrix.
                    ymm0 = _mm256_broadcast_sd(tB + tb_inc_col * 0);
                    tB += tb_inc_row;

                    //broadcasted matrix B elements are multiplied
                    //with matrix A columns.
                    ymm3 = _mm256_loadu_pd(tA);
                    ymm4 = _mm256_fmadd_pd(ymm0, ymm3, ymm4);

                    ymm3 = _mm256_loadu_pd(tA + 4);
                    ymm5 = _mm256_fmadd_pd(ymm0, ymm3, ymm5);

                    tA += lda;
                }
                // alpha, beta multiplication.
                ymm0 = _mm256_broadcast_sd(alpha_cast);
                ymm1 = _mm256_broadcast_sd(beta_cast);

                ymm4 = _mm256_mul_pd(ymm4, ymm0);
                ymm5 = _mm256_mul_pd(ymm5, ymm0);

                // multiply C by beta and accumulate.
                ymm2 = _mm256_loadu_pd(tC);
                ymm4 = _mm256_fmadd_pd(ymm2, ymm1, ymm4);
                ymm2 = _mm256_loadu_pd(tC + 4);
                ymm5 = _mm256_fmadd_pd(ymm2, ymm1, ymm5);
                _mm256_storeu_pd(tC, ymm4);
                _mm256_storeu_pd(tC + 4, ymm5);

            }

            row_idx += 8;
        }

        if (m_remainder >= 4)
        {
            //printf("HERE\n");
            m_remainder -= 4;

            for (col_idx = 0; (col_idx + 2) < N; col_idx += 3)
            {
                //pointer math to point to proper memory
                tC = C + ldc * col_idx + row_idx;
                tB = B + tb_inc_col * col_idx;
                tA = A + row_idx;

                // clear scratch registers.
                ymm4 = _mm256_setzero_pd();
                ymm5 = _mm256_setzero_pd();
                ymm6 = _mm256_setzero_pd();

                for (k = 0; k < K; ++k)
                {
                    // The inner loop broadcasts the B matrix data and
                    // multiplies it with the A matrix.
                    ymm0 = _mm256_broadcast_sd(tB + tb_inc_col * 0);
                    ymm1 = _mm256_broadcast_sd(tB + tb_inc_col * 1);
                    ymm2 = _mm256_broadcast_sd(tB + tb_inc_col * 2);
                    tB += tb_inc_row;

                    //broadcasted matrix B elements are multiplied
                    //with matrix A columns.
                    ymm3 = _mm256_loadu_pd(tA);
                    ymm4 = _mm256_fmadd_pd(ymm0, ymm3, ymm4);
                    ymm5 = _mm256_fmadd_pd(ymm1, ymm3, ymm5);
                    ymm6 = _mm256_fmadd_pd(ymm2, ymm3, ymm6);

                    tA += lda;
                }
                // alpha, beta multiplication.
                ymm0 = _mm256_broadcast_sd(alpha_cast);
                ymm1 = _mm256_broadcast_sd(beta_cast);

                //multiply A*B by alpha.
                ymm4 = _mm256_mul_pd(ymm4, ymm0);
                ymm5 = _mm256_mul_pd(ymm5, ymm0);
                ymm6 = _mm256_mul_pd(ymm6, ymm0);

                // multiply C by beta and accumulate.
                ymm2 = _mm256_loadu_pd(tC);
                ymm4 = _mm256_fmadd_pd(ymm2, ymm1, ymm4);
                _mm256_storeu_pd(tC, ymm4);

                // multiply C by beta and accumulate.
                tC += ldc;
                ymm2 = _mm256_loadu_pd(tC);
                ymm5 = _mm256_fmadd_pd(ymm2, ymm1, ymm5);
                _mm256_storeu_pd(tC, ymm5);

                // multiply C by beta and accumulate.
                tC += ldc;
                ymm2 = _mm256_loadu_pd(tC);
                ymm6 = _mm256_fmadd_pd(ymm2, ymm1, ymm6);
                _mm256_storeu_pd(tC, ymm6);
            }
            n_remainder = N - col_idx;
            // if the N is not multiple of 3.
            // handling edge case.
            if (n_remainder == 2)
            {
                //pointer math to point to proper memory
                tC = C + ldc * col_idx + row_idx;
                tB = B + tb_inc_col * col_idx;
                tA = A + row_idx;

                ymm4 = _mm256_setzero_pd();
                ymm5 = _mm256_setzero_pd();

                for (k = 0; k < K; ++k)
                {
                    // The inner loop broadcasts the B matrix data and
                    // multiplies it with the A matrix.
                    ymm0 = _mm256_broadcast_sd(tB + tb_inc_col * 0);
                    ymm1 = _mm256_broadcast_sd(tB + tb_inc_col * 1);
                    tB += tb_inc_row;

                    //broadcasted matrix B elements are multiplied
                    //with matrix A columns.
                    ymm3 = _mm256_loadu_pd(tA);
                    ymm4 = _mm256_fmadd_pd(ymm0, ymm3, ymm4);
                    ymm5 = _mm256_fmadd_pd(ymm1, ymm3, ymm5);

                    tA += lda;
                }
                // alpha, beta multiplication.
                ymm0 = _mm256_broadcast_sd(alpha_cast);
                ymm1 = _mm256_broadcast_sd(beta_cast);

                //multiply A*B by alpha.
                ymm4 = _mm256_mul_pd(ymm4, ymm0);
                ymm5 = _mm256_mul_pd(ymm5, ymm0);

                // multiply C by beta and accumulate.
                ymm2 = _mm256_loadu_pd(tC);
                ymm4 = _mm256_fmadd_pd(ymm2, ymm1, ymm4);
                _mm256_storeu_pd(tC, ymm4);

                // multiply C by beta and accumulate.
                tC += ldc;
                ymm2 = _mm256_loadu_pd(tC);
                ymm5 = _mm256_fmadd_pd(ymm2, ymm1, ymm5);
                _mm256_storeu_pd(tC, ymm5);

                col_idx += 2;

            }
            // if the N is not multiple of 3.
            // handling edge case.
            if (n_remainder == 1)
            {
                //pointer math to point to proper memory
                tC = C + ldc * col_idx + row_idx;
                tB = B + tb_inc_col * col_idx;
                tA = A + row_idx;

                ymm4 = _mm256_setzero_pd();

                for (k = 0; k < K; ++k)
                {
                    // The inner loop broadcasts the B matrix data and
                    // multiplies it with the A matrix.
                    ymm0 = _mm256_broadcast_sd(tB + tb_inc_col * 0);
                    tB += tb_inc_row;

                    //broadcasted matrix B elements are multiplied
                    //with matrix A columns.
                    ymm3 = _mm256_loadu_pd(tA);
                    ymm4 = _mm256_fmadd_pd(ymm0, ymm3, ymm4);

                    tA += lda;
                }
                // alpha, beta multiplication.
                ymm0 = _mm256_broadcast_sd(alpha_cast);
                ymm1 = _mm256_broadcast_sd(beta_cast);

                ymm4 = _mm256_mul_pd(ymm4, ymm0);

                // multiply C by beta and accumulate.
                ymm2 = _mm256_loadu_pd(tC);
                ymm4 = _mm256_fmadd_pd(ymm2, ymm1, ymm4);
                _mm256_storeu_pd(tC, ymm4);

            }

            row_idx += 4;
        }
        // M is not a multiple of 32.
        // The handling of edge case where the remainder
        // dimension is less than 8. The padding takes place
        // to handle this case.
        if ((m_remainder) && (lda > 3))
        {
            double f_temp[8];

            for (col_idx = 0; (col_idx + 2) < N; col_idx += 3)
            {
                //pointer math to point to proper memory
                tC = C + ldc * col_idx + row_idx;
                tB = B + tb_inc_col * col_idx;
                tA = A + row_idx;

                // clear scratch registers.
                ymm5 = _mm256_setzero_pd();
                ymm7 = _mm256_setzero_pd();
                ymm9 = _mm256_setzero_pd();

                for (k = 0; k < (K - 1); ++k)
                {
                    // The inner loop broadcasts the B matrix data and
                    // multiplies it with the A matrix.
                    ymm0 = _mm256_broadcast_sd(tB + tb_inc_col * 0);
                    ymm1 = _mm256_broadcast_sd(tB + tb_inc_col * 1);
                    ymm2 = _mm256_broadcast_sd(tB + tb_inc_col * 2);
                    tB += tb_inc_row;

                    //broadcasted matrix B elements are multiplied
                    //with matrix A columns.
                    ymm3 = _mm256_loadu_pd(tA);
                    ymm5 = _mm256_fmadd_pd(ymm0, ymm3, ymm5);
                    ymm7 = _mm256_fmadd_pd(ymm1, ymm3, ymm7);
                    ymm9 = _mm256_fmadd_pd(ymm2, ymm3, ymm9);

                    tA += lda;
                }
                // alpha, beta multiplication.
                ymm0 = _mm256_broadcast_sd(tB + tb_inc_col * 0);
                ymm1 = _mm256_broadcast_sd(tB + tb_inc_col * 1);
                ymm2 = _mm256_broadcast_sd(tB + tb_inc_col * 2);
                tB += tb_inc_row;

                for (int i = 0; i < m_remainder; i++)
                {
                    f_temp[i] = tA[i];
                }
                ymm3 = _mm256_loadu_pd(f_temp);
                ymm5 = _mm256_fmadd_pd(ymm0, ymm3, ymm5);
                ymm7 = _mm256_fmadd_pd(ymm1, ymm3, ymm7);
                ymm9 = _mm256_fmadd_pd(ymm2, ymm3, ymm9);

                ymm0 = _mm256_broadcast_sd(alpha_cast);
                ymm1 = _mm256_broadcast_sd(beta_cast);

                //multiply A*B by alpha.
                ymm5 = _mm256_mul_pd(ymm5, ymm0);
                ymm7 = _mm256_mul_pd(ymm7, ymm0);
                ymm9 = _mm256_mul_pd(ymm9, ymm0);


                for (int i = 0; i < m_remainder; i++)
                {
                    f_temp[i] = tC[i];
                }
                ymm2 = _mm256_loadu_pd(f_temp);
                ymm5 = _mm256_fmadd_pd(ymm2, ymm1, ymm5);
                _mm256_storeu_pd(f_temp, ymm5);
                for (int i = 0; i < m_remainder; i++)
                {
                    tC[i] = f_temp[i];
                }

                tC += ldc;
                for (int i = 0; i < m_remainder; i++)
                {
                    f_temp[i] = tC[i];
                }
                ymm2 = _mm256_loadu_pd(f_temp);
                ymm7 = _mm256_fmadd_pd(ymm2, ymm1, ymm7);
                _mm256_storeu_pd(f_temp, ymm7);
                for (int i = 0; i < m_remainder; i++)
                {
                    tC[i] = f_temp[i];
                }

                tC += ldc;
                for (int i = 0; i < m_remainder; i++)
                {
                    f_temp[i] = tC[i];
                }
                ymm2 = _mm256_loadu_pd(f_temp);
                ymm9 = _mm256_fmadd_pd(ymm2, ymm1, ymm9);
                _mm256_storeu_pd(f_temp, ymm9);
                for (int i = 0; i < m_remainder; i++)
                {
                    tC[i] = f_temp[i];
                }
            }
            n_remainder = N - col_idx;
            // if the N is not multiple of 3.
            // handling edge case.
            if (n_remainder == 2)
            {
                //pointer math to point to proper memory
                tC = C + ldc * col_idx + row_idx;
                tB = B + tb_inc_col * col_idx;
                tA = A + row_idx;

                ymm5 = _mm256_setzero_pd();
                ymm7 = _mm256_setzero_pd();

                for (k = 0; k < (K - 1); ++k)
                {
                    // The inner loop broadcasts the B matrix data and
                    // multiplies it with the A matrix.
                    ymm0 = _mm256_broadcast_sd(tB + tb_inc_col * 0);
                    ymm1 = _mm256_broadcast_sd(tB + tb_inc_col * 1);
                    tB += tb_inc_row;

                    ymm3 = _mm256_loadu_pd(tA);
                    ymm5 = _mm256_fmadd_pd(ymm0, ymm3, ymm5);
                    ymm7 = _mm256_fmadd_pd(ymm1, ymm3, ymm7);

                    tA += lda;
                }

                ymm0 = _mm256_broadcast_sd(tB + tb_inc_col * 0);
                ymm1 = _mm256_broadcast_sd(tB + tb_inc_col * 1);
                tB += tb_inc_row;

                for (int i = 0; i < m_remainder; i++)
                {
                    f_temp[i] = tA[i];
                }
                ymm3 = _mm256_loadu_pd(f_temp);
                ymm5 = _mm256_fmadd_pd(ymm0, ymm3, ymm5);
                ymm7 = _mm256_fmadd_pd(ymm1, ymm3, ymm7);

                ymm0 = _mm256_broadcast_sd(alpha_cast);
                ymm1 = _mm256_broadcast_sd(beta_cast);

                ymm5 = _mm256_mul_pd(ymm5, ymm0);
                ymm7 = _mm256_mul_pd(ymm7, ymm0);

                for (int i = 0; i < m_remainder; i++)
                {
                    f_temp[i] = tC[i];
                }
                ymm2 = _mm256_loadu_pd(f_temp);
                ymm5 = _mm256_fmadd_pd(ymm2, ymm1, ymm5);
                _mm256_storeu_pd(f_temp, ymm5);
                for (int i = 0; i < m_remainder; i++)
                {
                    tC[i] = f_temp[i];
                }

                tC += ldc;
                for (int i = 0; i < m_remainder; i++)
                {
                    f_temp[i] = tC[i];
                }
                ymm2 = _mm256_loadu_pd(f_temp);
                ymm7 = _mm256_fmadd_pd(ymm2, ymm1, ymm7);
                _mm256_storeu_pd(f_temp, ymm7);
                for (int i = 0; i < m_remainder; i++)
                {
                    tC[i] = f_temp[i];
                }
            }
            // if the N is not multiple of 3.
            // handling edge case.
            if (n_remainder == 1)
            {
                //pointer math to point to proper memory
                tC = C + ldc * col_idx + row_idx;
                tB = B + tb_inc_col * col_idx;
                tA = A + row_idx;

                ymm5 = _mm256_setzero_pd();

                for (k = 0; k < (K - 1); ++k)
                {
                    // The inner loop broadcasts the B matrix data and
                    // multiplies it with the A matrix.
                    ymm0 = _mm256_broadcast_sd(tB + tb_inc_col * 0);
                    tB += tb_inc_row;

                    ymm3 = _mm256_loadu_pd(tA);
                    ymm5 = _mm256_fmadd_pd(ymm0, ymm3, ymm5);

                    tA += lda;
                }

                ymm0 = _mm256_broadcast_sd(tB + tb_inc_col * 0);
                tB += tb_inc_row;

                for (int i = 0; i < m_remainder; i++)
                {
                    f_temp[i] = tA[i];
                }
                ymm3 = _mm256_loadu_pd(f_temp);
                ymm5 = _mm256_fmadd_pd(ymm0, ymm3, ymm5);

                ymm0 = _mm256_broadcast_sd(alpha_cast);
                ymm1 = _mm256_broadcast_sd(beta_cast);

                // multiply C by beta and accumulate.
                ymm5 = _mm256_mul_pd(ymm5, ymm0);

                for (int i = 0; i < m_remainder; i++)
                {
                    f_temp[i] = tC[i];
                }
                ymm2 = _mm256_loadu_pd(f_temp);
                ymm5 = _mm256_fmadd_pd(ymm2, ymm1, ymm5);
                _mm256_storeu_pd(f_temp, ymm5);
                for (int i = 0; i < m_remainder; i++)
                {
                    tC[i] = f_temp[i];
                }
            }
            m_remainder = 0;
        }

        if (m_remainder)
        {
            double result;
            for (; row_idx < M; row_idx += 1)
            {
                for (col_idx = 0; col_idx < N; col_idx += 1)
                {
                    //pointer math to point to proper memory
                    tC = C + ldc * col_idx + row_idx;
                    tB = B + tb_inc_col * col_idx;
                    tA = A + row_idx;

                    result = 0;
                    for (k = 0; k < K; ++k)
                    {
                        result += (*tA) * (*tB);
                        tA += lda;
                        tB += tb_inc_row;
                    }

                    result *= (*alpha_cast);
                    (*tC) = (*tC) * (*beta_cast) + result;
                }
            }
        }
        return BLIS_SUCCESS;
    }
    else
        return BLIS_NONCONFORMAL_DIMENSIONS;


};
