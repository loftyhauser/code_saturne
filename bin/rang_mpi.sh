#!/bin/sh
#============================================================================
#
#                    Code_Saturne version 1.3
#                    ------------------------
#
#
#     This file is part of the Code_Saturne Kernel, element of the
#     Code_Saturne CFD tool.
#
#     Copyright (C) 1998-2011 EDF S.A., France
#
#     contact: saturne-support@edf.fr
#
#     The Code_Saturne Kernel is free software; you can redistribute it
#     and/or modify it under the terms of the GNU General Public License
#     as published by the Free Software Foundation; either version 2 of
#     the License, or (at your option) any later version.
#
#     The Code_Saturne Kernel is distributed in the hope that it will be
#     useful, but WITHOUT ANY WARRANTY; without even the implied warranty
#     of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
#     GNU General Public License for more details.
#
#     You should have received a copy of the GNU General Public License
#     along with the Code_Saturne Kernel; if not, write to the
#     Free Software Foundation, Inc.,
#     51 Franklin St, Fifth Floor,
#     Boston, MA  02110-1301  USA
#
#============================================================================
#
# A priori detection of the rank of a MPI process
#================================================
#
# In order to get the MPI rank from a script launched
# by mpirun (or prun, mpijob, or equivalent):
#
# MPI_RANK=`$CS_HOME/bin/rang_mpi.sh $@`
#
# Mainly useful to launch MPMD applications
# like coupling within MPI 1.2 environment
# which does not have a command like mpiexec
# (or some appschemes as LAM)

# For MPICH2 (which also provides the mpiexec command)
if [ "$PMI_RANK" != "" ] ; then
  MPI_RANK="$PMI_RANK"

# For Open MPI (which also provides the mpiexec command)
elif [ "$OMPI_MCA_ns_nds_vpid" != "" ] ; then # Open MPI 1.2
  MPI_RANK="$OMPI_MCA_ns_nds_vpid"
elif [ "$OMPI_COMM_WORLD_RANK" != "" ] ; then # Open MPI 1.3 and above
  MPI_RANK="$OMPI_COMM_WORLD_RANK"

# For MPICH-GM
elif [ "$GMPI_ID" != "" ] ; then
  MPI_RANK="$GMPI_ID"

# For MVAPICH
elif [ "$SMPIRUN_RANK" != "" ] ; then
  MPI_RANK="$MPIRUN_RANK"

# For LAM 7
elif [ "$LAMRANK" != "" ] ; then
  MPI_RANK="$LAMRANK"

# On cluster with HP-MPI (CCRT: Tantale, old system)
elif [ "$MPI_PROC" != "" ] ; then
  MPI_RANK=`echo $MPI_PROC | cut -f 2 -d,`

# On HP AlphaServer cluster
elif [ "$RMS_RANK" != "" ] ; then
  MPI_RANK="$RMS_RANK"

# For Scali MPI (SCI network)
elif [ "$SCAMPI_PROCESS_PARAM" != "" ] ; then
  MPI_RANK=`echo $SCAMPI_PROCESS_PARAM | cut -f4 -d' '`

# When run under SLURM using srun
elif [ "$SLURM_PROCID" != "" ] ; then
  MPI_RANK="$SLURM_PROCID"

# For "standard" MPICH 1.2 (with "usual" chp4 communication)
else
  MPI_RANK=0
  next_arg_is_rank=""
  for arg in "$@" ; do
    if [ "$arg" = "-p4rmrank" ] ; then
      next_arg_is_rank="1"
    elif [ "$next_arg_is_rank" = "1" ] ; then
      MPI_RANK="$arg"
      next_arg_is_rank="0"
    fi
  done

# End of known cases
fi

# Output of the obtained rank

echo "$MPI_RANK"

