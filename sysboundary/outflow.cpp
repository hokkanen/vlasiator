/*
 * This file is part of Vlasiator.
 * Copyright 2010-2016 Finnish Meteorological Institute
 *
 * For details of usage, see the COPYING file and read the "Rules of the Road"
 * at http://www.physics.helsinki.fi/vlasiator/
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License along
 * with this program; if not, write to the Free Software Foundation, Inc.,
 * 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA.
 */

/*!\file outflow.cpp
 * \brief Implementation of the class SysBoundaryCondition::Outflow to handle cells classified as sysboundarytype::OUTFLOW.
 */

#include <cstdlib>
#include <iostream>

#include "../object_wrapper.h"
#include "outflow.h"
#include "../projects/projects_common.h"
#include "../fieldsolver/fs_common.h"
#include "../fieldsolver/ldz_magnetic_field.hpp"
#include "../vlasovmover.h"

#ifndef NDEBUG
   #define DEBUG_OUTFLOW
#endif
#ifdef DEBUG_SYSBOUNDARY
   #define DEBUG_OUTFLOW
#endif

using namespace std;

namespace SBC {
   Outflow::Outflow(): OuterBoundaryCondition() { }
   Outflow::~Outflow() { }
   
   void Outflow::addParameters() {
      const string defStr = "Copy";
      Readparameters::addComposing("outflow.faceNoFields", "List of faces on which no field outflow boundary conditions are to be applied ([xyz][+-]).");
      Readparameters::add("outflow.precedence", "Precedence value of the outflow system boundary condition (integer), the higher the stronger.", 4);
      Readparameters::add("outflow.reapplyUponRestart", "If 0 (default), keep going with the state existing in the restart file. If 1, calls again applyInitialState. Can be used to change boundary condition behaviour during a run.", 0);

      // Per-population parameters
      for(uint i=0; i< getObjectWrapper().particleSpecies.size(); i++) {
        const string& pop = getObjectWrapper().particleSpecies[i].name;

        Readparameters::addComposing(pop + "_outflow.reapplyFaceUponRestart", "List of faces on which outflow boundary conditions are to be reapplied upon restart ([xyz][+-]).");
        Readparameters::addComposing(pop + "_outflow.face", "List of faces on which outflow boundary conditions are to be applied ([xyz][+-]).");
        Readparameters::add(pop + "_outflow.vlasovScheme_face_x+", "Scheme to use on the face x+ (Copy, Limit, None)", defStr);
        Readparameters::add(pop + "_outflow.vlasovScheme_face_x-", "Scheme to use on the face x- (Copy, Limit, None)", defStr);
        Readparameters::add(pop + "_outflow.vlasovScheme_face_y+", "Scheme to use on the face y+ (Copy, Limit, None)", defStr);
        Readparameters::add(pop + "_outflow.vlasovScheme_face_y-", "Scheme to use on the face y- (Copy, Limit, None)", defStr);
        Readparameters::add(pop + "_outflow.vlasovScheme_face_z+", "Scheme to use on the face z+ (Copy, Limit, None)", defStr);
        Readparameters::add(pop + "_outflow.vlasovScheme_face_z-", "Scheme to use on the face z- (Copy, Limit, None)", defStr);

        Readparameters::add(pop + "_outflow.quench", "Factor by which to quench the inflowing parts of the velocity distribution function.", 1.0);
      }
   }
   
   void Outflow::getParameters() {
      int myRank;
      MPI_Comm_rank(MPI_COMM_WORLD,&myRank);
      Readparameters::get("outflow.faceNoFields", this->faceNoFieldsList);
      Readparameters::get("outflow.precedence", precedence);
      uint reapply;
      Readparameters::get("outflow.reapplyUponRestart", reapply);
      this->applyUponRestart = false;
      if(reapply == 1) {
         this->applyUponRestart = true;
      }

      // Per-species parameters
      for(uint i=0; i< getObjectWrapper().particleSpecies.size(); i++) {
        const string& pop = getObjectWrapper().particleSpecies[i].name;
        OutflowSpeciesParameters sP;

        // Unless we find out otherwise, we assume that this species will not be treated at any boundary
        for(int j=0; j<6; j++) {
          sP.facesToSkipVlasov[j] = true;
        }

        vector<string> thisSpeciesFaceList;
        Readparameters::get(pop + "_outflow.face", thisSpeciesFaceList);

        for(auto& face : thisSpeciesFaceList) {
          if(face == "x+") { facesToProcess[0] = true; sP.facesToSkipVlasov[0] = false; }
          if(face == "x-") { facesToProcess[1] = true; sP.facesToSkipVlasov[1] = false; }
          if(face == "y+") { facesToProcess[2] = true; sP.facesToSkipVlasov[2] = false; }
          if(face == "y-") { facesToProcess[3] = true; sP.facesToSkipVlasov[3] = false; }
          if(face == "z+") { facesToProcess[4] = true; sP.facesToSkipVlasov[4] = false; }
          if(face == "z-") { facesToProcess[5] = true; sP.facesToSkipVlasov[5] = false; }
        }

        Readparameters::get(pop + "_outflow.reapplyFaceUponRestart", sP.faceToReapplyUponRestartList);
        array<string, 6> vlasovSysBoundarySchemeName;
        Readparameters::get(pop + "_outflow.vlasovScheme_face_x+", vlasovSysBoundarySchemeName[0]);
        Readparameters::get(pop + "_outflow.vlasovScheme_face_x-", vlasovSysBoundarySchemeName[1]);
        Readparameters::get(pop + "_outflow.vlasovScheme_face_y+", vlasovSysBoundarySchemeName[2]);

        Readparameters::get(pop + "_outflow.vlasovScheme_face_y-", vlasovSysBoundarySchemeName[3]);
        Readparameters::get(pop + "_outflow.vlasovScheme_face_z+", vlasovSysBoundarySchemeName[4]);
        Readparameters::get(pop + "_outflow.vlasovScheme_face_z-", vlasovSysBoundarySchemeName[5]);
        for(uint j=0; j<6 ; j++) {
           if(vlasovSysBoundarySchemeName[j] == "None") {
              sP.faceVlasovScheme[j] = vlasovscheme::NONE;
           } else if (vlasovSysBoundarySchemeName[j] == "Copy") {
              sP.faceVlasovScheme[j] = vlasovscheme::COPY;
           } else if(vlasovSysBoundarySchemeName[j] == "Limit") {
              sP.faceVlasovScheme[j] = vlasovscheme::LIMIT;
           } else {
              if(myRank == MASTER_RANK) cerr << __FILE__ << ":" << __LINE__ << " ERROR: " << vlasovSysBoundarySchemeName[j] << " is an invalid Outflow Vlasov scheme!" << endl;
              exit(1);
           }
        }

        Readparameters::get(pop + "_outflow.quench", sP.quenchFactor);

        speciesParams.push_back(sP);
      }
   }
   
   bool Outflow::initSysBoundary(
      creal& t,
      Project &project
   ) {
      /* The array of bool describes which of the x+, x-, y+, y-, z+, z- faces are to have outflow system boundary conditions.
       * A true indicates the corresponding face will have outflow.
       * The 6 elements correspond to x+, x-, y+, y-, z+, z- respectively.
       */
      for(uint i=0; i<6; i++) {
         facesToProcess[i] = false;
         facesToSkipFields[i] = false;
         facesToReapply[i] = false;
      }
      
      this->getParameters();
      
      isThisDynamic = false;
      
      vector<string>::const_iterator it;
      for (it = faceNoFieldsList.begin();
           it != faceNoFieldsList.end();
      it++) {
         if(*it == "x+") facesToSkipFields[0] = true;
         if(*it == "x-") facesToSkipFields[1] = true;
         if(*it == "y+") facesToSkipFields[2] = true;
         if(*it == "y-") facesToSkipFields[3] = true;
         if(*it == "z+") facesToSkipFields[4] = true;
         if(*it == "z-") facesToSkipFields[5] = true;
      }

      for(uint i=0; i< getObjectWrapper().particleSpecies.size(); i++) {
         OutflowSpeciesParameters& sP = this->speciesParams[i];
         for (it = sP.faceToReapplyUponRestartList.begin();
              it != sP.faceToReapplyUponRestartList.end();
         it++) {
            if(*it == "x+") facesToReapply[0] = true;
            if(*it == "x-") facesToReapply[1] = true;
            if(*it == "y+") facesToReapply[2] = true;
            if(*it == "y-") facesToReapply[3] = true;
            if(*it == "z+") facesToReapply[4] = true;
            if(*it == "z-") facesToReapply[5] = true;
         }
      }
      return true;
   }

   bool Outflow::applyInitialState(
      const dccrg::Dccrg<SpatialCell,dccrg::Cartesian_Geometry>& mpiGrid,
      FsGrid< fsgrids::technical, FS_STENCIL_WIDTH> & technicalGrid,
      FsGrid< array<Real, fsgrids::bfield::N_BFIELD>, FS_STENCIL_WIDTH> & perBGrid,
      Project &project
   ) {
      const vector<CellID>& cells = getLocalCells();
      #pragma omp parallel for
      for (uint i=0; i<cells.size(); ++i) {
         CellID id = cells[i];
         SpatialCell* cell = mpiGrid[id];
         if (cell->sysBoundaryFlag != this->getIndex()) {
            continue;
         }
         
         bool doApply = true;
         
         if(Parameters::isRestart) {
            std::array<bool, 6> isThisCellOnAFace;
            determineFace(isThisCellOnAFace, mpiGrid, id);
            
            doApply=false;
            // Comparison of the array defining which faces to use and the array telling on which faces this cell is
            for (uint j=0; j<6; j++) {
               doApply = doApply || (facesToReapply[j] && isThisCellOnAFace[j]);
            }
         }

         if (doApply) {
            project.setCell(cell);
            cell->parameters[CellParams::RHOM_DT2] = cell->parameters[CellParams::RHOM];
            cell->parameters[CellParams::RHOQ_DT2] = cell->parameters[CellParams::RHOQ];
            cell->parameters[CellParams::VX_DT2] = cell->parameters[CellParams::VX];
            cell->parameters[CellParams::VY_DT2] = cell->parameters[CellParams::VY];
            cell->parameters[CellParams::VZ_DT2] = cell->parameters[CellParams::VZ];
            cell->parameters[CellParams::P_11_DT2] = cell->parameters[CellParams::P_11];
            cell->parameters[CellParams::P_22_DT2] = cell->parameters[CellParams::P_22];
            cell->parameters[CellParams::P_33_DT2] = cell->parameters[CellParams::P_33];
         }
      }

      return true;
   }

   Real Outflow::fieldSolverBoundaryCondMagneticField(
      FsGrid< array<Real, fsgrids::bfield::N_BFIELD>, FS_STENCIL_WIDTH> & bGrid,
      FsGrid< fsgrids::technical, FS_STENCIL_WIDTH> & technicalGrid,
      cint i,
      cint j,
      cint k,
      creal& dt,
      cuint& component
   ) {
      switch(component) {
      case 0:
         return fieldBoundaryCopyFromSolvingNbrMagneticField(bGrid, technicalGrid, i, j, k, component, compute::BX);
      case 1:
         return fieldBoundaryCopyFromSolvingNbrMagneticField(bGrid, technicalGrid, i, j, k, component, compute::BY);
      case 2:
         return fieldBoundaryCopyFromSolvingNbrMagneticField(bGrid, technicalGrid, i, j, k, component, compute::BZ);
      default:
         return 0.0;
      }
   }

   void Outflow::fieldSolverBoundaryCondMagneticFieldProjection(
      FsGrid< array<Real, fsgrids::bfield::N_BFIELD>, FS_STENCIL_WIDTH> & bGrid,
      FsGrid< fsgrids::technical, FS_STENCIL_WIDTH> & technicalGrid,
      cint i,
      cint j,
      cint k
   ) {
   }
   void Outflow::fieldSolverBoundaryCondElectricField(
      FsGrid< array<Real, fsgrids::efield::N_EFIELD>, FS_STENCIL_WIDTH> & EGrid,
      cint i,
      cint j,
      cint k,
      cuint component
   ) {
      EGrid.get(i,j,k)->at(fsgrids::efield::EX+component) = 0.0;
   }
   
   void Outflow::fieldSolverBoundaryCondHallElectricField(
      FsGrid< array<Real, fsgrids::ehall::N_EHALL>, FS_STENCIL_WIDTH> & EHallGrid,
      cint i,
      cint j,
      cint k,
      cuint component
   ) {
      array<Real, fsgrids::ehall::N_EHALL> * cp = EHallGrid.get(i,j,k);
      switch (component) {
         case 0:
            cp->at(fsgrids::ehall::EXHALL_000_100) = 0.0;
            cp->at(fsgrids::ehall::EXHALL_010_110) = 0.0;
            cp->at(fsgrids::ehall::EXHALL_001_101) = 0.0;
            cp->at(fsgrids::ehall::EXHALL_011_111) = 0.0;
            break;
         case 1:
            cp->at(fsgrids::ehall::EYHALL_000_010) = 0.0;
            cp->at(fsgrids::ehall::EYHALL_100_110) = 0.0;
            cp->at(fsgrids::ehall::EYHALL_001_011) = 0.0;
            cp->at(fsgrids::ehall::EYHALL_101_111) = 0.0;
            break;
         case 2:
            cp->at(fsgrids::ehall::EZHALL_000_001) = 0.0;
            cp->at(fsgrids::ehall::EZHALL_100_101) = 0.0;
            cp->at(fsgrids::ehall::EZHALL_010_011) = 0.0;
            cp->at(fsgrids::ehall::EZHALL_110_111) = 0.0;
            break;
         default:
            cerr << __FILE__ << ":" << __LINE__ << ":" << " Invalid component" << endl;
      }
   }
   
   void Outflow::fieldSolverBoundaryCondGradPeElectricField(
      FsGrid< array<Real, fsgrids::egradpe::N_EGRADPE>, FS_STENCIL_WIDTH> & EGradPeGrid,
      cint i,
      cint j,
      cint k,
      cuint component
   ) {
      EGradPeGrid.get(i,j,k)->at(fsgrids::egradpe::EXGRADPE+component) = 0.0;
   }
   
   void Outflow::fieldSolverBoundaryCondDerivatives(
      FsGrid< array<Real, fsgrids::dperb::N_DPERB>, FS_STENCIL_WIDTH> & dPerBGrid,
      FsGrid< array<Real, fsgrids::dmoments::N_DMOMENTS>, FS_STENCIL_WIDTH> & dMomentsGrid,
      cint i,
      cint j,
      cint k,
      cuint& RKCase,
      cuint& component
   ) {
      this->setCellDerivativesToZero(dPerBGrid, dMomentsGrid, i, j, k, component);
   }
   
   void Outflow::fieldSolverBoundaryCondBVOLDerivatives(
      FsGrid< array<Real, fsgrids::volfields::N_VOL>, FS_STENCIL_WIDTH> & volGrid,
      cint i,
      cint j,
      cint k,
      cuint& component
   ) {
      this->setCellBVOLDerivativesToZero(volGrid, i, j, k, component);
   }
   
   /**
    * NOTE that this is called once for each particle species!
    * @param mpiGrid
    * @param cellID
    */
   void Outflow::vlasovBoundaryCondition(
      const dccrg::Dccrg<SpatialCell,dccrg::Cartesian_Geometry>& mpiGrid,
      const CellID& cellID,
      const uint popID,
      const bool calculate_V_moments
   ) {
//      phiprof::start("vlasovBoundaryCondition (Outflow)");
      
      const OutflowSpeciesParameters& sP = this->speciesParams[popID];
      if (mpiGrid[cellID]->sysBoundaryFlag != this->getIndex()) {
         return;
      }

      std::array<bool, 6> isThisCellOnAFace;
      determineFace(isThisCellOnAFace, mpiGrid, cellID, true);
      
      for(uint i=0; i<6; i++) {
         if(isThisCellOnAFace[i] && facesToProcess[i] && !sP.facesToSkipVlasov[i]) {
            switch(sP.faceVlasovScheme[i]) {
               case vlasovscheme::NONE:
                  break;
               case vlasovscheme::COPY:
                  vlasovBoundaryCopyFromTheClosestNbr(mpiGrid,cellID,false,popID,calculate_V_moments);
                  break;
               case vlasovscheme::LIMIT:
                  vlasovBoundaryCopyFromTheClosestNbrAndLimit(mpiGrid,cellID,popID);
                  break;
               default:
                  cerr << __FILE__ << ":" << __LINE__ << "ERROR: invalid Outflow Vlasov scheme!" << endl;
                  exit(1);
                  break;
            }
         }
      }
      
//      phiprof::stop("vlasovBoundaryCondition (Outflow)");
   }
   
   void Outflow::getFaces(bool* faces) {
      for(uint i=0; i<6; i++) faces[i] = facesToProcess[i];
   }
   
   string Outflow::getName() const {return "Outflow";}
   uint Outflow::getIndex() const {return sysboundarytype::OUTFLOW;}
      
}
