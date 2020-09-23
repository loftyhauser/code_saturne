/*============================================================================
 * Sparse Linear Equation Solvers using AMGX
 *============================================================================*/

/*
  This file is part of Code_Saturne, a general-purpose CFD tool.

  Copyright (C) 1998-2020 EDF S.A.

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
 * AMGX headers
 *----------------------------------------------------------------------------*/

#include <amgx_c.h>

/*----------------------------------------------------------------------------
 * Local headers
 *----------------------------------------------------------------------------*/

#include "bft_mem.h"
#include "bft_error.h"
#include "bft_printf.h"

#include "cs_base.h"
#include "cs_log.h"
#include "cs_fp_exception.h"
#include "cs_halo.h"
#include "cs_matrix.h"
#include "cs_matrix_default.h"
#include "cs_timer.h"

/*----------------------------------------------------------------------------
 *  Header for the current file
 *----------------------------------------------------------------------------*/

#include "cs_sles.h"
#include "cs_sles_amgx.h"

/*----------------------------------------------------------------------------*/

BEGIN_C_DECLS

/*=============================================================================
 * Additional doxygen documentation
 *============================================================================*/

/*!
  \file cs_sles_amgx.c

  \brief handling of AMGX-based linear solvers

  \page sles_amgx AMGX-based linear solvers.
*/

/*! \cond DOXYGEN_SHOULD_SKIP_THIS */

/*=============================================================================
 * Local Macro Definitions
 *============================================================================*/

/*=============================================================================
 * Local Structure Definitions
 *============================================================================*/

/* Basic per linear system options and logging */
/*---------------------------------------------*/

typedef struct _cs_sles_amgx_setup_t {

  AMGX_solver_handle   solver;           /* Linear solver context */
  AMGX_matrix_handle   matrix;           /* Linear system matrix */

  double               r_norm;           /* residue normalization */
  void                 *cctx;            /* convergence context */

} cs_sles_amgx_setup_t;

struct _cs_sles_amgx_t {

  /* Performance data */

  int                  n_setups;           /* Number of times system setup */
  int                  n_solves;           /* Number of times system solved */

  int                  n_iterations_last;  /* Number of iterations for last
                                              system resolution */
  int                  n_iterations_min;   /* Minimum number of iterations
                                              in system resolution history */
  int                  n_iterations_max;   /* Maximum number of iterations
                                              in system resolution history */
  int long long        n_iterations_tot;   /* Total accumulated number of
                                              iterations */

  cs_timer_counter_t   t_setup;            /* Total setup */
  cs_timer_counter_t   t_solve;            /* Total time used */

  /* Additional setup options */

  void                        *hook_context;   /* Optional user context */

  /* Setup data */

  char                   *solver_config_file;
  char                   *solver_config_string;

  AMGX_Mode               amgx_mode;
  bool                    pin_memory;

  AMGX_config_handle      solver_config;       /* Solver configuration */

  cs_sles_amgx_setup_t   *setup_data;

};

/*============================================================================
 *  Global variables
 *============================================================================*/

static int  _n_amgx_systems = 0;

static char                   *_resource_config_string = NULL;
static AMGX_config_handle     _amgx_config;
static AMGX_resources_handle  _amgx_resources;

#if defined(HAVE_MPI)
static MPI_Comm _amgx_comm = MPI_COMM_NULL;
#endif

/*============================================================================
 * Private function definitions
 *============================================================================*/

/*----------------------------------------------------------------------------*/
/*!
 * \brief Print function for AMGX.
 *
 * \param[in]  msg     message to print
 * \param[in]  length  length of message to print
 */
/*----------------------------------------------------------------------------*/

static void
_print_callback(const char  *msg,
                int          length)
{
  CS_NO_WARN_IF_UNUSED(length);

  bft_printf("%s", msg);
}

/*----------------------------------------------------------------------------*/
/*!
 * \brief Initialize AMGX.
 */
/*----------------------------------------------------------------------------*/

static void
_amgx_initialize(void)
{
  char err_str[4096];
  const char error_fmt[] = N_("%s returned %d.\n"
                              "%s");
  const char warning_fmt[] = N_("\nwarning: %s returned %d.\n"
                                "%s\n");

  AMGX_RC retval = AMGX_RC_OK;

  retval = AMGX_register_print_callback(_print_callback);
  if (retval != AMGX_RC_OK) {
    AMGX_get_error_string(retval, err_str, 4096);
    bft_printf(_(warning_fmt), "AMGX_initialize", (int)retval, err_str);
  }

  retval = AMGX_initialize();
  if (retval != AMGX_RC_OK) {
    AMGX_get_error_string(retval, err_str, 4096);
    bft_error(__FILE__, __LINE__, 0, _(error_fmt),
              "AMGX_initialize", retval, err_str);
  }

  retval = AMGX_initialize_plugins();
  if (retval != AMGX_RC_OK) {
    AMGX_get_error_string(retval, err_str, 4096);
    bft_error(__FILE__, __LINE__, 0, _(error_fmt),
              "AMGX_initialize_plugins", retval, err_str);
  }

  int major, minor;
  AMGX_get_api_version(&major, &minor);
  bft_printf(_("\nAMGX API version %d.%d\n"), major, minor);

  /* TODO: for mult-device configurations, this will need to be adapted */

  int device_num = 1;
  const int devices[] = {0};

  AMGX_config_create(&_amgx_config,
                     "communicator=MPI, min_rows_latency_hiding=10000");

  /* Note: if MPI supports GPUDirect, MPI_DIRECT is also allowed */

  void*comm_ptr = NULL;

#if defined(HAVE_MPI)
  if (cs_glob_n_ranks > 1) {
    MPI_Comm_dup(cs_glob_mpi_comm, &_amgx_comm);
    comm_ptr = &_amgx_comm;
  }
#endif

  retval = AMGX_resources_create(&_amgx_resources, _amgx_config,
                                 comm_ptr,
                                 device_num, devices);

  if (retval != AMGX_RC_OK) {
    AMGX_get_error_string(retval, err_str, 4096);
    bft_error(__FILE__, __LINE__, 0, _(error_fmt),
              "AMGX_resources_create", retval, err_str);
  }
}

/*----------------------------------------------------------------------------*/
/*!
 * \brief Finalize AMGX.
 */
/*----------------------------------------------------------------------------*/

static void
_amgx_finalize(void)
{
  char err_str[4096];
  const char warning_fmt[] = N_("\nwarning: %s returned %d.\n"
                                "%s\n");

  AMGX_RC retval = AMGX_resources_destroy(_amgx_resources);
  if (retval != AMGX_RC_OK) {
    AMGX_get_error_string(retval, err_str, 4096);
    bft_printf(_(warning_fmt), "AMGX_resources_destroy", (int)retval, err_str);
  }

  retval = AMGX_config_destroy(_amgx_config);
  if (retval != AMGX_RC_OK) {
    AMGX_get_error_string(retval, err_str, 4096);
    bft_printf(_(warning_fmt), "AMGX_config_destroy", (int)retval, err_str);
  }

  retval = AMGX_finalize_plugins();
  if (retval != AMGX_RC_OK) {
    AMGX_get_error_string(retval, err_str, 4096);
    bft_printf(_(warning_fmt), "AMGX_finallize_plugins", (int)retval, err_str);
  }

  retval = AMGX_finalize();
  if (retval != AMGX_RC_OK) {
    AMGX_get_error_string(retval, err_str, 4096);
    bft_printf(_(warning_fmt), "AMGX_finallize", (int)retval, err_str);
  }

#if defined(HAVE_MPI)
  if (_amgx_comm != MPI_COMM_NULL) {
    MPI_Comm_free(&_amgx_comm);
    _amgx_comm = MPI_COMM_NULL;
  }
#endif

}

/*----------------------------------------------------------------------------*/
/*!
 * \brief Load AMGX solver configuration.
 *
 * \param[in, out]  c   pointer to AMGX solver info and context
 */
/*----------------------------------------------------------------------------*/

static void
_load_solver_config(cs_sles_amgx_t  *c)
{
  char err_str[4096];
  const char error_fmt[] = N_("%s returned %d.\n"
                              "%s");
  AMGX_RC retval = AMGX_RC_OK;

  if (c->solver_config_file == NULL) {
    retval = AMGX_config_create(&(c->solver_config),
                                cs_sles_amgx_get_config(c));
    if (retval != AMGX_RC_OK) {
      AMGX_get_error_string(retval, err_str, 4096);
      bft_error(__FILE__, __LINE__, 0, _(error_fmt),
                "AMGX_config_create", retval, err_str);
    }
  }
  else {
    retval = AMGX_config_create_from_file(&(c->solver_config),
                                          c->solver_config_file);
    if (retval != AMGX_RC_OK) {
      AMGX_get_error_string(retval, err_str, 4096);
      bft_error(__FILE__, __LINE__, 0, _(error_fmt),
                "AMGX_config_create_from_file", retval, err_str);
    }
  }
}

/*! (DOXYGEN_SHOULD_SKIP_THIS) \endcond */

/*============================================================================
 * Public function definitions
 *============================================================================*/

/*----------------------------------------------------------------------------*/
/*!
 * \brief Define and associate an AMGX linear system solver
 *        for a given field or equation name.
 *
 * If this system did not previously exist, it is added to the list of
 * "known" systems. Otherwise, its definition is replaced by the one
 * defined here.
 *
 * This is a utility function: if finer control is needed, see
 * \ref cs_sles_define and \ref cs_sles_amgx_create.
 *
 * In case of rotational periodicity for a block (non-scalar) matrix,
 * the matrix type will be forced to MATSHELL ("shell") regardless
 * of the option used.
 *
 * Note that this function returns a pointer directly to the solver
 * management structure. This may be used to set further options.
 * If needed, \ref cs_sles_find may be used to obtain a pointer to the matching
 * \ref cs_sles_t container.
 *
 * \param[in]      f_id          associated field id, or < 0
 * \param[in]      name          associated name if f_id < 0, or NULL
 * \param[in,out]  context       pointer to optional (untyped) value or
 *                               structure for setup_hook, or NULL
 *
 * \return  pointer to newly created AMGX solver info object.
 */
/*----------------------------------------------------------------------------*/

cs_sles_amgx_t *
cs_sles_amgx_define(int           f_id,
                     const char  *name,
                     void        *context)
{
  cs_sles_amgx_t * c = cs_sles_amgx_create(context);

  cs_sles_t *sc = cs_sles_define(f_id,
                                 name,
                                 c,
                                 "cs_sles_amgx_t",
                                 cs_sles_amgx_setup,
                                 cs_sles_amgx_solve,
                                 cs_sles_amgx_free,
                                 cs_sles_amgx_log,
                                 cs_sles_amgx_copy,
                                 cs_sles_amgx_destroy);

  CS_NO_WARN_IF_UNUSED(sc);

  return c;
}

/*----------------------------------------------------------------------------*/
/*!
 * \brief Create AMGX linear system solver info and context.
 *
 * In case of rotational periodicity for a block (non-scalar) matrix,
 * the matrix type will be forced to MATSHELL ("shell") regardless
 * of the option used.
 *
 * \param[in,out]  context       pointer to optional (untyped) value or
 *                               structure for setup_hook, or NULL
 *
 * \return  pointer to associated linear system object.
 */
/*----------------------------------------------------------------------------*/

cs_sles_amgx_t *
cs_sles_amgx_create(void  *context)

{
  cs_sles_amgx_t *c;

  if (_n_amgx_systems < 1)
    _amgx_initialize();

  _n_amgx_systems += 1;

  BFT_MALLOC(c, 1, cs_sles_amgx_t);
  c->n_setups = 0;
  c->n_solves = 0;
  c->n_iterations_last = 0;
  c->n_iterations_min = 0;
  c->n_iterations_max = 0;
  c->n_iterations_tot = 0;

  CS_TIMER_COUNTER_INIT(c->t_setup);
  CS_TIMER_COUNTER_INIT(c->t_solve);

  /* Options */

  c->hook_context = context;

  /* Setup data */

  c->setup_data = NULL;

  c->solver_config_file = NULL;
  c->solver_config_string = NULL;

  cs_sles_amgx_set_use_device(c, true);

  c->pin_memory = true;

  return c;
}

/*----------------------------------------------------------------------------*/
/*!
 * \brief Create AMGX linear system solver info and context
 *        based on existing info and context.
 *
 * \param[in]  context  pointer to reference info and context
 *                     (actual type: cs_sles_amgx_t  *)
 *
 * \return  pointer to newly created solver info object.
 *          (actual type: cs_sles_amgx_t  *)
 */
/*----------------------------------------------------------------------------*/

void *
cs_sles_amgx_copy(const void  *context)
{
  cs_sles_amgx_t *d = NULL;

  if (context != NULL) {
    const cs_sles_amgx_t *c = context;
    d = cs_sles_amgx_create(c->hook_context);
  }

  return d;
}

/*----------------------------------------------------------------------------*/
/*!
 * \brief Destroy AMGX linear system solver info and context.
 *
 * \param[in, out]  context  pointer to AMGX solver info and context
 *                           (actual type: cs_sles_amgx_t  **)
 */
/*----------------------------------------------------------------------------*/

void
cs_sles_amgx_destroy(void **context)
{
  cs_sles_amgx_t *c = (cs_sles_amgx_t *)(*context);
  if (c != NULL) {

    /* Free local strings */

    BFT_FREE(c->solver_config_file);
    BFT_FREE(c->solver_config_string);

    if (c->n_setups >= 1)
      AMGX_config_destroy(c->solver_config);

    /* Free structure */

    cs_sles_amgx_free(c);
    BFT_FREE(c);
    *context = c;

    _n_amgx_systems -= 1;
    if (_n_amgx_systems == 0)
      _amgx_finalize();

  }
}

/*----------------------------------------------------------------------------*/
/*!
 * \brief Return the resources configuration string for AMGX.
 *
 * Check the AMGX docummentation for configuration strings syntax.
 *
 * \return  configuration string
 */
/*----------------------------------------------------------------------------*/

const char *
cs_sles_amgx_get_config_resources(void)
{
  if (_resource_config_string == NULL) {

    if (cs_glob_n_ranks > 1)
      cs_sles_amgx_set_config_resources
        ("communicator=MPI, min_rows_latency_hiding=10000");
    else
      cs_sles_amgx_set_config_resources("min_rows_latency_hiding=10000");

  }

  return _resource_config_string;
}

/*----------------------------------------------------------------------------*/
/*!
 * \brief Define the resources configuration for AMGX.
 *
 * Check the AMGX docummentation for configuration strings syntax.
 *
 * This function must be called before the first system using AMGX is set up.
 * If it is not called, or called after that point, a default configuration
 * will be used.
 *
 * \param[in]   config  string defining resources to use
 */
/*----------------------------------------------------------------------------*/

void
cs_sles_amgx_set_config_resources(const char  *config)
{
  assert(config != NULL);

  size_t l = strlen(config);

  BFT_REALLOC(_resource_config_string, l, char);
  strncpy(_resource_config_string, config, l);
  _resource_config_string[l] = '\0';
}

/*----------------------------------------------------------------------------*/
/*!
 * \brief return the solver configuration for an AMGX solver.
 *
 * Check the AMGX docummentation for configuration strings syntax.
 *
 * \param[in, out]  context  pointer to AMGX solver info and context
 *
 * \return  configuration string
 */
/*----------------------------------------------------------------------------*/

const char *
cs_sles_amgx_get_config(void  *context)
{
  cs_sles_amgx_t  *c = context;

  if (   c->solver_config_file == NULL
      && c->solver_config_string == NULL) {

    const char config[] =
      "config_version=2, "
      "solver=PCGF, "
      "max_iters=100, "
      "norm=L2, "
      "convergence=RELATIVE_INI_CORE, "
      "monitor_residual=1, "
      "tolerance=1e-8, "
      "preconditioner(amg_solver)=AMG, "
      "amg_solver:algorithm=CLASSICAL, "
      "amg_solver:max_iters=2, "
      "amg_solver:presweeps=1, "
      "amg_solver:postsweeps=1, "
      "amg_solver:cycle=V, "
      "print_solve_stats=1, "
      "print_grid_stats=1, "
      "obtain_timings=1";

    cs_sles_amgx_set_config(context, config);

  }

  return c->solver_config_string;
}

/*----------------------------------------------------------------------------*/
/*!
 * \brief Define the solver configuration for an AMGX solver.
 *
 * Check the AMGX docummentation for configuration strings syntax.
 *
 * If this function is not called, a default configuration will be used.
 *
 * \param[in, out]  context  pointer to AMGX solver info and context
 * \param[in]       config   string defining configuration to use
 */
/*----------------------------------------------------------------------------*/

void
cs_sles_amgx_set_config(void        *context,
                        const char  *config)
{
  cs_sles_amgx_t  *c = context;

  size_t l = strlen(config);

  BFT_REALLOC(c->solver_config_string, l+1, char);
  strncpy(c->solver_config_string, config, l);
  c->solver_config_string[l] = '\0';
}

/*----------------------------------------------------------------------------*/
/*!
 * \brief return the name of the solver configuration file for an AMGX solver.
 *
 * Check the AMGX docummentation for configuration file syntax.
 *
 * \param[in, out]  context  pointer to AMGX solver info and context
 *
 * \return  configuration file name, or NULL
 */
/*----------------------------------------------------------------------------*/

const char *
cs_sles_amgx_get_config_file(void  *context)
{
  cs_sles_amgx_t  *c = context;

  return c->solver_config_file;
}

/*----------------------------------------------------------------------------*/
/*!
 * \brief Set the solver configuration file for an AMGX solver.
 *
 * Check the AMGX docummentation for configuration file syntax.
 *
 * If this function is not called, a default configuration will be used.
 *
 * \param[in, out]  context  pointer to AMGX solver info and context
 * \param[in]       path     path to configuration file
 */
/*----------------------------------------------------------------------------*/

void
cs_sles_amgx_set_config_file(void        *context,
                             const char  *path)
{
  cs_sles_amgx_t  *c = context;

  size_t l = strlen(path);

  BFT_REALLOC(c->solver_config_file, l+1, char);
  strncpy(c->solver_config_file, path, l);
  c->solver_config_file[l] = '\0';
}

/*----------------------------------------------------------------------------*/
/*!
 * \brief Indicate whether an AMGX solver should pin host memory.
 *
 * By default, memory will be pinned for faster transfers, but by calling
 * this function with "use_device = false", only the host will be used.
 *
 * \param[in]  context  pointer to AMGX solver info and context
 *
 * \return  true for device, false for host only
 */
/*----------------------------------------------------------------------------*/

bool
cs_sles_amgx_get_pin_memory(void  *context)
{
  cs_sles_amgx_t  *c = context;

  return c->pin_memory;
}

/*----------------------------------------------------------------------------*/
/*!
 * \brief Define whether an AMGX solver should pin host memory.
 *
 * By default, memory will be pinned for faster transfers, but by calling
 * this function with "pin_memory = false", thie may be deactivated.
 *
 * \param[in, out]  context       pointer to AMGX solver info and context
 * \param[in]       pin_memory   true for devince, false for host only
 */
/*----------------------------------------------------------------------------*/

void
cs_sles_amgx_set_pin_memory(void  *context,
                            bool   pin_memory)
{
  cs_sles_amgx_t  *c = context;

  c->pin_memory = pin_memory;
}

/*----------------------------------------------------------------------------*/
/*!
 * \brief Query whether an AMGX solver should use the device or host.
 *
 * \param[in]  context  pointer to AMGX solver info and context
 *
 * \return  true for device, false for host only
 */
/*----------------------------------------------------------------------------*/

bool
cs_sles_amgx_get_use_device(void  *context)
{
  cs_sles_amgx_t  *c = context;
  bool use_device = true;

  if (   c->amgx_mode == AMGX_mode_hDDI
      || c->amgx_mode == AMGX_mode_hFFI) {
    use_device = false;
  }

  return use_device;
}

/*----------------------------------------------------------------------------*/
/*!
 * \brief Define whether an AMGX solver should use the device or host.
 *
 * By default, the device will be used, but by calling this function
 * with "use_device = false", only the host will be used.
 *
 * \param[in, out]  context       pointer to AMGX solver info and context
 * \param[in]       use_device   true for devince, false for host only
 */
/*----------------------------------------------------------------------------*/

void
cs_sles_amgx_set_use_device(void  *context,
                            bool   use_device)
{
  cs_sles_amgx_t  *c = context;

  if (use_device) {
    if (sizeof(cs_real_t) == sizeof(double))
      c->amgx_mode = AMGX_mode_dDDI;
    else if (sizeof(cs_real_t) == sizeof(float))
      c->amgx_mode = AMGX_mode_dFFI;
  }

  else {  /* To run on host instead of device */
    if (sizeof(cs_real_t) == sizeof(double))
      c->amgx_mode = AMGX_mode_hDDI;
    else if (sizeof(cs_real_t) == sizeof(float))
      c->amgx_mode = AMGX_mode_hFFI;
  }
}

/*----------------------------------------------------------------------------*/
/*!
 * \brief Setup AMGX linear equation solver.
 *
 * \param[in, out]  context    pointer to AMGX solver info and context
 *                             (actual type: cs_sles_amgx_t  *)
 * \param[in]       name       pointer to system name
 * \param[in]       a          associated matrix
 * \param[in]       verbosity  associated verbosity
 */
/*----------------------------------------------------------------------------*/

void
cs_sles_amgx_setup(void               *context,
                   const char         *name,
                   const cs_matrix_t  *a,
                   int                 verbosity)
{
  CS_NO_WARN_IF_UNUSED(verbosity);

  cs_timer_t t0;
  t0 = cs_timer_time();

  char err_str[4096];
  const char error_fmt[] = N_("%s returned %d.\n"
                              "%s");

  cs_sles_amgx_t  *c = context;
  cs_sles_amgx_setup_t *sd = c->setup_data;

  if (sd == NULL) {
    BFT_MALLOC(c->setup_data, 1, cs_sles_amgx_setup_t);
    sd = c->setup_data;
  }

  if (c->n_setups < 1)
    _load_solver_config(c);

  const cs_matrix_type_t cs_mat_type = cs_matrix_get_type(a);
  const int n_rows = cs_matrix_get_n_rows(a);
  const int db_size = cs_matrix_get_diag_block_size(a)[0];
  const cs_halo_t *halo = cs_matrix_get_halo(a);

  bool have_perio = false;
  if (halo != NULL) {
    if (halo->n_transforms > 0)
      have_perio = true;
  }

  if (sizeof(cs_lnum_t) != sizeof(int))
    bft_error
      (__FILE__, __LINE__, 0,
       _("AMGX bindings are not currently handled for code_saturne builds\n"
         "using long cs_lnumt_ types (i.e. --enable-long-lnum)."));

  /* Periodicity is not handled (at least not) in serial mode, as the matrix
     is not square due to ghost cells */

  if (   db_size > 1
      || (cs_mat_type != CS_MATRIX_CSR && cs_mat_type != CS_MATRIX_MSR))
    bft_error
      (__FILE__, __LINE__, 0,
       _("Matrix type %s with block size %d for system \"%s\"\n"
         "is not usable by AMGX.\n"
         "Only block size 1 with CSR or MSR format "
         "is currently supported by AMGX."),
       cs_matrix_type_name[cs_matrix_get_type(a)], db_size,
       name);

  /* TODO: handle periodicity, by renumbering local periodic cells
     so as to use the main (and not ghost) cell id */

  assert(have_perio == false);

  const cs_gnum_t *grow_id = cs_matrix_get_block_row_g_id(n_rows, halo);

  const cs_lnum_t *a_row_index, *a_col_id;

  int *col_gid = NULL;
  const cs_real_t *a_val = NULL, *a_d_val = NULL;

  if (cs_mat_type == CS_MATRIX_CSR)
    cs_matrix_get_csr_arrays(a, &a_row_index, &a_col_id, &a_val);
  else if (cs_mat_type == CS_MATRIX_MSR)
    cs_matrix_get_msr_arrays(a, &a_row_index, &a_col_id, &a_d_val, &a_val);

  BFT_MALLOC(col_gid, a_row_index[n_rows], int);

  for (cs_lnum_t j = 0; j < n_rows; j++) {
    for (cs_lnum_t i = a_row_index[j]; i < a_row_index[j+1]; ++i)
      col_gid[i] = grow_id[a_col_id[i]];
  }

  const int   *row_index = a_row_index;
  int         *_row_index = NULL;

  if (sizeof(int) != sizeof(cs_lnum_t)) {
    BFT_MALLOC(_row_index, n_rows, int);
    for (cs_lnum_t i = 0; i < n_rows; i++)
      _row_index[i] = a_row_index[i];
    row_index = row_index;
  }

  /* Matrix */

  AMGX_RC retval;

  retval = AMGX_matrix_create(&(sd->matrix),
                              _amgx_resources,
                              c->amgx_mode);

  if (retval != AMGX_RC_OK) {
    AMGX_get_error_string(retval, err_str, 4096);
    bft_error(__FILE__, __LINE__, 0, _(error_fmt),
              "AMGX_matrix_create", retval, err_str);
  }

  if (cs_glob_n_ranks > 1) {

    int *send_sizes, *recv_sizes;
    BFT_MALLOC(send_sizes, halo->n_c_domains, int);
    BFT_MALLOC(recv_sizes, halo->n_c_domains, int);
    for (int i = 0; i < halo->n_c_domains; i++) {
      send_sizes[i] =   halo->send_index[2*i + 1]
                      - halo->send_index[2*i];
      recv_sizes[i] =   halo->index[2*i + 1]
                      - halo->index[2*i];
    }
    int **send_maps, **recv_maps;
    BFT_MALLOC(send_maps, halo->n_c_domains, int *);
    BFT_MALLOC(recv_maps, halo->n_c_domains, int *);

    assert(sizeof(cs_lnum_t) == sizeof(int));

    for (int i = 0; i < halo->n_c_domains; i++) {
      BFT_MALLOC(send_maps[i], send_sizes[i], int);
      int *_send_map = send_maps[i];
      for (int j = 0; j < send_sizes[i]; j++)
        _send_map[j] = halo->send_list[halo->send_index[2*i] + j];
      BFT_MALLOC(recv_maps[i], recv_sizes[i], int);
      int *_recv_map = recv_maps[i];
      for (int j = 0; j < recv_sizes[i]; j++)
        _recv_map[j] = halo->index[2*i] + j;
    }

    retval = AMGX_matrix_comm_from_maps_one_ring(sd->matrix,
                                                 1, /* allocated_halo_depth */
                                                 halo->n_c_domains,
                                                 halo->c_domain_rank,
                                                 send_sizes,
                                                 (const int **)send_maps,
                                                 recv_sizes,
                                                 (const int **)recv_maps);

    if (retval != AMGX_RC_OK) {
      AMGX_get_error_string(retval, err_str, 4096);
      bft_error(__FILE__, __LINE__, 0, _(error_fmt),
                "AMGX_matrix_comm_from_maps_one_ring", retval, err_str);
    }

    for (int i = 0; i < halo->n_c_domains; i++) {
      BFT_FREE(recv_maps[i]);
      BFT_FREE(send_maps[i]);
    }
    BFT_FREE(recv_sizes);
    BFT_FREE(send_sizes);

  }

  const int b_mem_size = cs_matrix_get_diag_block_size(a)[3] *sizeof(cs_real_t);

  if (c->pin_memory) {
    AMGX_pin_memory(row_index, (n_rows+1)*sizeof(int));
    AMGX_pin_memory(col_gid, a_row_index[n_rows]*sizeof(int));
    AMGX_pin_memory(a_val, a_row_index[n_rows]*b_mem_size);
    if (a_d_val != NULL)
      AMGX_pin_memory(a_d_val, n_rows*b_mem_size);
  }

  retval = AMGX_matrix_upload_all(sd->matrix,
                                  n_rows,
                                  cs_matrix_get_n_entries(a),
                                  db_size,
                                  db_size,
                                  row_index,
                                  col_gid,
                                  a_val,
                                  a_d_val);

  if (retval != AMGX_RC_OK) {
    AMGX_get_error_string(retval, err_str, 4096);
    bft_error(__FILE__, __LINE__, 0, _(error_fmt),
              "AMGX_matrix_upload_all", retval, err_str);
  }

  if (c->pin_memory) {
    if (a_d_val != NULL)
      AMGX_unpin_memory(a_d_val);
    AMGX_unpin_memory(a_val);
    AMGX_unpin_memory(col_gid);
    AMGX_unpin_memory(row_index);
  }

  BFT_FREE(_row_index);
  BFT_FREE(col_gid);

  /* Solver */

  retval = AMGX_solver_create(&(sd->solver),
                              _amgx_resources,
                              c->amgx_mode,
                              c->solver_config);

  if (retval != AMGX_RC_OK) {
    AMGX_get_error_string(retval, err_str, 4096);
    bft_error(__FILE__, __LINE__, 0, _(error_fmt),
              "AMGX_solver_create", retval, err_str);
  }

  retval = AMGX_solver_setup(sd->solver, sd->matrix);

  if (retval != AMGX_RC_OK) {
    AMGX_get_error_string(retval, err_str, 4096);
    bft_error(__FILE__, __LINE__, 0, _(error_fmt),
              "AMGX_solver_setup", retval, err_str);
  }

  sd->r_norm = -1;

  /* Update return values */
  c->n_setups += 1;

  cs_timer_t t1 = cs_timer_time();
  cs_timer_counter_add_diff(&(c->t_setup), &t0, &t1);
}

/*----------------------------------------------------------------------------*/
/*!
 * \brief Call AMGX linear equation solver.
 *
 * \warning The precision, r_norm, and n_iter parameters are ignored here.
 *          the matching configuration options should be set earlier, using
 *          the \ref cs_sles_amgx_set_config function
 *
 * \param[in, out]  context        pointer to AMGX solver info and context
 *                                 (actual type: cs_sles_amgx_t  *)
 * \param[in]       name           pointer to system name
 * \param[in]       a              matrix
 * \param[in]       verbosity      associated verbosity
 * \param[in]       rotation_mode  halo update option for rotational periodicity
 * \param[in]       precision      solver precision
 * \param[in]       r_norm         residue normalization
 * \param[out]      n_iter         number of "equivalent" iterations
 * \param[out]      residue        residue
 * \param[in]       rhs            right hand side
 * \param[in, out]  vx             system solution
 * \param[in]       aux_size       number of elements in aux_vectors (in bytes)
 * \param           aux_vectors    optional working area
 *                                 (internal allocation if NULL)
 *
 * \return  convergence state
 */
/*----------------------------------------------------------------------------*/

cs_sles_convergence_state_t
cs_sles_amgx_solve(void                *context,
                   const char          *name,
                   const cs_matrix_t   *a,
                   int                  verbosity,
                   cs_halo_rotation_t   rotation_mode,
                   double               precision,
                   double               r_norm,
                   int                 *n_iter,
                   double              *residue,
                   const cs_real_t     *rhs,
                   cs_real_t           *vx,
                   size_t               aux_size,
                   void                *aux_vectors)
{
  CS_UNUSED(aux_size);
  CS_UNUSED(aux_vectors);

  char err_str[4096];
  const char error_fmt[] = N_("%s returned %d.\n"
                              "%s");

  cs_sles_convergence_state_t cvg = CS_SLES_ITERATING;

  cs_timer_t t0;
  t0 = cs_timer_time();

  cs_sles_amgx_t  *c = context;
  cs_sles_amgx_setup_t  *sd = c->setup_data;

  if (sd == NULL) {
    cs_sles_amgx_setup(c, name, a, verbosity);
    sd = c->setup_data;
  }

  AMGX_vector_handle  x, b;

  sd->r_norm = r_norm;

  int       its = -1;
  double    _residue = -1;
  const int n_rows = cs_matrix_get_n_rows(a);
  const int n_cols = cs_matrix_get_n_columns(a);
  const int  db_size = cs_matrix_get_diag_block_size(a)[0];

  if (rotation_mode != CS_HALO_ROTATION_COPY) {
    if (db_size > 1)
      bft_error(__FILE__, __LINE__, 0,
        _("Rotation mode %d with block size %d for system \"%s\"\n"
          "is not usable by AMGX."),
          rotation_mode, db_size, name);
  }

  /* Vector */

  AMGX_vector_create(&x, _amgx_resources, c->amgx_mode);
  AMGX_vector_create(&b, _amgx_resources, c->amgx_mode);

  if (cs_glob_n_ranks > 1) {
    AMGX_vector_bind(x, c->setup_data->matrix);
    AMGX_vector_bind(b, c->setup_data->matrix);
  }

  unsigned int n_bytes = n_rows*db_size*sizeof(cs_real_t);

  if (c->pin_memory) {
    AMGX_pin_memory(vx, n_bytes);
    AMGX_pin_memory(rhs, n_bytes);
  }

  AMGX_RC retval = AMGX_vector_upload(x, n_rows, db_size, vx);
  if (retval != AMGX_RC_OK) {
    AMGX_get_error_string(retval, err_str, 4096);
    bft_error(__FILE__, __LINE__, 0, _(error_fmt),
              "AMGX_vector_upload", retval, err_str);
  }

  retval = AMGX_vector_upload(b, n_rows, db_size, rhs);
  if (retval != AMGX_RC_OK) {
    AMGX_get_error_string(retval, err_str, 4096);
    bft_error(__FILE__, __LINE__, 0, _(error_fmt),
              "AMGX_vector_upload", retval, err_str);
  }

  /* Resolution */

  cs_fp_exception_disable_trap();

  AMGX_solver_solve(sd->solver, b, x);

  retval = AMGX_vector_download(x, vx);
  if (retval != AMGX_RC_OK) {
    AMGX_get_error_string(retval, err_str, 4096);
    bft_error(__FILE__, __LINE__, 0, _(error_fmt),
              "AMGX_vector_download", retval, err_str);
  }

  AMGX_vector_destroy(x);
  AMGX_vector_destroy(b);

  AMGX_solver_get_iterations_number(sd->solver, &its);
  // AMGX_solver_get_iteration_residual(sd->solver, its, 0, &_residue);

  if (c->pin_memory) {
    AMGX_unpin_memory(vx);
    AMGX_unpin_memory(rhs);
  }

  AMGX_SOLVE_STATUS  solve_status;
  AMGX_solver_get_status(sd->solver, &solve_status);

  switch(solve_status) {
  case AMGX_SOLVE_SUCCESS:
    cvg = CS_SLES_CONVERGED;
    break;
  case AMGX_SOLVE_FAILED:
    cvg = CS_SLES_DIVERGED;
    break;
  case AMGX_SOLVE_DIVERGED:
    if (its >= c->n_iterations_max)
      cvg = CS_SLES_MAX_ITERATION;
    else
      cvg = CS_SLES_DIVERGED;
  }

  cs_fp_exception_restore_trap();

  *residue = _residue;
  *n_iter = its;

  /* Update return values */

  if (c->n_solves == 0)
    c->n_iterations_min = its;

  c->n_iterations_last = its;
  c->n_iterations_tot += its;
  if (c->n_iterations_min > its)
    c->n_iterations_min = its;
  if (c->n_iterations_max < its)
    c->n_iterations_max = its;
  c->n_solves += 1;
  cs_timer_t t1 = cs_timer_time();
  cs_timer_counter_add_diff(&(c->t_solve), &t0, &t1);

  return cvg;
}

/*----------------------------------------------------------------------------*/
/*!
 * \brief Free AMGX linear equation solver setup context.
 *
 * This function frees resolution-related data, such as
 * buffers and preconditioning but does not free the whole context,
 * as info used for logging (especially performance data) is maintained.
 *
 * \param[in, out]  context  pointer to AMGX solver info and context
 *                           (actual type: cs_sles_amgx_t  *)
 */
/*----------------------------------------------------------------------------*/

void
cs_sles_amgx_free(void  *context)
{
  cs_timer_t t0;
  t0 = cs_timer_time();

  cs_sles_amgx_t  *c  = context;
  cs_sles_amgx_setup_t *sd = c->setup_data;

  if (sd != NULL) {

    AMGX_solver_destroy(sd->solver);
    AMGX_matrix_destroy(sd->matrix);

  }
  if (c->setup_data != NULL)
    BFT_FREE(c->setup_data);

  cs_timer_t t1 = cs_timer_time();
  cs_timer_counter_add_diff(&(c->t_setup), &t0, &t1);
}

/*----------------------------------------------------------------------------*/
/*!
 * \brief Log sparse linear equation solver info.
 *
 * \param[in]  context   pointer to AMGX solver info and context
 *                       (actual type: cs_sles_amgx_t  *)
 * \param[in]  log_type  log type
 */
/*----------------------------------------------------------------------------*/

void
cs_sles_amgx_log(const void  *context,
                 cs_log_t     log_type)
{
  const cs_sles_amgx_t  *c = context;

  const char m_type[] = "CSR";

  if (log_type == CS_LOG_SETUP) {

    cs_log_printf(log_type,
                  _("  Solver type:                       AMGX\n"
                    "    Matrix format:                     %s\n"),
                  m_type);

  }
  else if (log_type == CS_LOG_PERFORMANCE) {

    int n_calls = c->n_solves;
    int n_it_min = c->n_iterations_min;
    int n_it_max = c->n_iterations_max;
    int n_it_mean = 0;

    if (n_calls > 0)
      n_it_mean = (int)(  c->n_iterations_tot
                        / ((unsigned long long)n_calls));

    cs_log_printf(log_type,
                  _("\n"
                    "  Solver type:                   AMGX\n"
                    "    Matrix format:               %s\n"
                    "  Number of setups:              %12d\n"
                    "  Number of calls:               %12d\n"
                    "  Minimum number of iterations:  %12d\n"
                    "  Maximum number of iterations:  %12d\n"
                    "  Mean number of iterations:     %12d\n"
                    "  Total setup time:              %12.3f\n"
                    "  Total solution time:           %12.3f\n"),
                  m_type,
                  c->n_setups, n_calls, n_it_min, n_it_max, n_it_mean,
                  c->t_setup.wall_nsec*1e-9,
                  c->t_solve.wall_nsec*1e-9);

  }
}

/*----------------------------------------------------------------------------*/

END_C_DECLS
