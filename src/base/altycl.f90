!-------------------------------------------------------------------------------

! This file is part of code_saturne, a general-purpose CFD tool.
!
! Copyright (C) 1998-2023 EDF S.A.
!
! This program is free software; you can redistribute it and/or modify it under
! the terms of the GNU General Public License as published by the Free Software
! Foundation; either version 2 of the License, or (at your option) any later
! version.
!
! This program is distributed in the hope that it will be useful, but WITHOUT
! ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS
! FOR A PARTICULAR PURPOSE.  See the GNU General Public License for more
! details.
!
! You should have received a copy of the GNU General Public License along with
! this program; if not, write to the Free Software Foundation, Inc., 51 Franklin
! Street, Fifth Floor, Boston, MA 02110-1301, USA.

!-------------------------------------------------------------------------------

!> \file altycl.f90
!> \brief Boundary condition code for the ALE module
!
!------------------------------------------------------------------------------

!------------------------------------------------------------------------------
! Arguments
!------------------------------------------------------------------------------
!   mode          name          role
!------------------------------------------------------------------------------
!> \param[in]     itypfb        boundary face types
!> \param[in,out] ialtyb        boundary face types for ALE
!> \param[in,out] impale        imposed displacement indicator
!> \param[in]     init          partial treatment (before time loop) if true
!> \param[in]     dt            time step (per cell)
!______________________________________________________________________________

subroutine altycl                          &
 ( itypfb , ialtyb , impale , init , dt )  &
 bind(C, name='cs_f_altycl')

!===============================================================================
! Module files
!===============================================================================

use paramx
use numvar
use optcal
use cstnum
use cstphy
use entsor
use parall
use mesh
use field
use albase, only: fdiale, iale
use cs_c_bindings

!===============================================================================

implicit none

! Arguments

integer(c_int) :: itypfb(nfabor)
integer(c_int) :: ialtyb(nfabor)
integer(c_int) :: impale(nnod)

logical(c_bool), value :: init
real(c_double) ::  dt(ncelet)

! Local variables

integer          ifac, iel
integer          ii, inod, iecrw, icpt, ierror(4), icoder(4), irkerr
double precision ddepx, ddepy, ddepz
double precision srfbnf, rnx, rny, rnz
double precision rcodcx, rcodcy, rcodcz, rcodsn

double precision, dimension(:,:), pointer :: disale
double precision, dimension(:,:), pointer :: xyzno0
double precision, allocatable, dimension(:,:) :: b_fluid_vel
integer, pointer, dimension(:,:) :: icodcl
double precision, pointer, dimension(:,:,:) :: rcodcl

!===============================================================================

!===============================================================================
! 1. Initializations
!===============================================================================

call field_build_bc_codes_all(icodcl, rcodcl) ! Get map

irkerr = -1

ierror(1) = 0
ierror(2) = 0
ierror(3) = 0
ierror(4) = 0

icoder(1) = -1
icoder(2) = -1
icoder(3) = -1
icoder(4) = -1

call field_get_val_v(fdiale, disale)
call field_get_val_v_by_name("vtx_coord0", xyzno0)

! Set to 0 non specified RCODCL arrays
do ifac = 1, nfabor
  if (rcodcl(ifac,iuma,1).gt.rinfin*0.5d0)                        &
    rcodcl(ifac,iuma,1) = 0.d0
  if (rcodcl(ifac,ivma,1).gt.rinfin*0.5d0)                        &
    rcodcl(ifac,ivma,1) = 0.d0
  if (rcodcl(ifac,iwma,1).gt.rinfin*0.5d0)                        &
    rcodcl(ifac,iwma,1) = 0.d0
enddo

!===============================================================================
! 2. Check the consistency of BC types (IALTYB)
!===============================================================================

! When using CDO solver, no need of checks.
if (iale.eq.2) then

  allocate(b_fluid_vel(3, nfabor))

  do ifac = 1, nfabor
    b_fluid_vel(1, ifac) = 0.d0
    b_fluid_vel(2, ifac) = 0.d0
    b_fluid_vel(3, ifac) = 0.d0
  enddo

  call cs_ale_update_bcs(ialtyb, b_fluid_vel)

  ! Copy back to deprecated rcodcl
  do ifac = 1, nfabor
    rcodcl(ifac, iuma, 1) = b_fluid_vel(1, ifac)
    rcodcl(ifac, ivma, 1) = b_fluid_vel(2, ifac)
    rcodcl(ifac, iwma, 1) = b_fluid_vel(3, ifac)
  enddo

  goto 100

endif

!  (valeur 0 autorisee)
do ifac = 1, nfabor
  if (ialtyb(ifac).ne.0      .and.                                &
      ialtyb(ifac).ne.ibfixe .and.                                &
      ialtyb(ifac).ne.igliss .and.                                &
      ialtyb(ifac).ne.ibalfs .and.                                &
      ialtyb(ifac).ne.ivimpo ) then
    if (ialtyb(ifac).gt.0) then
      ialtyb(ifac) = -ialtyb(ifac)
    endif
    ierror(1) = ierror(1) + 1
  endif
enddo

if (irangp.ge.0) call parcmx(ierror(1))
if (ierror(1).ne.0) then
  write(nfecra,1000)
  call boundary_conditions_error(ialtyb)
endif

!===============================================================================
! 3. Conversion  into BC codes and values (ICODCL and RCODCL)
!===============================================================================

! If all the nodes of a face have an imposed displacement, RCODCL is computed or
! overwritten, ALE BC type is therfore ivimpo

do ifac = 1, nfabor
  iecrw = 0
  ddepx = 0.d0
  ddepy = 0.d0
  ddepz = 0.d0
  icpt  = 0
  do ii = ipnfbr(ifac), ipnfbr(ifac+1)-1
    inod = nodfbr(ii)
    if (impale(inod).eq.0) iecrw = iecrw + 1
    icpt = icpt + 1
    ddepx = ddepx + disale(1,inod)+xyzno0(1,inod)-xyznod(1,inod)
    ddepy = ddepy + disale(2,inod)+xyzno0(2,inod)-xyznod(2,inod)
    ddepz = ddepz + disale(3,inod)+xyzno0(3,inod)-xyznod(3,inod)
  enddo
  ! For Sliding walls, no imposed velocity
  if (iecrw.eq.0.and.ialtyb(ifac).ne.igliss) then
    iel = ifabor(ifac)
    ialtyb(ifac) = ivimpo
    rcodcl(ifac,iuma,1) = ddepx/dt(iel)/icpt
    rcodcl(ifac,ivma,1) = ddepy/dt(iel)/icpt
    rcodcl(ifac,iwma,1) = ddepz/dt(iel)/icpt
  endif
enddo

! Remplissage des autres RCODCL a partir des ITYALB

do ifac = 1, nfabor

  iel = ifabor(ifac)

  ! --> Fixed faces
  !     On force alors les noeuds en question a etre fixes, pour eviter
  !       des problemes eventuels aux coins

  if (ialtyb(ifac).eq.ibfixe) then
    icpt = 0
    if (icodcl(ifac,iuma).eq.0) then
      icpt = icpt + 1
      icodcl(ifac,iuma) = 1
      rcodcl(ifac,iuma,1) = 0.d0
    endif
    if (icodcl(ifac,ivma).eq.0) then
      icpt = icpt + 1
      icodcl(ifac,ivma) = 1
      rcodcl(ifac,ivma,1) = 0.d0
    endif
    if (icodcl(ifac,iwma).eq.0) then
      icpt = icpt + 1
      icodcl(ifac,iwma) = 1
      rcodcl(ifac,iwma,1) = 0.d0
    endif
    ! Si on a fixe les trois composantes, alors on fixe les noeuds
    !   correspondants. Sinon c'est que l'utilisateur a modifie quelque
    !   chose     ... on le laisse seul maitre
    if (icpt.eq.3) then
      do ii = ipnfbr(ifac), ipnfbr(ifac+1)-1
        inod = nodfbr(ii)
        if (impale(inod).eq.0) then
          disale(1,inod) = 0.d0
          disale(2,inod) = 0.d0
          disale(3,inod) = 0.d0
          impale(inod) = 1
        endif
      enddo
    endif

  ! --> Sliding face
  elseif (ialtyb(ifac).eq.igliss) then

    if (icodcl(ifac,iuma).eq.0) icodcl(ifac,iuma) = 4
    if (icodcl(ifac,ivma).eq.0) icodcl(ifac,ivma) = 4
    if (icodcl(ifac,iwma).eq.0) icodcl(ifac,iwma) = 4

  ! --> Imposed mesh velocity face
  elseif (ialtyb(ifac).eq.ivimpo) then

    if (icodcl(ifac,iuma).eq.0) icodcl(ifac,iuma) = 1
    if (icodcl(ifac,ivma).eq.0) icodcl(ifac,ivma) = 1
    if (icodcl(ifac,iwma).eq.0) icodcl(ifac,iwma) = 1

  ! --> Free surface face: the mesh velocity is imposed by the mass flux
  elseif (ialtyb(ifac).eq.ibalfs) then

    if (icodcl(ifac,iuma).eq.0) then
      icodcl(ifac,iuma) = 1
    endif
    if (icodcl(ifac,ivma).eq.0) then
      icodcl(ifac,ivma) = 1
    endif
    if (icodcl(ifac,iwma).eq.0) then
      icodcl(ifac,iwma) = 1
    endif

  endif

enddo

!===============================================================================
! 4. Check ICODCL consistency
!===============================================================================

! When called before time loop, some values are not yet available.
if (init .eqv. .true.) return

do ifac = 1, nfabor

  if (icodcl(ifac,iuma).ne.1 .and.                                &
      icodcl(ifac,iuma).ne.2 .and.                                &
      icodcl(ifac,iuma).ne.3 .and.                                &
      icodcl(ifac,iuma).ne.4 ) then
    if (ialtyb(ifac).gt.0) then
      ialtyb(ifac) = -ialtyb(ifac)
    endif
    ierror(1) = ierror(1) + 1
  endif
  if (icodcl(ifac,ivma).ne.1 .and.                                &
      icodcl(ifac,ivma).ne.2 .and.                                &
      icodcl(ifac,ivma).ne.3 .and.                                &
      icodcl(ifac,ivma).ne.4 ) then
    if (ialtyb(ifac).gt.0) then
      ialtyb(ifac) = -ialtyb(ifac)
    endif
    ierror(2) = ierror(2) + 1
  endif
  if (icodcl(ifac,iwma).ne.1 .and.                                &
      icodcl(ifac,iwma).ne.2 .and.                                &
      icodcl(ifac,iwma).ne.3 .and.                                &
      icodcl(ifac,iwma).ne.4 ) then
    if (ialtyb(ifac).gt.0) then
      ialtyb(ifac) = -ialtyb(ifac)
    endif
    ierror(3) = ierror(3) + 1
  endif

  if ( ( icodcl(ifac,iuma).eq.4 .or.                              &
         icodcl(ifac,ivma).eq.4 .or.                              &
         icodcl(ifac,iwma).eq.4    ) .and.                        &
       ( icodcl(ifac,iuma).ne.4 .or.                              &
         icodcl(ifac,ivma).ne.4 .or.                              &
         icodcl(ifac,iwma).ne.4    ) ) then
    if (ialtyb(ifac).gt.0) then
      ialtyb(ifac) = -ialtyb(ifac)
    endif
    ierror(4) = ierror(4) + 1
  endif

  if (ialtyb(ifac).lt.0) then
    irkerr = irangp
    icoder(1) = -ialtyb(ifac)
    icoder(2) = icodcl(ifac,iuma)
    icoder(3) = icodcl(ifac,iuma)
    icoder(4) = icodcl(ifac,iuma)
  endif

enddo

if (irangp.ge.0) call parimx(4, ierror)

if (ierror(1).gt.0) then
  write(nfecra,2000) 'x', 'iuma'
endif
if (ierror(2).gt.0) then
  ierror(1) = 1
  write(nfecra,2000) 'y', 'ivma'
endif
if (ierror(3).gt.0) then
  ierror(1) = 1
  write(nfecra,2000) 'z', 'iwma'
endif
if (ierror(4).gt.0) then
  ierror(1) = 1
  if (irangp.ge.0) then
    call parcmx(irkerr)
    call parbci(irkerr, 4, icoder)
    write(nfecra,3000) icoder(1), icoder(2), icoder(3), icoder(4)
  endif
endif

if (ierror(1).gt.0) then
  write(nfecra,4000)
  call boundary_conditions_error(ialtyb)
endif

!===============================================================================
! 5. Fluid velocity BCs for walls and symmetries (due to mesh movment)
!===============================================================================

100  continue

! Pour les symetries on rajoute toujours la vitesse de maillage, car on
!   ne conserve que la vitesse normale
! Pour les parois, on prend la vitesse de maillage si l'utilisateur n'a
!   pas specifie RCODCL, sinon on laisse RCODCL pour la vitesse tangente
!   et on prend la vitesse de maillage pour la composante normale.
! On se base uniquement sur ITYPFB, a l'utilisateur de gere les choses
!   s'il rentre en CL non standards.

do ifac = 1, nfabor

  if (ialtyb(ifac).eq.ivimpo) then

    ! Warning: only the normal component is kept in clsyvt
    if (itypfb(ifac).eq.isymet) then
      rcodcl(ifac,iu,1) = rcodcl(ifac,iuma,1)
      rcodcl(ifac,iv,1) = rcodcl(ifac,ivma,1)
      rcodcl(ifac,iw,1) = rcodcl(ifac,iwma,1)
    endif

    if (itypfb(ifac).eq.iparoi .or.                        &
        itypfb(ifac).eq.iparug) then
      ! WARNING
      ! If nothing is set by the user, then the wall is supposed
      ! to move with the sliding wall!
      if (rcodcl(ifac,iu,1).gt.rinfin*0.5d0 .and.              &
          rcodcl(ifac,iv,1).gt.rinfin*0.5d0 .and.              &
          rcodcl(ifac,iw,1).gt.rinfin*0.5d0) then
        rcodcl(ifac,iu,1) = rcodcl(ifac,iuma,1)
        rcodcl(ifac,iv,1) = rcodcl(ifac,ivma,1)
        rcodcl(ifac,iw,1) = rcodcl(ifac,iwma,1)
      else
        ! Otherwise, if the user has set the fluid velocity
        ! Then the normal part is the one of the mesh velocity
        ! and the tangential part is the one of the user (completed with 0
        ! for non set values)
        if (rcodcl(ifac,iu,1).gt.rinfin*0.5d0)                 &
          rcodcl(ifac,iu,1) = 0.d0
        if (rcodcl(ifac,iv,1).gt.rinfin*0.5d0)                 &
          rcodcl(ifac,iv,1) = 0.d0
        if (rcodcl(ifac,iw,1).gt.rinfin*0.5d0)                 &
          rcodcl(ifac,iw,1) = 0.d0

        srfbnf = surfbn(ifac)
        rnx = surfbo(1,ifac)/srfbnf
        rny = surfbo(2,ifac)/srfbnf
        rnz = surfbo(3,ifac)/srfbnf
        rcodcx = rcodcl(ifac,iu,1)
        rcodcy = rcodcl(ifac,iv,1)
        rcodcz = rcodcl(ifac,iw,1)
        rcodsn = (rcodcl(ifac,iuma,1)-rcodcx)*rnx                 &
             +   (rcodcl(ifac,ivma,1)-rcodcy)*rny                 &
             +   (rcodcl(ifac,iwma,1)-rcodcz)*rnz
        rcodcl(ifac,iu,1) = rcodcx + rcodsn*rnx
        rcodcl(ifac,iv,1) = rcodcy + rcodsn*rny
        rcodcl(ifac,iw,1) = rcodcz + rcodsn*rnz
      endif
    endif

  endif

enddo

!========
! Formats
!========

 1000 format(                                                     &
'@'                                                            ,/,&
'@ ALE METHOD'                                                 ,/,&
'@'                                                            ,/,&
'@ At least one boundary face has an unknown boundary type.'   ,/,&
'@'                                                            ,/,&
'@    The calculation will not be run.'                        ,/,&
'@'                                                            ,/,&
'@ Check boundary condition definitions.'                      ,/,&
'@'                                                              )
 2000 format(                                                     &
'@'                                                            ,/,&
'@ ALE METHOD'                                                 ,/,&
'@'                                                            ,/,&
'@ At least one boundary face has a boundary condition type'   ,/,&
'@    (icodcl) which is not recognized for mesh velocity'      ,/,&
'@    along ',a1,' (',a4,').'                                  ,/,&
'@'                                                            ,/,&
'@ The only allowed values for icodcl are'                     ,/,&
'@   1 : Dirichlet'                                            ,/,&
'@   2 : Convective outlet'                                    ,/,&
'@   3 : Neumann'                                              ,/,&
'@   4 : Slip'                                                 ,/,&
'@'                                                              )
 3000 format(                                                     &
'@'                                                            ,/,&
'@ ALE METHOD'                                                 ,/,&
'@'                                                            ,/,&
'@ Inconsistency in boundary condition types'                  ,/,&
'@   for mesh velocity.'                                       ,/,&
'@'                                                            ,/,&
'@ At least one boundary face has the following boundary'      ,/,&
'@   conditions:'                                              ,/,&
'@'                                                            ,/,&
'@   itypcl : ',i10                                            ,/,&
'@   icodcl(.,iuma) : ', i10                                   ,/,&
'@   icodcl(.,ivma) : ', i10                                   ,/,&
'@   icodcl(.,iwma) : ', i10                                   ,/,&
'@'                                                            ,/,&
'@ If one component has slip conditions(icodcl=4),'            ,/,&
'@   all components must have slip conditions.'                ,/,&
'@'                                                              )
 4000 format(                                                     &
'@'                                                            ,/,&
'@ ALE METHOD'                                                 ,/,&
'@'                                                            ,/,&
'@ Inconsistency in boundary condition types'                  ,/,&
'@   for mesh velocity.'                                       ,/,&
'@   (cf. message(s) above)'                                   ,/,&
'@'                                                            ,/,&
'@ Check boundary condition definitions.'                      ,/,&
'@'                                                              )

return
end subroutine
