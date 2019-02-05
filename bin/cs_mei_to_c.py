#!/usr/bin/env python
# -*- coding: utf-8 -*-

#-------------------------------------------------------------------------------

# This file is part of Code_Saturne, a general-purpose CFD tool.
#
# Copyright (C) 1998-2019 EDF S.A.
#
# This program is free software; you can redistribute it and/or modify it under
# the terms of the GNU General Public License as published by the Free Software
# Foundation; either version 2 of the License, or (at your option) any later
# version.
#
# This program is distributed in the hope that it will be useful, but WITHOUT
# ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS
# FOR A PARTICULAR PURPOSE.  See the GNU General Public License for more
# details.
#
# You should have received a copy of the GNU General Public License along with
# this program; if not, write to the Free Software Foundation, Inc., 51 Franklin
# Street, Fifth Floor, Boston, MA 02110-1301, USA.

#-------------------------------------------------------------------------------

import os
import re

from code_saturne.model.NotebookModel import NotebookModel

#===============================================================================
# Code block templates
#===============================================================================

#-------------------------------------------------------------------------------

_file_header = \
"""/*----------------------------------------------------------------------------*/
/*
  This file is generated by Code_Saturne, a general-purpose CFD tool.
*/
/*----------------------------------------------------------------------------*/

#include "cs_defs.h"

/*----------------------------------------------------------------------------
 * Standard C library headers
 *----------------------------------------------------------------------------*/

#include <assert.h>
#include <math.h>

#if defined(HAVE_MPI)
#include <mpi.h>
#endif

/*----------------------------------------------------------------------------
 *  Local headers
 *----------------------------------------------------------------------------*/

#include "cs_headers.h"
"""

_file_header2 = \
"""#include "nc_phases.h"

"""

_file_header3 = \
"""/*----------------------------------------------------------------------------*/

BEGIN_C_DECLS

/*----------------------------------------------------------------------------*/

"""

_file_footer = \
"""}

/*----------------------------------------------------------------------------*/

END_C_DECLS

"""

_vol_function_header = \
"""void
cs_meg_volume_function(cs_field_t              *f,
                       const cs_volume_zone_t  *vz)
{
"""

_bnd_function_header = \
"""cs_real_t *
cs_meg_boundary_function(const char               *field_name,
                         const char               *condition,
                         const cs_boundary_zone_t *bz)
{
  cs_real_t *new_vals = NULL;

"""

#-------------------------------------------------------------------------------

_cs_math_internal_name = {'abs':'cs_math_fabs',
                          'min':'cs_math_fmin',
                          'max':'cs_math_fmax'}

_pkg_fluid_prop_dict = {}
_pkg_fluid_prop_dict['code_saturne'] = {'rho0':'ro0',
                                        'mu0':'viscl0',
                                        'p0':'p0',
                                        'cp0':'cp0'}

_pkg_fluid_prop_dict['neptune_cfd'] = {'rho0':'ro0',
                                       'mu0':'viscl0',
                                       'cp0':'cp0',
                                       'lambda0':'lambda0'}

_pkg_glob_struct = {'code_saturne':'cs_glob_fluid_properties',
                    'neptune_cfd':'nc_phases->p_ini[PHASE_ID]'}



#===============================================================================
# Main class
#===============================================================================

class mei_to_c_interpreter:

    #---------------------------------------------------------------------------

    def __init__(self, case, create_functions=True):

        self.case = case
        self.pkg_name = case['package'].name

        self.tmp_path = os.path.join(case['case_path'], 'DATA', 'tmp')

        # function name to file name dictionary
        self.vol_funcs = {}
        self.bnd_funcs = {}

        self.code_to_write = ""

        nb = NotebookModel(self.case)
        self.notebook = {}
        for (nme, val) in nb.getNotebookList():
            self.notebook[nme] = str(val)

        if create_functions:
            # Volume code
            self.generate_volume_code()

            # Bouondary code
            self.generate_boundary_code()

    #---------------------------------------------------------------------------

    def break_expression(self, exp):

        expression_lines = []

        for line in exp.split('\n'):
            line_comp = []
            for elt in re.split('=|\+|-|\*|\/|\(|\)|;|,',line):
                if elt != '':
                    line_comp.append((elt.lstrip()).rstrip())

            expression_lines.append(line_comp)

        return expression_lines

    #---------------------------------------------------------------------------

    def update_cell_block_expression(self, new_exp, key):

        self.vol_funcs[key]['exp'] = new_exp
        self.vol_funcs[key]['lines'] = self.break_expression(new_exp)

    #---------------------------------------------------------------------------

    def init_cell_block(self,
                        expression,
                        required,
                        symbols,
                        scalars,
                        name,
                        zone_name='all_cells'):

        if name in self.vol_funcs.keys():
            if self.vol_funcs[name]['zone'] == zone_name:
                msg = "Formula for variable %s in zone %s was already defined:\n %s" \
                        % (name, zone_name, self.vol_funcs[name]['exp'])
                raise Exception(msg)

        self.vol_funcs[name] = {'exp':expression,
                                'sym':symbols,
                                'sca':scalars,
                                'req':required,
                                'zone':zone_name}

        self.vol_funcs[name]['lines'] = self.break_expression(expression)

    #---------------------------------------------------------------------------

    def write_cell_block(self, name):

        # Check if function exists
        if name not in self.vol_funcs.keys():
            return

        expression = self.vol_funcs[name]['exp']
        symbols    = self.vol_funcs[name]['sym']
        scalars    = self.vol_funcs[name]['sca']
        required   = self.vol_funcs[name]['req']
        zone       = self.vol_funcs[name]['zone']
        exp_lines_comp = self.vol_funcs[name]['lines']

        # Get user definitions and code
        usr_defs = ''
        usr_code = ''
        usr_blck = ''

        tab   = "  "
        ntabs = 2

        known_symbols = []
        coords = ['x', 'y', 'z']
        need_coords = False

        internal_fields = []

        # Add to definitions useful arrays or scalars
        for line_comp in exp_lines_comp:
            for s in symbols:
                sn = s[0]
                if sn in line_comp and sn not in known_symbols:
                    if sn == 'dt':
                        usr_defs += ntabs*tab
                        usr_defs += 'const cs_real_t dt = cs_glob_time_step->dt;\n'
                        known_symbols.append(sn)
                    elif sn == 't':
                        usr_defs += ntabs*tab
                        usr_defs += 'const cs_real_t time = cs_glob_time_step->t_cur;\n'
                        known_symbols.append(sn)
                    elif sn == 'iter':
                        usr_defs += ntabs*tab
                        usr_defs += 'const int iter = cs_glob_time_step->nt_cur;\n'
                        known_symbols.append(sn)
                    elif sn in coords:
                        ic = coords.index(sn)
                        lxyz = 'const cs_real_t %s = xyz[c_id][%s];\n' % (sn, str(ic))
                        usr_code += (ntabs+1)*tab + lxyz
                        known_symbols.append(sn)
                        need_coords = True
                    elif sn in self.notebook.keys():
                        l = 'const cs_real_t %s = cs_notebook_parameter_value_by_name("%s");\n' \
                                % (sn, sn)
                        usr_defs += ntabs*tab + l
                        known_symbols.append(sn)

                    elif sn in _pkg_fluid_prop_dict[self.pkg_name].keys():
                        if len(name.split("_")) > 1:
                            try:
                                phase_id = int(name.split('_')[-1])-1
                            except:
                                phase_id = -1
                        else:
                            phase_id = -1
                        gs = _pkg_glob_struct[self.pkg_name].replace('PHASE_ID',
                                                                     str(phase_id))
                        pn = _pkg_fluid_prop_dict[self.pkg_name][sn]
                        ms = 'const cs_real_t %s = %s->%s;\n' %(sn, gs, pn)
                        usr_defs += ntabs*tab + ms
                        known_symbols.append(sn)

                    elif s not in scalars:
                        if len(s[1].split('=')) > 1:
                            sval = s[1].split('=')[-1]
                            usr_defs += ntabs*tab + 'const cs_real_t '+sn+' = '+str(sval)+';\n'
                            known_symbols.append(sn)
                        else:
                            internal_fields.append((sn, s[1].lower()))

        if need_coords:
            usr_defs = ntabs*tab \
                     + 'const cs_real_3_t xyz = (cs_real_3_t *)cs_glob_mesh_quantities->cell_cen;' \
                     + '\n\n' \
                     + usr_defs

        use_internal_fields = False
        for f in internal_fields:
            for line in exp_lines_comp:
                if f[0] in line:
                    use_internal_fields = True
                    l = 'const cs_real_t *%s_vals = cs_field_by_name("%s")->val;\n' \
                            % (f[0], f[1])
                    usr_defs += ntabs*tab + l
                    known_symbols.append(f[0])
                    usr_code += (ntabs+1)*tab + 'const cs_real_t %s = %s_vals[c_id];\n'\
                            % (f[0], f[0])

                    break

        if use_internal_fields:
            usr_code += '\n'

        use_scalars = False
        for s in scalars:
            sn = s[0]
            for line in exp_lines_comp:
                if sn in line:
                    use_scalars = True
                    l = 'const cs_real_t *%s_vals = cs_field_by_name("%s")->val;\n' \
                    % (sn,sn)
                    usr_defs += ntabs*tab + l
                    known_symbols.append(sn)
                    usr_code += (ntabs+1)*tab + 'const cs_real_t %s = %s_vals[c_id];\n' \
                    % (sn, sn)
                    break

        if use_scalars:
            usr_code += '\n'

        # Internal names of mathematical functions
        for key in _cs_math_internal_name.keys():
            if key in expression:
                expression = expression.replace(key+'(',
                                                _cs_math_internal_name[key]+'(')

        for line in exp_lines_comp:
            if 'pi' in line:
                usr_defs += ntabs*tab + 'const cs_real_t pi = cs_math_pi;\n'

        for s in required:
            known_symbols.append(s[0]);


        ntabs += 1
        if_loop = False

        exp_lines = expression.split("\n")
        for l in exp_lines:
            if len(l) > 0:
                lf = l.split('=')
                if len(lf) > 1 and lf[0].rstrip() not in known_symbols:
                    known_symbols.append(lf[0])
                    l = 'const cs_real_t '+l

                if l[:2] == 'if' and l[-1] != '{':
                    l = l + ' {'
                    if_loop = True

                if 'else' in l:
                    if l[0] != '}':
                        l = '} '+l
                    if l[-1] != '{':
                        l = l + ' {'

                if l[0] == '}':
                    ntabs -= 1
                usr_code += '\n'
                usr_code += ntabs*tab + l
                if l[-1] == '{':
                    ntabs += 1
            else:
                usr_code += '\n'

        if if_loop and usr_code[-1] != '}':
            usr_code += '\n' + (ntabs-1)*tab + '}\n'
        else:
            usr_code += '\n'

        usr_code = usr_code.replace(required[0][0], 'f->val[c_id]', 1)

        # Change comments symbol from # to //
        usr_code = usr_code.replace('#', '//')

        # Write the block
        usr_blck = tab + 'if (strcmp(f->name, "%s") == 0 && strcmp(vz->name, "%s") == 0) {\n' \
                % (name, zone)

        usr_blck += usr_defs + '\n'

        usr_blck += 2*tab + 'for (cs_lnum_t e_id = 0; e_id < vz->n_cells; e_id++) {\n'
        usr_blck += 3*tab + 'cs_lnum_t c_id = vz->cell_ids[e_id];\n'

        usr_blck += usr_code
        usr_blck += 2*tab + '}\n'
        usr_blck += tab + '}\n'

        return usr_blck

    #---------------------------------------------------------------------------

    def init_bnd_block(self,
                       expression,
                       required,
                       name,
                       bnd_name,
                       condition):

        func_key = '::'.join([bnd_name, name])
        if func_key in self.bnd_funcs.keys():
            msg = "Formula for variable %s in boundary %s was already defined:\n %s" \
                    % (name, bnd_name, self.vol_funcs[name]['exp'])
            raise Exception(msg)

        self.bnd_funcs[func_key] = {'exp':expression,
                                    'req':required,
                                    'cnd':condition}

        self.bnd_funcs[func_key]['lines'] = self.break_expression(expression)

    #---------------------------------------------------------------------------

    def write_bnd_block(self, func_key):

        if func_key not in self.bnd_funcs.keys():
            return

        expression = self.bnd_funcs[func_key]['exp']
        required   = self.bnd_funcs[func_key]['req']
        cname      = self.bnd_funcs[func_key]['cnd']

        exp_lines_comp = self.bnd_funcs[func_key]['lines']

        zone, field_name = func_key.split('::')

        # Check if for loop is needed
        need_for_loop = True
        if cname == 'flow1_formula' or cname == 'flow2_formula':
            need_for_loop = False

        if need_for_loop:
            symbols = ['x', 'y', 'z', 't', 'dt', 'iter']
        else:
            symbols = ['t', 'dt', 'iter']

        # Get user definitions and code
        usr_defs = ''
        usr_code = ''
        usr_blck = ''

        tab   = "  "
        ntabs = 2

        known_symbols = []
        for req in required:
            known_symbols.append(req)
        coords = ['x', 'y', 'z']
        need_coords = False

        internal_fields = []

        # allocate the new array
        if need_for_loop:
            usr_defs += ntabs*tab + 'const int vals_size = bz->n_faces * %d;\n' % (len(required))
        else:
            usr_defs += ntabs*tab + 'const int vals_size = %d;\n' % (len(required))

        usr_defs += ntabs*tab + 'BFT_MALLOC(new_vals, vals_size, cs_real_t);\n'
        usr_defs += '\n'

        # Add to definitions useful arrays or scalars
        for line_comp in exp_lines_comp:
            for s in symbols:
                sn = s[0]
                if sn in line_comp and sn not in known_symbols:
                    if sn == 'dt':
                        usr_defs += ntabs*tab
                        usr_defs += 'const cs_real_t dt = cs_glob_time_step->dt;\n'
                        known_symbols.append(sn)
                    elif sn == 't':
                        usr_defs += ntabs*tab
                        usr_defs += 'const cs_real_t t = cs_glob_time_step->t_cur;\n'
                        known_symbols.append(sn)
                    elif sn == 'iter':
                        usr_defs += ntabs*tab
                        usr_defs += 'const int iter = cs_glob_time_step->nt_cur;\n'
                        known_symbols.append(sn)
                    elif sn in coords:
                        ic = coords.index(sn)
                        lxyz = 'const cs_real_t %s = xyz[f_id][%s];\n' % (sn, str(ic))
                        usr_code += (ntabs+1)*tab + lxyz
                        known_symbols.append(sn)
                        need_coords = True
            for nb in self.notebook.keys():
                if nb in line_comp and nb not in known_symbols:
                        l = 'const cs_real_t %s = cs_notebook_parameter_value_by_name("%s");\n' \
                                % (sn, sn)
                        usr_defs += ntabs*tab + l
                        known_symbols.append(sn)

        if need_coords:
            usr_defs = ntabs*tab \
                     + 'const cs_real_3_t *xyz = (cs_real_3_t *)cs_glob_mesh_quantities->b_face_cog;' \
                     + '\n\n' \
                     + usr_defs

        # Internal names of mathematical functions
        for key in _cs_math_internal_name.keys():
            if key in expression:
                expression = expression.replace(key+'(',
                                                _cs_math_internal_name[key]+'(')

        for line in exp_lines_comp:
            if 'pi' in line:
                usr_defs += ntabs*tab + 'const cs_real_t pi = cs_math_pi;\n'

        if need_for_loop:
            ntabs += 1

        if_loop = False

        # Parse the Mathematical expression and generate the C block code
        exp_lines = expression.split("\n")
        for l in exp_lines:
            if len(l) > 0:
                lf = l.split('=')
                if len(lf) > 1 and lf[0].rstrip() not in known_symbols:
                    known_symbols.append(lf[0])
                    l = 'const cs_real_t '+l

                if l[:2] == 'if' and l[-1] != '{':
                    l = l + ' {'
                    if_loop = True

                if 'else' in l:
                    if l[0] != '}':
                        l = '} '+l
                    if l[-1] != '{':
                        l = l + ' {'

                if l[0] == '}':
                    ntabs -= 1
                usr_code += '\n'
                usr_code += ntabs*tab + l
                if l[-1] == '{':
                    ntabs += 1
            else:
                usr_code += '\n'

        if if_loop and usr_code[-1] != '}':
            usr_code += '\n' + (ntabs-1)*tab + '}\n'
        else:
            usr_code += '\n'

        # Change comments symbol from # to //
        usr_code = usr_code.replace('#', '//')

        for ir in range(len(required)):
            if need_for_loop:
                new_v = 'new_vals[%d * bz->n_faces + e_id]' % (ir)
            else:
                new_v = 'new_vals[%d]' % (ir)

            usr_code = usr_code.replace(required[ir], new_v, 1)

        # Write the block
        block_cond  = tab + 'if (strcmp(field_name, "%s") == 0 &&\n' % (field_name)
        block_cond += tab + '    strcmp(condition, "%s") == 0 &&\n' % (cname)
        block_cond += tab + '    strcmp(bz->name, "%s") == 0) {\n' % (zone)
        usr_blck = block_cond + '\n'

        usr_blck += usr_defs + '\n'

        if need_for_loop:
            usr_blck += 2*tab + 'for (cs_lnum_t e_id = 0; e_id < bz->n_faces; e_id++) {\n'
            usr_blck += 3*tab + 'cs_lnum_t f_id = bz->face_ids[e_id];\n'

        usr_blck += usr_code

        if need_for_loop:
            usr_blck += 2*tab + '}\n'

        usr_blck += tab + '}\n'

        return usr_blck

    #---------------------------------------------------------------------------

    def generate_volume_code(self):

        if self.pkg_name == 'code_saturne':
            authorized_fields = ['density', 'molecular_viscosity',
                                 'specific_heat', 'thermal_conductivity']
            from code_saturne.model.FluidCharacteristicsModel import FluidCharacteristicsModel

            from code_saturne.model.CompressibleModel import CompressibleModel
            if CompressibleModel(self.case).getCompressibleModel() != 'off':
                authorized_fields.append('volume_viscosity')

            fcm = FluidCharacteristicsModel(self.case)
            for fk in authorized_fields:
                if fcm.getPropertyMode(fk) == 'user_law':
                    exp, req, sca, sym = fcm.getFormulaComponents(fk)
                    self.init_cell_block(exp, req, sym, sca, fk)

            slist = fcm.m_sca.getUserScalarNameList()
            for s in fcm.m_sca.getScalarsVarianceList():
                if s in slist: slist.remove(s)
            if slist != []:
                for s in slist:
                    diff_choice = fcm.m_sca.getScalarDiffusivityChoice(s)
                    if diff_choice == 'variable':
                        dname = fcm.m_sca.getScalarDiffusivityName(s)
                        exp, req, sca, sym, = \
                        fcm.getFormulaComponents('scalar_diffusivity', s)
                        self.init_cell_block(exp, req, sym, sca, dname)

        else:
            from code_saturne.model.ThermodynamicsModel import ThermodynamicsModel
            from code_saturne.model.MainFieldsModel import MainFieldsModel

            tm = ThermodynamicsModel(self.case)
            mfm = MainFieldsModel(self.case)

            authorized_fields = ['density', 'molecular_viscosity',
                                 'specific_heat', 'thermal_conductivity']

            compressible_fields = ['d_rho_d_P', 'd_rho_d_h']

            for fieldId in tm.getFieldIdList():
                if tm.getMaterials(fieldId) == 'user_material':
                    for fk in authorized_fields:
                        if tm.getPropertyMode(fieldId, fk) == 'user_law':
                            name = fk + '_' + str(fieldId)
                            exp, req, sca, sym = tm.getFormulaComponents(fieldId,fk)
                            self.init_cell_block(exp, req, sym, sca, name)

                    if mfm.getCompressibleStatus(fieldId) == 'on':
                        for fk in compressible_fields:
                            name = fk + '_' + str(fieldId)
                            exp, req, sca, sym = tm.getFormulaComponents(fieldId,fk)
                            self.init_cell_block(exp, req, sym, sca, name)

                    # Temperature as a function of enthalpie
                    if mfm.getEnergyResolution(fieldId) == 'on':
                        name = 'temperature_' + str(fieldId)
                        exp, req, sca, sym = tm.getFormulaComponents(fieldId,
                                                                    'temperature')
                        self.init_cell_block(exp, req, sym, sca, name)

    #---------------------------------------------------------------------------

    def generate_boundary_code(self):

        if self.pkg_name == 'code_saturne':
            from code_saturne.model.LocalizationModel import LocalizationModel
            from code_saturne.model.Boundary import Boundary
            from code_saturne.model.TurbulenceModel import TurbulenceModel

            blm = LocalizationModel('BoundaryZone', self.case)
            tm = TurbulenceModel(self.case)

            for zone in blm.getZones():
                if zone._nature == "symmetry":
                    continue

                boundary = Boundary(zone._nature, zone._label, self.case)

                # Velocity for inlets
                if 'inlet' in zone._nature and zone._nature != 'free_inlet_outlet':
                    c = boundary.getVelocityChoice()
                    if '_formula' in c:
                        if c == 'norm_formula':
                            req = ['u_norm']
                        elif c == 'flow1_formula':
                            req = ['q_m']
                        elif c == 'flow2_formula':
                            req = ['q_v']

                        name = 'velocity'

                        exp = boundary.getVelocity()
                        self.init_bnd_block(exp, req, name, zone._label, c)

                    d = boundary.getDirectionChoice()
                    if d == 'formula':
                        req  = ['dir_x', 'dir_y', 'dir_z']
                        exp  = boundary.getDirection('direction_formula')
                        name = 'direction'

                        self.init_bnd_block(exp, req, name, zone._label, d)

                    # Turbulence
                    tc = boundary.getTurbulenceChoice()
                    if tc == 'formula':
                        turb_model = tm.getTurbulenceModel()
                        if turb_model in ('k-epsilon', 'k-epsilon-PL'):
                            name = 'turbulence_ke'
                            req  = ['k', 'epsilon']
                        elif turb_model in ('Rij-epsilon', 'Rij-SSG'):
                            name = 'turbulence_rije'
                            # Carefull! The order of rij components must be the same
                            # as in the code (r23 before r13)
                            req  = ['r11', 'r22', 'r33',
                                    'r12', 'r23', 'r13',
                                    'epsilon']
                        elif turb_model == 'Rij-EBRSM':
                            name = 'turbulence_rij_ebrsm'
                            # Carefull! The order of rij components must be the same
                            # as in the code (r23 before r13)
                            req  = ['r11', 'r22', 'r33',
                                    'r12', 'r23', 'r13',
                                    'epsilon', 'alpha']
                        elif turb_model == 'v2f-BL-v2/k':
                            name = 'turbulence_v2f'
                            req  = ['k', 'epsilon', 'phi', 'alpha']
                        elif turb_model == 'k-omega-SST':
                            name = 'turbulence_kw'
                            req  = ['k', 'omega']
                        elif turb_model == 'Spalart-Allmaras':
                            name = 'turbulence_spalart'
                            req  = ['nu_tilda']

                        exp = boundary.getTurbFormula()
                        self.init_bnd_block(exp, req, name, zone._label, tc)

                # Specific free_inlet_outlet head loss
                if zone._nature == 'free_inlet_outlet':
                    name = "head_loss"
                    req  = ['K']
                    exp = boundary.getHeadLossesFormula()
                    self.init_bnd_block(exp, req, name, zone._label, 'formula')

                # Hydraulic head for groundwater flow
                if zone._nature == 'groundwater':
                    c = boundary.getHydraulicHeadChoice()
                    if c == 'dirichlet_formula':
                        name = 'hydraulic_head'
                        req  = ['H']
                        exp  = boundary.getHydraulicHeadFormula()
                        self.init_bnd_block(exp, req, name, zone._label, c)



                # Scalars
                scalar_list = boundary.sca_model.getScalarNameList()
                if boundary.sca_model.getMeteoScalarsNameList() != None:
                    for sca in boundary.sca_model.getMeteoScalarsNameList():
                        scalar_list.append(sca)
                if len(boundary.sca_model.getThermalScalarName()) > 0:
                    scalar_list.append(boundary.sca_model.getThermalScalarName()[0])

                for sca in scalar_list:
                    c = boundary.getScalarChoice(sca)
                    if '_formula' in c:
                        exp = boundary.getScalarFormula(sca, c)
                        if c == 'dirichlet_formula':
                            req = [sca]
                        elif c == 'neumann_formula':
                            req = ['flux']
                        else:
                            req = [sca, 'hc']
                        self.init_bnd_block(exp, req, sca, zone._label, c)

    #---------------------------------------------------------------------------

    def check_volume_code_syntax(self):

        if not os.path.exists(self.tmp_path):
            os.makedirs(self.tmp_path)

        self.save_volume_function(self.tmp_path)

        from code_saturne import cs_compile

        os.chdir(self.tmp_path)
        out = open('comp.out', 'w')
        err = open('comp.err', 'w')
        compilation_test = cs_compile.compile_and_link(self.case['package'],
                                                       self.tmp_path,
                                                       opt_cflags='-w',
                                                       stdout=out,
                                                       stderr=err)
        out.close()
        err.close()

        n_errors = 0
        if compilation_test != 0:
            errors = open('comp.err', 'r').readlines()
            for i in range(len(errors)):
                if 'error:' in errors[i]:
                    msg = (errors[i].split('error:')[-1].rstrip()).lstrip()+'\n'
                    msg += (errors[i+1].rstrip()).lstrip() + '\n'

                    n_errors += 1
        else:
            msg = None

        return compilation_test, msg, n_errors

    #---------------------------------------------------------------------------

    def clean_tmp_dir(self):

        if os.path.exists(self.tmp_path):
            fl = os.listdir(self.tmp_path)
            for f in fl:
                self.delete_file(f, self.tmp_path)
            os.rmdir(self.tmp_path)

    #---------------------------------------------------------------------------


    #---------------------------------------------------------------------------

    def has_meg_code(self):

        retcode = False

        if len(self.vol_funcs) > 0 or len(self.bnd_funcs) > 0:
            retcode = True

        return retcode

    #---------------------------------------------------------------------------

    def delete_file(self, c_file_name, hard_path=None):

        # Copy function file if needed
        if hard_path:
            fpath = os.path.join(hard_path, c_file_name)
        else:
            fpath = os.path.join(self.case['case_path'], 'SRC', c_file_name);

        if os.path.isfile(fpath):
            os.remove(fpath)

    #---------------------------------------------------------------------------

    def save_file(self, c_file_name, code_to_write, hard_path=None):

        if code_to_write != '':
            # Try and write the volume function in the src if in RESU folder
            # For debugging purposes
            try:
                if hard_path:
                    fpath = os.path.join(hard_path, c_file_name)
                else:
                    fpath = os.path.join(self.case['case_path'],
                                         'src',
                                         c_file_name)

                new_file = open(fpath, 'w')
                new_file.write(code_to_write)
                new_file.close()
                return 1

            except:
                # Cant save the function. xml file will still be saved
                return -1

        # Return 0 if nothing is written for robustness
        else:
            return 0

    #---------------------------------------------------------------------------

    def save_volume_function(self, hard_path = None):

        # Delete previous existing file
        file2write = 'cs_meg_volume_function.c'
        self.delete_file(file2write)

        # Generate the functions code if needed
        code_to_write = ''
        if len(self.vol_funcs.keys()) > 0:
            code_to_write = _file_header
            if self.pkg_name != "code_saturne":
                code_to_write += _file_header2
            code_to_write += _file_header3 + _vol_function_header
            for key in self.vol_funcs.keys():
                m1 = 'User defined formula for variable %s over zone %s' \
                        % (key, self.vol_funcs[key]['zone'])
                m2 = '/*'+'-'*(74) +'*/\n'
                m1 = '/* '+ m1 +' */\n'

                code_to_write += "  " + m2
                code_to_write += "\n"
                code_to_write += "  " + m1 + '\n'
                code_to_write += self.write_cell_block(key)
                code_to_write += "\n"

            code_to_write += "  " + m2
            code_to_write += _file_footer

        # Write the C file if necessary
        write_status = self.save_file(file2write,
                                      code_to_write,
                                      hard_path = hard_path)

        return write_status

    #---------------------------------------------------------------------------

    def save_boundary_function(self):

        # Delete previous existing file
        file2write = 'cs_meg_boundary_function.c'
        self.delete_file(file2write)

        # Write the functions if needed
        code_to_write = ''
        if len(self.bnd_funcs.keys()) > 0:
            code_to_write = _file_header + _file_header3 + _bnd_function_header
            for key in self.bnd_funcs.keys():
                bnd, varname = key.split('::')
                m1 = 'User defined formula for "%s" over BC=%s' \
                        % (varname, bnd)
                m2 = '/* '+'-'*(len(m1))+' */\n'
                m1 = '/* '+m1+' */'

                code_to_write += "  " + m2
                code_to_write += "  " + m1 + '\n'
                code_to_write += self.write_bnd_block(key)
                code_to_write += "  " + m2 + '\n'

            code_to_write += "  return new_vals;\n"
            code_to_write += _file_footer


        # Write the C file if necessary
        write_status = self.save_file(file2write, code_to_write)

        return write_status

    #---------------------------------------------------------------------------

    def save_all_functions(self):

        save_status = 0

        vol_func_save_status = self.save_volume_function()
        if vol_func_save_status != 0:
            save_status = vol_func_save_status

        bnd_func_save_status = self.save_boundary_function()
        if bnd_func_save_status != 0:
            save_status = bnd_func_save_status

        return save_status

#-------------------------------------------------------------------------------
# End
#-------------------------------------------------------------------------------
