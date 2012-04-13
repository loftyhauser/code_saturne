/*============================================================================
 * Main structure for a nodal representation associated with a mesh
 *============================================================================*/

/*
  This file is part of Code_Saturne, a general-purpose CFD tool.

  Copyright (C) 1998-2012 EDF S.A.

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

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/*----------------------------------------------------------------------------
 * BFT library headers
 *----------------------------------------------------------------------------*/

#include <bft_mem.h>
#include <bft_printf.h>

/*----------------------------------------------------------------------------
 *  Local headers
 *----------------------------------------------------------------------------*/

#include "fvm_defs.h"
#include "fvm_io_num.h"
#include "fvm_parall.h"
#include "fvm_tesselation.h"

/*----------------------------------------------------------------------------
 *  Header for the current file
 *----------------------------------------------------------------------------*/

#include "fvm_nodal.h"
#include "fvm_nodal_priv.h"

/*----------------------------------------------------------------------------*/

#ifdef __cplusplus
extern "C" {
#if 0
} /* Fake brace to force back Emacs auto-indentation back to column 0 */
#endif
#endif /* __cplusplus */

/*============================================================================
 * Static global variables
 *============================================================================*/

/* Number of vertices associated with each "nodal" element type */

const int  fvm_nodal_n_vertices_element[] = {2,   /* Edge */
                                             3,   /* Triangle */
                                             4,   /* Quadrangle */
                                             0,   /* Simple polygon */
                                             4,   /* Tetrahedron */
                                             5,   /* Pyramid */
                                             6,   /* Prism */
                                             8,   /* Hexahedron */
                                             0};  /* Simple polyhedron */

/* Number of vertices associated with each "nodal" element type */

static int  fvm_nodal_n_edges_element[] = {1,   /* Edge */
                                           3,   /* Triangle */
                                           4,   /* Quadrangle */
                                           0,   /* Simple polygon */
                                           6,   /* Tetrahedron */
                                           8,   /* Pyramid */
                                           9,   /* Prism */
                                           12,  /* Hexahedron */
                                           0};  /* Simple polyhedron */

/*============================================================================
 * Private function definitions
 *============================================================================*/

/*----------------------------------------------------------------------------
 * Compare edges (qsort function).
 *
 * parameters:
 *   x <-> pointer to first edge definition
 *   y <-> pointer to second edge definition
 *
 * returns:
 *   result of strcmp() on group names
 *----------------------------------------------------------------------------*/

static int _compare_edges(const void *x, const void *y)
{
  int retval = 1;

  const cs_lnum_t *e0 = x;
  const cs_lnum_t *e1 = y;

  if (e0[0] < e1[0])
    retval = -1;

  else if (e0[0] == e1[0]) {
    if (e0[1] < e1[1])
      retval = -1;
    else if (e0[1] == e1[1])
      retval = 0;
  }

  return retval;
}

/*----------------------------------------------------------------------------
 * Copy a nodal mesh section representation structure, sharing arrays
 * with the original structure.
 *
 * parameters:
 *   this_section <-> pointer to structure that should be copied
 *
 * returns:
 *   pointer to created nodal mesh section representation structure
 *----------------------------------------------------------------------------*/

static fvm_nodal_section_t *
_fvm_nodal_section_copy(const fvm_nodal_section_t *this_section)
{
  fvm_nodal_section_t  *new_section;

  BFT_MALLOC(new_section, 1, fvm_nodal_section_t);

  /* Global information */

  new_section->entity_dim = this_section->entity_dim;

  new_section->n_elements = this_section->n_elements;
  new_section->type = this_section->type;

  /* Connectivity */

  new_section->connectivity_size = this_section->connectivity_size;
  new_section->stride = this_section->stride;

  new_section->n_faces = this_section->n_faces;

  new_section->face_index = this_section->face_index;
  new_section->face_num = this_section->face_num;
  new_section->vertex_index = this_section->vertex_index;
  new_section->vertex_num = this_section->vertex_num;

  new_section->_face_index = NULL;
  new_section->_face_num   = NULL;
  new_section->_vertex_index = NULL;
  new_section->_vertex_num = NULL;

  new_section->gc_id = NULL;

  new_section->tesselation = NULL;  /* TODO: copy tesselation */

  /* Numbering */
  /*-----------*/

  new_section->parent_element_num = this_section->parent_element_num;
  new_section->_parent_element_num = NULL;

  if (this_section->global_element_num != NULL) {
    cs_lnum_t n_ent
      = fvm_io_num_get_local_count(this_section->global_element_num);
    cs_gnum_t global_count
      = fvm_io_num_get_global_count(this_section->global_element_num);
    const cs_gnum_t *global_num
      = fvm_io_num_get_global_num(this_section->global_element_num);

    new_section->global_element_num
      = fvm_io_num_create_shared(global_num, global_count, n_ent);
  }
  else
    new_section->global_element_num = NULL;

  return (new_section);
}

/*----------------------------------------------------------------------------
 * Reduction of a nodal mesh representation section: only the associations
 * (numberings) necessary to redistribution of fields for output are
 * conserved, the full connectivity being no longer useful once it has been
 * output.
 *
 * parameters:
 *   this_section      <-> pointer to structure that should be reduced
 *
 * returns:
 *   true if connectivity has been reduced
 *----------------------------------------------------------------------------*/

static _Bool
_fvm_nodal_section_reduce(fvm_nodal_section_t  * this_section)
{
  _Bool retval = false;

  /* If we have a tesselation of polyhedra (face index != NULL),
     we may need to keep the connectivity information, to
     interpolate nodal values to added vertices */

  if (   this_section->tesselation == NULL
      || this_section->_face_index == NULL) {

      /* Connectivity */

    this_section->connectivity_size = 0;

    if (this_section->_face_index != NULL)
      BFT_FREE(this_section->_face_index);
    this_section->face_index = NULL;

    if (this_section->_face_num != NULL)
      BFT_FREE(this_section->_face_num);
    this_section->face_num = NULL;

    if (this_section->_vertex_index != NULL)
      BFT_FREE(this_section->_vertex_index);
    this_section->vertex_index = NULL;

    if (this_section->_vertex_num != NULL)
      BFT_FREE(this_section->_vertex_num);
    this_section->vertex_num = NULL;

    retval = true;
  }

  if (this_section->gc_id != NULL)
    BFT_FREE(this_section->gc_id);

  if (this_section->tesselation != NULL)
    fvm_tesselation_reduce(this_section->tesselation);

  return retval;
}

/*----------------------------------------------------------------------------
 * Change entity parent numbering; this is useful when entities of the
 * parent mesh have been renumbered after a nodal mesh representation
 * structure's creation. As the parent_num[] array is defined only when
 * non trivial (i.e. not 1, 2, ..., n), it may be allocated or freed
 * by this function. The return argument corresponds to the new
 * pointer which should replace the parent_num input argument.
 *
 * parameters:
 *   parent_num_size     <-- size of local parent numbering array
 *   new_parent_num      <-- pointer to local parent renumbering array
 *                           ({1, ..., n} <-- {1, ..., n})
 *   parent_num          <-> pointer to local parent numbering array
 *   _parent_num         <-> pointer to local parent numbering array if
 *                           owner, NULL otherwise
 *
 * returns:
 *   pointer to resulting parent_num[] array
 *----------------------------------------------------------------------------*/

static cs_lnum_t *
_renumber_parent_num(cs_lnum_t          parent_num_size,
                     const cs_lnum_t    new_parent_num[],
                     const cs_lnum_t    parent_num[],
                     cs_lnum_t          _parent_num[])
{
  int  i;
  cs_lnum_t   old_num_id;
  cs_lnum_t *parent_num_p = _parent_num;
  _Bool trivial = true;

  if (parent_num_size > 0 && new_parent_num != NULL) {

    if (parent_num_p != NULL) {
      for (i = 0; i < parent_num_size; i++) {
        old_num_id = parent_num_p[i] - 1;
        parent_num_p[i] = new_parent_num[old_num_id];
        if (parent_num_p[i] != i+1)
          trivial = false;
      }
    }
    else {
      BFT_MALLOC(parent_num_p, parent_num_size, cs_lnum_t);
      if (parent_num != NULL) {
        for (i = 0; i < parent_num_size; i++) {
          old_num_id = parent_num[i] - 1;
          parent_num_p[i] = new_parent_num[old_num_id];
          if (parent_num_p[i] != i+1)
            trivial = false;
        }
      }
      else {
        for (i = 0; i < parent_num_size; i++) {
          parent_num_p[i] = new_parent_num[i];
          if (parent_num_p[i] != i+1)
            trivial = false;
        }
      }
    }
  }

  if (trivial == true)
    BFT_FREE(parent_num_p);

  return parent_num_p;
}

/*----------------------------------------------------------------------------
 * Renumber vertices based on those actually referenced, and update
 * connectivity arrays and parent numbering in accordance.
 *
 * The number of vertices assigned to the nodal mesh (this_nodal->n_vertices)
 * is computed and set by this function. If this number was previously
 * non-zero (i.e. vertices have already been assigned to the structure),
 * those vertices are considered as referenced. This is useful if we want
 * to avoid discarding a given set of vertices, such as when building a
 * nodal mesh representation containing only vertices.
 *
 * parameters:
 *   this_nodal <-> nodal mesh structure
 *----------------------------------------------------------------------------*/

static void
_renumber_vertices(fvm_nodal_t  *this_nodal)
{
  size_t      i;
  int         section_id;
  cs_lnum_t   j;
  cs_lnum_t   vertex_id;
  cs_lnum_t   n_vertices;
  fvm_nodal_section_t  *section;

  cs_lnum_t   *loc_vertex_num = NULL;
  cs_lnum_t    max_vertex_num = 0;

  /* Find maximum vertex reference */
  /*-------------------------------*/

  /* The mesh may already contain direct vertex references
     (as in the case of a "mesh" only containing vertices) */

  if (this_nodal->n_vertices > 0) {
    if (this_nodal->parent_vertex_num != NULL) {
      for (j = 0; j < this_nodal->n_vertices; j++) {
        if (this_nodal->parent_vertex_num[j] > max_vertex_num)
          max_vertex_num = this_nodal->parent_vertex_num[j];
      }
    }
    else
      max_vertex_num = this_nodal->n_vertices;
  }

  /* In most cases, the mesh will reference vertices through elements */

  for (section_id = 0; section_id < this_nodal->n_sections; section_id++) {
    section = this_nodal->sections[section_id];
    if (this_nodal->parent_vertex_num != NULL) {
      for (i = 0; i < section->connectivity_size; i++) {
        cs_lnum_t vertex_num
          = this_nodal->parent_vertex_num[section->vertex_num[i] - 1];
        if (vertex_num > max_vertex_num)
          max_vertex_num = vertex_num;
      }
    }
    else {
      for (i = 0; i < section->connectivity_size; i++) {
        if (section->vertex_num[i] > max_vertex_num)
          max_vertex_num = section->vertex_num[i];
      }
    }
  }

  /* Flag referenced vertices and compute size */
  /*-------------------------------------------*/

  BFT_MALLOC(loc_vertex_num, max_vertex_num, cs_lnum_t);

  for (vertex_id = 0; vertex_id < max_vertex_num; vertex_id++)
    loc_vertex_num[vertex_id] = 0;

  if (this_nodal->n_vertices > 0) {
    if (this_nodal->parent_vertex_num != NULL) {
      for (j = 0; j < this_nodal->n_vertices; j++) {
        vertex_id = this_nodal->parent_vertex_num[j] - 1;
        if (loc_vertex_num[vertex_id] == 0)
          loc_vertex_num[vertex_id] = 1;
      }
    }
    else {
      for (j = 0; j < this_nodal->n_vertices; j++) {
        if (loc_vertex_num[j] == 0)
          loc_vertex_num[j] = 1;
      }
    }
  }

  for (section_id = 0; section_id < this_nodal->n_sections; section_id++) {
    section = this_nodal->sections[section_id];
    if (this_nodal->parent_vertex_num != NULL) {
      for (i = 0; i < section->connectivity_size; i++) {
        vertex_id
          = this_nodal->parent_vertex_num[section->vertex_num[i] - 1] - 1;
        if (loc_vertex_num[vertex_id] == 0)
          loc_vertex_num[vertex_id] = 1;
      }
    }
    else {
      for (i = 0; i < section->connectivity_size; i++) {
        vertex_id = section->vertex_num[i] - 1;
        if (loc_vertex_num[vertex_id] == 0)
          loc_vertex_num[vertex_id] = 1;
      }
    }
  }

  /* Build vertices renumbering */
  /*----------------------------*/

  n_vertices = 0;

  for (vertex_id = 0; vertex_id < max_vertex_num; vertex_id++) {
    if (loc_vertex_num[vertex_id] == 1) {
      n_vertices += 1;
      loc_vertex_num[vertex_id] = n_vertices;
    }
  }
  this_nodal->n_vertices = n_vertices;

  /* Update connectivity and vertex parent numbering */
  /*-------------------------------------------------*/

  /* If all vertices are flagged, no need to renumber */

  if (n_vertices == max_vertex_num)
    BFT_FREE(loc_vertex_num);

  else {

    /* Update connectivity */

    for (section_id = 0; section_id < this_nodal->n_sections; section_id++) {
      section = this_nodal->sections[section_id];
      if (section->_vertex_num == NULL)
        fvm_nodal_section_copy_on_write(section, false, false, false, true);
      if (this_nodal->parent_vertex_num != NULL) {
        for (i = 0; i < section->connectivity_size; i++) {
          vertex_id
            = this_nodal->parent_vertex_num[section->vertex_num[i] - 1] - 1;
          section->_vertex_num[i] = loc_vertex_num[vertex_id];
        }
      }
      else {
        for (i = 0; i < section->connectivity_size; i++) {
          vertex_id = section->vertex_num[i] - 1;
          section->_vertex_num[i] = loc_vertex_num[vertex_id];
        }
      }
    }

    /* Build or update vertex parent numbering */

    this_nodal->parent_vertex_num = NULL;
    if (this_nodal->_parent_vertex_num != NULL)
      BFT_FREE(this_nodal->_parent_vertex_num);

    if (loc_vertex_num != NULL) {
      BFT_MALLOC(this_nodal->_parent_vertex_num, n_vertices, cs_lnum_t);
      for (vertex_id = 0; vertex_id < max_vertex_num; vertex_id++) {
        if (loc_vertex_num[vertex_id] > 0)
          this_nodal->_parent_vertex_num[loc_vertex_num[vertex_id] - 1]
            = vertex_id + 1;
      }
      this_nodal->parent_vertex_num = this_nodal->_parent_vertex_num;
    }
  }

  /* Free renumbering array */

  BFT_FREE(loc_vertex_num);
}

/*----------------------------------------------------------------------------
 * Dump printout of a nodal representation structure section.
 *
 * parameters:
 *   this_section <-- pointer to structure that should be dumped
 *----------------------------------------------------------------------------*/

static void
_fvm_nodal_section_dump(const fvm_nodal_section_t  *this_section)
{
  cs_lnum_t   n_elements, i, j;
  const cs_lnum_t   *idx, *num;

  /* Global indicators */
  /*--------------------*/

  bft_printf("\n"
             "Entity dimension:     %d\n"
             "Number of elements:   %ld\n"
             "Element type:         %s\n",
             this_section->entity_dim, (long)this_section->n_elements,
             fvm_elements_type_name[this_section->type]);

  bft_printf("\n"
             "Connectivity_size:     %llu\n"
             "Stride:                %d\n"
             "Number of faces:       %ld\n",
             (unsigned long long)(this_section->connectivity_size),
             this_section->stride,
             (long)(this_section->n_faces));

  bft_printf("\n"
             "Pointers to shareable arrays:\n"
             "  face_index:           %p\n"
             "  face_num:             %p\n"
             "  vertex_index:         %p\n"
             "  vertex_num:           %p\n"
             "  parent_element_num:   %p\n",
             (const void *)this_section->face_index,
             (const void *)this_section->face_num,
             (const void *)this_section->vertex_index,
             (const void *)this_section->vertex_num,
             (const void *)this_section->parent_element_num);

  bft_printf("\n"
             "Pointers to local arrays:\n"
             "  _face_index:          %p\n"
             "  _face_num:            %p\n"
             "  _vertex_index:        %p\n"
             "  _vertex_num:          %p\n"
             "  _parent_element_num:  %p\n"
             "  gc_id:                %p\n",
             (const void *)this_section->_face_index,
             (const void *)this_section->_face_num,
             (const void *)this_section->_vertex_index,
             (const void *)this_section->_vertex_num,
             (const void *)this_section->_parent_element_num,
             (const void *)this_section->gc_id);

  if (this_section->face_index != NULL) {
    bft_printf("\nPolyhedra -> faces (polygons) connectivity:\n\n");
    n_elements = this_section->n_elements;
    idx = this_section->face_index;
    num = this_section->face_num;
    for (i = 0; i < n_elements; i++) {
      bft_printf("%10d (idx = %10d) %10d\n",
                 i+1, idx[i], num[idx[i]]);
      for (j = idx[i] + 1; j < idx[i + 1]; j++)
        bft_printf("                              %10d\n", num[j]);
    }
    bft_printf("      end  (idx = %10d)\n", idx[n_elements]);
  }

  if (this_section->vertex_index != NULL) {
    cs_lnum_t   n_faces = (this_section->n_faces > 0) ?
                          this_section->n_faces : this_section->n_elements;
    bft_printf("\nPolygons -> vertices connectivity:\n\n");
    idx = this_section->vertex_index;
    num = this_section->vertex_num;
    for (i = 0; i < n_faces; i++) {
      bft_printf("%10d (idx = %10d) %10d\n",
                i + 1, idx[i], num[idx[i]]);
      for (j = idx[i] + 1; j < idx[i + 1]; j++)
        bft_printf("                              %10d\n", num[j]);
    }
    bft_printf("      end  (idx = %10d)\n", idx[n_faces]);
  }

  else {
    bft_printf("\nElements -> vertices connectivity:\n\n");
    n_elements = this_section->n_elements;
    num = this_section->vertex_num;
    switch(this_section->stride) {
    case 2:
      for (i = 0; i < n_elements; i++)
        bft_printf("%10d : %10d %10d\n",
                   i+1, num[i*2], num[i*2+1]);
      break;
    case 3:
      for (i = 0; i < n_elements; i++)
        bft_printf("%10d : %10d %10d %10d\n",
                   i+1, num[i*3], num[i*3+1], num[i*3+2]);
      break;
    case 4:
      for (i = 0; i < n_elements; i++)
        bft_printf("%10d : %10d %10d %10d %10d\n",
                   i+1, num[i*4], num[i*4+1], num[i*4+2],
                   num[i*4+3]);
      break;
    case 5:
      for (i = 0; i < n_elements; i++)
        bft_printf("%10d : %10d %10d %10d %10d %10d\n",
                   i+1, num[i*5], num[i*5+1], num[i*5+2],
                   num[i*5+3], num[i*5+4]);
      break;
    case 6:
      for (i = 0; i < n_elements; i++)
        bft_printf("%10d : %10d %10d %10d %10d %10d %10d\n",
                   i+1, num[i*6], num[i*6+1], num[i*6+2],
                   num[i*6+3], num[i*6+4], num[i*6+5]);
      break;
    case 8:
      for (i = 0; i < n_elements; i++)
        bft_printf("%10d : %10d %10d %10d %10d %10d %10d %10d %10d\n",
                   i+1, num[i*8], num[i*8+1], num[i*8+2], num[i*8+3],
                   num[i*8+4], num[i*8+5], num[i*8+6], num[i*8+7]);
      break;
    default:
      for (i = 0; i < n_elements; i++) {
        bft_printf("%10d :", i+1);
        for (j = 0; j < this_section->stride; j++)
          bft_printf(" %10d", num[i*this_section->stride + j]);
        bft_printf("\n");
      }
    }
  }

  if (this_section->gc_id != NULL) {
    bft_printf("\nGroup class ids:\n\n");
    for (i = 0; i < this_section->n_elements; i++)
      bft_printf("%10d : %10d\n", i + 1, this_section->gc_id[i]);
    bft_printf("\n");
  }

  /* Faces tesselation */

  if (this_section->tesselation != NULL)
    fvm_tesselation_dump(this_section->tesselation);

  /* Numbers of associated elements in the parent mesh */

  bft_printf("\nLocal element numbers in parent mesh:\n");
  if (this_section->parent_element_num == NULL)
    bft_printf("\n  Nil\n\n");
  else {
    for (i = 0; i < this_section->n_elements; i++)
      bft_printf("  %10d %10d\n", i+1, this_section->parent_element_num[i]);
  }

  /* Global element numbers (only for parallel execution) */

  if (this_section->global_element_num != NULL) {
    bft_printf("\nGlobal element numbers:\n");
    fvm_io_num_dump(this_section->global_element_num);
  }
}

/*============================================================================
 * Semi-private function definitions (prototypes in fvm_nodal_priv.h)
 *============================================================================*/

/*----------------------------------------------------------------------------
 * Creation of a nodal mesh section representation structure.
 *
 * parameters:
 *   type <-- type of element defined by this section
 *
 * returns:
 *   pointer to created nodal mesh section representation structure
 *----------------------------------------------------------------------------*/

fvm_nodal_section_t *
fvm_nodal_section_create(const fvm_element_t  type)
{
  fvm_nodal_section_t  *this_section;

  BFT_MALLOC(this_section, 1, fvm_nodal_section_t);

  /* Global information */

  if (type == FVM_EDGE)
    this_section->entity_dim = 1;
  else if (type >= FVM_FACE_TRIA && type <= FVM_FACE_POLY)
    this_section->entity_dim = 2;
  else
    this_section->entity_dim = 3;

  this_section->n_elements = 0;
  this_section->type = type;

  /* Connectivity */

  this_section->connectivity_size = 0;

  if (type != FVM_FACE_POLY && type != FVM_CELL_POLY)
    this_section->stride = fvm_nodal_n_vertices_element[type];
  else
    this_section->stride = 0;

  this_section->n_faces = 0;
  this_section->face_index = NULL;
  this_section->face_num   = NULL;
  this_section->vertex_index = NULL;
  this_section->vertex_num = NULL;

  this_section->_face_index = NULL;
  this_section->_face_num   = NULL;
  this_section->_vertex_index = NULL;
  this_section->_vertex_num = NULL;

  this_section->gc_id = NULL;

  this_section->tesselation = NULL;

  /* Numbering */
  /*-----------*/

  this_section->parent_element_num = NULL;
  this_section->_parent_element_num = NULL;

  this_section->global_element_num = NULL;

  return (this_section);
}

/*----------------------------------------------------------------------------
 * Destruction of a nodal mesh section representation structure.
 *
 * parameters:
 *   this_section <-> pointer to structure that should be destroyed
 *
 * returns:
 *   NULL pointer
 *----------------------------------------------------------------------------*/

fvm_nodal_section_t *
fvm_nodal_section_destroy(fvm_nodal_section_t  * this_section)
{
  /* Connectivity */

  if (this_section->_face_index != NULL)
    BFT_FREE(this_section->_face_index);
  if (this_section->_face_num != NULL)
    BFT_FREE(this_section->_face_num);

  if (this_section->_vertex_index != NULL)
    BFT_FREE(this_section->_vertex_index);
  if (this_section->_vertex_num != NULL)
    BFT_FREE(this_section->_vertex_num);

  if (this_section->gc_id != NULL)
    BFT_FREE(this_section->gc_id);

  if (this_section->tesselation != NULL)
    fvm_tesselation_destroy(this_section->tesselation);

  /* Numbering */
  /*-----------*/

  if (this_section->parent_element_num != NULL) {
    this_section->parent_element_num = NULL;
    BFT_FREE(this_section->_parent_element_num);
  }

  if (this_section->global_element_num != NULL)
    fvm_io_num_destroy(this_section->global_element_num);

  /* Main structure destroyed and NULL returned */

  BFT_FREE(this_section);

  return (this_section);
}

/*----------------------------------------------------------------------------
 * Copy selected shared connectivity information to private connectivity
 * for a nodal mesh section.
 *
 * parameters:
 *   this_section      <-> pointer to section structure
 *   copy_face_index   <-- copy face index (polyhedra only) ?
 *   copy_face_num     <-- copy face numbers (polyhedra only) ?
 *   copy_vertex_index <-- copy vertex index (polyhedra/polygons only) ?
 *   copy_vertex_num   <-- copy vertex numbers ?
 *----------------------------------------------------------------------------*/

void
fvm_nodal_section_copy_on_write(fvm_nodal_section_t  *this_section,
                                _Bool                 copy_face_index,
                                _Bool                 copy_face_num,
                                _Bool                 copy_vertex_index,
                                _Bool                 copy_vertex_num)
{
  cs_lnum_t   n_faces;
  size_t  i;

  if (copy_face_index == true
      && this_section->face_index != NULL && this_section->_face_index == NULL) {
    BFT_MALLOC(this_section->_face_index, this_section->n_elements + 1, cs_lnum_t);
    for (i = 0; i < (size_t)(this_section->n_elements + 1); i++) {
      this_section->_face_index[i] = this_section->face_index[i];
    }
    this_section->face_index = this_section->_face_index;
  }

  if (copy_face_num == true
      && this_section->face_num != NULL && this_section->_face_num == NULL) {
    n_faces = this_section->face_index[this_section->n_elements];
    BFT_MALLOC(this_section->_face_num, n_faces, cs_lnum_t);
    for (i = 0; i < (size_t)n_faces; i++) {
      this_section->_face_num[i] = this_section->face_num[i];
    }
    this_section->face_num = this_section->_face_num;
  }

  if (   copy_vertex_index == true
      && this_section->vertex_index != NULL
      && this_section->_vertex_index == NULL) {
    if (this_section->n_faces != 0)
      n_faces = this_section->n_faces;
    else
      n_faces = this_section->n_elements;
    BFT_MALLOC(this_section->_vertex_index, n_faces + 1, cs_lnum_t);
    for (i = 0; i < (size_t)n_faces + 1; i++) {
      this_section->_vertex_index[i] = this_section->vertex_index[i];
    }
    this_section->vertex_index = this_section->_vertex_index;
  }

  if (copy_vertex_num == true && this_section->_vertex_num == NULL) {
    BFT_MALLOC(this_section->_vertex_num,
               this_section->connectivity_size, cs_lnum_t);
    for (i = 0; i < this_section->connectivity_size; i++) {
      this_section->_vertex_num[i] = this_section->vertex_num[i];
    }
    this_section->vertex_num = this_section->_vertex_num;
  }

}

/*----------------------------------------------------------------------------
 * Return global number of elements associated with section.
 *
 * parameters:
 *   this_section      <-- pointer to section structure
 *
 * returns:
 *   global number of elements associated with section
 *----------------------------------------------------------------------------*/

cs_gnum_t
fvm_nodal_section_n_g_elements(const fvm_nodal_section_t  *this_section)
{
  if (this_section->global_element_num != NULL)
    return fvm_io_num_get_global_count(this_section->global_element_num);
  else
    return this_section->n_elements;
}

/*----------------------------------------------------------------------------
 * Return global number of vertices associated with nodal mesh.
 *
 * parameters:
 *   this_nodal           <-- pointer to nodal mesh structure
 *
 * returns:
 *   global number of vertices associated with nodal mesh
 *----------------------------------------------------------------------------*/

cs_gnum_t
fvm_nodal_n_g_vertices(const fvm_nodal_t  *this_nodal)
{
  cs_gnum_t   n_g_vertices;

  if (this_nodal->global_vertex_num != NULL)
    n_g_vertices = fvm_io_num_get_global_count(this_nodal->global_vertex_num);
  else
    n_g_vertices = this_nodal->n_vertices;

  return n_g_vertices;
}

/*----------------------------------------------------------------------------
 * Define cell->face connectivity for strided cell types.
 *
 * parameters:
 *   element_type     <-- type of strided element
 *   n_faces          --> number of element faces
 *   n_face_vertices  --> number of vertices of each face
 *   face_vertices    --> face -> vertex base connectivity (0 to n-1)
 *----------------------------------------------------------------------------*/

void
fvm_nodal_cell_face_connect(fvm_element_t   element_type,
                            int            *n_faces,
                            int             n_face_vertices[6],
                            int             face_vertices[6][4])
{
  int i, j;

  /* Initialization */

  *n_faces = 0;

  for (i = 0; i < 6; i++) {
    n_face_vertices[i] = 0;
    for (j = 0; j < 4; j++)
      face_vertices[i][j] = 0;
  }

  /* Define connectivity based on element type */

  switch(element_type) {

  case FVM_CELL_TETRA:
    {
      cs_lnum_t _face_vertices[4][3] = {{1, 3, 2},     /*       x 4     */
                                         {1, 2, 4},     /*      /|\      */
                                         {1, 4, 3},     /*     / | \     */
                                         {2, 3, 4}};    /*    /  |  \    */
                                                        /* 1 x- -|- -x 3 */
      for (i = 0; i < 4; i++) {                         /*    \  |  /    */
        n_face_vertices[i] = 3;                         /*     \ | /     */
        for (j = 0; j < 3; j++)                         /*      \|/      */
          face_vertices[i][j] = _face_vertices[i][j];   /*       x 2     */
      }
      *n_faces = 4;
    }
    break;

  case FVM_CELL_PYRAM:
    {
      cs_lnum_t _n_face_vertices[5] = {3, 3, 3, 3, 4};
      cs_lnum_t _face_vertices[5][4] = {{1, 2, 5, 0},  /*        5 x       */
                                         {1, 5, 4, 0},  /*         /|\      */
                                         {2, 3, 5, 0},  /*        //| \     */
                                         {3, 4, 5, 0},  /*       // |  \    */
                                         {1, 4, 3, 2}}; /*    4 x/--|---x 3 */
                                                        /*     //   |  /    */
      for (i = 0; i < 5; i++) {                         /*    //    | /     */
        n_face_vertices[i] = _n_face_vertices[i];       /* 1 x-------x 2    */
        for (j = 0; j < 4; j++)
          face_vertices[i][j] = _face_vertices[i][j];
      }
      *n_faces = 5;
    }
    break;

  case FVM_CELL_PRISM:
    {
      cs_lnum_t _n_face_vertices[5] = {3, 3, 4, 4, 4};
      cs_lnum_t _face_vertices[5][4] = {{1, 3, 2, 0},  /* 4 x-------x 6 */
                                         {4, 5, 6, 0},  /*   |\     /|   */
                                         {1, 2, 5, 4},  /*   | \   / |   */
                                         {1, 4, 6, 3},  /* 1 x- \-/ -x 3 */
                                         {2, 3, 6, 5}}; /*    \ 5x  /    */
                                                        /*     \ | /     */
      for (i = 0; i < 5; i++) {                         /*      \|/      */
        n_face_vertices[i] = _n_face_vertices[i];       /*       x 2     */
        for (j = 0; j < 4; j++)
          face_vertices[i][j] = _face_vertices[i][j];
      }
      *n_faces = 5;
    }
    break;

  case FVM_CELL_HEXA:
    {
      cs_lnum_t _n_face_vertices[6] = {4, 4, 4, 4, 4, 4};
      cs_lnum_t _face_vertices[6][4] = {{1, 4, 3, 2},  /*    8 x-------x 7 */
                                         {1, 2, 6, 5},  /*     /|      /|   */
                                         {1, 5, 8, 4},  /*    / |     / |   */
                                         {2, 3, 7, 6},  /* 5 x-------x6 |   */
                                         {3, 4, 8, 7},  /*   | 4x----|--x 3 */
                                         {5, 6, 7, 8}}; /*   | /     | /    */
      for (i = 0; i < 6; i++) {                         /*   |/      |/     */
        n_face_vertices[i] = _n_face_vertices[i];       /* 1 x-------x 2    */
        for (j = 0; j < 4; j++)
          face_vertices[i][j] = _face_vertices[i][j];
      }
      *n_faces = 6;
    }
    break;

  default:
    assert(0);
  }

  /* Switch from (1, n) to (0, n-1) numbering */

  for (i = 0; i < 6; i++) {
    for (j = 0; j < 4; j++)
      face_vertices[i][j] -= 1;
  }
}

/*============================================================================
 * Public function definitions
 *============================================================================*/

/*----------------------------------------------------------------------------
 * Creation of a nodal mesh representation structure.
 *
 * parameters:
 *   name <-- name that should be assigned to the nodal mesh
 *   dim  <-- spatial dimension
 *
 * returns:
 *  pointer to created nodal mesh representation structure
 *----------------------------------------------------------------------------*/

fvm_nodal_t *
fvm_nodal_create(const char  *name,
                 int          dim)
{
  fvm_nodal_t  *this_nodal;

  BFT_MALLOC(this_nodal, 1, fvm_nodal_t);

  /* Global indicators */

  if (name != NULL) {
    BFT_MALLOC(this_nodal->name, strlen(name) + 1, char);
    strcpy(this_nodal->name, name);
  }
  else
    this_nodal->name = NULL;

  this_nodal->dim     = dim;
  this_nodal->num_dom = (cs_glob_rank_id >= 0) ? cs_glob_rank_id + 1 : 1;
  this_nodal->n_doms  = cs_glob_n_ranks;
  this_nodal->n_sections = 0;

  /* Local dimensions */

  this_nodal->n_cells = 0;
  this_nodal->n_faces = 0;
  this_nodal->n_edges = 0;
  this_nodal->n_vertices = 0;

  /* Local structures */

  this_nodal->vertex_coords = NULL;
  this_nodal->_vertex_coords = NULL;

  this_nodal->parent_vertex_num = NULL;
  this_nodal->_parent_vertex_num = NULL;

  this_nodal->global_vertex_num = NULL;

  this_nodal->sections = NULL;

  this_nodal->gc_set = NULL;

  return (this_nodal);
}

/*----------------------------------------------------------------------------
 * Destruction of a nodal mesh representation structure.
 *
 * parameters:
 *   this_nodal  <-> pointer to structure that should be destroyed
 *
 * returns:
 *  NULL pointer
 *----------------------------------------------------------------------------*/

fvm_nodal_t *
fvm_nodal_destroy(fvm_nodal_t  * this_nodal)
{
  int           i;

  /* Local structures */

  if (this_nodal->name != NULL)
    BFT_FREE(this_nodal->name);

  if (this_nodal->_vertex_coords != NULL)
    BFT_FREE(this_nodal->_vertex_coords);

  if (this_nodal->parent_vertex_num != NULL) {
    this_nodal->parent_vertex_num = NULL;
    BFT_FREE(this_nodal->_parent_vertex_num);
  }

  if (this_nodal->global_vertex_num != NULL)
    fvm_io_num_destroy(this_nodal->global_vertex_num);

  for (i = 0; i < this_nodal->n_sections; i++)
    fvm_nodal_section_destroy(this_nodal->sections[i]);

  if (this_nodal->sections != NULL)
    BFT_FREE(this_nodal->sections);

  if (this_nodal->gc_set != NULL)
    this_nodal->gc_set = fvm_group_class_set_destroy(this_nodal->gc_set);

  /* Main structure destroyed and NULL returned */

  BFT_FREE(this_nodal);

  return (this_nodal);
}

/*----------------------------------------------------------------------------
 * Copy a nodal mesh representation structure, sharing arrays with the
 * original structure.
 *
 * Element group classes and mesh group class descriptions are not currently
 * copied.
 *
 * parameters:
 *   this_nodal  <-> pointer to structure that should be copied
 *
 * returns:
 *   pointer to created nodal mesh representation structure
 *----------------------------------------------------------------------------*/

fvm_nodal_t *
fvm_nodal_copy(const fvm_nodal_t *this_nodal)
{
  int i;
  fvm_nodal_t  *new_nodal;

  BFT_MALLOC(new_nodal, 1, fvm_nodal_t);

  /* Global indicators */

  if (this_nodal->name != NULL) {
    BFT_MALLOC(new_nodal->name, strlen(this_nodal->name) + 1, char);
    strcpy(new_nodal->name, this_nodal->name);
  }
  else
    new_nodal->name = NULL;

  new_nodal->dim     = this_nodal->dim;
  new_nodal->num_dom = this_nodal->num_dom;
  new_nodal->n_doms  = this_nodal->n_doms;
  new_nodal->n_sections = this_nodal->n_sections;

  /* Local dimensions */

  new_nodal->n_cells = this_nodal->n_cells;
  new_nodal->n_faces = this_nodal->n_faces;
  new_nodal->n_edges = this_nodal->n_edges;
  new_nodal->n_vertices = this_nodal->n_vertices;

  /* Local structures */

  new_nodal->vertex_coords = this_nodal->vertex_coords;
  new_nodal->_vertex_coords = NULL;

  new_nodal->parent_vertex_num = this_nodal->parent_vertex_num;
  new_nodal->_parent_vertex_num = NULL;

  if (this_nodal->global_vertex_num != NULL) {
    cs_lnum_t n_ent
      = fvm_io_num_get_local_count(this_nodal->global_vertex_num);
    cs_gnum_t global_count
      = fvm_io_num_get_global_count(this_nodal->global_vertex_num);
    const cs_gnum_t *global_num
      = fvm_io_num_get_global_num(this_nodal->global_vertex_num);

    new_nodal->global_vertex_num
      = fvm_io_num_create_shared(global_num, global_count, n_ent);
  }
  else
    new_nodal->global_vertex_num = NULL;

  BFT_MALLOC(new_nodal->sections,
             new_nodal->n_sections,
             fvm_nodal_section_t *);
  for (i = 0; i < new_nodal->n_sections; i++)
    new_nodal->sections[i] = _fvm_nodal_section_copy(this_nodal->sections[i]);

  new_nodal->gc_set = NULL;

  return (new_nodal);
}

/*----------------------------------------------------------------------------
 * Reduction of a nodal mesh representation structure: only the associations
 * (numberings) necessary to redistribution of fields for output are
 * conserved, the full connectivity being in many cases no longer useful
 * once it has been output. If the del_vertex_num value is set
 * to true, vertex-based values may not be output in parallel mode
 * after this function is called.
 *
 * parameters:
 *   this_nodal        <-> pointer to structure that should be reduced
 *   del_vertex_num    <-- indicates if vertex parent indirection and
 *                         I/O numbering are destroyed (1) or not (0)
 *----------------------------------------------------------------------------*/

void
fvm_nodal_reduce(fvm_nodal_t  *this_nodal,
                 int           del_vertex_num)
{
  int  i;
  _Bool reduce_vertices = true;

  /* Connectivity */

  for (i = 0; i < this_nodal->n_sections; i++) {
    if (_fvm_nodal_section_reduce(this_nodal->sections[i]) == false)
      reduce_vertices = false;
  }

  /* Vertices */

  if (reduce_vertices == true) {

    if (this_nodal->_vertex_coords != NULL)
      BFT_FREE(this_nodal->_vertex_coords);
    this_nodal->vertex_coords = NULL;

  }

  /* Depending on this option, output on vertices may not remain possible */

  if (del_vertex_num > 0) {

    if (this_nodal->parent_vertex_num != NULL) {
      this_nodal->parent_vertex_num = NULL;
      BFT_FREE(this_nodal->_parent_vertex_num);
    }

    if (this_nodal->global_vertex_num != NULL)
      this_nodal->global_vertex_num
        = fvm_io_num_destroy(this_nodal->global_vertex_num);

  }

  if (this_nodal->gc_set != NULL)
    this_nodal->gc_set = fvm_group_class_set_destroy(this_nodal->gc_set);
}

/*----------------------------------------------------------------------------
 * Change entity parent numbering; this is useful when entities of the
 * parent mesh have been renumbered after a nodal mesh representation
 * structure's creation.
 *
 * parameters:
 *   this_nodal          <-- nodal mesh structure
 *   new_parent_num      <-- pointer to local parent renumbering array
 *                           ({1, ..., n} <-- {1, ..., n})
 *   entity_dim          <-- 3 for cells, 2 for faces, 1 for edges,
 *                           and 0 for vertices
 *----------------------------------------------------------------------------*/

void
fvm_nodal_change_parent_num(fvm_nodal_t       *this_nodal,
                            const cs_lnum_t    new_parent_num[],
                            int                entity_dim)
{
  /* Vertices */

  if (entity_dim == 0) {

    this_nodal->_parent_vertex_num
      = _renumber_parent_num(this_nodal->n_vertices,
                             new_parent_num,
                             this_nodal->parent_vertex_num,
                             this_nodal->_parent_vertex_num);
    this_nodal->parent_vertex_num = this_nodal->_parent_vertex_num;

  }

  /* Other elements */

  else {

    int  i = 0;
    fvm_nodal_section_t  *section = NULL;

    for (i = 0; i < this_nodal->n_sections; i++) {
      section = this_nodal->sections[i];
      if (section->entity_dim == entity_dim) {
        section->_parent_element_num
          = _renumber_parent_num(section->n_elements,
                                 new_parent_num,
                                 section->parent_element_num,
                                 section->_parent_element_num);
        section->parent_element_num = section->_parent_element_num;
      }
    }

  }

}

/*----------------------------------------------------------------------------
 * Remove entity parent numbering; this is useful for example when we
 * want to assign coordinates or fields to an extracted mesh using
 * arrays relative to the mesh, and not to its parent.
 *
 * This is equivalent to calling fvm_nodal_change_parent_num(), with
 * 'trivial' (1 o n) new_parent_num[] values.
 *
 * parameters:
 *   this_nodal          <-- nodal mesh structure
 *   entity_dim          <-- 3 for cells, 2 for faces, 1 for edges,
 *                           and 0 for vertices
 *----------------------------------------------------------------------------*/

void
fvm_nodal_remove_parent_num(fvm_nodal_t  *this_nodal,
                            int           entity_dim)
{
  /* Vertices */

  if (entity_dim == 0) {
    this_nodal->parent_vertex_num = NULL;
    if (this_nodal->_parent_vertex_num != NULL)
      BFT_FREE(this_nodal->_parent_vertex_num);
  }

  /* Other elements */

  else {

    int  i = 0;
    fvm_nodal_section_t  *section = NULL;

    for (i = 0; i < this_nodal->n_sections; i++) {
      section = this_nodal->sections[i];
      if (section->entity_dim == entity_dim) {
        section->parent_element_num = NULL;
        if (section->_parent_element_num != NULL)
          BFT_FREE(section->_parent_element_num);
      }
    }

  }

}

/*----------------------------------------------------------------------------
 * Build external numbering for entities based on global numbers.
 *
 * parameters:
 *   this_nodal           <-- nodal mesh structure
 *   parent_global_number <-- pointer to list of global (i.e. domain splitting
 *                            independent) parent entity numbers
 *   entity_dim           <-- 3 for cells, 2 for faces, 1 for edges,
 *                            and 0 for vertices
 *----------------------------------------------------------------------------*/

void
fvm_nodal_init_io_num(fvm_nodal_t       *this_nodal,
                      const cs_gnum_t    parent_global_numbers[],
                      int                entity_dim)
{
  int  i;
  fvm_nodal_section_t  *section;

  if (entity_dim == 0)
    this_nodal->global_vertex_num
      = fvm_io_num_create(this_nodal->parent_vertex_num,
                          parent_global_numbers,
                          this_nodal->n_vertices,
                          0);

  else {
    for (i = 0; i < this_nodal->n_sections; i++) {
      section = this_nodal->sections[i];
      if (section->entity_dim == entity_dim) {
        section->global_element_num
          = fvm_io_num_create(section->parent_element_num,
                              parent_global_numbers,
                              section->n_elements,
                              0);
      }
    }
  }

}

/*----------------------------------------------------------------------------
 * Preset number and list of vertices to assign to a nodal mesh.
 *
 * If the parent_vertex_num argument is NULL, the list is assumed to
 * be {1, 2, ..., n}. If parent_vertex_num is given, it specifies a
 * list of n vertices from a larger set (1 to n numbering).
 *
 * Ownership of the given parent vertex numbering array is
 * transferred to the nodal mesh representation structure.
 *
 * This function should be called before fvm_nodal_set_shared_vertices()
 * or fvm_nodal_transfer_vertices() if we want to force certain
 * vertices to appear in the mesh (especially if we want to define
 * a mesh containing only vertices).
 *
 * parameters:
 *   this_nodal        <-> nodal mesh structure
 *   n_vertices        <-- number of vertices to assign
 *   parent_vertex_num <-- parent numbers of vertices to assign
 *----------------------------------------------------------------------------*/

void
fvm_nodal_define_vertex_list(fvm_nodal_t  *this_nodal,
                             cs_lnum_t     n_vertices,
                             cs_lnum_t     parent_vertex_num[])
{
  assert(this_nodal != NULL);

  this_nodal->n_vertices = n_vertices;

  this_nodal->parent_vertex_num = NULL;
  if (this_nodal->_parent_vertex_num != NULL)
    BFT_FREE(this_nodal->_parent_vertex_num);

  if (parent_vertex_num != NULL) {
    this_nodal->_parent_vertex_num = parent_vertex_num;
    this_nodal->parent_vertex_num = parent_vertex_num;
  }
}

/*----------------------------------------------------------------------------
 * Assign shared vertex coordinates to an extracted nodal mesh,
 * renumbering vertex numbers based on those really referenced,
 * and updating connectivity arrays in accordance.
 *
 * This function should only be called once all element sections
 * have been added to a nodal mesh representation.
 *
 * parameters:
 *   this_nodal      <-> nodal mesh structure
 *   vertex_coords   <-- coordinates of parent vertices (interlaced)
 *----------------------------------------------------------------------------*/

void
fvm_nodal_set_shared_vertices(fvm_nodal_t       *this_nodal,
                              const cs_coord_t   vertex_coords[])
{
  assert(this_nodal != NULL);

  /* Map vertex coordinates to array passed as argument
     (this_nodal->_vertex_coords remains NULL, so only
     the const pointer may be used for a shared array) */

  this_nodal->vertex_coords = vertex_coords;

  /* If the mesh contains only vertices, its n_vertices and
     parent_vertex_num must already have been set, and do not
     require updating */

  if (this_nodal->n_sections == 0)
    return;

  /* Renumber vertices based on those really referenced */

  _renumber_vertices(this_nodal);

}

/*----------------------------------------------------------------------------
 * Assign private vertex coordinates to a nodal mesh,
 * renumbering vertex numbers based on those really referenced,
 * and updating connectivity arrays in accordance.
 *
 * Ownership of the given coordinates array is transferred to
 * the nodal mesh representation structure.
 *
 * This function should only be called once all element sections
 * have been added to a nodal mesh representation.
 *
 * parameters:
 *   this_nodal      <-> nodal mesh structure
 *   vertex_coords   <-- coordinates of parent vertices (interlaced)
 *
 * returns:
 *   updated pointer to vertex_coords (may be different from initial
 *   argument if vertices were renumbered).
 *----------------------------------------------------------------------------*/

cs_coord_t *
fvm_nodal_transfer_vertices(fvm_nodal_t  *this_nodal,
                            cs_coord_t    vertex_coords[])
{
  cs_lnum_t   i;
  int         j;

  cs_coord_t  *_vertex_coords = vertex_coords;

  assert(this_nodal != NULL);

  /* Renumber vertices based on those really referenced, and
     update connectivity arrays in accordance. */

  _renumber_vertices(this_nodal);

  /* If renumbering is necessary, update connectivity */

  if (this_nodal->parent_vertex_num != NULL) {

    int dim = this_nodal->dim;
    const cs_lnum_t *parent_vertex_num = this_nodal->parent_vertex_num;

    BFT_MALLOC(_vertex_coords, this_nodal->n_vertices * dim, cs_coord_t);

    for (i = 0; i < this_nodal->n_vertices; i++) {
      for (j = 0; j < dim; j++)
        _vertex_coords[i*dim + j]
          = vertex_coords[(parent_vertex_num[i]-1)*dim + j];
    }

    BFT_FREE(vertex_coords);

    this_nodal->parent_vertex_num = NULL;
    if (this_nodal->_parent_vertex_num != NULL)
      BFT_FREE(this_nodal->_parent_vertex_num);
  }

  this_nodal->_vertex_coords = _vertex_coords;
  this_nodal->vertex_coords = _vertex_coords;

  return _vertex_coords;
}

/*----------------------------------------------------------------------------
 * Make vertex coordinates of a nodal mesh private.
 *
 * If vertex coordinates were previously shared, those coordinates that
 * are actually refernces are copied, and the relation to parent vertices
 * is discarded.
 *
 * If vertices were already private, the mesh is not modified.
 *
 * parameters:
 *   this_nodal <-> nodal mesh structure
 *----------------------------------------------------------------------------*/

void
fvm_nodal_make_vertices_private(fvm_nodal_t  *this_nodal)
{
  assert(this_nodal != NULL);

  if (this_nodal->_vertex_coords == NULL) {

    cs_coord_t *_vertex_coords = NULL;
    const cs_coord_t *vertex_coords = this_nodal->vertex_coords;
    const cs_lnum_t n_vertices = this_nodal->n_vertices;
    const int dim = this_nodal->dim;

    BFT_MALLOC(vertex_coords, n_vertices * dim, cs_coord_t);

    /* If renumbering is necessary, update connectivity */

    if (this_nodal->parent_vertex_num != NULL) {

      cs_lnum_t i;
      int j;
      const cs_lnum_t *parent_vertex_num = this_nodal->parent_vertex_num;

      for (i = 0; i < n_vertices; i++) {
        for (j = 0; j < dim; j++)
          _vertex_coords[i*dim + j]
            = vertex_coords[(parent_vertex_num[i]-1)*dim + j];
      }

      this_nodal->parent_vertex_num = NULL;
      if (this_nodal->_parent_vertex_num != NULL)
        BFT_FREE(this_nodal->_parent_vertex_num);
    }
    else
      memcpy(_vertex_coords, vertex_coords, n_vertices*dim*sizeof(cs_coord_t));

    /* Assign new array to structure */

    this_nodal->_vertex_coords = _vertex_coords;
    this_nodal->vertex_coords = _vertex_coords;
  }
}

/*----------------------------------------------------------------------------
 * Assign group class set descriptions to a nodal mesh.
 *
 * The structure builds its own copy of the group class sets,
 * renumbering them so as to discard those not referenced.
 * Empty group classes are also renumbered to zero.
 *
 * This function should only be called once all element sections
 * have been added to a nodal mesh representation.
 *
 * parameters:
 *   this_nodal <-> nodal mesh structure
 *   gc_set     <-- group class set descriptions
 *----------------------------------------------------------------------------*/

void
fvm_nodal_set_group_class_set(fvm_nodal_t                  *this_nodal,
                              const fvm_group_class_set_t  *gc_set)
{
  int gc_id, section_id;
  int n_gc = fvm_group_class_set_size(gc_set);
  int n_gc_new = 0;
  cs_lnum_t *gc_renum = NULL;

  assert(this_nodal != NULL);

  if (this_nodal->gc_set != NULL)
    this_nodal->gc_set = fvm_group_class_set_destroy(this_nodal->gc_set);

  if (gc_set == NULL)
    return;

  /* Mark referenced group classes */

  BFT_MALLOC(gc_renum, n_gc, cs_lnum_t);

  for (gc_id = 0; gc_id < n_gc; gc_id++)
    gc_renum[gc_id] = 0;

  for (section_id = 0; section_id < this_nodal->n_sections; section_id++) {
    cs_lnum_t i;
    const fvm_nodal_section_t  *section = this_nodal->sections[section_id];
    if (section->gc_id == NULL)
      continue;
    for (i = 0; i < section->n_elements; i++) {
      if (section->gc_id[i] != 0)
        gc_renum[section->gc_id[i] - 1] = 1;
    }
  }

  fvm_parall_counter_max(gc_renum, n_gc);

  /* Renumber group classes if necessary */

  for (gc_id = 0; gc_id < n_gc; gc_id++) {
    if (gc_renum[gc_id] != 0) {
      gc_renum[gc_id] = n_gc_new + 1;
      n_gc_new++;
    }
  }

  if (n_gc_new < n_gc) {
    for (section_id = 0; section_id < this_nodal->n_sections; section_id++) {
      cs_lnum_t i;
      const fvm_nodal_section_t  *section = this_nodal->sections[section_id];
      if (section->gc_id == NULL)
        continue;
      for (i = 0; i < section->n_elements; i++) {
        if (section->gc_id[i] != 0)
          section->gc_id[i] = gc_renum[section->gc_id[i] - 1];
      }
    }
  }

  /* Transform renumbering array to list */

  n_gc_new = 0;
  for (gc_id = 0; gc_id < n_gc; gc_id++) {
    if (gc_renum[gc_id] != 0)
      gc_renum[n_gc_new++] = gc_id;
  }

  if (n_gc_new > 0)
    this_nodal->gc_set = fvm_group_class_set_copy(gc_set,
                                                  n_gc_new,
                                                  gc_renum);

  BFT_FREE(gc_renum);
}

/*----------------------------------------------------------------------------
 * Obtain the name of a nodal mesh.
 *
 * parameters:
 *   this_nodal           <-- pointer to nodal mesh structure
 *
 * returns:
 *   pointer to constant string containing the mesh name
 *----------------------------------------------------------------------------*/

const char *
fvm_nodal_get_name(const fvm_nodal_t  *this_nodal)
{
  assert(this_nodal != NULL);

  return this_nodal->name;
}

/*----------------------------------------------------------------------------
 * Return spatial dimension of the nodal mesh.
 *
 * parameters:
 *   this_nodal <-- pointer to nodal mesh structure
 *
 * returns:
 *  spatial dimension.
 *----------------------------------------------------------------------------*/

int
fvm_nodal_get_dim(const fvm_nodal_t  *this_nodal)
{
  return this_nodal->dim;
}

/*----------------------------------------------------------------------------
 * Return maximum dimension of entities in a nodal mesh.
 *
 * parameters:
 *   this_nodal <-- pointer to nodal mesh structure
 *
 * returns:
 *  maximum dimension of entities in mesh (0 to 3)
 *----------------------------------------------------------------------------*/

int
fvm_nodal_get_max_entity_dim(const fvm_nodal_t  *this_nodal)
{
  int  section_id;
  int  max_entity_dim = 0;

  assert(this_nodal != NULL);

  for (section_id = 0; section_id < this_nodal->n_sections; section_id++) {
    const fvm_nodal_section_t  *section = this_nodal->sections[section_id];
    if (section->entity_dim > max_entity_dim)
      max_entity_dim = section->entity_dim;
  }

  return max_entity_dim;
}

/*----------------------------------------------------------------------------
 * Return number of entities of a given dimension in a nodal mesh.
 *
 * parameters:
 *   this_nodal <-- pointer to nodal mesh structure
 *   entity_dim <-- dimension of entities we want to count (0 to 3)
 *
 * returns:
 *  number of entities of given dimension in mesh
 *----------------------------------------------------------------------------*/

cs_lnum_t
fvm_nodal_get_n_entities(const fvm_nodal_t  *this_nodal,
                         int                 entity_dim)
{
  cs_lnum_t n_entities;

  assert(this_nodal != NULL);

  switch(entity_dim) {
  case 0:
    n_entities = this_nodal->n_vertices;
    break;
  case 1:
    n_entities = this_nodal->n_edges;
    break;
  case 2:
    n_entities = this_nodal->n_faces;
    break;
  case 3:
    n_entities = this_nodal->n_cells;
    break;
  default:
    n_entities = 0;
  }

  return n_entities;
}

/*----------------------------------------------------------------------------
 * Return global number of vertices associated with nodal mesh.
 *
 * parameters:
 *   this_nodal           <-- pointer to nodal mesh structure
 *
 * returns:
 *   global number of vertices associated with nodal mesh
 *----------------------------------------------------------------------------*/

cs_gnum_t
fvm_nodal_get_n_g_vertices(const fvm_nodal_t  *this_nodal)
{
  return fvm_nodal_n_g_vertices(this_nodal);
}

/*----------------------------------------------------------------------------
 * Return global number of elements of a given type associated with nodal mesh.
 *
 * parameters:
 *   this_nodal           <-- pointer to nodal mesh structure
 *   element_type         <-- type of elements for query
 *
 * returns:
 *   global number of elements of the given type associated with nodal mesh
 *----------------------------------------------------------------------------*/

cs_gnum_t
fvm_nodal_get_n_g_elements(const fvm_nodal_t  *this_nodal,
                           fvm_element_t       element_type)
{
  int  i;
  cs_gnum_t   n_g_elements = 0;

  assert(this_nodal != NULL);

  for (i = 0; i < this_nodal->n_sections; i++) {
    fvm_nodal_section_t  *section = this_nodal->sections[i];
    if (section->type == element_type)
      n_g_elements += fvm_nodal_section_n_g_elements(section);
  }

  return n_g_elements;
}

/*----------------------------------------------------------------------------
 * Return local number of elements of a given type associated with nodal mesh.
 *
 * parameters:
 *   this_nodal           <-- pointer to nodal mesh structure
 *   element_type         <-- type of elements for query
 *
 * returns:
 *   local number of elements of the given type associated with nodal mesh
 *----------------------------------------------------------------------------*/

cs_lnum_t
fvm_nodal_get_n_elements(const fvm_nodal_t  *this_nodal,
                         fvm_element_t       element_type)
{
  int  i;
  cs_lnum_t   n_elements = 0;

  assert(this_nodal != NULL);

  for (i = 0; i < this_nodal->n_sections; i++) {
    fvm_nodal_section_t  *section = this_nodal->sections[i];
    if (section->type == element_type)
      n_elements += section->n_elements;
  }

  return n_elements;
}

/*----------------------------------------------------------------------------
 * Return local parent numbering array for all entities of a given
 * dimension in a nodal mesh.
 *
 * The number of entities of the given dimension may be obtained
 * through fvm_nodal_get_n_entities(), the parent_num[] array is populated
 * with the parent entity numbers of those entities, in order (i.e. in
 * local section order, section by section).
 *
 * parameters:
 *   this_nodal <-- pointer to nodal mesh structure
 *   entity_dim <-- dimension of entities we are interested in (0 to 3)
 *   parent_num --> entity parent numbering (array must be pre-allocated)
 *----------------------------------------------------------------------------*/

void
fvm_nodal_get_parent_num(const fvm_nodal_t  *this_nodal,
                         int                 entity_dim,
                         cs_lnum_t           parent_num[])
{
  int section_id;
  cs_lnum_t i;

  cs_lnum_t entity_count = 0;

  assert(this_nodal != NULL);

  /* Entity dimension 0: vertices */

  if (entity_dim == 0) {
    if (this_nodal->parent_vertex_num != NULL) {
      for (i = 0; i < this_nodal->n_vertices; i++)
        parent_num[entity_count++] = this_nodal->parent_vertex_num[i];
    }
    else {
      for (i = 0; i < this_nodal->n_vertices; i++)
        parent_num[entity_count++] = i + 1;
    }
  }

  /* Entity dimension > 0: edges, faces, or cells */

  else {

    for (section_id = 0; section_id < this_nodal->n_sections; section_id++) {

      const fvm_nodal_section_t  *section = this_nodal->sections[section_id];

      if (section->entity_dim == entity_dim) {
        if (section->parent_element_num != NULL) {
          for (i = 0; i < section->n_elements; i++)
            parent_num[entity_count++] = section->parent_element_num[i];
        }
        else {
          for (i = 0; i < section->n_elements; i++)
            parent_num[entity_count++] = i + 1;
        }
      }

    } /* end loop on sections */

  }
}

/*----------------------------------------------------------------------------
 * Compute tesselation a a nodal mesh's sections of a given type, and add the
 * corresponding structure to the mesh representation.
 *
 * If global element numbers are used (i.e. in parallel mode), this function
 * should be only be used after calling fvm_nodal_init_io_num().
 *
 * If some mesh sections have already been tesselated, their tesselation
 * is unchanged.
 *
 * parameters:
 *   this_nodal  <-> pointer to nodal mesh structure
 *   type        <-> element type that should be tesselated
 *   error_count --> number of elements with a tesselation error
 *                   counter (optional)
 *----------------------------------------------------------------------------*/

void
fvm_nodal_tesselate(fvm_nodal_t    *this_nodal,
                    fvm_element_t   type,
                    cs_lnum_t      *error_count)
{
  int section_id;
  cs_lnum_t section_error_count;

  assert(this_nodal != NULL);

  if (error_count != NULL)
    *error_count = 0;

  for (section_id = 0; section_id < this_nodal->n_sections; section_id++) {

    fvm_nodal_section_t  *section = this_nodal->sections[section_id];

    if (section->type == type && section->tesselation == NULL) {

      section->tesselation = fvm_tesselation_create(type,
                                                    section->n_elements,
                                                    section->face_index,
                                                    section->face_num,
                                                    section->vertex_index,
                                                    section->vertex_num,
                                                    section->global_element_num);

      fvm_tesselation_init(section->tesselation,
                           this_nodal->dim,
                           this_nodal->vertex_coords,
                           this_nodal->parent_vertex_num,
                           &section_error_count);

      if (error_count != NULL)
        *error_count += section_error_count;
    }

  }
}

/*----------------------------------------------------------------------------
 * Build a nodal representation structure based on extraction of a
 * mesh's edges.
 *
 * parameters:
 *   name        <-- name to assign to extracted mesh
 *   this_nodal  <-> pointer to nodal mesh structure
 *----------------------------------------------------------------------------*/

fvm_nodal_t *
fvm_nodal_copy_edges(const char         *name,
                     const fvm_nodal_t  *this_nodal)
{
  int i;
  cs_lnum_t j, k;
  cs_lnum_t n_edges = 0, n_max_edges = 0;
  fvm_nodal_t *new_nodal = NULL;
  fvm_nodal_section_t *new_section = NULL;

  BFT_MALLOC(new_nodal, 1, fvm_nodal_t);

  /* Global indicators */

  if (name != NULL) {
    BFT_MALLOC(new_nodal->name, strlen(name) + 1, char);
    strcpy(new_nodal->name, name);
  }
  else
    new_nodal->name = NULL;

  new_nodal->dim     = this_nodal->dim;
  new_nodal->num_dom = this_nodal->num_dom;
  new_nodal->n_doms  = this_nodal->n_doms;
  new_nodal->n_sections = 1;

  /* Local dimensions */

  new_nodal->n_cells = 0;
  new_nodal->n_faces = 0;
  new_nodal->n_edges = 0;
  new_nodal->n_vertices = this_nodal->n_vertices;

  /* Local structures */

  new_nodal->vertex_coords = this_nodal->vertex_coords;
  new_nodal->_vertex_coords = NULL;

  new_nodal->parent_vertex_num = this_nodal->parent_vertex_num;
  new_nodal->_parent_vertex_num = NULL;

  if (this_nodal->global_vertex_num != NULL) {
    cs_lnum_t n_ent
      = fvm_io_num_get_local_count(this_nodal->global_vertex_num);
    cs_gnum_t global_count
      = fvm_io_num_get_global_count(this_nodal->global_vertex_num);
    const cs_gnum_t *global_num
      = fvm_io_num_get_global_num(this_nodal->global_vertex_num);

    new_nodal->global_vertex_num
      = fvm_io_num_create_shared(global_num, global_count, n_ent);
  }
  else
    new_nodal->global_vertex_num = NULL;

  /* Counting step */

  for (i = 0; i < this_nodal->n_sections; i++) {
    const fvm_nodal_section_t *this_section = this_nodal->sections[i];
    if (this_section->vertex_index == NULL)
      n_max_edges += (  fvm_nodal_n_edges_element[this_section->type]
                      * this_section->n_elements);
    else if (this_section->type == FVM_FACE_POLY)
      n_max_edges += this_section->vertex_index[this_section->n_elements];
    else if (this_section->type == FVM_CELL_POLY)
      n_max_edges += this_section->vertex_index[this_section->n_faces];
  }

  BFT_MALLOC(new_nodal->sections, 1, fvm_nodal_section_t *);

  new_section = fvm_nodal_section_create(FVM_EDGE);
  new_nodal->sections[0] = new_section;

  BFT_MALLOC(new_section->_vertex_num, n_max_edges*2, cs_lnum_t);

  /* Add edges */

  for (i = 0; i < this_nodal->n_sections; i++) {

    const fvm_nodal_section_t *this_section = this_nodal->sections[i];

    if (   this_section->type == FVM_FACE_POLY
        || this_section->type == FVM_CELL_POLY) {

      cs_lnum_t n_faces = this_section->type == FVM_FACE_POLY ?
        this_section->n_elements : this_section->n_faces;

      for (j = 0; j < n_faces; j++) {
        const cs_lnum_t face_start_id = this_section->vertex_index[j];
        const cs_lnum_t n_face_edges
          = this_section->vertex_index[j+1] - this_section->vertex_index[j];
        for (k = 0; k < n_face_edges; k++) {
          new_section->_vertex_num[n_edges*2]
            = this_section->vertex_num[face_start_id + k];
          new_section->_vertex_num[n_edges*2 + 1]
            = this_section->vertex_num[face_start_id + (k + 1)%n_face_edges];
          n_edges += 1;
        }
      }

    }
    else {

      cs_lnum_t edges[2][12];

      cs_lnum_t n_elt_edges = fvm_nodal_n_edges_element[this_section->type];
      cs_lnum_t n_elts = this_section->n_elements;
      cs_lnum_t stride = this_section->stride;

      switch (this_section->type) {

      case FVM_EDGE:
      case FVM_FACE_TRIA:
      case FVM_FACE_QUAD:
        for (j = 0; j < n_elt_edges; j++) {
          edges[0][j] = j;
          edges[1][j] = (j+1)%n_elt_edges;
        }
        break;

      case FVM_CELL_TETRA:
        edges[0][0] = 0; edges[1][0] = 1;
        edges[0][1] = 1; edges[1][1] = 2;
        edges[0][2] = 2; edges[1][2] = 0;
        edges[0][3] = 0; edges[1][3] = 3;
        edges[0][4] = 1; edges[1][4] = 3;
        edges[0][5] = 2; edges[1][5] = 3;
        break;

      case FVM_CELL_PYRAM:
        edges[0][0] = 0; edges[1][0] = 1;
        edges[0][1] = 1; edges[1][1] = 2;
        edges[0][2] = 2; edges[1][2] = 3;
        edges[0][3] = 3; edges[1][3] = 0;
        edges[0][4] = 0; edges[1][4] = 4;
        edges[0][5] = 1; edges[1][5] = 4;
        edges[0][6] = 2; edges[1][6] = 4;
        edges[0][7] = 3; edges[1][7] = 4;
        break;

      case FVM_CELL_PRISM:
        edges[0][0] = 0; edges[1][0] = 1;
        edges[0][1] = 1; edges[1][1] = 2;
        edges[0][2] = 2; edges[1][2] = 0;
        edges[0][3] = 0; edges[1][3] = 3;
        edges[0][4] = 1; edges[1][4] = 4;
        edges[0][5] = 2; edges[1][5] = 5;
        edges[0][6] = 3; edges[1][6] = 4;
        edges[0][7] = 4; edges[1][7] = 5;
        edges[0][8] = 5; edges[1][8] = 3;
        break;

      case FVM_CELL_HEXA:
        edges[0][0] = 0; edges[1][0] = 1;
        edges[0][1] = 1; edges[1][1] = 2;
        edges[0][2] = 2; edges[1][2] = 3;
        edges[0][3] = 3; edges[1][3] = 0;
        edges[0][4] = 0; edges[1][4] = 4;
        edges[0][5] = 1; edges[1][5] = 5;
        edges[0][6] = 2; edges[1][6] = 6;
        edges[0][7] = 3; edges[1][7] = 7;
        edges[0][8] = 4; edges[1][8] = 5;
        edges[0][9] = 5; edges[1][9] = 6;
        edges[0][10] = 6; edges[1][10] = 7;
        edges[0][11] = 7; edges[1][11] = 4;
        break;

      default:
        assert(0);
        edges[0][0] = -1; /* For nonempty default clause */
      }

      for (j = 0; j < n_elts; j++) {
        const cs_lnum_t *_vertex_num = this_section->vertex_num + (j*stride);
        for (k = 0; k < n_elt_edges; k++) {
          new_section->_vertex_num[n_edges*2] = _vertex_num[edges[0][k]];
          new_section->_vertex_num[n_edges*2 + 1] = _vertex_num[edges[1][k]];
          n_edges += 1;
        }
      }
    }
  } /* End of loop on sections */

  assert(n_edges == n_max_edges);

  /* Ensure edges are oriented in the same direction */

  if (this_nodal->global_vertex_num != NULL) {

    const cs_gnum_t *v_num_g
      = fvm_io_num_get_global_num(this_nodal->global_vertex_num);

    for (j = 0; j < n_max_edges; j++) {
      cs_lnum_t vnum_1 = new_section->_vertex_num[j*2];
      cs_lnum_t vnum_2 = new_section->_vertex_num[j*2 + 1];
      if (v_num_g[vnum_1 - 1] > v_num_g[vnum_2 - 1]) {
        new_section->_vertex_num[j*2] = vnum_2;
        new_section->_vertex_num[j*2 + 1] = vnum_1;
      }

    }

  }
  else {

    for (j = 0; j < n_max_edges; j++) {
      cs_lnum_t vnum_1 = new_section->_vertex_num[j*2];
      cs_lnum_t vnum_2 = new_section->_vertex_num[j*2 + 1];
      if (vnum_1 > vnum_2) {
        new_section->_vertex_num[j*2] = vnum_2;
        new_section->_vertex_num[j*2 + 1] = vnum_1;
      }
    }

  }

  /* Sort and remove duplicates
     (use qsort rather than cs_order_..._s() so as to sort in place) */

  qsort(new_section->_vertex_num,
        n_max_edges,
        sizeof(cs_lnum_t) * 2,
        &_compare_edges);

  {
    cs_lnum_t vn_1_p = -1;
    cs_lnum_t vn_2_p = -1;

    n_edges = 0;

    for (j = 0; j < n_max_edges; j++) {

      cs_lnum_t vn_1 = new_section->_vertex_num[j*2];
      cs_lnum_t vn_2 = new_section->_vertex_num[j*2 + 1];

      if (vn_1 != vn_1_p || vn_2 != vn_2_p) {
        new_section->_vertex_num[n_edges*2]     = vn_1;
        new_section->_vertex_num[n_edges*2 + 1] = vn_2;
        vn_1_p = vn_1;
        vn_2_p = vn_2;
        n_edges += 1;
      }
    }
  }

  /* Resize edge connectivity to adjust to final size */

  BFT_REALLOC(new_section->_vertex_num, n_edges*2, cs_lnum_t);
  new_section->vertex_num = new_section->_vertex_num;

  new_section->n_elements = n_edges;
  new_nodal->n_edges = n_edges;

  /* Build  global edge numbering if necessary */

  if (new_nodal->n_doms > 1) {

    cs_gnum_t *edge_vertices_g; /* edges -> global vertices */

    BFT_MALLOC(edge_vertices_g, n_edges*2, cs_gnum_t);

    if (this_nodal->global_vertex_num != NULL) {
      const cs_gnum_t *v_num_g
        = fvm_io_num_get_global_num(this_nodal->global_vertex_num);
      for (j = 0; j < n_edges; j++) {
        edge_vertices_g[j*2]   = v_num_g[new_section->_vertex_num[j*2] - 1];
        edge_vertices_g[j*2+1] = v_num_g[new_section->_vertex_num[j*2+1] - 1];
      }
    }
    else {
      for (j = 0; j < n_edges; j++) {
        edge_vertices_g[j*2]     = new_section->_vertex_num[j*2];
        edge_vertices_g[j*2 + 1] = new_section->_vertex_num[j*2 + 1];
      }
    }

    new_section->global_element_num
      = fvm_io_num_create_from_adj_s(NULL, edge_vertices_g, n_edges, 2);


    BFT_FREE(edge_vertices_g);
  };

  new_nodal->gc_set = NULL;

  return (new_nodal);
}

/*----------------------------------------------------------------------------
 * Dump printout of a nodal representation structure.
 *
 * parameters:
 *   this_nodal <-- pointer to structure that should be dumped
 *----------------------------------------------------------------------------*/

void
fvm_nodal_dump(const fvm_nodal_t  *this_nodal)
{
  cs_lnum_t   i;
  cs_lnum_t   num_vertex = 1;
  const cs_coord_t  *coord = this_nodal->vertex_coords;

  /* Global indicators */
  /*--------------------*/

  bft_printf("\n"
             "Mesh name:\"%s\"\n",
             this_nodal->name);

  bft_printf("\n"
             "Mesh dimension:               %d\n"
             "Domain number:                %d\n"
             "Number of domains:            %d\n"
             "Number of sections:           %d\n",
             this_nodal->dim, this_nodal->num_dom, this_nodal->n_doms,
             this_nodal->n_sections);

  bft_printf("\n"
             "Number of cells:               %d\n"
             "Number of faces:               %d\n"
             "Number of edges:               %d\n"
             "Number of vertices:            %d\n",
            this_nodal->n_cells,
            this_nodal->n_faces,
            this_nodal->n_edges,
            this_nodal->n_vertices);

  if (this_nodal->n_vertices > 0) {

    bft_printf("\n"
               "Pointers to shareable arrays:\n"
               "  vertex_coords:        %p\n"
               "  parent_vertex_num:    %p\n",
               (const void *)this_nodal->vertex_coords,
               (const void *)this_nodal->parent_vertex_num);

    bft_printf("\n"
               "Pointers to local arrays:\n"
               "  _vertex_coords:       %p\n"
               "  _parent_vertex_num:   %p\n",
               (const void *)this_nodal->_vertex_coords,
               (const void *)this_nodal->_parent_vertex_num);

    /* Output coordinates depending on parent numbering */

    if (this_nodal->parent_vertex_num == NULL) {

      bft_printf("\nVertex coordinates:\n\n");
      switch(this_nodal->dim) {
      case 1:
        for (i = 0; i < this_nodal->n_vertices; i++)
          bft_printf("%10d : %12.5f\n",
                     num_vertex++, (double)(coord[i]));
        break;
      case 2:
        for (i = 0; i < this_nodal->n_vertices; i++)
          bft_printf("%10d : %12.5f %12.5f\n",
                     num_vertex++, (double)(coord[i*2]),
                     (double)(coord[i*2+1]));
        break;
      case 3:
        for (i = 0; i < this_nodal->n_vertices; i++)
          bft_printf("%10d : %12.5f %12.5f %12.5f\n",
                     num_vertex++, (double)(coord[i*3]),
                     (double)(coord[i*3+1]), (double)(coord[i*3+2]));
        break;
      default:
        bft_printf("coordinates not output\n"
                   "dimension = %d unsupported\n", this_nodal->dim);
      }

    }
    else { /* if (this_nodal->parent_vertex_num != NULL) */

      bft_printf("\nVertex parent and coordinates:\n\n");

      switch(this_nodal->dim) {
      case 1:
        for (i = 0; i < this_nodal->n_vertices; i++) {
          coord =   this_nodal->vertex_coords
                  + (this_nodal->parent_vertex_num[i]-1);
          bft_printf("%10d : %12.5f\n",
                     num_vertex++, (double)(coord[0]));
        }
        break;
      case 2:
        for (i = 0; i < this_nodal->n_vertices; i++) {
          coord =   this_nodal->vertex_coords
                  + ((this_nodal->parent_vertex_num[i]-1)*2);
          bft_printf("%10d : %12.5f %12.5f\n",
                     num_vertex++, (double)(coord[0]), (double)(coord[1]));
        }
        break;
      case 3:
        for (i = 0; i < this_nodal->n_vertices; i++) {
          coord =   this_nodal->vertex_coords
                  + ((this_nodal->parent_vertex_num[i]-1)*3);
          bft_printf("%10d : %12.5f %12.5f %12.5f\n",
                     num_vertex++, (double)(coord[0]), (double)(coord[1]),
                     (double)(coord[2]));
        }
        break;
      default:
        bft_printf("coordinates not output\n"
                   "dimension = %d unsupported\n", this_nodal->dim);
      }

    }

  }

  /* Global vertex numbers (only for parallel execution) */
  if (this_nodal->global_vertex_num != NULL) {
    bft_printf("\nGlobal vertex numbers:\n\n");
    fvm_io_num_dump(this_nodal->global_vertex_num);
  }

  /* Dump element sections */
  /*-----------------------*/

  for (i = 0; i < this_nodal->n_sections; i++)
    _fvm_nodal_section_dump(this_nodal->sections[i]);

  /* Dump group class set (NULL allowed) */

  fvm_group_class_set_dump(this_nodal->gc_set);
}

/*----------------------------------------------------------------------------*/

#ifdef __cplusplus
}
#endif /* __cplusplus */
