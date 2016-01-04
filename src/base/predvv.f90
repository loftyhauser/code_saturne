!-------------------------------------------------------------------------------

! This file is part of Code_Saturne, a general-purpose CFD tool.
!
! Copyright (C) 1998-2014 EDF S.A.
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

!===============================================================================
! Function:
! ---------

!> \file predvv.f90
!>
!> \brief This subroutine perform the velocity prediction step of the Navier
!> Stokes equations for incompressible or slightly compressible flows for
!> the coupled velocity components solver.
!>
!> - at the first call, the predicted velocities are computed and also
!>   an estimator on the predicted velocity is computed.
!>
!> - at the second call, a global estimator on Navier Stokes is computed.
!>   This second call is done after the correction step (\ref resopv).
!>
!-------------------------------------------------------------------------------

!-------------------------------------------------------------------------------
! Arguments
!______________________________________________________________________________.
!  mode           name          role                                           !
!______________________________________________________________________________!
!> \param[in]     iappel        call number (1 or 2)
!> \param[in]     nvar          total number of variables
!> \param[in]     nscal         total number of scalars
!> \param[in]     iterns        index of the iteration on Navier-Stokes
!> \param[in]     ncepdp        number of cells with head loss
!> \param[in]     ncesmp        number of cells with mass source term
!> \param[in]     icepdc        index of cells with head loss
!> \param[in]     icetsm        index of cells with mass source term
!> \param[in]     itypsm        type of mass source term for the variables
!> \param[in]     dt            time step (per cell)
!> \param[in,out] rtp, rtpa     calculated variables at cell centers
!>                               (at current and previous time steps)
!> \param[in]     propce        physical properties at cell centers
!> \param[in,out] propfa        physical properties at interior face centers
!> \param[in,out] propfb        physical properties at boundary face centers
!> \param[in]     flumas        internal mass flux (depending on iappel)
!> \param[in]     flumab        boundary mass flux (depending on iappel)
!> \param[in]     coefa, coefb  boundary conditions
!> \param[in]     ckupdc        work array for the head loss
!> \param[in]     smacel        variable value associated to the mass source
!>                               term (for ivar=ipr, smacel is the mass flux
!>                               \f$ \Gamma^n \f$)
!> \param[in]     frcxt         external forces making hydrostatic pressure
!> \param[in]     trava         working array for the velocity-pressure coupling
!> \param[in]     ximpa         idem
!> \param[in]     uvwk          idem (stores the velocity at the previous iteration)*
!> \param[in]     dfrcxt        variation of the external forces
!> \param[in]                    making the hydrostatic pressure
!> \param[in]     grdphd        hydrostatic pressure gradient to handle the imbalance
!> \param[in]                   between the pressure gradient and gravity source term
!> \param[in]     tpucou        non scalar time step in case of
!>                               velocity pressure coupling
!> \param[in]     trav          right hand side for the normalizing
!>                               the residual
!> \param[in]     viscf         visc*surface/dist aux faces internes
!> \param[in]     viscb         visc*surface/dist aux faces de bord
!> \param[in]     viscfi        idem viscf pour increments
!> \param[in]     viscbi        idem viscb pour increments
!> \param[in]     secvif        secondary viscosity at interior faces
!> \param[in]     secvib        secondary viscosity at boundary faces
!> \param[in]     w1            working array
!> \param[in]     w7            working array
!> \param[in]     w8            working array
!> \param[in]     w9            working array
!> \param[in]     xnormp        workig array for the norm of the pressure
!_______________________________________________________________________________

subroutine predvv &
 ( iappel ,                                                       &
   nvar   , nscal  , iterns , ncepdp , ncesmp ,                   &
   icepdc , icetsm , itypsm ,                                     &
   dt     , rtp    , rtpa   , vel    , vela   ,                   &
   propce , propfa , propfb ,                                     &
   flumas , flumab ,                                              &
   tslagr , coefa  , coefb  , coefav , coefbv , cofafv , cofbfv , &
   ckupdc , smacel , frcxt  , grdphd ,                            &
   trava  , ximpa  , uvwk   , dfrcxt , tpucou , trav   ,          &
   viscf  , viscb  , viscfi , viscbi , secvif , secvib ,          &
   w1     , w7     , w8     , w9     , xnormp )

!===============================================================================

!===============================================================================
! Module files
!===============================================================================

use paramx
use dimens, only: ndimfb
use numvar
use pointe, only: forbr, porosi
use entsor
use cstphy
use cstnum
use optcal
use parall
use period
use lagpar
use lagran
use ppppar
use ppthch
use ppincl
use cplsat
use ihmpre, only: iihmpr
use mesh

!===============================================================================

implicit none

! Arguments

integer          iappel
integer          nvar   , nscal  , iterns
integer          ncepdp , ncesmp

integer          icepdc(ncepdp)
integer          icetsm(ncesmp), itypsm(ncesmp,nvar)

double precision dt(ncelet), rtp(ncelet,*), rtpa(ncelet,*)
double precision propce(ncelet,*)
double precision propfa(nfac,*), propfb(ndimfb,*)
double precision flumas(nfac), flumab(nfabor)
double precision tslagr(ncelet,*)
double precision coefa(ndimfb,*), coefb(ndimfb,*)
double precision ckupdc(ncepdp,6), smacel(ncesmp,nvar)
double precision frcxt(ncelet,3), dfrcxt(ncelet,3)
double precision grdphd(ncelet,3)
double precision trava(ndim,ncelet)
double precision ximpa(ndim,ndim,ncelet),uvwk(ndim,ncelet)
double precision tpucou(6, ncelet)
double precision trav(3,ncelet)
double precision viscf(*), viscb(nfabor)
double precision viscfi(*), viscbi(nfabor)
double precision secvif(nfac), secvib(nfabor)
double precision w1(ncelet)
double precision w7(ncelet), w8(ncelet), w9(ncelet)
double precision xnormp(ncelet)
double precision coefav(3  ,ndimfb)
double precision cofafv(3  ,ndimfb)
double precision coefbv(3,3,ndimfb)
double precision cofbfv(3,3,ndimfb)

double precision vel   (3  ,ncelet)
double precision vela  (3  ,ncelet)

! Local variables

integer          iel   , ielpdc, ifac  , ivar  , isou  , itypfl
integer          iccocg, inc   , init  , ii    , isqrt
integer          ireslp, nswrgp, imligp, iwarnp, ippt  , ipp
integer          iswdyp, idftnp
integer          iclipr
integer          iclik
integer          ipcrom, ipcroa, ipcroo, ipcrho, ipcvis, ipcvst
integer          iconvp, idiffp, ndircp, nitmap, nswrsp
integer          ircflp, ischcp, isstpp, iescap
integer          imgrp , ncymxp, nitmfp
integer          iesprp, iestop
integer          iptsna
integer          iflmb0, nswrp , ipbrom
integer          idtva0
integer          ippu  , ippv  , ippw  , jsou, ivisep

double precision rnorm , vitnor
double precision romvom, drom  , rom
double precision epsrgp, climgp, extrap, relaxp, blencp, epsilp
double precision epsrsp
double precision vit1  , vit2  , vit3, xkb, pip, pfac, pfac1
double precision cpdc11, cpdc22, cpdc33, cpdc12, cpdc13, cpdc23
double precision d2s3  , thetap, thetp1, thets , dtsrom
double precision diipbx, diipby, diipbz
double precision cx    , cy    , cz

double precision rvoid(1)

! Working arrays
double precision, allocatable, dimension(:,:) :: eswork
double precision, allocatable, dimension(:,:) :: grad
double precision, dimension(:,:), allocatable :: smbr
double precision, dimension(:,:,:), allocatable :: fimp
double precision, dimension(:,:), allocatable :: gavinj
double precision, dimension(:,:), allocatable :: tsexp
double precision, dimension(:,:,:), allocatable :: tsimp
double precision, dimension(:,:), allocatable :: vect

!===============================================================================

!===============================================================================
! 1.  INITIALISATION
!===============================================================================

! Allocate temporary arrays
allocate(smbr(3,ncelet))
allocate(fimp(3,3,ncelet))
allocate(tsexp(3,ncelet))
allocate(tsimp(3,3,ncelet))

! Allocate a temporary array for the prediction-stage error estimator
if (iescal(iespre).gt.0) then
  allocate(eswork(3,ncelet))
endif

iclipr = iclrtp(ipr,icoef)

if(itytur.eq.2 .or. itytur.eq.5 .or. iturb.eq.60) then
  iclik  = iclrtp(ik ,icoef)
else
  iclik = 0
endif

! Reperage de rho au bord
ipbrom = ipprob(irom  )
! Reperage de rho courant (ie en cas d'extrapolation rho^n+1/2)
ipcrom = ipproc(irom  )
! Reperage de rho^n en cas d'extrapolation
if (iroext.gt.0.or.idilat.gt.1) then
  ipcroa = ipproc(iroma)
else
  ipcroa = 0
endif

ipcvis = ipproc(iviscl)
ipcvst = ipproc(ivisct)
! Theta relatif aux termes sources explicites
thets  = thetsn
if(isno2t.gt.0) then
  iptsna = ipproc(itsnsa)
else
  iptsna = 0
endif

! Compute the porosity if needed
if (iterns.eq.1.and.iporos.eq.1) then
  call usporo
endif


!===============================================================================
! 2.  GRADIENT DE PRESSION ET GRAVITE
!===============================================================================

!-------------------------------------------------------------------------------
! ---> PRISE EN COMPTE DE LA PRESSION HYDROSTATIQUE :
!       UNIQUEMENT AU PREMIER APPEL (I.E. POUR LE CALCUL NORMAL,
!       LE DEUXIEME APPEL EST DESTINE A UN CALCUL D'ESTIMATEUR)

if (iappel.eq.1.and.iphydr.eq.1) then

! force ext au pas de temps precedent :
!     FRCXT a ete initialise a zero
!     (est deja utilise dans typecl, et est mis a jour a la fin
!     de navsto)

  do iel = 1, ncel

! variation de force (utilise dans resolp)
    drom = (propce(iel,ipcrom)-ro0)
    dfrcxt(iel,1) = drom*gx - frcxt(iel,1)
    dfrcxt(iel,2) = drom*gy - frcxt(iel,2)
    dfrcxt(iel,3) = drom*gz - frcxt(iel,3)
  enddo
  ! Add head losses
  if (ncepdp.gt.0) then
    do ielpdc = 1, ncepdp
      iel=icepdc(ielpdc)
      vit1   = vela(1,iel)
      vit2   = vela(2,iel)
      vit3   = vela(3,iel)
      cpdc11 = ckupdc(ielpdc,1)
      cpdc22 = ckupdc(ielpdc,2)
      cpdc33 = ckupdc(ielpdc,3)
      cpdc12 = ckupdc(ielpdc,4)
      cpdc23 = ckupdc(ielpdc,5)
      cpdc13 = ckupdc(ielpdc,6)
      dfrcxt(iel,1) = dfrcxt(iel,1) &
                    - propce(iel,ipcrom)*(cpdc11*vit1+cpdc12*vit2+cpdc13*vit3)
      dfrcxt(iel,2) = dfrcxt(iel,2) &
                    - propce(iel,ipcrom)*(cpdc12*vit1+cpdc22*vit2+cpdc23*vit3)
      dfrcxt(iel,3) = dfrcxt(iel,3) &
                    - propce(iel,ipcrom)*(cpdc13*vit1+cpdc23*vit2+cpdc33*vit3)
    enddo
  endif
!     Ajout eventuel de la force de Coriolis
  if (icorio.eq.1) then
    do iel = 1, ncel
      cx = omegay*vela(3,iel) - omegaz*vela(2,iel)
      cy = omegaz*vela(1,iel) - omegax*vela(3,iel)
      cz = omegax*vela(2,iel) - omegay*vela(1,iel)
      dfrcxt(iel,1) = dfrcxt(iel,1) - 2.d0*propce(iel,ipcrom)*cx
      dfrcxt(iel,2) = dfrcxt(iel,2) - 2.d0*propce(iel,ipcrom)*cy
      dfrcxt(iel,3) = dfrcxt(iel,3) - 2.d0*propce(iel,ipcrom)*cz
    enddo
  endif

  if (irangp.ge.0.or.iperio.eq.1) then
    call synvec(dfrcxt(1,1), dfrcxt(1,2), dfrcxt(1,3))
    !==========
  endif

endif

!-------------------------------------------------------------------------------
! ---> PRISE EN COMPTE DU GRADIENT DE PRESSION

! Allocate a work array for the gradient calculation
allocate(grad(ncelet,3))

iccocg = 1
inc    = 1
nswrgp = nswrgr(ipr)
imligp = imligr(ipr)
iwarnp = iwarni(ipr)
epsrgp = epsrgr(ipr)
climgp = climgr(ipr)
extrap = extrag(ipr)


call grdpot &
!==========
 ( ipr , imrgra , inc    , iccocg , nswrgp , imligp , iphydr ,    &
   iwarnp , nfecra , epsrgp , climgp , extrap ,                   &
   rvoid  ,                                                       &
   frcxt(1,1), frcxt(1,2), frcxt(1,3),                            &
   rtpa(1,ipr)  , coefa(1,iclipr) , coefb(1,iclipr) ,             &
   grad   )

! With porosity
if (iporos.eq.1) then
  do iel = 1, ncel
    grad(iel,1) = grad(iel,1)*porosi(iel)
    grad(iel,2) = grad(iel,2)*porosi(iel)
    grad(iel,3) = grad(iel,3)*porosi(iel)
  enddo
endif

!    Calcul des efforts aux parois (partie 2/5), si demande
!    La pression a la face est calculee comme dans gradrc/gradmc
!    et on la transforme en pression totale
!    On se limite a la premiere iteration (pour faire simple par
!      rapport a la partie issue de condli, hors boucle)
if (ineedf.eq.1 .and. iterns.eq.1) then
  do ifac = 1, nfabor
    iel = ifabor(ifac)
    diipbx = diipb(1,ifac)
    diipby = diipb(2,ifac)
    diipbz = diipb(3,ifac)
    pip = rtpa(iel,ipr) &
        + diipbx*grad(iel,1) + diipby*grad(iel,2) + diipbz*grad(iel,3)
    pfac = coefa(ifac,iclipr) +coefb(ifac,iclipr)*pip
    pfac1= rtpa(iel,ipr)                                          &
         +(cdgfbo(1,ifac)-xyzcen(1,iel))*grad(iel,1)              &
         +(cdgfbo(2,ifac)-xyzcen(2,iel))*grad(iel,2)              &
         +(cdgfbo(3,ifac)-xyzcen(3,iel))*grad(iel,3)
    pfac = coefb(ifac,iclipr)*(extrag(ipr)*pfac1                  &
         +(1.d0-extrag(ipr))*pfac)                                &
         +(1.d0-coefb(ifac,iclipr))*pfac                          &
         + ro0*(gx*(cdgfbo(1,ifac)-xyzp0(1))                      &
         + gy*(cdgfbo(2,ifac)-xyzp0(2))                           &
         + gz*(cdgfbo(3,ifac)-xyzp0(3)) )                         &
         - pred0
! on ne rajoute pas P0, pour garder un maximum de precision
!     &         + P0
    do isou = 1, 3
      forbr(isou,ifac) = forbr(isou,ifac) + pfac*surfbo(isou,ifac)
    enddo
  enddo
endif

!-------------------------------------------------------------------------------
! ---> RESIDU DE NORMALISATION POUR RESOLP
!     Test d'un residu de normalisation de l'etape de pression
!       plus comprehensible = div(rho u* + dt gradP^(n))-Gamma
!       i.e. second membre du systeme en pression hormis la partie
!       pression (sinon a convergence, on tend vers 0)
!       Represente les termes que la pression doit equilibrer
!     On calcule ici div(rho dt/rho gradP^(n)) et on complete a la fin
!       avec  div(rho u*)
!     Pour grad P^(n) on suppose que des CL de Neumann homogenes
!       s'appliquent partout : on peut donc utiliser les CL de la
!       vitesse pour u*+dt/rho gradP^(n). Comme on calcule en deux fois,
!       on utilise les CL de vitesse homogenes pour dt/rho gradP^(n)
!       ci-dessous et les CL de vitesse completes pour u* a la fin.

if (iappel.eq.1.and.irnpnw.eq.1) then

!     Calcul de dt/rho*grad P
  do iel = 1, ncel
    dtsrom = dt(iel)/propce(iel,ipcrom)
    trav(1,iel) = grad(iel,1)*dtsrom
    trav(2,iel) = grad(iel,2)*dtsrom
    trav(3,iel) = grad(iel,3)*dtsrom
  enddo

  if (irangp.ge.0.or.iperio.eq.1) then
    call synvin(trav)
    !==========
  endif

!     Calcul de rho dt/rho*grad P.n aux faces
!       Pour gagner du temps, on ne reconstruit pas.
  itypfl = 1
  init   = 1
  inc    = 0
  iflmb0 = 1
  nswrp  = 1
  imligp = imligr(iu )
  iwarnp = iwarni(ipr)
  epsrgp = epsrgr(iu )
  climgp = climgr(iu )
  extrap = extrag(iu )

  call inimav                                                     &
  !==========
 ( nvar   , nscal  ,                                              &
   iu     , itypfl ,                                              &
   iflmb0 , init   , inc    , imrgra , nswrp  , imligp ,          &
   iwarnp , nfecra ,                                              &
   epsrgp , climgp , extrap ,                                     &
   propce(1,ipcrom), propfb(1,ipbrom),                            &
   trav   ,                                                       &
   coefav , coefbv ,                                              &
   viscf  , viscb  )

!     Calcul de div(rho dt/rho*grad P)
  init = 1
  call divmas(ncelet,ncel,nfac,nfabor,init,nfecra,                &
                                ifacel,ifabor,viscf,viscb,xnormp)

!     Ajout de -Gamma
  if (ncesmp.gt.0) then
    do ii = 1, ncesmp
      iel = icetsm(ii)
      xnormp(iel) = xnormp(iel)-volume(iel)*smacel(ii,ipr)
    enddo
  endif

  ! Semi-analytic weakly compressible algorithm add + 1/rho Drho/Dt
  if (idilat.eq.4)then
    do iel = 1, ncel
      xnormp(iel) = xnormp(iel) + propce(iel,ipproc(iustdy(itsrho)))
    enddo

  endif

!     On conserve XNORMP, on complete avec u* a la fin et
!       on le transfere a resolp

endif


!     Au premier appel, TRAV est construit directement ici.
!     Au second  appel (estimateurs), TRAV contient deja
!       l'increment temporel.
!     On pourrait fusionner en initialisant TRAV a zero
!       avant le premier appel, mais ca fait des operations en plus.

!     Remarques :
!       rho g sera a l'ordre 2 s'il est extrapole.
!       si on itere sur navsto, ca ne sert a rien de recalculer rho g a
!         chaque fois (ie on pourrait le passer dans trava) mais ce n'est
!         pas cher.
if (iappel.eq.1) then

  if (iphydr.eq.1) then
    do iel = 1, ncel
      trav(1,iel) = (frcxt(iel,1) - grad(iel,1)) * volume(iel)
      trav(2,iel) = (frcxt(iel,2) - grad(iel,2)) * volume(iel)
      trav(3,iel) = (frcxt(iel,3) - grad(iel,3)) * volume(iel)
    enddo
  elseif (iphydr.eq.2) then
    do iel = 1, ncel
      rom = propce(iel, ipcrom)
      trav(1,iel) = (rom*gx - grdphd(iel,1) - grad(iel,1)) * volume(iel)
      trav(2,iel) = (rom*gy - grdphd(iel,2) - grad(iel,2)) * volume(iel)
      trav(3,iel) = (rom*gz - grdphd(iel,3) - grad(iel,3)) * volume(iel)
    enddo
  else
    do iel = 1, ncel
      drom = (propce(iel,ipcrom)-ro0)
      trav(1,iel) = ( drom*gx - grad(iel,1) ) * volume(iel)
      trav(2,iel) = ( drom*gy - grad(iel,2) ) * volume(iel)
      trav(3,iel) = ( drom*gz - grad(iel,3) ) * volume(iel)
    enddo
  endif

elseif(iappel.eq.2) then

  if (iphydr.eq.1) then
    do iel = 1, ncel
      trav(1,iel) = trav(1,iel) + ( frcxt(iel,1) - grad(iel,1) ) * volume(iel)
      trav(2,iel) = trav(2,iel) + ( frcxt(iel,2) - grad(iel,2) ) * volume(iel)
      trav(3,iel) = trav(3,iel) + ( frcxt(iel,3) - grad(iel,3) ) * volume(iel)
    enddo
  elseif (iphydr.eq.2) then
    do iel = 1, ncel
      rom = propce(iel,ipcrom)
      trav(1,iel) = trav(1,iel) + (rom*gx - grdphd(iel,1) - grad(iel,1)) * volume(iel)
      trav(2,iel) = trav(2,iel) + (rom*gy - grdphd(iel,2) - grad(iel,2)) * volume(iel)
      trav(3,iel) = trav(3,iel) + (rom*gz - grdphd(iel,3) - grad(iel,3)) * volume(iel)
    enddo
  else
    do iel = 1, ncel
      drom = (propce(iel,ipcrom)-ro0)
      trav(1,iel) = trav(1,iel) + ( drom*gx - grad(iel,1) )*volume(iel)
      trav(2,iel) = trav(2,iel) + ( drom*gy - grad(iel,2) )*volume(iel)
      trav(3,iel) = trav(3,iel) + ( drom*gz - grad(iel,3) )*volume(iel)
    enddo
  endif

endif

! Free memory
deallocate(grad)


!   Pour IAPPEL = 1 (ie appel standard sans les estimateurs)
!     TRAV rassemble les termes sources  qui seront recalcules
!       a toutes les iterations sur navsto
!     Si on n'itere pas sur navsto et qu'on n'extrapole pas les
!       termes sources, TRAV contient tous les termes sources
!       jusqu'au basculement dans SMBR
!     A ce niveau, TRAV contient -grad P et rho g
!       P est suppose pris a n+1/2
!       rho est eventuellement interpole a n+1/2


!-------------------------------------------------------------------------------
! ---> INITIALISATION DU TABLEAU TRAVA et PROPCE AU PREMIER PASSAGE
!     (A LA PREMIERE ITER SUR NAVSTO)

!     TRAVA rassemble les termes sources qu'il suffit de calculer
!       a la premiere iteration sur navsto quand il y a plusieurs iter.
!     Quand il n'y a qu'une iter, on cumule directement dans TRAV
!       ce qui serait autrement alle dans TRAVA
!     PROPCE rassemble les termes sources explicites qui serviront
!       pour le pas de temps suivant en cas d'extrapolation (plusieurs
!       iter sur navsto ou pas)

!     A la premiere iter sur navsto
if(iterns.eq.1) then

    ! Si on   extrapole     les T.S. : -theta*valeur precedente
    if(isno2t.gt.0) then
      ! S'il n'y a qu'une    iter : TRAV  incremente
      if(nterup.eq.1) then
        do ii = 1, ndim
          do iel = 1, ncel
            trav (ii,iel) = trav (ii,iel) - thets*propce(iel,iptsna+ii-1)
          enddo
        enddo
      ! S'il   y a plusieurs iter : TRAVA initialise
      else
        do ii = 1, ndim
          do iel = 1, ncel
            trava(ii,iel) = - thets*propce(iel,iptsna+ii-1)
          enddo
      enddo
    endif
    ! Et on initialise PROPCE pour le remplir ensuite
    do ii = 1, ndim
      do iel = 1, ncel
        propce(iel,iptsna+ii-1) = 0.d0
      enddo
    enddo

  ! Si on n'extrapole pas les T.S. : pas de PROPCE
  else
    ! S'il   y a plusieurs iter : TRAVA initialise
    !  sinon TRAVA n'existe pas
    if(nterup.gt.1) then
      do ii = 1, ndim
        do iel = 1, ncel
          trava(ii,iel)  = 0.d0
        enddo
      enddo
    endif
  endif

endif

!-------------------------------------------------------------------------------
! ---> 2/3 RHO * GRADIENT DE K SI k-epsilon ou k-omega
!      NB : ON NE PREND PAS LE GRADIENT DE (RHO K), MAIS
!           CA COMPLIQUERAIT LA GESTION DES CL ...
!     On peut se demander si l'extrapolation en temps sert a
!       quelquechose

!     Ce terme explicite est calcule une seule fois,
!       a la premiere iter sur navsto : il va dans PROPCE si on
!       doit l'extrapoler en temps ; il va dans TRAVA si on n'extrapole
!       pas mais qu'on itere sur navsto. Il va dans TRAV si on
!       n'extrapole pas et qu'on n'itere pas sur navsto.
if(     (itytur.eq.2 .or. itytur.eq.5 .or. iturb.eq.60) &
   .and. igrhok.eq.1 .and. iterns.eq.1) then

  ! Allocate a work array for the gradient calculation
  allocate(grad(ncelet,3))

  iccocg = 1
  inc    = 1
  nswrgp = nswrgr(ik)
  imligp = imligr(ik)
  epsrgp = epsrgr(ik)
  climgp = climgr(ik)
  extrap = extrag(ik)

  iwarnp = iwarni(iu)

  call grdcel &
  !==========
 ( ik  , imrgra , inc    , iccocg , nswrgp , imligp ,             &
   iwarnp , nfecra , epsrgp , climgp , extrap ,                   &
   rtpa(1,ik)   , coefa(1,iclik)  , coefb(1,iclik)  ,             &
   grad   )

  d2s3 = 2.d0/3.d0

  ! With porosity
  if (iporos.eq.1) then
    do iel = 1, ncel
      grad(iel,1) = grad(iel,1)*porosi(iel)
      grad(iel,2) = grad(iel,2)*porosi(iel)
      grad(iel,3) = grad(iel,3)*porosi(iel)
    enddo
  endif

  ! Si on extrapole les termes source en temps : PROPCE
  if(isno2t.gt.0) then
    ! Calcul de rho^n grad k^n      si rho non extrapole
    !           rho^n grad k^n      si rho     extrapole
    ipcroo = ipcrom
    if(iroext.gt.0) ipcroo = ipcroa
    do iel = 1, ncel
      romvom = -propce(iel,ipcroo)*volume(iel)*d2s3
      propce(iel,iptsna  )=propce(iel,iptsna  )+grad(iel,1)*romvom
      propce(iel,iptsna+1)=propce(iel,iptsna+1)+grad(iel,2)*romvom
      propce(iel,iptsna+2)=propce(iel,iptsna+2)+grad(iel,3)*romvom
    enddo
  ! Si on n'extrapole pas les termes sources en temps : TRAV ou TRAVA
  else
    if(nterup.eq.1) then
      do iel = 1, ncel
        romvom = -propce(iel,ipcrom)*volume(iel)*d2s3
        do isou = 1, 3
          trav(isou,iel) = trav(isou,iel) + grad(iel,isou) * romvom
        enddo
      enddo
    else
      do iel = 1, ncel
        romvom = -propce(iel,ipcrom)*volume(iel)*d2s3
        do isou = 1, 3
          trava(isou,iel) = trava(isou,iel) + grad(iel,isou) * romvom
        enddo
      enddo
    endif
  endif

  ! Calcul des efforts aux parois (partie 3/5), si demande
  if (ineedf.eq.1) then
    do ifac = 1, nfabor
      iel = ifabor(ifac)
      diipbx = diipb(1,ifac)
      diipby = diipb(2,ifac)
      diipbz = diipb(3,ifac)
      xkb = rtpa(iel,ik) + diipbx*grad(iel,1)                      &
           + diipby*grad(iel,2) + diipbz*grad(iel,3)
      xkb = coefa(ifac,iclik)+coefb(ifac,iclik)*xkb
      xkb = d2s3*propce(iel,ipcrom)*xkb
      do isou = 1, 3
        forbr(isou,ifac) = forbr(isou,ifac) + xkb*surfbo(isou,ifac)
      enddo
    enddo
  endif

  ! Free memory
  deallocate(grad)

endif


!-------------------------------------------------------------------------------
! ---> TERMES DE GRADIENT TRANSPOSE

!     These terms are taken into account in bilscv.
!     We only compute here the secondary viscosity.

if (ivisse.eq.1) then

  call visecv &
  !==========
 ( nvar   ,                             &
   propce ,                             &
   secvif , secvib )

endif

!-------------------------------------------------------------------------------
! ---> TERMES DE PERTES DE CHARGE
!     SI IPHYDR=1 LE TERME A DEJA ETE PRIS EN COMPTE AVANT

if((ncepdp.gt.0).and.(iphydr.eq.0)) then

  ! Les termes diagonaux sont places dans TRAV ou TRAVA,
  !   La prise en compte de uvwk a partir de la seconde iteration
  !   est faite directement dans coditv.
  if(iterns.eq.1) then

    ! On utilise temporairement TRAV comme tableau de travail.
    ! Son contenu est stocke dans W7, W8 et W9 jusqu'apres tspdcv
    do iel = 1,ncel
      w7(iel) = trav(1,iel)
      w8(iel) = trav(2,iel)
      w9(iel) = trav(3,iel)
      trav(1,iel) = 0.d0
      trav(2,iel) = 0.d0
      trav(3,iel) = 0.d0
    enddo

    call tspdcv                                                   &
    !==========
 ( nvar   , nscal  , ncepdp ,                                     &
   icepdc ,                                                       &
   rtpa   , vela   ,                                              &
   propce , propfa , propfb ,                                     &
   coefa  , coefb  , ckupdc , trav   )

    ! With porosity
    if (iporos.eq.1) then
      do iel = 1, ncel
        trav(iel,1) = trav(iel,1)*porosi(iel)
        trav(iel,2) = trav(iel,2)*porosi(iel)
        trav(iel,3) = trav(iel,3)*porosi(iel)
      enddo
    endif
    ! Si on itere sur navsto, on utilise TRAVA ; sinon TRAV
    if(nterup.gt.1) then
      do iel = 1, ncel
        trava(1,iel) = trava(1,iel) + trav(1,iel)
        trava(2,iel) = trava(2,iel) + trav(2,iel)
        trava(3,iel) = trava(3,iel) + trav(3,iel)
        trav(1,iel)  = w7(iel)
        trav(2,iel)  = w8(iel)
        trav(3,iel)  = w9(iel)
      enddo
    else
      do iel = 1, ncel
        trav(1,iel)  = w7(iel) + trav(1,iel)
        trav(2,iel)  = w8(iel) + trav(2,iel)
        trav(3,iel)  = w9(iel) + trav(3,iel)
      enddo
    endif
  endif

endif


!-------------------------------------------------------------------------------
! ---> TERMES DE CORIOLIS !FIXME with porosity
!     SI IPHYDR=1 LE TERME A DEJA ETE PRIS EN COMPTE AVANT

if (icorio.eq.1.and.iphydr.eq.0) then

  ! A la premiere iter sur navsto, on ajoute la partie issue des
  ! termes explicites
  if (iterns.eq.1) then

    ! Si on n'itere pas sur navsto : TRAV
    if (nterup.eq.1) then

      do iel = 1, ncel
        cx = omegay*vela(3,iel) - omegaz*vela(2,iel)
        cy = omegaz*vela(1,iel) - omegax*vela(3,iel)
        cz = omegax*vela(2,iel) - omegay*vela(1,iel)
        romvom = -2.d0*propce(iel,ipcrom)*volume(iel)
        trav(1,iel) = trav(1,iel) + romvom*cx
        trav(2,iel) = trav(2,iel) + romvom*cy
        trav(3,iel) = trav(3,iel) + romvom*cz
      enddo

    ! Si on itere sur navsto : TRAVA
    else

      do iel = 1, ncel
        cx = omegay*vela(3,iel) - omegaz*vela(2,iel)
        cy = omegaz*vela(1,iel) - omegax*vela(3,iel)
        cz = omegax*vela(2,iel) - omegay*vela(1,iel)
        romvom = -2.d0*propce(iel,ipcrom)*volume(iel)
        trava(1,iel) = trava(1,iel) + romvom*cx
        trava(2,iel) = trava(2,iel) + romvom*cy
        trava(3,iel) = trava(3,iel) + romvom*cz
      enddo

    endif
  endif
endif


!-------------------------------------------------------------------------------
! ---> - DIVERGENCE DE RIJ

if(itytur.eq.3.and.iterns.eq.1) then

  do isou = 1, 3

    if(isou.eq.1) ivar = iu
    if(isou.eq.2) ivar = iv
    if(isou.eq.3) ivar = iw

    call divrij                                                   &
    !==========
 ( nvar   , nscal  ,                                              &
   isou   , ivar   ,                                              &
   rtpa   , propce , propfa , propfb ,                            &
   coefa  , coefb  ,                                              &
   viscf  , viscb  )

    init = 1
    call divmas(ncelet,ncel,nfac,nfabor,init,nfecra,              &
                                   ifacel,ifabor,viscf,viscb,w1)

!     Si on extrapole les termes source en temps :
!       PROPCE recoit les termes de divergence
    if(isno2t.gt.0) then
      do iel = 1, ncel
        propce(iel,iptsna+isou-1 ) =                              &
        propce(iel,iptsna+isou-1 ) - w1(iel)
      enddo
!     Si on n'extrapole pas les termes source en temps :
    else
!       si on n'itere pas sur navsto : TRAV
      if(nterup.eq.1) then
        do iel = 1, ncel
          trav(isou,iel) = trav(isou,iel) - w1(iel)
        enddo
!       si on itere sur navsto       : TRAVA
      else
        do iel = 1, ncel
          trava(isou,iel) = trava(isou,iel) - w1(iel)
        enddo
      endif
    endif

  enddo

endif


!-------------------------------------------------------------------------------
! ---> Face diffusivity for the velocity

if (idiff(iu).ge. 1) then

  if (itytur.eq.3) then
    do iel = 1, ncel
      w1(iel) = propce(iel,ipcvis)
    enddo
  else
    do iel = 1, ncel
      w1(iel) = propce(iel,ipcvis) + idifft(iu)*propce(iel,ipcvst)
    enddo
  endif

  call viscfa &
  !==========
 ( imvisf ,                                                       &
   w1     ,                                                       &
   viscf  , viscb  )

  ! When using Rij-epsilon model with th option irijnu=1, the face
  ! viscosity for the Matrix (viscfi and viscbi) is increased

  if(itytur.eq.3.and.irijnu.eq.1) then

    do iel = 1, ncel
      w1(iel) = propce(iel,ipcvis) + idifft(iu)*propce(iel,ipcvst)
    enddo

    call viscfa &
    !==========
 ( imvisf ,                                                       &
   w1     ,                                                       &
   viscfi , viscbi )
  endif

! --- If no dissusion, viscosity is set to 0.
else

  do ifac = 1, nfac
    viscf(ifac) = 0.d0
  enddo
  do ifac = 1, nfabor
    viscb(ifac) = 0.d0
  enddo

  if(itytur.eq.3.and.irijnu.eq.1) then
    do ifac = 1, nfac
      viscfi(ifac) = 0.d0
    enddo
    do ifac = 1, nfabor
      viscbi(ifac) = 0.d0
    enddo
  endif

endif

!===============================================================================
! 3.  RESOLUTION IMPLICITE COUPLEE DES 3 COMPO. DE VITESSES
!===============================================================================


! ---> AU PREMIER APPEL,
!      MISE A ZERO DE L'ESTIMATEUR POUR LA VITESSE PREDITE
!      S'IL DOIT ETRE CALCULE

if (iappel.eq.1) then
  if(iescal(iespre).gt.0) then
    iesprp = ipproc(iestim(iespre))
    do iel = 1, ncel
      propce(iel,iesprp) =  0.d0
    enddo
  endif
endif

! ---> AU DEUXIEME APPEL,
!      MISE A ZERO DE L'ESTIMATEUR TOTAL POUR NAVIER-STOKES
!      (SI ON FAIT UN DEUXIEME APPEL, ALORS IL DOIT ETRE CALCULE)

if(iappel.eq.2) then
  iestop = ipproc(iestim(iestot))
  do iel = 1, ncel
    propce(iel,iestop) = 0.d0
  enddo
endif

! ---> TERMES SOURCES UTILISATEURS

do iel = 1, ncel
  do isou = 1, 3
    tsexp(isou,iel) = 0.d0
    do jsou = 1, 3
      tsimp(isou,jsou,iel) = 0.d0
    enddo
  enddo
enddo

ipp  = ipprtp(iu)
!     Le calcul des parties implicite et explicite des termes sources
!       utilisateurs est faite uniquement a la premiere iter sur navstv.
! FIXME with porosity
if(iterns.eq.1) then

  if (iihmpr.eq.1) then
    call uitsnv (vel, tsexp, tsimp)
  endif

  call ustsnv &
  !==========
 ( nvar   , nscal  , ncepdp , ncesmp ,                            &
   iu   ,                                                         &
   icepdc , icetsm , itypsm ,                                     &
   dt     , rtpa   , propce , propfa , propfb ,                   &
   ckupdc , smacel , tsexp  , tsimp  )

  ! Coupling between two Code_Saturne
  if (nbrcpl.gt.0) then
  !vectorial interleaved exchange
    call cscelv &
    !==========
 ( nvar   , nscal  ,                                              &
   iu   ,                                                       &
   dt     , rtpa   , vela   ,                                     &
   propce , propfa , propfb ,                                     &
   coefa  , coefb  , coefav , coefbv ,                            &
   tsexp  , tsimp  )
  endif

endif


!     On conserve la partie implicite pour les autres iter sur navsto
if(iterns.eq.1.and.nterup.gt.1) then
  do iel = 1, ncel
    do isou = 1, 3
      do jsou =1, 3
        ximpa(isou,jsou,iel) = tsimp(isou,jsou,iel)
      enddo
    enddo
  enddo
endif

!     On ajoute a TRAV ou TRAVA la partie issue des termes implicites
!       en utilisant DRTP
!       La prise en compte de UVWK a partir de la seconde iteration
!       est faite directement dans coditv.
!     En schema std en temps, on continue a mettre MAX(-DRTP,0) dans la matrice
!     Avec termes sources a l'ordre 2, on implicite DRTP quel que soit son signe
!       (si on le met dans la matrice ou non selon son signe, on risque de ne pas
!        avoir le meme traitement d'un pas de temps au suivant)
if(iterns.eq.1) then
  if(nterup.gt.1) then
    do iel = 1, ncel
      do isou = 1, 3
        do jsou = 1, 3
          trava(isou,iel) = trava(isou,iel)             &
               + tsimp(isou,jsou,iel)*vela(jsou,iel)
        enddo
      enddo
    enddo
  else
    do iel = 1, ncel
      do isou = 1, 3
        do jsou = 1, 3
          trav(isou,iel) = trav(isou,iel)                           &
               + tsimp(isou,jsou,iel)*vela(jsou,iel)
        enddo
      enddo
    enddo
  endif
endif

!     A la premiere iter sur navsto, on ajoute la partie issue des
!       termes explicites
if(iterns.eq.1) then
!     Si on extrapole les termes source en temps :
!       PROPCE recoit les termes explicites
  if(isno2t.gt.0) then
    do iel = 1, ncel
      do isou = 1, 3
        propce(iel,iptsna+isou-1 ) =                              &
        propce(iel,iptsna+isou-1 ) + tsexp(isou,iel)
      enddo
    enddo
!     Si on n'extrapole pas les termes source en temps :
  else
!       si on n'itere pas sur navsto : TRAV
    if(nterup.eq.1) then
      do iel = 1, ncel
        do isou = 1, 3
          trav(isou,iel) = trav(isou,iel) + tsexp(isou,iel)
        enddo
      enddo
!       si on itere sur navsto : TRAVA
    else
      do iel = 1, ncel
        do isou = 1, 3
          trava(isou,iel) = trava(isou,iel) + tsexp(isou,iel)
        enddo
      enddo
    endif
  endif
endif

if(iappel.eq.1) then

  ! Low Mach compressible Algos
  if (idilat.gt.1) then
    ipcrho = ipcroa

  ! Standard algo
  else
    ipcrho = ipcrom
  endif

  do iel = 1, ncel
    do isou = 1, 3
      fimp(isou,isou,iel) = istat(iu)*propce(iel,ipcrho)/dt(iel)*volume(iel)
      do jsou = 1, 3
        if(jsou.ne.isou) fimp(isou,jsou,iel) = 0.d0
      enddo
    enddo
  enddo

!     Le remplissage de FIMP est toujours indispensable,
!       meme si on peut se contenter de n'importe quoi pour IAPPEL=2.
else
  do iel = 1, ncel
    do isou = 1, 3
      do jsou = 1, 3
        fimp(isou,jsou,iel) = 0.d0
      enddo
    enddo
  enddo
endif

! ---> TERMES SOURCES UTILISATEUR

if(iappel.eq.1) then
  if(isno2t.gt.0) then
    thetap = thetav(iu)
    if(iterns.gt.1) then
      do iel = 1, ncel
        do isou = 1, 3
          do jsou = 1, 3
            fimp(isou,jsou,iel) = fimp(isou,jsou,iel)          &
                       -ximpa(isou,jsou,iel)*thetap
          enddo
        enddo
      enddo
    else
      do iel = 1, ncel
        do isou = 1, 3
          do jsou = 1, 3
            fimp(isou,jsou,iel) = fimp(isou,jsou,iel) -tsimp(isou,jsou,iel)*thetap
          enddo
        enddo
      enddo
    endif
  else
    if(iterns.gt.1) then
      do iel = 1, ncel
        do isou = 1, 3
          do jsou = 1, 3
            fimp(isou,jsou,iel) = fimp(isou,jsou,iel)                      &
               + max(-ximpa(isou,jsou,iel),zero)
          enddo
        enddo
      enddo
    else
      do iel = 1, ncel
        do isou = 1, 3
          do jsou = 1, 3
            fimp(isou,jsou,iel) = fimp(isou,jsou,iel)                      &
               + max(-tsimp(isou,jsou,iel),zero)
          enddo
        enddo
      enddo
    endif
  endif
endif


! ---> Head loss

!  At the second call, fimp is not needed anymore
if(iappel.eq.1) then
  if (ncepdp.gt.0) then
    ! The theta-scheme for the head loss is the same as the other terms
    thetap = thetav(iu)
    do ielpdc = 1, ncepdp
      iel = icepdc(ielpdc)
      romvom = propce(iel,ipcrom)*volume(iel)*thetap
      ! Diagonal part
      do isou = 1, 3
        fimp(isou,isou,iel) = fimp(isou,isou,iel) +                     &
                          romvom*ckupdc(ielpdc,isou)
      enddo
      ! Extra-diagonal part
      cpdc12 = ckupdc(ielpdc,4)
      cpdc23 = ckupdc(ielpdc,5)
      cpdc13 = ckupdc(ielpdc,6)

      fimp(1,2,iel) = fimp(1,2,iel) + romvom*cpdc12
      fimp(2,1,iel) = fimp(2,1,iel) + romvom*cpdc12
      fimp(1,3,iel) = fimp(1,3,iel) + romvom*cpdc13
      fimp(3,1,iel) = fimp(3,1,iel) + romvom*cpdc13
      fimp(2,3,iel) = fimp(2,3,iel) + romvom*cpdc23
      fimp(3,2,iel) = fimp(3,2,iel) + romvom*cpdc23
    enddo
  endif
endif


! --->  Coriolis source terms

!  At the second call, fimp is not needed anymore
if(iappel.eq.1) then
  if (icorio.eq.1) then
    ! The theta-scheme for the Coriolis term is the same as the other terms
    thetap = thetav(iu)

    do iel = 1, ncel
      romvom = propce(iel,ipcrom)*volume(iel)*thetap
      fimp(1,2,iel) = fimp(1,2,iel) + 2.d0*romvom*omegaz
      fimp(2,1,iel) = fimp(2,1,iel) - 2.d0*romvom*omegaz
      fimp(1,3,iel) = fimp(1,3,iel) - 2.d0*romvom*omegay
      fimp(3,1,iel) = fimp(3,1,iel) + 2.d0*romvom*omegay
      fimp(2,3,iel) = fimp(2,3,iel) + 2.d0*romvom*omegax
      fimp(3,2,iel) = fimp(3,2,iel) - 2.d0*romvom*omegax
    enddo

  endif
endif


! --->  TERMES DE SOURCE DE MASSE

if (ncesmp.gt.0) then

!     On calcule les termes Gamma (uinj - u)
!       -Gamma u a la premiere iteration est mis dans
!          TRAV ou TRAVA selon qu'on itere ou non sur navsto
!       Gamma uinj a la premiere iteration est placee dans W1
!       ROVSDT a chaque iteration recoit Gamma
  allocate(gavinj(3,ncelet))
  if(nterup.eq.1) then
    call catsmv                                                   &
    !==========
  ( ncelet , ncel , ncesmp , iterns , isno2t, thetav(iu),       &
    icetsm , itypsm(1,iu),                                      &
    volume , vela , smacel(1,iu) ,smacel(1,ipr) ,               &
    trav   , fimp , gavinj )
  else
    call catsmv                                                   &
    !==========
  ( ncelet , ncel , ncesmp , iterns , isno2t, thetav(iu),       &
    icetsm , itypsm(1,iu),                                      &
    volume , vela , smacel(1,iu) ,smacel(1,ipr) ,               &
    trava  , fimp  , gavinj )
  endif

!     A la premiere iter sur navsto, on ajoute la partie Gamma uinj
  if(iterns.eq.1) then
!     Si on extrapole les termes source en temps :
!       PROPCE recoit les termes explicites
    if(isno2t.gt.0) then
      do iel = 1,ncel
        do isou = 1, 3
          propce(iel,iptsna+isou-1 ) =                            &
          propce(iel,iptsna+isou-1 ) + gavinj(isou,iel)
        enddo
      enddo
!     Si on n'extrapole pas les termes source en temps :
    else
!       si on n'itere pas sur navsto : TRAV
      if(nterup.eq.1) then
        do iel = 1,ncel
          do isou = 1, 3
            trav(isou,iel)  = trav(isou,iel) + gavinj(isou,iel)
          enddo
        enddo
!       si on itere sur navsto : TRAVA
      else
        do iel = 1,ncel
          do isou = 1, 3
            trava(isou,iel) =                               &
            trava(isou,iel) + gavinj(isou,iel)
          enddo
        enddo
      endif
    endif
  endif

  deallocate(gavinj)

endif

! ---> INITIALISATION DU SECOND MEMBRE

!     Si on extrapole les TS
if(isno2t.gt.0) then
  thetp1 = 1.d0 + thets
!       Si on n'itere pas sur navsto : TRAVA n'existe pas
  if(nterup.eq.1) then
    do iel = 1, ncel
      do isou = 1, 3
        smbr(isou,iel) =  trav(isou,iel)                               &
             + thetp1*propce(iel,iptsna+isou-1)
      enddo
    enddo
!       Si on   itere     sur navsto : tout existe
  else
    do iel = 1, ncel
      do isou = 1, 3
        smbr(isou,iel) =  trav(isou,iel) + trava(isou,iel)       &
             + thetp1*propce(iel,iptsna+isou-1)
      enddo
    enddo
  endif
!     Si on n'extrapole pas les TS : PROPCE n'existe pas
else
!       Si on n'itere pas sur navsto : TRAVA n'existe pas
  if(nterup.eq.1) then
    do iel = 1, ncel
      do isou = 1, 3
        smbr(isou,iel) =  trav(isou,iel)
      enddo
    enddo
!       Si on   itere     sur navsto : TRAVA existe
  else
    do iel = 1, ncel
      do isou = 1, 3
        smbr(isou,iel) =  trav(isou,iel) + trava(isou,iel)
      enddo
    enddo
  endif
endif


! ---> LAGRANGIEN : COUPLAGE RETOUR

!     L'ordre 2 sur les termes issus du lagrangien necessiterait de
!       decomposer TSLAGR(IEL,ISOU) en partie implicite et
!       explicite, comme c'est fait dans ustsns.
!     Pour le moment, on n'y touche pas.
if (iilagr.eq.2 .and. ltsdyn.eq.1)  then

  do iel = 1, ncel
    do isou = 1, 3
      smbr(isou,iel)   = smbr(isou,iel) + tslagr(iel,itsvx+isou-1)
    enddo
  enddo
!  Au second appel, on n'a pas besoin de FIMP
  if(iappel.eq.1) then
    do iel = 1, ncel
      do isou = 1, 3
        fimp(isou,isou,iel) = fimp(isou,isou,iel) + max(-tslagr(iel,itsli),zero)
      enddo
    enddo
  endif

endif

! ---> VERSIONS ELECTRIQUES : Arc Electrique (Force de Laplace)
!     Pour le moment, pas d'ordre 2 en temps.

if ( ippmod(ielarc) .ge. 1 ) then
  do iel = 1,ncel
    do isou = 1, 3
      smbr(isou,iel) = smbr(isou,iel)                               &
                   + volume(iel)*propce(iel,ipproc(ilapla(isou)))
    enddo
  enddo
endif

! With porosity: has to be done just before calling coditv
if (iporos.eq.1) then
  do iel = 1, ncel
    do isou = 1, 3
      do jsou = 1, 3
        fimp(isou,jsou,iel) = fimp(isou,jsou,iel)*porosi(iel)
      enddo
    enddo
  enddo
endif

! ---> PARAMETRES POUR LA RESOLUTION DU SYSTEME OU LE CALCUL DE l'ESTIMATEUR

iconvp = iconv (iu)
idiffp = idiff (iu)
ireslp = iresol(iu)
ndircp = ndircl(iu)
nitmap = nitmax(iu)
nswrsp = nswrsm(iu)
nswrgp = nswrgr(iu)
imligp = imligr(iu)
ircflp = ircflu(iu)
ischcp = ischcv(iu)
isstpp = isstpc(iu)
idftnp = idften(iu)
iswdyp = iswdyn(iu)
imgrp  = imgr  (iu)
ncymxp = ncymax(iu)
nitmfp = nitmgf(iu)
iwarnp = iwarni(iu)
blencp = blencv(iu)
epsilp = epsilo(iu)
epsrsp = epsrsm(iu)
epsrgp = epsrgr(iu)
climgp = climgr(iu)
extrap = extrag(iu)
relaxp = relaxv(iu)
thetap = thetav(iu)
ippu   = ipprtp(iu)
ippv   = ipprtp(iv)
ippw   = ipprtp(iw)

if(iappel.eq.1) then

  iescap = iescal(iespre)

! ---> FIN DE LA CONSTRUCTION ET DE LA RESOLUTION DU SYSTEME

  if(iterns.eq.1) then

!  Attention, dans le cas des estimateurs, eswork fournit l'estimateur
!     des vitesses predites
    call coditv &
    !==========
 ( nvar   , nscal  ,                                              &
   idtvar , iu     , iconvp , idiffp , ireslp , ndircp , nitmap , &
   imrgra , nswrsp , nswrgp , imligp , ircflp , ivisse ,          &
   ischcp , isstpp , iescap , idftnp , iswdyp ,                   &
   imgrp  , ncymxp , nitmfp , ippu   , ippv   , ippw   , iwarnp , &
   blencp , epsilp , epsrsp , epsrgp , climgp , extrap ,          &
   relaxp , thetap ,                                              &
   vela   , vela   ,                                              &
   coefav , coefbv , cofafv , cofbfv ,                            &
   flumas , flumab ,                                              &
   viscfi , viscbi , viscf  , viscb  , secvif , secvib ,          &
   fimp   ,                                                       &
   smbr   ,                                                       &
   vel    ,                                                       &
   eswork )

  elseif(iterns.gt.1) then

    call coditv &
    !==========
 ( nvar   , nscal  ,                                              &
   idtvar , iu     , iconvp , idiffp , ireslp , ndircp , nitmap , &
   imrgra , nswrsp , nswrgp , imligp , ircflp , ivisse ,          &
   ischcp , isstpp , iescap , idftnp , iswdyp ,                   &
   imgrp  , ncymxp , nitmfp , ippu   , ippv   , ippw   , iwarnp , &
   blencp , epsilp , epsrsp , epsrgp , climgp , extrap ,          &
   relaxp , thetap ,                                              &
   vela   , uvwk   ,                                              &
   coefav , coefbv , cofafv , cofbfv ,                            &
   flumas , flumab ,                                              &
   viscfi , viscbi , viscf  , viscb  , secvif , secvib ,          &
   fimp   ,                                                       &
   smbr   ,                                                       &
   vel    ,                                                       &
   eswork )

  endif

  ! Velocity-pression coupling: compute the vector T, stored in tpucou,
  !  coditv is called, only one sweep is done, and tpucou is initialized
  !  by 0. so that the advection/diffusion added by bilscv is 0.
  !  nswrsp = -1 indicated that only one sweep is required and inc=0
  !  for boundary contitions on the weight matrix.
  if (ipucou.eq.1) then

    ! Allocate temporary arrays for the velocity-pressure resolution
    allocate(vect(3,ncelet))

    nswrsp = -1
    do iel = 1, ncel
      do isou = 1, 3
        smbr(isou,iel) = volume(iel)
      enddo
    enddo
    do iel = 1, ncelet
      do isou = 1, 3
        vect(isou,iel) = 0.d0
      enddo
    enddo
    iescap = 0

    ! We do not take into account transpose of grad
    ivisep = 0

    call coditv &
    !==========
 ( nvar   , nscal  ,                                              &
   idtvar , iu     , iconvp , idiffp , ireslp , ndircp , nitmap , &
   imrgra , nswrsp , nswrgp , imligp , ircflp , ivisep ,          &
   ischcp , isstpp , iescap , idftnp , iswdyp ,                   &
   imgrp  , ncymxp , nitmfp , ippu   , ippv   , ippw   , iwarnp , &
   blencp , epsilp , epsrsp , epsrgp , climgp , extrap ,          &
   relaxp , thetap ,                                              &
   vect   , vect   ,                                              &
   coefav , coefbv , cofafv , cofbfv ,                            &
   flumas , flumab ,                                              &
   viscfi , viscbi , viscf  , viscb  , secvif , secvib ,          &
   fimp   ,                                                       &
   smbr   ,                                                       &
   vect   ,                                                       &
   rvoid  )

    do iel = 1, ncelet
      rom = propce(iel,ipcrom)
      do isou = 1, 3
        tpucou(isou,iel) = rom*vect(isou,iel)
      enddo
      do isou = 4, 6
        tpucou(isou,iel) = 0.d0
      enddo
    enddo

    ! Free memory
    deallocate(vect)

  endif

! --->  ESTIMATEUR SUR LA VITESSE PREDITE : ON SOMME SUR LES COMPOSANTES

  if(iescal(iespre).gt.0) then
    iesprp = ipproc(iestim(iespre))
    do iel = 1, ncel
      do isou = 1, 3
        propce(iel,iesprp) =  propce(iel,iesprp) + eswork(isou,iel)
      enddo
    enddo
  endif


elseif(iappel.eq.2) then

! ---> FIN DE LA CONSTRUCTION DE L'ESTIMATEUR
!        RESIDU SECOND MEMBRE(Un+1,Pn+1) + RHO*VOLUME*( Un+1 - Un )/DT

  inc = 1
!     Pas de relaxation en stationnaire
  idtva0 = 0

  ippu  = ipprtp(iu)
  ippv  = ipprtp(iv)
  ippw  = ipprtp(iw)

  call bilscv &
  !==========
 ( nvar   , nscal  ,                                              &
   idtva0 , iu     , iconvp , idiffp , nswrgp , imligp , ircflp , &
   ischcp , isstpp , inc    , imrgra , ivisep ,                   &
   ippu   , ippv   , ippw   , iwarnp , idftnp ,                   &
   blencp , epsrgp , climgp , extrap , relaxp , thetap ,          &
   vel    , vel    ,                                              &
   coefav , coefbv , cofafv , cofbfv ,                            &
   flumas , flumab , viscf  , viscb  , secvif , secvib ,          &
   smbr   )

  iestop = ipproc(iestim(iestot))
  do iel = 1, ncel
    do isou = 1, 3
      propce(iel,iestop) =                                        &
           propce(iel,iestop)+ (smbr(isou,iel)/volume(iel))**2
    enddo
  enddo
endif


!===============================================================================
! 4.     FIN DU CALCUL DE LA NORME POUR RESOLP
!===============================================================================
! --->  APRES LA BOUCLE SUR U, V, W,

if(iappel.eq.1.and.irnpnw.eq.1) then

  ! Calcul de div(rho u*)

  if (irangp.ge.0.or.iperio.eq.1) then
    call synvin(vel)
    !==========
  endif

  ! Pour gagner du temps, on ne reconstruit pas.
  itypfl = 1
  init   = 1
  inc    = 1
  iflmb0 = 1
  nswrp  = 1
  imligp = imligr(iu )
  iwarnp = iwarni(ipr)
  epsrgp = epsrgr(iu )
  climgp = climgr(iu )
  extrap = extrag(iu )

  call inimav &
  !==========
 ( nvar   , nscal  ,                                              &
   iu     , itypfl ,                                              &
   iflmb0 , init   , inc    , imrgra , nswrp  , imligp ,          &
   iwarnp , nfecra ,                                              &
   epsrgp , climgp , extrap ,                                     &
   propce(1,ipcrom), propfb(1,ipbrom),                            &
   vel    ,                                                       &
   coefav , coefbv ,                                              &
   viscf  , viscb  )

  init = 0
  call divmas(ncelet,ncel,nfac,nfabor,init,nfecra,                &
                                ifacel,ifabor,viscf,viscb,xnormp)

  ! Calcul de la norme
  ! RNORMP qui servira dans resolp
  isqrt = 1
  call prodsc(ncel,isqrt,xnormp,xnormp,rnormp)

endif

! --->  APRES LA BOUCLE SUR U, V, W,
!        FIN DU CALCUL DES ESTIMATEURS ET IMPRESSION

if(iappel.eq.1) then

! --->  ESTIMATEUR SUR LA VITESSE PREDITE : ON PREND LA RACINE (NORME)
!         SANS OU AVEC VOLUME (ET DANS CE CAS C'EST LA NORME L2)

  if(iescal(iespre).gt.0) then
    iesprp = ipproc(iestim(iespre))
    if(iescal(iespre).eq.1) then
      do iel = 1, ncel
        propce(iel,iesprp) =  sqrt(propce(iel,iesprp)            )
      enddo
    elseif(iescal(iespre).eq.2) then
      do iel = 1, ncel
        propce(iel,iesprp) =  sqrt(propce(iel,iesprp)*volume(iel))
      enddo
    endif
  endif

! ---> IMPRESSION DE NORME

  if (iwarni(iu).ge.2) then
    rnorm = -1.d0
    do iel = 1, ncel
      vitnor = sqrt(vel(1,iel)**2+vel(2,iel)**2+vel(3,iel)**2)
      rnorm = max(rnorm,vitnor)
    enddo
    if (irangp.ge.0) call parmax (rnorm)
                     !==========
    write(nfecra,1100) rnorm
  endif

elseif (iappel.eq.2) then

! --->  ESTIMATEUR SUR NAVIER-STOKES TOTAL : ON PREND LA RACINE (NORME)
!         SANS OU AVEC VOLUME (ET DANS CE CAS C'EST LA NORME L2)

  iestop = ipproc(iestim(iestot))
  if(iescal(iestot).eq.1) then
    do iel = 1, ncel
      propce(iel,iestop) = sqrt(propce(iel,iestop)            )
    enddo
  elseif(iescal(iestot).eq.2) then
    do iel = 1, ncel
      propce(iel,iestop) = sqrt(propce(iel,iestop)*volume(iel))
    enddo
  endif

endif

! Free memory
!------------
deallocate(smbr)
deallocate(fimp)

deallocate(tsexp)
deallocate(tsimp)
!--------
! Formats
!--------
#if defined(_CS_LANG_FR)

 1100 format(/,                                                   &
 1X,'Vitesse maximale apres prediction ',E12.4)

#else

 1100 format(/,                                                   &
 1X,'Maximum velocity after prediction ',E12.4)

#endif

!----
! End
!----

return

end subroutine
