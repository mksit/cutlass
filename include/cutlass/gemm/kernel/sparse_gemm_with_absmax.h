/***************************************************************************************************
 * Copyright (c) 2024 - 2025 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are met:
 *
 * 1. Redistributions of source code must retain the above copyright notice, this
 * list of conditions and the following disclaimer.
 *
 * 2. Redistributions in binary form must reproduce the above copyright notice,
 * this list of conditions and the following disclaimer in the documentation
 * and/or other materials provided with the distribution.
 *
 * 3. Neither the name of the copyright holder nor the names of its
 * contributors may be used to endorse or promote products derived from
 * this software without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
 * AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE
 * DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE LIABLE
 * FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
 * DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR
 * SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER
 * CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY,
 * OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
 * OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 *
 **************************************************************************************************/
/*! \file
    \brief Sparse GEMM kernel with an epilogue that computes the absolute maximum value of the output
    and a pre-activation-function auxiliary output. The auxiliary output is also (optionally)
    stored to global memory.
*/

#pragma once

#include "cutlass/cutlass.h"

#include "cutlass/gemm/gemm.h"
#include "cutlass/gemm/kernel/params_sparse_base.h"
#include "cutlass/matrix_coord.h"
#include "cutlass/semaphore.h"

/////////////////////////////////////////////////////////////////////////////////////////////////

namespace cutlass {
namespace gemm {
namespace kernel {

/////////////////////////////////////////////////////////////////////////////////////////////////

template <
  typename Mma_,                  ///! Threadblock-scoped matrix multiply-accumulate 
  typename Epilogue_,             ///! Epilogue
  typename ThreadblockSwizzle_,   ///! Threadblock swizzling function
  bool SplitKSerial               ///! If true, code supporting split-K via serial reduction is enabled.
>
struct SparseGemmWithAbsmax {

  using Mma = Mma_;
  using Epilogue = Epilogue_;
  using OutputOp = typename Epilogue::OutputOp;
  using ThreadblockSwizzle = ThreadblockSwizzle_;
  static bool const kSplitKSerial = SplitKSerial;

  static int const kSparse = Mma::kSparse;
  static int const kMetaSizeInBits = Mma::kMetaSizeInBits;
  static int const kMaxID2 = Mma::kMaxID2;
  static int const kElementsPerElementE = Mma::kElementsPerElementE;

  using ElementE = typename Mma::ElementE;
  using LayoutE = typename Mma::LayoutE;

  using LayoutC = typename Epilogue::OutputTileIterator::Layout;

  /// Warp count (concept: GemmShape)
  using WarpCount = typename Mma::WarpCount;
  static int const kThreadCount = 32 * WarpCount::kCount;

  using ParamsA = typename Mma::IteratorA::Params;
  using TensorRefA = typename Mma::IteratorA::TensorRef;
  using ParamsB = typename Mma::IteratorB::Params;
  using TensorRefB = typename Mma::IteratorB::TensorRef;
  using ParamsE = typename Mma::IteratorE::Params;
  using TensorRefE = typename Mma::IteratorE::TensorRef;

  using ParamsC = typename Epilogue::OutputTileIterator::Params;
  using TensorRefC = typename Epilogue::OutputTileIterator::TensorRef;
  using ParamsD = typename Epilogue::OutputTileIterator::Params;
  using TensorRefD = typename Epilogue::OutputTileIterator::TensorRef;
  using ParamsAux = typename Epilogue::AuxOutputTileIterator::Params;
  using TensorRefAux = typename Epilogue::AuxOutputTileIterator::TensorRef;

  /// Argument structure
  struct Arguments {

    //
    // Data members
    //

    GemmCoord problem_size;
    TensorRefA ref_A;
    TensorRefB ref_B;
    TensorRefC ref_C;
    TensorRefD ref_D;
    TensorRefE ref_E;
    TensorRefAux ref_Aux;
    void* ptr_Vector;
    typename LayoutC::Stride::Index ldr;

    typename Epilogue::OutputOp::Params epilogue;
    int split_k_slices;

    //
    // Methods
    //

    /// Default ctor
    CUTLASS_HOST_DEVICE
    Arguments(): problem_size(0, 0, 0), split_k_slices(1) {

    }

    /// Constructs an Arguments structure 
    CUTLASS_HOST_DEVICE
    Arguments(
      GemmCoord problem_size_,
      TensorRefA ref_A_,
      TensorRefB ref_B_,
      TensorRefC ref_C_,
      TensorRefD ref_D_,
      TensorRefE ref_E_,
      TensorRefAux ref_Aux_,
      void* ptr_Vector_,
      typename LayoutC::Stride::Index ldr_,
      typename OutputOp::Params epilogue_ = 
        typename OutputOp::Params(),
      int split_k_slices = 1
    ):
      problem_size(problem_size_),
      ref_A(ref_A_),
      ref_B(ref_B_),
      ref_C(ref_C_),
      ref_D(ref_D_),
      ref_E(ref_E_),
      ref_Aux(ref_Aux_),
      ptr_Vector(ptr_Vector_),
      ldr(ldr_),
      epilogue(epilogue_),
      split_k_slices(split_k_slices) {

    }
  };

  /// Parameters structure
  struct Params : public SparseParamsBase<
      ThreadblockSwizzle, ParamsA, TensorRefA, ParamsB, TensorRefB,
      ParamsE, TensorRefE> {

    using Base = SparseParamsBase<
        ThreadblockSwizzle, ParamsA, TensorRefA, ParamsB, TensorRefB,
        ParamsE, TensorRefE>;

    //
    // Data members
    //

    ParamsC params_C;
    TensorRefC ref_C;
    ParamsD params_D;
    TensorRefD ref_D;
    ParamsAux params_Aux;
    TensorRefAux ref_Aux;

    void* ptr_Vector;
    typename LayoutC::Stride::Index ldr;

    typename OutputOp::Params output_op;
    int *semaphore;

    //
    // Methods
    //

    CUTLASS_HOST_DEVICE
    Params() { }

    CUTLASS_HOST_DEVICE
    Params(
      cutlass::gemm::GemmCoord const & problem_size,
      cutlass::gemm::GemmCoord const & grid_tiled_shape,
      TensorRefA ref_A,
      TensorRefB ref_B,
      TensorRefC ref_C,
      TensorRefD ref_D,
      TensorRefE ref_E,
      TensorRefAux ref_Aux,
      void* ptr_Vector,
      typename LayoutC::Stride::Index ldr,
      typename OutputOp::Params output_op = typename OutputOp::Params(),
      int *workspace = nullptr
    ):
      Base(problem_size, grid_tiled_shape, ref_A, ref_B, ref_E, Mma::Shape::kK),
      params_C(ref_C.layout()),
      ref_C(ref_C),
      params_D(ref_D.layout()),
      ref_D(ref_D),
      output_op(output_op),
      ref_Aux(ref_Aux),
      params_Aux(ref_Aux.layout()),
      ptr_Vector(ptr_Vector),
      ldr(ldr) {
    semaphore = workspace;
    }
  };

  /// Shared memory storage structure
  union SharedStorage {
    typename Mma::SharedStorage main_loop;
    typename Epilogue::SharedStorage epilogue;
  };

  //
  // Methods
  //

  CUTLASS_HOST_DEVICE
  SparseGemmWithAbsmax() { } 

  /// Determines whether kernel satisfies alignment
  static Status can_implement(
      cutlass::gemm::GemmCoord const & problem_size,
      typename Mma::IteratorA::TensorRef ref_A,
      typename Mma::IteratorB::TensorRef ref_B,
      typename Epilogue::OutputTileIterator::TensorRef ref_C,
      typename Epilogue::OutputTileIterator::TensorRef ref_D,
      typename Mma::IteratorE::TensorRef ref_E) {

    static int const kAlignmentA = Mma::IteratorA::AccessType::kElements;
    static int const kAlignmentB = Mma::IteratorB::AccessType::kElements;
    static int const kAlignmentC = Epilogue::OutputTileIterator::kElementsPerAccess;
    static int const kAlignmentE = Mma::IteratorE::AccessType::kElements;

    if (!TensorRef_aligned(ref_A, kAlignmentA)) {
      return Status::kErrorMisalignedOperand;
    }

    if (!TensorRef_aligned(ref_B, kAlignmentB)) {
      return Status::kErrorMisalignedOperand;
    }

    if (!TensorRef_aligned(ref_C, kAlignmentC)) {
      return Status::kErrorMisalignedOperand;
    }

    if (!TensorRef_aligned(ref_D, kAlignmentC)) {
      return Status::kErrorMisalignedOperand;
    }

    if (!TensorRef_aligned(ref_E, kAlignmentE)) {
      return Status::kErrorMisalignedOperand;
    }

    if ((problem_size.m() % kAlignmentA) || ((problem_size.k() / kSparse) % kAlignmentA) ||
      (problem_size.n() % kAlignmentB) || (problem_size.k() % kAlignmentB) ||
      (problem_size.m() % kAlignmentC) || (problem_size.n() % kAlignmentC) ||
      (problem_size.m() % kAlignmentE) || ((problem_size.k() / kSparse) % kAlignmentE)) {

      return Status::kErrorMisalignedOperand;
    }

    // The k dimension has to be the multiple of the Threadblock k because out
    // of bound meta data would be initialized to 0 by acync.zfill but 0 is not
    // a valid meta data.
    if (problem_size.k() % Mma::Shape::kK) {
      return Status::kErrorMisalignedOperand;
    }

    // M dimension has to be multiple of 32 (sparse float) or 16 (sparse int) 
    // because of the row reordering of operand E
    static int const kAlignmentM = (sizeof(ElementE) == 2) ? 32 : 16;

    if (problem_size.m() % kAlignmentM) {
      return Status::kErrorMisalignedOperand;
    }

    return Status::kSuccess;
  }

  /// Executes one GEMM
  CUTLASS_DEVICE
  void operator()(Params const &params, SharedStorage &shared_storage) {

    // Compute threadblock location
    ThreadblockSwizzle threadblock_swizzle;

    cutlass::gemm::GemmCoord threadblock_tile_offset =
        threadblock_swizzle.get_tile_offset(params.swizzle_log_tile);

    // Early exit if CTA is out of range
    if (params.grid_tiled_shape.m() <= threadblock_tile_offset.m() ||
      params.grid_tiled_shape.n() <= threadblock_tile_offset.n()) {

      return;
    }

    // Compute initial location in logical coordinates
    cutlass::MatrixCoord tb_offset_A{
      threadblock_tile_offset.m() * Mma::Shape::kM,
      threadblock_tile_offset.k() * params.gemm_k_size / kSparse,
    };

    cutlass::MatrixCoord tb_offset_B{
      threadblock_tile_offset.k() * params.gemm_k_size,
      threadblock_tile_offset.n() * Mma::Shape::kN
    };

    cutlass::MatrixCoord tb_offset_E{
      threadblock_tile_offset.m() * Mma::Shape::kM,
      threadblock_tile_offset.k() * params.gemm_k_size / kSparse,
    };

    // Problem size is a function of threadblock index in the K dimension
    int problem_size_k = min(
      params.problem_size.k(), 
      (threadblock_tile_offset.k() + 1) * params.gemm_k_size);

    // Compute threadblock-scoped matrix multiply-add
    int gemm_k_iterations = (problem_size_k - tb_offset_B.row() + Mma::Shape::kK - 1) / Mma::Shape::kK;

    // Compute position within threadblock
    int thread_idx = threadIdx.x;

    // Construct iterators to A, B, and E operands
    typename Mma::IteratorA iterator_A(
      params.params_A,
      params.ref_A.data(),
      {params.problem_size.m(), problem_size_k / kSparse},
      thread_idx,
      tb_offset_A);

    typename Mma::IteratorB iterator_B(
      params.params_B,
      params.ref_B.data(),
      {problem_size_k, params.problem_size.n()},
      thread_idx,
      tb_offset_B);

    typename Mma::IteratorE iterator_E(
        params.params_E, params.ref_E.data(),
        {params.problem_size.m(),
         problem_size_k / kSparse / kElementsPerElementE},
        thread_idx, tb_offset_E);

    // Broadcast the warp_id computed by lane 0 to ensure dependent code
    // is compiled as warp-uniform.
    int warp_idx = canonical_warp_idx_sync();
    int lane_idx = threadIdx.x % 32;

    //
    // Main loop
    //

    // Construct thread-scoped matrix multiply
    Mma mma(shared_storage.main_loop, thread_idx, warp_idx, lane_idx);

    typename Mma::FragmentC accumulators;

    accumulators.clear();

    if (!kSplitKSerial || gemm_k_iterations > 0) {
      // Compute threadblock-scoped matrix multiply-add
      mma(gemm_k_iterations, accumulators, iterator_A, iterator_B, iterator_E, accumulators);
    }

    //
    // Epilogue
    //

    OutputOp output_op(params.output_op);

    //
    // Masked tile iterators constructed from members
    //

    threadblock_tile_offset =
        threadblock_swizzle.get_tile_offset(params.swizzle_log_tile);

    //assume identity swizzle
    MatrixCoord threadblock_offset(
      threadblock_tile_offset.m() * Mma::Shape::kM,
      threadblock_tile_offset.n() * Mma::Shape::kN
    );

    int block_idx = threadblock_tile_offset.m() + threadblock_tile_offset.n() * params.grid_tiled_shape.m();

    // Construct the semaphore.
    Semaphore semaphore(params.semaphore + block_idx, thread_idx);

    // If performing a reduction via split-K, fetch the initial synchronization
    if (kSplitKSerial && params.grid_tiled_shape.k() > 1) {
      
      // Fetch the synchronization lock initially but do not block.
      semaphore.fetch();

      // Indicate which position in a serial reduction the output operator is currently updating
      output_op.set_k_partition(threadblock_tile_offset.k(), params.grid_tiled_shape.k());
    }

    typename Epilogue::ElementVector *ptr_Vector = static_cast<typename Epilogue::ElementVector *>(params.ptr_Vector);
    // Move to appropriate location for this output tile
    if (ptr_Vector) {
      ptr_Vector += threadblock_offset.column() + threadblock_tile_offset.m() * params.ldr;
    }

    // Tile iterator loading from source tensor.
    typename Epilogue::OutputTileIterator iterator_C(
      params.params_C,
      params.ref_C.data(),
      params.problem_size.mn(),
      thread_idx,
      threadblock_offset
    );

    // Tile iterator writing to destination tensor.
    typename Epilogue::OutputTileIterator iterator_D(
      params.params_D,
      params.ref_D.data(),
      params.problem_size.mn(),
      thread_idx,
      threadblock_offset
    );

    // Tile iterator writing to auxiliary destination tensor.
    typename Epilogue::AuxOutputTileIterator iterator_Aux(
      params.params_Aux,
      // Only the final block writes the auxiliary tensor
      ((kSplitKSerial && params.grid_tiled_shape.k() > 1) &&
          (params.grid_tiled_shape.k() != threadblock_tile_offset.k() + 1))
          ? nullptr
          : params.ref_Aux.data(),
      params.problem_size.mn(),
      thread_idx,
      threadblock_offset
    );

    Epilogue epilogue(
      shared_storage.epilogue, 
      thread_idx, 
      warp_idx, 
      lane_idx);

    // Wait on the semaphore - this latency may have been covered by iterator construction
    if (kSplitKSerial && params.grid_tiled_shape.k() > 1) {
        
      // For subsequent threadblocks, the source matrix is held in the 'D' tensor.
      if (threadblock_tile_offset.k()) {
        iterator_C = iterator_D;
      }

      semaphore.wait(threadblock_tile_offset.k());

      __threadfence();
    }

    // Execute the epilogue operator to update the destination tensor.
    epilogue(output_op,
             // Only the final block uses Vector
             ((kSplitKSerial && params.grid_tiled_shape.k() > 1) &&
              (params.grid_tiled_shape.k() != threadblock_tile_offset.k() + 1))
                 ? nullptr
                 : ptr_Vector,
             iterator_D,
             accumulators,
             iterator_C,
             iterator_Aux,
             params.problem_size.mn(),
             threadblock_offset);
    
    //
    // Release the semaphore
    //

    if (kSplitKSerial && params.grid_tiled_shape.k() > 1) {
      
      int lock = 0;
      if (params.grid_tiled_shape.k() == threadblock_tile_offset.k() + 1) {

        // The final threadblock resets the semaphore for subsequent grids.
        lock = 0;
      }
      else {
        // Otherwise, the semaphore is incremented
        lock = threadblock_tile_offset.k() + 1;
      }

      __threadfence();
      semaphore.release(lock);
    }
  }
};

/////////////////////////////////////////////////////////////////////////////////////////////////

} // namespace kernel
} // namespace gemm
} // namespace cutlass
