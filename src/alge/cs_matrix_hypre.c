/*============================================================================
 * Sparse Matrix Representation and Operations using HYPRE library.
 *============================================================================*/

/*
  This file is part of Code_Saturne, a general-purpose CFD tool.

  Copyright (C) 1998-2021 EDF S.A.

  This program is free software; you can redistribute it and/or modify it under
  the terms of the GNU General Public License as published by the Free Software
  Foundation; either version 2 of the License, or (at your option) any later
  version.

  This program is distributed in the hope that it will be useful, but WITHOUT
  ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS
  FOR A PARTICULAR PURPOSE.  See the GNU General Public License for more
  details.

  You should have received a copy of the GNU General Public License along with
  this program; if not, write to the Free Software Foundation, Inc., 51 Franklin
  Street, Fifth Floor, Boston, MA 02110-1301, USA.
*/

/*----------------------------------------------------------------------------*/

#include "cs_defs.h"

/*----------------------------------------------------------------------------
 * Standard C library headers
 *----------------------------------------------------------------------------*/

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>
#include <math.h>

#if defined(HAVE_MPI)
#include <mpi.h>
#endif

/*----------------------------------------------------------------------------
 * HYPRE headers
 *----------------------------------------------------------------------------*/

#include <HYPRE.h>
#include <HYPRE_IJ_mv.h>
#include <HYPRE_parcsr_mv.h>
#include <HYPRE_utilities.h>

/*----------------------------------------------------------------------------
 * Local headers
 *----------------------------------------------------------------------------*/

#include "bft_mem.h"
#include "bft_error.h"
#include "bft_printf.h"

#include "cs_base.h"
#include "cs_halo.h"
#include "cs_log.h"
#include "cs_numbering.h"
#include "cs_timer.h"

/*----------------------------------------------------------------------------
 *  Header for the current file
 *----------------------------------------------------------------------------*/

#include "cs_matrix.h"
#include "cs_matrix_default.h"
#include "cs_matrix_hypre.h"
#include "cs_matrix_hypre_priv.h"
#include "cs_matrix_priv.h"

/*----------------------------------------------------------------------------*/

BEGIN_C_DECLS

/*----------------------------------------------------------------------------*/
/*! \file cs_matrix_hypre.c
 *
 * \brief Sparse Matrix Representation and Operations using HYPRE.
 */
/*----------------------------------------------------------------------------*/

/*! \cond DOXYGEN_SHOULD_SKIP_THIS */

/*=============================================================================
 * Local Macro Definitions
 *============================================================================*/

/* Fixed coefficient buffer size for accumulation
   (a reasonably small fixed size has the advantage of being easily usable
   on the stack and in a threading context, and that size should still
   be large enough to amortize calls to lower-level functions */

#undef  COEFF_GROUP_SIZE
#define COEFF_GROUP_SIZE 512

/*=============================================================================
 * Local Type Definitions
 *============================================================================*/

/*============================================================================
 *  Global variables
 *============================================================================*/

static const char _hypre_ij_type_name[] = "HYPRE_PARCSR";
static const char _hypre_ij_type_fullname[] = "HYPRE IJ (HYPRE_ParCSR)";

/*============================================================================
 * Private function definitions
 *============================================================================*/

/*----------------------------------------------------------------------------
 * Matrix.vector product y = A.x with HYPRE matrix
 *
 * Note that since this function creates vectors, it may have significant
 * overhead. Since it is present for completednes, this should not be an issue.
 * If used more often, vectors could be maintained in the coeffs structure
 * along with the matrix.
 *
 * parameters:
 *   matrix       <-- pointer to matrix structure
 *   exclude_diag <-- exclude diagonal if true
 *   sync         <-- synchronize ghost cells if true
 *   x            <-> multipliying vector values
 *   y            --> resulting vector
 *----------------------------------------------------------------------------*/

static void
_mat_vec_p_parcsr(const cs_matrix_t  *matrix,
                  bool                exclude_diag,
                  bool                sync,
                  cs_real_t          *restrict x,
                  cs_real_t          *restrict y)
{
  assert(exclude_diag == false);

  const cs_lnum_t  n_rows = matrix->n_rows * matrix->db_size[0];
  const cs_matrix_coeffs_hypre_t  *coeffs = matrix->coeffs;

  /* Get pointers to structures through coefficients,
     and copy input values */

  HYPRE_ParCSRMatrix p_a;
  HYPRE_IJMatrixGetObject(coeffs->hm, (void **)&p_a);

  HYPRE_Real *_t = NULL;

  if (sizeof(cs_real_t) == sizeof(HYPRE_Real)) {
    HYPRE_IJVectorSetValues(coeffs->hx, n_rows, NULL, x);
  }
  else {
    BFT_MALLOC(_t, n_rows, HYPRE_Real);
    for (HYPRE_BigInt ii = 0; ii < n_rows; ii++) {
      _t[ii] = x[ii];;
    }
    HYPRE_IJVectorSetValues(coeffs->hx, n_rows, NULL, _t);
  }
  if (sync)
    HYPRE_IJVectorAssemble(coeffs->hx);

  HYPRE_ParVector p_x, p_y;
  HYPRE_IJVectorGetObject(coeffs->hx, (void **)&p_x);
  HYPRE_IJVectorGetObject(coeffs->hy, (void **)&p_y);

  /* SpMv operation */

  HYPRE_ParCSRMatrixMatvec(1.0, p_a, p_x, 0, p_y);

  /* Copy data back */

  if (sizeof(cs_real_t) == sizeof(HYPRE_Real)) {
    HYPRE_IJVectorGetValues(coeffs->hy, n_rows, NULL, y);
  }
  else {
    HYPRE_IJVectorGetValues(coeffs->hy, n_rows, NULL, _t);
    for (HYPRE_BigInt ii = 0; ii < n_rows; ii++) {
      y[ii] = _t[ii];
    }
    BFT_FREE(_t);
  }
}

/*----------------------------------------------------------------------------
 * Compute local and distant counts of matrix entries using an assembler
 *
 * The caller is responsible for freeing the returned diag_sizes and
 * offdiag_sizes arrays.
 *
 * parameters:
 *   ma            <-- pointer to matrix assembler structure
 *   diag_sizes    --> diagonal values
 *   offdiag_sizes --> off-diagonal values
 *----------------------------------------------------------------------------*/

static void
_compute_diag_sizes_assembler(const cs_matrix_assembler_t   *ma,
                              HYPRE_Int                    **diag_sizes,
                              HYPRE_Int                    **offdiag_sizes)
{
  const cs_lnum_t n_rows = cs_matrix_assembler_get_n_rows(ma);

  cs_lnum_t n_diag_add = (cs_matrix_assembler_get_separate_diag(ma)) ? 1 : 0;

  const cs_lnum_t *row_index = cs_matrix_assembler_get_row_index(ma);
  const cs_lnum_t *col_ids = cs_matrix_assembler_get_col_ids(ma);

  HYPRE_Int *_diag_sizes, *_offdiag_sizes;
  BFT_MALLOC(_diag_sizes, n_rows, HYPRE_Int);
  BFT_MALLOC(_offdiag_sizes, n_rows, HYPRE_Int);

  /* Separate local and distant loops for better first touch logic */

# pragma omp parallel for  if(n_rows > CS_THR_MIN)
  for (cs_lnum_t i = 0; i < n_rows; i++) {
    cs_lnum_t s_id = row_index[i];
    cs_lnum_t e_id = row_index[i+1];

    HYPRE_Int n_r_diag = 0;
    HYPRE_Int n_cols = e_id - s_id;

    for (cs_lnum_t j = s_id; j < e_id; j++) {
      if (col_ids[j] < n_rows)
        n_r_diag++;
      else
        break;
    }

    _diag_sizes[i] = n_r_diag + n_diag_add;
    _offdiag_sizes[i] = n_cols - n_r_diag;
  }

  *diag_sizes = _diag_sizes;
  *offdiag_sizes = _offdiag_sizes;
}

/*----------------------------------------------------------------------------
 * Compute local and distant counts of matrix entries using an assembler
 * with full diagonal blocks and A.I extradiangonal blocks fill.
 *
 * The caller is responsible for freeing the returned diag_sizes and
 * offdiag_sizes arrays.
 *
 * parameters:
 *   ma            <-- pointer to matrix assembler structure
 *   db_size       <-- diagonal block size
 *   diag_sizes    --> diagonal values
 *   offdiag_sizes --> off-diagonal values
 *----------------------------------------------------------------------------*/

static void
_compute_diag_sizes_assembler_db(const cs_matrix_assembler_t   *ma,
                                 cs_lnum_t                     db_size,
                                 HYPRE_Int                    **diag_sizes,
                                 HYPRE_Int                    **offdiag_sizes)
{
  const cs_lnum_t n_rows = cs_matrix_assembler_get_n_rows(ma);

  cs_lnum_t n_diag_add
    = (cs_matrix_assembler_get_separate_diag(ma)) ? db_size : 0;

  const cs_lnum_t *row_index = cs_matrix_assembler_get_row_index(ma);
  const cs_lnum_t *col_ids = cs_matrix_assembler_get_col_ids(ma);

  HYPRE_Int *_diag_sizes, *_offdiag_sizes;
  BFT_MALLOC(_diag_sizes, n_rows*db_size, HYPRE_Int);
  BFT_MALLOC(_offdiag_sizes, n_rows*db_size, HYPRE_Int);

  /* Separate local and distant loops for better first touch logic */

# pragma omp parallel for  if(n_rows > CS_THR_MIN)
  for (cs_lnum_t i = 0; i < n_rows; i++) {
    cs_lnum_t s_id = row_index[i];
    cs_lnum_t e_id = row_index[i+1];

    HYPRE_Int n_r_diag = 0;
    HYPRE_Int n_cols = e_id - s_id;

    for (cs_lnum_t j = s_id; j < e_id; j++) {
      if (col_ids[j] < n_rows) {
        n_r_diag++;
        if (col_ids[j] == i)
          n_r_diag += db_size -1;
      }
      else
        break;
    }

    for (cs_lnum_t j = 0; j < db_size; j++) {
      _diag_sizes[i*db_size + j] = n_r_diag + n_diag_add;
      _offdiag_sizes[i*db_size + j] = n_cols - n_r_diag;
    }
  }

  *diag_sizes = _diag_sizes;
  *offdiag_sizes = _offdiag_sizes;
}

/*----------------------------------------------------------------------------
 * Compute local and distant counts of matrix entries using an assembler
 * with full blocks fill.
 *
 * The caller is responsible for freeing the returned diag_sizes and
 * offdiag_sizes arrays.
 *
 * parameters:
 *   ma            <-- pointer to matrix assembler structure
 *   b_size        <-- diagonal block size
 *   diag_sizes    --> diagonal values
 *   offdiag_sizes --> off-diagonal values
 *----------------------------------------------------------------------------*/

static void
_compute_diag_sizes_assembler_b(const cs_matrix_assembler_t   *ma,
                                cs_lnum_t                      b_size,
                                HYPRE_Int                    **diag_sizes,
                                HYPRE_Int                    **offdiag_sizes)
{
  const cs_lnum_t n_rows = cs_matrix_assembler_get_n_rows(ma);

  cs_lnum_t n_diag_add
    = (cs_matrix_assembler_get_separate_diag(ma)) ? 1 : 0;

  const cs_lnum_t *row_index = cs_matrix_assembler_get_row_index(ma);
  const cs_lnum_t *col_ids = cs_matrix_assembler_get_col_ids(ma);

  HYPRE_Int *_diag_sizes, *_offdiag_sizes;
  BFT_MALLOC(_diag_sizes, n_rows*b_size, HYPRE_Int);
  BFT_MALLOC(_offdiag_sizes, n_rows*b_size, HYPRE_Int);

  /* Separate local and distant loops for better first touch logic */

# pragma omp parallel for  if(n_rows > CS_THR_MIN)
  for (cs_lnum_t i = 0; i < n_rows; i++) {
    cs_lnum_t s_id = row_index[i];
    cs_lnum_t e_id = row_index[i+1];

    HYPRE_Int n_r_diag = 0;
    HYPRE_Int n_cols = e_id - s_id;

    for (cs_lnum_t j = s_id; j < e_id; j++) {
      if (col_ids[j] < n_rows) {
        n_r_diag++;
      }
      else
        break;
    }

    for (cs_lnum_t j = 0; j < b_size; j++) {
      _diag_sizes[i*b_size + j] = (n_r_diag + n_diag_add)*b_size;
      _offdiag_sizes[i*b_size + j] = (n_cols - n_r_diag)*b_size;
    }
  }

  *diag_sizes = _diag_sizes;
  *offdiag_sizes = _offdiag_sizes;
}

/*----------------------------------------------------------------------------
 * Compute local and distant counts of a native matrix's entries.
 *
 * The caller is responsible for freeing the returned diag_sizes and
 * offdiag_sizes arrays.
 *
 * parameters:
 *   matrix        <-- pointer to matrix structure
 *   have_diag     <-- does the matrix include a diagonal ?
 *   n_edges       <-- local number of graph edges
 *   edges         <-- edges (symmetric row <-> column) connectivity
 *   g_e_id        <-- global element ids
 *   diag_sizes    --> diagonal values
 *   offdiag_sizes --> off-diagonal values
 *----------------------------------------------------------------------------*/

static void
_compute_diag_sizes_native(cs_matrix_t        *matrix,
                           bool                have_diag,
                           cs_lnum_t           n_edges,
                           const cs_lnum_t     edges[restrict][2],
                           const cs_gnum_t     g_e_id[],
                           HYPRE_Int         **diag_sizes,
                           HYPRE_Int         **offdiag_sizes)
{
  cs_lnum_t  n_rows = matrix->n_rows;

  HYPRE_Int *_diag_sizes, *_offdiag_sizes;
  BFT_MALLOC(_diag_sizes, n_rows, HYPRE_Int);
  BFT_MALLOC(_offdiag_sizes, n_rows, HYPRE_Int);

  int n_diag = (have_diag) ? 1 : 0;

  cs_gnum_t g_id_lb = 0;
  cs_gnum_t g_id_ub = 0;

  if (n_rows > 0) {
    g_id_lb = g_e_id[0];
    g_id_ub = g_e_id[n_rows-1] + 1;
  }

  /* Separate local and distant loops for better first touch logic */

# pragma omp parallel for  if(n_rows > CS_THR_MIN)
  for (cs_lnum_t i = 0; i < n_rows; i++) {
    _diag_sizes[i] = n_diag;
    _offdiag_sizes[i] = 0;
  }

  const cs_lnum_t *group_index = NULL;
  if (matrix->numbering != NULL) {
    if (matrix->numbering->type == CS_NUMBERING_THREADS) {
      group_index = matrix->numbering->group_index;
    }
  }

  if (group_index != NULL) {

    const int n_threads = matrix->numbering->n_threads;
    const int n_groups = matrix->numbering->n_groups;

    for (int g_id = 0; g_id < n_groups; g_id++) {

#     pragma omp parallel for
      for (int t_id = 0; t_id < n_threads; t_id++) {

        for (cs_lnum_t edge_id = group_index[(t_id*n_groups + g_id)*2];
             edge_id < group_index[(t_id*n_groups + g_id)*2 + 1];
             edge_id++) {
          cs_lnum_t ii = edges[edge_id][0];
          cs_lnum_t jj = edges[edge_id][1];
          cs_gnum_t g_ii = g_e_id[ii];
          cs_gnum_t g_jj = g_e_id[jj];
          if (ii < n_rows) {
            if (g_jj >= g_id_lb && g_jj < g_id_ub)
              _diag_sizes[ii] += 1;
            else
              _offdiag_sizes[ii] += 1;
          }
          if (jj < n_rows) {
            if (g_ii >= g_id_lb && g_ii < g_id_ub)
              _diag_sizes[jj] += 1;
            else
              _offdiag_sizes[jj] += 1;
          }
        }
      }

    }

  }
  else {

    for (cs_lnum_t edge_id = 0; edge_id < n_edges; edge_id++) {
      cs_lnum_t ii = edges[edge_id][0];
      cs_lnum_t jj = edges[edge_id][1];
      cs_gnum_t g_ii = g_e_id[ii];
      cs_gnum_t g_jj = g_e_id[jj];
      if (ii < n_rows) {
        if (g_jj >= g_id_lb && g_jj < g_id_ub)
          _diag_sizes[ii] += 1;
        else
          _offdiag_sizes[ii] += 1;
      }
      if (jj < n_rows) {
        if (g_ii >= g_id_lb && g_ii < g_id_ub)
          _diag_sizes[jj] += 1;
        else
          _offdiag_sizes[jj] += 1;
      }
    }

  }

  *diag_sizes = _diag_sizes;
  *offdiag_sizes = _offdiag_sizes;
}

/*----------------------------------------------------------------------------*/
/*!
 * \brief Function for initialization of HYPRE matrix coefficients using
 *        local row ids and column indexes.
 *
 * \warning  The matrix pointer must point to valid data when the selection
 *           function is called, so the life cycle of the data pointed to
 *           should be at least as long as that of the assembler values
 *           structure.
 *
 * \param[in, out]  matrix_p  untyped pointer to matrix description structure
 * \param[in]       db_size   optional diagonal block sizes
 * \param[in]       eb_size   optional extra-diagonal block sizes
 */
/*----------------------------------------------------------------------------*/

static void
_assembler_values_init(void              *matrix_p,
                       const cs_lnum_t    db_size[4],
                       const cs_lnum_t    eb_size[4])
{
  cs_matrix_t  *matrix = (cs_matrix_t *)matrix_p;

  if (matrix->coeffs == NULL) {
    cs_matrix_coeffs_hypre_t  *coeffs;
    BFT_MALLOC(coeffs, 1, cs_matrix_coeffs_hypre_t);
    memset(coeffs, 0, sizeof(HYPRE_IJMatrix));
    coeffs->matrix_state = 0;

    matrix->coeffs = coeffs;
  }

  /* Associated matrix assembler */

  const cs_matrix_assembler_t  *ma = matrix->assembler;

  cs_matrix_coeffs_hypre_t  *coeffs = matrix->coeffs;

  /* Create HYPRE matrix */

  HYPRE_IJMatrix hm = coeffs->hm;

  if (coeffs->matrix_state == 0) {

    MPI_Comm comm = cs_glob_mpi_comm;
    if (comm == MPI_COMM_NULL)
      comm = MPI_COMM_WORLD;

    const cs_gnum_t *l_range = cs_matrix_assembler_get_l_range(ma);

    HYPRE_BigInt b_size = db_size[0];
    HYPRE_BigInt ilower = b_size*l_range[0];
    HYPRE_BigInt iupper = b_size*l_range[1] - 1;

    HYPRE_IJMatrixCreate(comm,
                         ilower,
                         iupper,
                         ilower,
                         iupper,
                         &hm);

    coeffs->l_range[0] = l_range[0];
    coeffs->l_range[1] = l_range[1];

    coeffs->hm = hm;

    HYPRE_IJMatrixSetObjectType(hm, HYPRE_PARCSR);

    HYPRE_Int *diag_sizes = NULL, *offdiag_sizes = NULL;

    if (db_size[0] == 1)
      _compute_diag_sizes_assembler(ma,
                                    &diag_sizes,
                                    &offdiag_sizes);
    else if (eb_size[0] == 1)
      _compute_diag_sizes_assembler_db(ma,
                                      db_size[0],
                                      &diag_sizes,
                                      &offdiag_sizes);
    else
      _compute_diag_sizes_assembler_b(ma,
                                      db_size[0],
                                      &diag_sizes,
                                      &offdiag_sizes);

    HYPRE_IJMatrixSetDiagOffdSizes(hm, diag_sizes, offdiag_sizes);
    HYPRE_IJMatrixSetMaxOffProcElmts(hm, 0);

    BFT_FREE(diag_sizes);
    BFT_FREE(offdiag_sizes);

    HYPRE_IJMatrixSetOMPFlag(hm, 0);

    HYPRE_MemoryLocation  memory_location = HYPRE_MEMORY_HOST;

    HYPRE_IJMatrixInitialize_v2(hm, memory_location);

    /* Also update SpMV function pointers, now that we know the matrix
       is using an assembler: regardless of the fill type, we handle it as
       a scalar IJ matrix with HYPRE during coefficient assignment */

    if (matrix->vector_multiply[matrix->fill_type][0] == NULL)
      matrix->vector_multiply[matrix->fill_type][0] = _mat_vec_p_parcsr;
  }
}

/*----------------------------------------------------------------------------*/
/*!
 * \brief Add HYPRE matrix coefficients using global row ids
 *        and column indexes, using intermediate copy for indexes and values.
 *
 * \warning  The matrix pointer must point to valid data when the selection
 *           function is called, so the life cycle of the data pointed to
 *           should be at least as long as that of the assembler values
 *           structure.
 *
 * \remark  Note that we pass column indexes (not ids) here; as the
 *          caller is already assumed to have identified the index
 *          matching a given column id.
 *
 * \param[in, out]  hm        HYPRE Matrix
 * \param[in]       l_b       lower bound under which rows are ignored
 * \param[in]       u_b       upper bound from which rows are ignored
 * \param[in]       n         number of values to add
 * \param[in]       b_size    associated data block size
 * \param[in]       stride    associated data block size
 * \param[in]       row_g_id  associated global row ids
 * \param[in]       col_g_id  associated global column ids
 * \param[in]       vals      pointer to values (size: n*stride)
 *
 * \return  HYPRE error code
 */
/*----------------------------------------------------------------------------*/

static HYPRE_Int
_assembler_values_add_block_cc(HYPRE_IJMatrix   hm,
                               HYPRE_BigInt     l_b,
                               HYPRE_BigInt     u_b,
                               HYPRE_Int        n,
                               HYPRE_Int        b_size,
                               HYPRE_Int        stride,
                               const cs_gnum_t  row_g_id[],
                               const cs_gnum_t  col_g_id[],
                               const cs_real_t  vals[])
{
  HYPRE_BigInt rows[COEFF_GROUP_SIZE];
  HYPRE_BigInt cols[COEFF_GROUP_SIZE];
  HYPRE_Real values[COEFF_GROUP_SIZE];

  HYPRE_Int block_step = COEFF_GROUP_SIZE / stride;

  for (HYPRE_Int s_id = 0; s_id < n; s_id += block_step) {

    HYPRE_Int n_group = block_step;
    if (s_id + n_group > n)
      n_group = n - s_id;

    HYPRE_Int l = 0;

    for (HYPRE_Int i = 0; i < n_group; i++) {
      HYPRE_Int r_g_id = row_g_id[s_id + i];
      if (r_g_id >= l_b && r_g_id < u_b) {
        for (cs_lnum_t j = 0; j < b_size; j++) {
          for (cs_lnum_t k = 0; k < b_size; k++) {
            rows[l*stride + j*b_size + k] = row_g_id[s_id + i]*b_size + j;
            cols[l*stride + j*b_size + k] = col_g_id[s_id + i]*b_size + k;
            values[l*stride + j*b_size + k]
              = vals[(s_id + i)*stride + j*b_size + k];
          }
        }
        l++;
      }
    }

    HYPRE_Int hypre_ierr
      = HYPRE_IJMatrixAddToValues(hm, l*stride,
                                  NULL, rows, cols, values);

    if (hypre_ierr != 0)
      return hypre_ierr;

  }

  return 0;
}

/*----------------------------------------------------------------------------*/
/*!
 * \brief Add extradiagonal HYPRE matrix coefficients using global row ids
 *        and column indexes, for fill type CS_MATRIX_BLOCK_D,
 *        CS_MATRIX_BLOCK_D_66, CS_MATRIX_BLOCK_D_SYM.
 *
 * \warning  The matrix pointer must point to valid data when the selection
 *           function is called, so the life cycle of the data pointed to
 *           should be at least as long as that of the assembler values
 *           structure.
 *
 * \remark  Note that we pass column indexes (not ids) here; as the
 *          caller is already assumed to have identified the index
 *          matching a given column id.
 *
 * \param[in, out]  hm        HYPRE Matrix
 * \param[in]       l_b       lower bound under which rows are ignored
 * \param[in]       u_b       upper bound from which rows are ignored
 * \param[in]       n         number of values to add
 * \param[in]       b_size    associated data block size
 * \param[in]       stride    associated data block size
 * \param[in]       row_g_id  associated global row ids
 * \param[in]       col_g_id  associated global column ids
 * \param[in]       vals      pointer to values (size: n*stride)
 *
 * \return  HYPRE error code
 */
/*----------------------------------------------------------------------------*/

static HYPRE_Int
_assembler_values_add_block_d_e(HYPRE_IJMatrix   hm,
                                HYPRE_BigInt     l_b,
                                HYPRE_BigInt     u_b,
                                HYPRE_Int        n,
                                HYPRE_Int        b_size,
                                const cs_gnum_t  row_g_id[],
                                const cs_gnum_t  col_g_id[],
                                const cs_real_t  vals[])
{
  HYPRE_BigInt rows[COEFF_GROUP_SIZE];
  HYPRE_BigInt cols[COEFF_GROUP_SIZE];
  HYPRE_Real values[COEFF_GROUP_SIZE];

  HYPRE_Int block_step = COEFF_GROUP_SIZE / b_size;

  for (HYPRE_Int s_id = 0; s_id < n; s_id += block_step) {

    HYPRE_Int n_group = block_step;
    if (s_id + n_group > n)
      n_group = n - s_id;

    HYPRE_Int l = 0;

    for (HYPRE_Int i = 0; i < n_group; i++) {
      HYPRE_Int r_g_id = row_g_id[s_id + i];
      if (r_g_id >= l_b && r_g_id < u_b) {
        for (cs_lnum_t j = 0; j < b_size; j++) {
          rows[l*b_size + j] = row_g_id[s_id + i]*b_size + j;
          cols[l*b_size + j] = col_g_id[s_id + i]*b_size + j;
          values[l*b_size + j] = vals[s_id + i];
        }
        l++;
      }
    }

    HYPRE_Int hypre_ierr
      = HYPRE_IJMatrixAddToValues(hm, l*b_size,
                                  NULL, rows, cols, values);

    if (hypre_ierr != 0)
      return hypre_ierr;

  }

  return 0;
}

/*----------------------------------------------------------------------------*/
/*!
 * \brief Function for addition to HYPRE matrix coefficients using
 *        local row ids and column indexes.
 *
 * This function can be used in all cases, including when
 *  sizeof(HYPRE_BigInt) != sizeof(cs_gnum_t)
 *  sizeof(HYPRE_Real) != sizeof(cs_real_t)
 *
 * Values whose associated row index is negative should be ignored;
 * Values whose column index is -1 are assumed to be assigned to a
 * separately stored diagonal. Other indexes should be valid.
 *
 * \warning  The matrix pointer must point to valid data when the selection
 *           function is called, so the life cycle of the data pointed to
 *           should be at least as long as that of the assembler values
 *           structure.
 *
 * \remark  Note that we pass column indexes (not ids) here; as the
 *          caller is already assumed to have identified the index
 *          matching a given column id.
 *
 * \param[in, out]  matrix_p  untyped pointer to matrix description structure
 * \param[in]       n         number of values to add
 * \param[in]       stride    associated data block size
 * \param[in]       row_g_id  associated global row ids
 * \param[in]       col_g_id  associated global column ids
 * \param[in]       vals      pointer to values (size: n*stride)
 */
/*----------------------------------------------------------------------------*/

static void
_assembler_values_add_g(void             *matrix_p,
                        cs_lnum_t         n,
                        cs_lnum_t         stride,
                        const cs_gnum_t   row_g_id[],
                        const cs_gnum_t   col_g_id[],
                        const cs_real_t   vals[])
{
  cs_matrix_t  *matrix = (cs_matrix_t *)matrix_p;

  cs_matrix_coeffs_hypre_t  *coeffs = matrix->coeffs;
  HYPRE_IJMatrix hm = coeffs->hm;

  HYPRE_BigInt l_b = coeffs->l_range[0];
  HYPRE_BigInt u_b = coeffs->l_range[1];

  assert(hm != NULL);

  HYPRE_Int hypre_ierr = 0;
  HYPRE_Int nrows = n;
  HYPRE_Int b_size = matrix->db_size[0];

  /* Scalar matrix
     ------------- */

  if (b_size == 1) {

    HYPRE_BigInt rows[COEFF_GROUP_SIZE];
    HYPRE_BigInt cols[COEFF_GROUP_SIZE];
    HYPRE_Real values[COEFF_GROUP_SIZE];

    HYPRE_Int l = 0;

    for (HYPRE_Int s_id = 0; s_id < nrows; s_id += COEFF_GROUP_SIZE) {

        HYPRE_Int n_group = COEFF_GROUP_SIZE;
        if (s_id + n_group > n)
          n_group = nrows - s_id;

        HYPRE_Int l = 0;

        for (HYPRE_Int i = 0; i < n_group; i++) {
          HYPRE_Int r_g_id = row_g_id[s_id + i];
          if (r_g_id >= l_b && r_g_id < u_b) {
            rows[l] = row_g_id[s_id + i];
            cols[l] = col_g_id[s_id + i];
            values[l] = vals[s_id + i];
            l++;
          }
        }

        hypre_ierr
          = HYPRE_IJMatrixAddToValues(hm, l, NULL, rows, cols, values);

        if (hypre_ierr != 0)
          break;

    }
  }

  /* Block matrix
     ------------ */

  else {

    /* Full blocks (including diagonal terms for diagonal fill) */

    if (   matrix->fill_type >= CS_MATRIX_BLOCK
        || row_g_id[0] == col_g_id[0])
      hypre_ierr = _assembler_values_add_block_cc(hm,
                                                  l_b,
                                                  u_b,
                                                  nrows,
                                                  b_size,
                                                  stride,
                                                  row_g_id,
                                                  col_g_id,
                                                  vals);

    /* Diagonal bloc extra-diagonal terms only */

    else if (matrix->fill_type >= CS_MATRIX_BLOCK_D)
      hypre_ierr = _assembler_values_add_block_d_e(hm,
                                                   l_b,
                                                   u_b,
                                                   nrows,
                                                   b_size,
                                                   row_g_id,
                                                   col_g_id,
                                                   vals);

  }

  if (hypre_ierr != 0) {
    char err_desc_buffer[64];
    HYPRE_DescribeError(hypre_ierr, err_desc_buffer);
    err_desc_buffer[63] = '\0';
    bft_error(__FILE__, __LINE__, 0,
              _("%s: error in HYPRE matrix assembly:\n  %s"),
              __func__, err_desc_buffer);
  }
}

/*----------------------------------------------------------------------------*/
/*!
 * \brief Function to start the final assembly of matrix coefficients.
 *
 * \warning  The matrix pointer must point to valid data when the selection
 *           function is called, so the life cycle of the data pointed to
 *           should be at least as long as that of the assembler values
 *           structure.
 *
 * \param[in, out]  matrix_p  untyped pointer to matrix description structure
 */
/*----------------------------------------------------------------------------*/

static void
_assembler_values_begin(void  *matrix_p)
{
  CS_UNUSED(matrix_p);

  /* Note: this function is called once all coefficients have
   *       been added, and before assembly is finalized.
   *       It could be used in a threading or tasking context to signify
   *       assembly finalization can start, returning immediately
   *       so the calling task can continue working during this finalization */
}

/*----------------------------------------------------------------------------*/
/*!
 * \brief Function to complete the final assembly of matrix
 *        coefficients.
 *
 * \warning  The matrix pointer must point to valid data when the selection
 *           function is called, so the life cycle of the data pointed to
 *           should be at least as long as that of the assembler values
 *           structure.
 *
 * \param[in, out]  matrix_p  untyped pointer to matrix description structure
 */
/*----------------------------------------------------------------------------*/

static void
_assembler_values_end(void  *matrix_p)
{
  cs_matrix_t  *matrix = (cs_matrix_t *)matrix_p;

  cs_matrix_coeffs_hypre_t  *coeffs = matrix->coeffs;
  HYPRE_IJMatrix hm = coeffs->hm;

  HYPRE_IJMatrixAssemble(hm);

  if (coeffs->matrix_state == 0) {

    MPI_Comm comm = cs_glob_mpi_comm;
    if (comm == MPI_COMM_NULL)
      comm = MPI_COMM_WORLD;

    /* Create associated vectors here also to avoid repeated creation
       (and possible overhead) where used */

    const HYPRE_Int  n_off_proc = matrix->n_cols_ext - matrix->n_rows;
    const HYPRE_BigInt b_size = matrix->db_size[0];

    HYPRE_BigInt ilower = b_size*(coeffs->l_range[0]);
    HYPRE_BigInt iupper = b_size*(coeffs->l_range[1]) - 1;

    HYPRE_IJVectorCreate(comm, ilower, iupper, &(coeffs->hx));
    HYPRE_IJVectorSetObjectType(coeffs->hx, HYPRE_PARCSR);
    HYPRE_IJVectorSetMaxOffProcElmts(coeffs->hx, n_off_proc);

    HYPRE_IJVectorCreate(comm, ilower, iupper, &(coeffs->hy));
    HYPRE_IJVectorSetObjectType(coeffs->hy, HYPRE_PARCSR);
    HYPRE_IJVectorSetMaxOffProcElmts(coeffs->hy, n_off_proc);

    HYPRE_IJVectorInitialize(coeffs->hx);
    HYPRE_IJVectorInitialize(coeffs->hy);
  }

  /* Set stat flag */

  coeffs->matrix_state = 1;
}

/*----------------------------------------------------------------------------*/
/*!
 * \brief Create and initialize a CSR matrix assembler values structure.
 *
 * The associated matrix's structure must have been created using
 * \ref cs_matrix_structure_create_from_assembler.
 *
 * Block sizes are defined by an optional array of 4 values:
 *   0: useful block size, 1: vector block extents,
 *   2: matrix line extents,  3: matrix line*column extents
 *
 * \param[in, out]  matrix                 pointer to matrix structure
 * \param[in]       diag_block_size        block sizes for diagonal, or NULL
 * \param[in]       extra_diag_block_size  block sizes for extra diagonal,
 *                                         or NULL
 *
 * \return  pointer to initialized matrix assembler values structure;
 */
/*----------------------------------------------------------------------------*/

static cs_matrix_assembler_values_t *
_assembler_values_create_hypre(cs_matrix_t      *matrix,
                               const cs_lnum_t  *diag_block_size,
                               const cs_lnum_t  *extra_diag_block_size)
{
  cs_matrix_assembler_values_t *mav
    = cs_matrix_assembler_values_create(matrix->assembler,
                                        false,
                                        diag_block_size,
                                        extra_diag_block_size,
                                        (void *)matrix,
                                        _assembler_values_init,
                                        NULL,
                                        _assembler_values_add_g,
                                        _assembler_values_begin,
                                        _assembler_values_end);

  return mav;
}

/*----------------------------------------------------------------------------
 * Set HYPRE ParCSR matrix coefficients.
 *
 * Depending on current options and initialization, values will be copied
 * or simply mapped.
 *
 * parameters:
 *   matrix    <-- pointer to matrix structure
 *   symmetric <-- indicates if extradiagonal values are symmetric
 *   copy      <-- indicates if coefficients should be copied
 *   n_edges   <-- local number of graph edges
 *   edges     <-- edges (symmetric row <-> column) connectivity
 *   da        <-- diagonal values
 *   xa        <-- extradiagonal values
 *----------------------------------------------------------------------------*/

static void
_set_coeffs_ij(cs_matrix_t        *matrix,
               bool                symmetric,
               bool                copy,
               cs_lnum_t           n_edges,
               const cs_lnum_t     edges[restrict][2],
               const cs_real_t     da[restrict],
               const cs_real_t     xa[restrict])
{
  CS_UNUSED(copy);

  bool direct_assembly = false;

  const cs_lnum_t  n_rows = matrix->n_rows;

  const cs_gnum_t *g_id = cs_matrix_get_block_row_g_id(n_rows, matrix->halo);
  const bool have_diag = (xa != NULL) ? true : false;

  MPI_Comm comm = cs_glob_mpi_comm;
  if (comm == MPI_COMM_NULL)
    comm = MPI_COMM_WORLD;

  assert(n_rows > 0);

  cs_matrix_coeffs_hypre_t  *coeffs = matrix->coeffs;

  /* Create HYPRE matrix */

  HYPRE_IJMatrix hm = coeffs->hm;

  cs_gnum_t  l_range[2] = {0, 0};

  if (n_rows > 0) {
    l_range[0] = g_id[0];
    l_range[1] = g_id[n_rows-1] + 1;
  }

  if (coeffs->matrix_state == 0) {

    HYPRE_BigInt b_size = matrix->db_size[0];
    HYPRE_BigInt ilower = b_size*l_range[0];
    HYPRE_BigInt iupper = b_size*l_range[1] - 1;

    coeffs->l_range[0] = l_range[0];
    coeffs->l_range[1] = l_range[1];

    HYPRE_IJMatrixCreate(comm,
                         ilower,
                         iupper,
                         ilower,
                         iupper,
                         &hm);

    coeffs->hm = hm;

    HYPRE_IJMatrixSetObjectType(hm, HYPRE_PARCSR);

    HYPRE_Int *diag_sizes = NULL, *offdiag_sizes = NULL;

    _compute_diag_sizes_native(matrix,
                               have_diag,
                               n_edges,
                               edges,
                               g_id,
                               &diag_sizes,
                               &offdiag_sizes);

    HYPRE_IJMatrixSetDiagOffdSizes(hm, diag_sizes, offdiag_sizes);
    HYPRE_IJMatrixSetMaxOffProcElmts(hm, 0);

    BFT_FREE(diag_sizes);
    BFT_FREE(offdiag_sizes);

    HYPRE_IJMatrixSetOMPFlag(hm, 0);

    HYPRE_MemoryLocation  memory_location = HYPRE_MEMORY_HOST;

    HYPRE_IJMatrixInitialize_v2(hm, memory_location);
  }

  HYPRE_Int max_chunk_size = 32768 - 1;

  HYPRE_BigInt *rows, *cols;
  HYPRE_Real *aij;
  BFT_MALLOC(rows, max_chunk_size+1, HYPRE_BigInt);
  BFT_MALLOC(cols, max_chunk_size+1, HYPRE_BigInt);
  BFT_MALLOC(aij, max_chunk_size+1, HYPRE_Real);

  if (have_diag) {
    cs_lnum_t s_id = 0;
    while (s_id < n_rows) {

      HYPRE_Int ic = 0;

      for (cs_lnum_t ii = s_id;
           ii < n_rows && ic < max_chunk_size;
           ii++) {
        rows[ic] = g_id[ii];
        cols[ic] = g_id[ii];
        aij[ic] = da[ii];
        ic++;
      }

      s_id = ic;

      if (direct_assembly)
        HYPRE_IJMatrixSetValues(hm, ic, NULL, rows, cols, aij);
      else
        HYPRE_IJMatrixAddToValues(hm, ic, NULL, rows, cols, aij);
    }
  }

  if (symmetric) {

    cs_lnum_t s_e_id = 0;

    while (s_e_id < n_edges) {

      HYPRE_Int ic = 0;

      for (cs_lnum_t e_id = s_e_id;
           e_id < n_edges && ic < max_chunk_size;
           e_id++) {
        cs_lnum_t ii = edges[e_id][0];
        cs_lnum_t jj = edges[e_id][1];
        cs_gnum_t g_ii = g_id[ii];
        cs_gnum_t g_jj = g_id[jj];
        if (ii < n_rows) {
          rows[ic] = g_ii;
          cols[ic] = g_jj;
          aij[ic] = xa[e_id];
          ic++;
        }
        if (jj < n_rows) {
          rows[ic] = g_jj;
          cols[ic] = g_ii;
          aij[ic] = xa[e_id];
          ic++;
        }
      }

      s_e_id = ic;

      if (direct_assembly)
        HYPRE_IJMatrixSetValues(hm, ic, NULL, rows, cols, aij);
      else
        HYPRE_IJMatrixAddToValues(hm, ic, NULL, rows, cols, aij);
    }

  }
  else { /* non-symmetric variant */

    cs_lnum_t s_e_id = 0;

    while (s_e_id < n_edges) {

      HYPRE_Int ic = 0;

      for (cs_lnum_t e_id = s_e_id;
           e_id < n_edges && ic < max_chunk_size;
           e_id++) {
        cs_lnum_t ii = edges[e_id][0];
        cs_lnum_t jj = edges[e_id][1];
        cs_gnum_t g_ii = g_id[ii];
        cs_gnum_t g_jj = g_id[jj];

        if (ii < n_rows) {
          rows[ic] = g_ii;
          cols[ic] = g_jj;
          aij[ic] = xa[e_id*2];
          ic++;
        }
        if (jj < n_rows) {
          rows[ic] = g_jj;
          cols[ic] = g_ii;
          aij[ic] = xa[e_id*2+1];
          ic++;
        }
      }

      s_e_id = ic;

      if (direct_assembly)
        HYPRE_IJMatrixSetValues(hm, ic, NULL, rows, cols, aij);
      else
        HYPRE_IJMatrixAddToValues(hm, ic, NULL, rows, cols, aij);

    }

  }

  BFT_FREE(rows);
  BFT_FREE(cols);
  BFT_FREE(aij);

  /* Finalize asembly and update state */

  _assembler_values_end(matrix);
}

/*----------------------------------------------------------------------------
 * Release HYPRE ParCSR matrix coefficients.
 *
 * Depending on current options and initialization, values will be copied
 * or simply mapped.
 *
 * parameters:
 *   matrix    <-- pointer to matrix structure
 *   symmetric <-- indicates if extradiagonal values are symmetric
 *   copy      <-- indicates if coefficients should be copied
 *   n_edges   <-- local number of graph edges
 *   edges     <-- edges (symmetric row <-> column) connectivity
 *   da        <-- diagonal values
 *   xa        <-- extradiagonal values
 *----------------------------------------------------------------------------*/

static void
_release_coeffs_ij(cs_matrix_t  *matrix)
{
  cs_matrix_coeffs_hypre_t  *coeffs = matrix->coeffs;

  if (matrix->coeffs != NULL) {

    if (coeffs->matrix_state > 0) {
      HYPRE_IJMatrixDestroy(coeffs->hm);

      HYPRE_IJVectorDestroy(coeffs->hx);
      HYPRE_IJVectorDestroy(coeffs->hy);

      coeffs->matrix_state = 0;
    }
  }
}

/*----------------------------------------------------------------------------
 * Release HYPRE ParCSR matrix coefficients.
 *
 * Depending on current options and initialization, values will be copied
 * or simply mapped.
 *
 * parameters:
 *   matrix    <-- pointer to matrix structure
 *   symmetric <-- indicates if extradiagonal values are symmetric
 *   copy      <-- indicates if coefficients should be copied
 *   n_edges   <-- local number of graph edges
 *   edges     <-- edges (symmetric row <-> column) connectivity
 *   da        <-- diagonal values
 *   xa        <-- extradiagonal values
 *----------------------------------------------------------------------------*/

static void
_destroy_coeffs_ij(cs_matrix_t  *matrix)
{
  cs_matrix_coeffs_hypre_t  *coeffs = matrix->coeffs;

  if (matrix->coeffs != NULL) {

    if (coeffs->matrix_state > 0) {
      HYPRE_IJMatrixDestroy(coeffs->hm);
      coeffs->matrix_state = 0;
    }

    BFT_FREE(coeffs);
  }
}

/*----------------------------------------------------------------------------
 * Copy diagonal of HYPRE ParCSR matrix.
 *
 * parameters:
 *   matrix <-- pointer to matrix structure
 *   da     --> diagonal (pre-allocated, size: n_rows)
 *----------------------------------------------------------------------------*/

static void
_copy_diagonal_ij(const cs_matrix_t  *matrix,
                  cs_real_t          *restrict da)
{
  const cs_matrix_coeffs_hypre_t  *coeffs = matrix->coeffs;

  const HYPRE_BigInt b_size = matrix->db_size[0];

  HYPRE_BigInt ilower = b_size*(coeffs->l_range[0]);

  const HYPRE_BigInt n_rows = matrix->n_rows * b_size;

  HYPRE_BigInt *rows, *cols;
  HYPRE_Int *n_rcols;
  HYPRE_Real *aij;

  BFT_MALLOC(n_rcols, n_rows, HYPRE_Int);
  BFT_MALLOC(rows, n_rows, HYPRE_BigInt);
  BFT_MALLOC(cols, n_rows, HYPRE_BigInt);

  if (sizeof(HYPRE_Real) == sizeof(cs_real_t))
    aij = da;
  else
    BFT_MALLOC(aij, n_rows, HYPRE_Real);

# pragma omp parallel for  if(n_rows > CS_THR_MIN)
  for (HYPRE_BigInt ii = 0; ii < n_rows; ii++) {
    n_rcols[ii] = 1;
    rows[ii] = ilower + ii;;
    cols[ii] = ilower + ii;;
  }

  HYPRE_IJMatrixGetValues(coeffs->hm,
                          matrix->n_rows,
                          n_rcols,
                          rows,
                          cols,
                          aij);

  if (aij != da) {
    for (HYPRE_BigInt ii = 0; ii < n_rows; ii++) {
      da[ii] = aij[ii];;
    }
    BFT_FREE(aij);
  }

  BFT_FREE(cols);
  BFT_FREE(rows);
  BFT_FREE(n_rcols);
}

/*============================================================================
 * Semi-private function definitions
 *============================================================================*/

/*----------------------------------------------------------------------------*/
/*!
 * \brief return coefficients structure associated with HYPRE matrix.
 *
 * \param[in]  matrix  pointer to matrix structure
 *
 * \return  pointer to matrix coefficients handler structure for HYPRE matrix.
 */
/*----------------------------------------------------------------------------*/

cs_matrix_coeffs_hypre_t *
cs_matrix_hypre_get_coeffs(const cs_matrix_t  *matrix)
{
  return (cs_matrix_coeffs_hypre_t *)(matrix->coeffs);
}

/*! (DOXYGEN_SHOULD_SKIP_THIS) \endcond */

/*============================================================================
 * Public function definitions
 *============================================================================*/

/*----------------------------------------------------------------------------*/
/*!
 * \brief Switch matrix type to hypre.
 *
 * This releases previous coefficients if present, so should be called
 * just after matrix creation, before assigning coefficients.
 *
 * \param[in, out]  matrix  pointer to matrix structure
 */
/*----------------------------------------------------------------------------*/

void
cs_matrix_set_type_hypre(cs_matrix_t  *matrix)
{
  matrix->type = CS_MATRIX_N_BUILTIN_TYPES;

  matrix->type_name = _hypre_ij_type_name;
  matrix->type_fname = _hypre_ij_type_fullname;

  /* Release previous coefficients if present */

  if (matrix->coeffs != NULL)
    matrix->destroy_coefficients(matrix);

  cs_matrix_coeffs_hypre_t  *coeffs;
  BFT_MALLOC(coeffs, 1, cs_matrix_coeffs_hypre_t);
  memset(coeffs, 0, sizeof(HYPRE_IJMatrix));
  coeffs->matrix_state = 0;

  matrix->coeffs = coeffs;

  /* Set function pointers here */

  matrix->set_coefficients = _set_coeffs_ij;
  matrix->release_coefficients = _release_coeffs_ij;
  matrix->destroy_coefficients = _destroy_coeffs_ij;
  matrix->assembler_values_create = _assembler_values_create_hypre;

  matrix->get_diagonal = NULL;

  for (int i = 0; i < CS_MATRIX_N_FILL_TYPES; i++) {
    matrix->vector_multiply[i][0] = NULL;
    matrix->vector_multiply[i][1] = NULL;
  }

  /* Remark: allowed fill type is initially based on current "set coefficients.",
     but using a matrix assembler, block values are transformed into scalar values,
     so SpMv products should be possible, (and the function pointers updated).
     HYPRE also seems to have support for block matrixes (hypre_ParCSRBlockMatrix)
     but the high-level documentation does not mention it. */

  cs_matrix_fill_type_t mft[] = {CS_MATRIX_SCALAR,
                                 CS_MATRIX_SCALAR_SYM};

  for (int i = 0; i < 2; i++)
    matrix->vector_multiply[mft[i]][0] = _mat_vec_p_parcsr;

  matrix->copy_diagonal = _copy_diagonal_ij;

  /* Force MPI initialization if not already done.
   * The main code_saturne communicator is not modifed, as
   * this is purely for external libraries use. */

#if defined(HAVE_MPI)

  if (cs_glob_mpi_comm == MPI_COMM_NULL) {
    int mpi_flag;
    MPI_Initialized(&mpi_flag);
    if (mpi_flag == 0)
      MPI_Init(NULL, NULL);
  }

#endif

}

/*----------------------------------------------------------------------------*/

END_C_DECLS
