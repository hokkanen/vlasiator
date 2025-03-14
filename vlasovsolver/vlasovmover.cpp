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

#include <cstdlib>
#include <iostream>
#include <vector>
#include <stdint.h>

#ifdef _OPENMP
   #include <omp.h>
#endif

#include <zoltan.h>
#include <dccrg.hpp>
#include <phiprof.hpp>

#include "../spatial_cell.hpp"
#include "../vlasovmover.h"
#include "../grid.h"
#include "../definitions.h"
#include "../object_wrapper.h"
#include "../mpiconversion.h"

#include "arch_moments.h"

#include "cpu_trans_pencils.hpp"

#ifdef USE_GPU
#include "gpu_acc_map.hpp"
#include "gpu_acc_semilag.hpp"
#include "gpu_trans_map_amr.hpp"
#else
#include "cpu_acc_semilag.hpp"
#include "cpu_trans_map_amr.hpp"
#endif

using namespace std;
using namespace spatial_cell;

/** Propagates the distribution function in spatial space.

    Based on SLICE-3D algorithm: Zerroukat, M., and T. Allen. "A
    three‐dimensional monotone and conservative semi‐Lagrangian scheme
    (SLICE‐3D) for transport problems." Quarterly Journal of the Royal
    Meteorological Society 138.667 (2012): 1640-1651.

 */
void calculateSpatialTranslation(
        dccrg::Dccrg<SpatialCell,dccrg::Cartesian_Geometry>& mpiGrid,
        const vector<CellID>& localCells,
        const vector<CellID>& local_propagated_cells,
        const vector<CellID>& local_target_cells,
        const vector<CellID>& remoteTargetCellsx,
        const vector<CellID>& remoteTargetCellsy,
        const vector<CellID>& remoteTargetCellsz,
        vector<uint>& nPencils,
        creal dt,
        const uint popID,
        Real &time
) {

    int trans_timer;
    //bool localTargetGridGenerated = false;
    bool AMRtranslationActive = false;
    if (P::amrMaxSpatialRefLevel > 0) {
       AMRtranslationActive = true;
    }
    double t1;

    int myRank;
    MPI_Comm_rank(MPI_COMM_WORLD,&myRank);

   int bt=phiprof::initializeTimer("barrier-trans-pre-z","Barriers","MPI");
   phiprof::start(bt);
   MPI_Barrier(MPI_COMM_WORLD);
   phiprof::stop(bt);

    // ------------- SLICE - map dist function in Z --------------- //
   if(P::zcells_ini > 1){

      trans_timer=phiprof::initializeTimer("transfer-stencil-data-z","MPI");
      phiprof::start(trans_timer);
      //updateRemoteVelocityBlockLists(mpiGrid,popID,VLASOV_SOLVER_Z_NEIGHBORHOOD_ID);
      SpatialCell::set_mpi_transfer_direction(2);
      SpatialCell::set_mpi_transfer_type(Transfer::VEL_BLOCK_DATA,false,AMRtranslationActive);
      mpiGrid.update_copies_of_remote_neighbors(VLASOV_SOLVER_Z_NEIGHBORHOOD_ID);
      phiprof::stop(trans_timer);

      // bt=phiprof::initializeTimer("barrier-trans-pre-trans_map_1d-z","Barriers","MPI");
      // phiprof::start(bt);
      // MPI_Barrier(MPI_COMM_WORLD);
      // phiprof::stop(bt);

      t1 = MPI_Wtime();
      phiprof::start("compute-mapping-z");
#ifdef USE_GPU
      gpu_trans_map_1d_amr(mpiGrid,local_propagated_cells, remoteTargetCellsz, nPencils, 2, dt,popID); // map along z//
#else
      trans_map_1d_amr(mpiGrid,local_propagated_cells, remoteTargetCellsz, nPencils, 2, dt,popID); // map along z//
#endif
      phiprof::stop("compute-mapping-z");
      time += MPI_Wtime() - t1;

      bt=phiprof::initializeTimer("barrier-trans-pre-update_remote-z","Barriers","MPI");
      phiprof::start(bt);
      MPI_Barrier(MPI_COMM_WORLD);
      phiprof::stop(bt);

      trans_timer=phiprof::initializeTimer("update_remote-z","MPI");
      phiprof::start("update_remote-z");
#ifdef USE_GPU
      gpu_update_remote_mapping_contribution_amr(mpiGrid, 2,+1,popID);
      gpu_update_remote_mapping_contribution_amr(mpiGrid, 2,-1,popID);
#else
      update_remote_mapping_contribution_amr(mpiGrid, 2,+1,popID);
      update_remote_mapping_contribution_amr(mpiGrid, 2,-1,popID);
#endif
      phiprof::stop("update_remote-z");

   }

   bt=phiprof::initializeTimer("barrier-trans-pre-x","Barriers","MPI");
   phiprof::start(bt);
   MPI_Barrier(MPI_COMM_WORLD);
   phiprof::stop(bt);

   // ------------- SLICE - map dist function in X --------------- //
   if(P::xcells_ini > 1){

      trans_timer=phiprof::initializeTimer("transfer-stencil-data-x","MPI");
      phiprof::start(trans_timer);
      //updateRemoteVelocityBlockLists(mpiGrid,popID,VLASOV_SOLVER_X_NEIGHBORHOOD_ID);
      SpatialCell::set_mpi_transfer_direction(0);
      SpatialCell::set_mpi_transfer_type(Transfer::VEL_BLOCK_DATA,false,AMRtranslationActive);
      mpiGrid.update_copies_of_remote_neighbors(VLASOV_SOLVER_X_NEIGHBORHOOD_ID);
      phiprof::stop(trans_timer);

      // bt=phiprof::initializeTimer("barrier-trans-pre-trans_map_1d-x","Barriers","MPI");
      // phiprof::start(bt);
      // MPI_Barrier(MPI_COMM_WORLD);
      // phiprof::stop(bt);

      t1 = MPI_Wtime();
      phiprof::start("compute-mapping-x");
#ifdef USE_GPU
      gpu_trans_map_1d_amr(mpiGrid,local_propagated_cells, remoteTargetCellsx, nPencils, 0,dt,popID); // map along x//
#else
      trans_map_1d_amr(mpiGrid,local_propagated_cells, remoteTargetCellsx, nPencils, 0,dt,popID); // map along x//
#endif
      phiprof::stop("compute-mapping-x");
      time += MPI_Wtime() - t1;

      bt=phiprof::initializeTimer("barrier-trans-pre-update_remote-x","Barriers","MPI");
      phiprof::start(bt);
      MPI_Barrier(MPI_COMM_WORLD);
      phiprof::stop(bt);

      trans_timer=phiprof::initializeTimer("update_remote-x","MPI");
      phiprof::start("update_remote-x");
#ifdef USE_GPU
      gpu_update_remote_mapping_contribution_amr(mpiGrid, 0,+1,popID);
      gpu_update_remote_mapping_contribution_amr(mpiGrid, 0,-1,popID);
#else
      update_remote_mapping_contribution_amr(mpiGrid, 0,+1,popID);
      update_remote_mapping_contribution_amr(mpiGrid, 0,-1,popID);
#endif
      phiprof::stop("update_remote-x");

   }

   bt=phiprof::initializeTimer("barrier-trans-pre-y","Barriers","MPI");
   phiprof::start(bt);
   MPI_Barrier(MPI_COMM_WORLD);
   phiprof::stop(bt);

   // ------------- SLICE - map dist function in Y --------------- //
   if(P::ycells_ini > 1) {

      trans_timer=phiprof::initializeTimer("transfer-stencil-data-y","MPI");
      phiprof::start(trans_timer);
      //updateRemoteVelocityBlockLists(mpiGrid,popID,VLASOV_SOLVER_Y_NEIGHBORHOOD_ID);
      SpatialCell::set_mpi_transfer_direction(1);
      SpatialCell::set_mpi_transfer_type(Transfer::VEL_BLOCK_DATA,false,AMRtranslationActive);
      mpiGrid.update_copies_of_remote_neighbors(VLASOV_SOLVER_Y_NEIGHBORHOOD_ID);
      phiprof::stop(trans_timer);

      // bt=phiprof::initializeTimer("barrier-trans-pre-trans_map_1d-y","Barriers","MPI");
      // phiprof::start(bt);
      // MPI_Barrier(MPI_COMM_WORLD);
      // phiprof::stop(bt);

      t1 = MPI_Wtime();
      phiprof::start("compute-mapping-y");
#ifdef USE_GPU
      gpu_trans_map_1d_amr(mpiGrid,local_propagated_cells, remoteTargetCellsy, nPencils, 1,dt,popID); // map along y//
#else
      trans_map_1d_amr(mpiGrid,local_propagated_cells, remoteTargetCellsy, nPencils, 1,dt,popID); // map along y//
#endif
      phiprof::stop("compute-mapping-y");
      time += MPI_Wtime() - t1;

      bt=phiprof::initializeTimer("barrier-trans-pre-update_remote-y","Barriers","MPI");
      phiprof::start(bt);
      MPI_Barrier(MPI_COMM_WORLD);
      phiprof::stop(bt);

      trans_timer=phiprof::initializeTimer("update_remote-y","MPI");
      phiprof::start("update_remote-y");
#ifdef USE_GPU
      gpu_update_remote_mapping_contribution_amr(mpiGrid, 1,+1,popID);
      gpu_update_remote_mapping_contribution_amr(mpiGrid, 1,-1,popID);
#else
      update_remote_mapping_contribution_amr(mpiGrid, 1,+1,popID);
      update_remote_mapping_contribution_amr(mpiGrid, 1,-1,popID);
#endif
      phiprof::stop("update_remote-y");

   }

   bt=phiprof::initializeTimer("barrier-trans-post-trans","Barriers","MPI");
   phiprof::start(bt);
   MPI_Barrier(MPI_COMM_WORLD);
   phiprof::stop(bt);

   // MPI_Barrier(MPI_COMM_WORLD);
   // bailout(true, "", __FILE__, __LINE__);
}

/*!

  Propagates the distribution function in spatial space.

  Based on SLICE-3D algorithm: Zerroukat, M., and T. Allen. "A
  three‐dimensional monotone and conservative semi‐Lagrangian scheme
  (SLICE‐3D) for transport problems." Quarterly Journal of the Royal
  Meteorological Society 138.667 (2012): 1640-1651.

*/
void calculateSpatialTranslation(
        dccrg::Dccrg<SpatialCell,dccrg::Cartesian_Geometry>& mpiGrid,
        creal dt) {
   typedef Parameters P;

   phiprof::start("semilag-trans");

   double t1 = MPI_Wtime();

   const vector<CellID>& localCells = getLocalCells();
   vector<CellID> remoteTargetCellsx;
   vector<CellID> remoteTargetCellsy;
   vector<CellID> remoteTargetCellsz;
   vector<CellID> local_propagated_cells;
   vector<CellID> local_target_cells;
   vector<uint> nPencils;
   Real time=0.0;

   // If dt=0 we are either initializing or distribution functions are not translated.
   // In both cases go to the end of this function and calculate the moments.
   if (dt == 0.0) goto momentCalculation;

   phiprof::start("compute_cell_lists");
   remoteTargetCellsx = mpiGrid.get_remote_cells_on_process_boundary(VLASOV_SOLVER_TARGET_X_NEIGHBORHOOD_ID);
   remoteTargetCellsy = mpiGrid.get_remote_cells_on_process_boundary(VLASOV_SOLVER_TARGET_Y_NEIGHBORHOOD_ID);
   remoteTargetCellsz = mpiGrid.get_remote_cells_on_process_boundary(VLASOV_SOLVER_TARGET_Z_NEIGHBORHOOD_ID);

   // Figure out which spatial cells are translated,
   // result independent of particle species.
   for (size_t c=0; c<localCells.size(); ++c) {
      if (do_translate_cell(mpiGrid[localCells[c]])) {
         local_propagated_cells.push_back(localCells[c]);
      }
   }

   // Figure out target spatial cells, result
   // independent of particle species.
   for (size_t c=0; c<localCells.size(); ++c) {
      if (mpiGrid[localCells[c]]->sysBoundaryFlag == sysboundarytype::NOT_SYSBOUNDARY) {
         local_target_cells.push_back(localCells[c]);
      }
   }
   if (P::prepareForRebalance == true) {
      // One more element to count the sums
      for (size_t c=0; c<local_propagated_cells.size()+1; c++) {
         nPencils.push_back(0);
      }
   }
   phiprof::stop("compute_cell_lists");

   // Translate all particle species
   for (uint popID=0; popID<getObjectWrapper().particleSpecies.size(); ++popID) {
      string profName = "translate "+getObjectWrapper().particleSpecies[popID].name;
      phiprof::start(profName);
      SpatialCell::setCommunicatedSpecies(popID);
      //      std::cout << "I am at line " << __LINE__ << " of " << __FILE__ << std::endl;
      calculateSpatialTranslation(
         mpiGrid,
         localCells,
         local_propagated_cells,
         local_target_cells,
         remoteTargetCellsx,
         remoteTargetCellsy,
         remoteTargetCellsz,
         nPencils,
         dt,
         popID,
         time
      );
      phiprof::stop(profName);
   }

   if (Parameters::prepareForRebalance == true) {
      if(P::amrMaxSpatialRefLevel == 0) {
//          const double deltat = (MPI_Wtime() - t1) / local_propagated_cells.size();
         for (size_t c=0; c<localCells.size(); ++c) {
//            mpiGrid[localCells[c]]->parameters[CellParams::LBWEIGHTCOUNTER] += time / localCells.size();
            for (uint popID=0; popID<getObjectWrapper().particleSpecies.size(); ++popID) {
               mpiGrid[localCells[c]]->parameters[CellParams::LBWEIGHTCOUNTER] += mpiGrid[localCells[c]]->get_number_of_velocity_blocks(popID);
            }
         }
      } else {
//          const double deltat = MPI_Wtime() - t1;
         for (size_t c=0; c<local_propagated_cells.size(); ++c) {
            Real counter = 0;
            for (uint popID=0; popID<getObjectWrapper().particleSpecies.size(); ++popID) {
               counter += mpiGrid[local_propagated_cells[c]]->get_number_of_velocity_blocks(popID);
            }
            mpiGrid[local_propagated_cells[c]]->parameters[CellParams::LBWEIGHTCOUNTER] += nPencils[c] * counter;
//            mpiGrid[localCells[c]]->parameters[CellParams::LBWEIGHTCOUNTER] += time / localCells.size();
         }
      }
   }

   // Mapping complete, update moments and maximum dt limits //
momentCalculation:
   calculateMoments_R(mpiGrid, localCells, true);

   phiprof::stop("semilag-trans");
}

/*
  --------------------------------------------------
  Acceleration (velocity space propagation)
  --------------------------------------------------
*/

/** Accelerate the given population to new time t+dt.
 * This function is AMR safe.
 * @param popID Particle population ID.
 * @param globalMaxSubcycles Number of times acceleration is subcycled.
 * @param step The current subcycle step.
 * @param mpiGrid Parallel grid library.
 * @param propagatedCells List of cells in which the population is accelerated.
 * @param dt Timestep.*/
void calculateAcceleration(const uint popID,const uint globalMaxSubcycles,const uint step,
                           dccrg::Dccrg<SpatialCell,dccrg::Cartesian_Geometry>& mpiGrid,
                           const std::vector<CellID>& propagatedCells,
                           const Real& dt) {
   // Set active population
   SpatialCell::setCommunicatedSpecies(popID);

   // Calculate velocity moments, these are needed to
   // calculate the transforms used in the accelerations.
   // Calculated moments are stored in the "_V" variables.
   calculateMoments_V(mpiGrid, propagatedCells, false);

   //generate pseudo-random order which is always the same irrespective of parallelization, restarts, etc.
   std::size_t rndInt = std::hash<uint>()(P::tstep);
   uint map_order=rndInt%3;

   // Semi-Lagrangian acceleration for those cells which are subcycled,
   // dimension-by-dimension
   #pragma omp parallel for schedule(dynamic,1)
   for (size_t c=0; c<propagatedCells.size(); ++c) {
      const CellID cellID = propagatedCells[c];
      const Real maxVdt = mpiGrid[cellID]->get_max_v_dt(popID);

      //compute subcycle dt. The length is maxVdt on all steps
      //except the last one. This is to keep the neighboring
      //spatial cells in sync, so that two neighboring cells with
      //different number of subcycles have similar timesteps,
      //except that one takes an additional short step. This keeps
      //spatial block neighbors as much in sync as possible for
      //adjust blocks.
      Real subcycleDt;
      if( (step + 1) * maxVdt > fabs(dt)) {
         subcycleDt = max(fabs(dt) - step * maxVdt, 0.0);
      } else{
         subcycleDt = maxVdt;
      }
      if (dt<0) {
         subcycleDt = -subcycleDt;
      }

      phiprof::start("cell-semilag-acc");
#ifdef USE_GPU
      gpu_accelerate_cell(mpiGrid[cellID],popID,map_order,subcycleDt);
#else
      cpu_accelerate_cell(mpiGrid[cellID],popID,map_order,subcycleDt);
#endif
      phiprof::stop("cell-semilag-acc");
   }
   //global adjust after each subcycle to keep number of blocks managable. Even the ones not
   //accelerating anyore participate. It is important to keep
   //the spatial dimension to make sure that we do not loose
   //stuff streaming in from other cells, perhaps not connected
   //to the existing distribution function in the cell.
   //- All cells update and communicate their lists of content blocks
   //- Only cells which were accerelated on this step need to be adjusted (blocks removed or added).
   //- Not done here on last step (done after loop)
   if(step < (globalMaxSubcycles - 1)) adjustVelocityBlocks(mpiGrid, propagatedCells, false, popID);
}

/** Accelerate all particle populations to new time t+dt.
 * This function is AMR safe.
 * @param mpiGrid Parallel grid library.
 * @param dt Time step.*/
void calculateAcceleration(dccrg::Dccrg<SpatialCell,dccrg::Cartesian_Geometry>& mpiGrid,
                           Real dt
                          ) {
   typedef Parameters P;
   const vector<CellID>& cells = getLocalCells();

   int myRank;
   MPI_Comm_rank(MPI_COMM_WORLD,&myRank);

   if (dt == 0.0 && P::tstep > 0) {

      // Even if acceleration is turned off we need to adjust velocity blocks
      // because the boundary conditions may have altered the velocity space,
      // and to update changes in no-content blocks during translation.
      for (uint popID=0; popID<getObjectWrapper().particleSpecies.size(); ++popID) {
         adjustVelocityBlocks(mpiGrid, cells, true, popID);
      }

      goto momentCalculation;
   }
   phiprof::start("semilag-acc");

   // Accelerate all particle species
   for (uint popID=0; popID<getObjectWrapper().particleSpecies.size(); ++popID) {
      uint gpuMaxBlockCount = 0; // would be better to be over all populations
      int maxSubcycles=0;
      int globalMaxSubcycles;

      // Set active population
      SpatialCell::setCommunicatedSpecies(popID);

      // Iterate through all local cells and collect cells to propagate.
      // Ghost cells (spatial cells at the boundary of the simulation
      // volume) do not need to be propagated:
      phiprof::start("Gather subcycles and propagated cells");
      vector<CellID> propagatedCells;
      #pragma omp parallel for
      for (size_t c=0; c<cells.size(); ++c) {
         SpatialCell* SC = mpiGrid[cells[c]];
         const vmesh::VelocityMesh* vmesh = SC->get_velocity_mesh(popID);
         // disregard boundary cells, in preparation for acceleration
         if (  (SC->sysBoundaryFlag == sysboundarytype::NOT_SYSBOUNDARY) ||
               // Include inflow-Maxwellian
               (P::vlasovAccelerateMaxwellianBoundaries && (SC->sysBoundaryFlag == sysboundarytype::SET_MAXWELLIAN)) ) {
            uint blockCount = vmesh->size();
            if (blockCount != 0){
               //do not propagate spatial cells with no blocks
               #pragma omp critical
               propagatedCells.push_back(cells[c]);
            }
            //prepare for acceleration, updates max dt for each cell, it
            //needs to be set to somthing sensible for _all_ cells, even if
            //they are not propagated
            prepareAccelerateCell(SC, popID);
            //update max subcycles for all cells in this process
            maxSubcycles = max((int)getAccelerationSubcycles(SC, dt, popID), maxSubcycles);
            spatial_cell::Population& pop = SC->get_population(popID);
            pop.ACCSUBCYCLES = getAccelerationSubcycles(SC, dt, popID);
#ifdef USE_GPU
#pragma omp critical
            {
               if (blockCount > gpuMaxBlockCount) {
                  gpuMaxBlockCount = blockCount;
               }
            }
#endif
         }
      }
      phiprof::stop("Gather subcycles and propagated cells");

#ifdef USE_GPU
      // Ensure accelerator has enough temporary memory allocated
      phiprof::start("gpu allocation verifications");
      gpu_vlasov_allocate(gpuMaxBlockCount);
      gpu_acc_allocate(gpuMaxBlockCount);
      phiprof::stop("gpu allocation verifications");
#endif

      // Compute global maximum for number of subcycles
      MPI_Allreduce(&maxSubcycles, &globalMaxSubcycles, 1, MPI_INT, MPI_MAX, MPI_COMM_WORLD);

      // TODO: move subcycling to lower level call in order to optimize GPU memory calls

      // substep global max times
      for(uint step=0; step<(uint)globalMaxSubcycles; ++step) {
         if(step > 0) {
            // prune list of cells to propagate to only contained those which are now subcycled
            vector<CellID> temp;
            for (const auto& cell: propagatedCells) {
               if (step < getAccelerationSubcycles(mpiGrid[cell], dt, popID) ) {
                  temp.push_back(cell);
               }
            }

            propagatedCells.swap(temp);
         }
         // Accelerate population over one subcycle step
         calculateAcceleration(popID,(uint)globalMaxSubcycles,step,mpiGrid,propagatedCells,dt);
      } // for-loop over acceleration substeps

      // final adjust for all cells, also fixing remote cells.
      adjustVelocityBlocks(mpiGrid, cells, true, popID);
   } // for-loop over particle species

   phiprof::stop("semilag-acc");

   // Recalculate "_V" velocity moments
  momentCalculation:
   calculateMoments_V(mpiGrid, cells, true);

   // Set CellParams::MAXVDT to be the minimum dt of all per-species values
#pragma omp parallel for
   for (size_t c=0; c<cells.size(); ++c) {
      SpatialCell* cell = mpiGrid[cells[c]];
      cell->parameters[CellParams::MAXVDT] = numeric_limits<Real>::max();
      for (uint popID=0; popID<getObjectWrapper().particleSpecies.size(); ++popID) {
         cell->parameters[CellParams::MAXVDT]
            = min(cell->get_max_v_dt(popID), cell->parameters[CellParams::MAXVDT]);
      }
   }
}

/*--------------------------------------------------
  Functions for computing moments
  --------------------------------------------------*/

void calculateInterpolatedVelocityMoments(
   dccrg::Dccrg<SpatialCell,dccrg::Cartesian_Geometry>& mpiGrid,
   const int cp_rhom,
   const int cp_vx,
   const int cp_vy,
   const int cp_vz,
   const int cp_rhoq,
   const int cp_p11,
   const int cp_p22,
   const int cp_p33
) {
   const vector<CellID>& cells = getLocalCells();

   //Iterate through all local cells
    #pragma omp parallel for
   for (size_t c=0; c<cells.size(); ++c) {
      const CellID cellID = cells[c];
      SpatialCell* SC = mpiGrid[cellID];
      SC->parameters[cp_rhom  ] = 0.5* ( SC->parameters[CellParams::RHOM_R] + SC->parameters[CellParams::RHOM_V] );
      SC->parameters[cp_vx] = 0.5* ( SC->parameters[CellParams::VX_R] + SC->parameters[CellParams::VX_V] );
      SC->parameters[cp_vy] = 0.5* ( SC->parameters[CellParams::VY_R] + SC->parameters[CellParams::VY_V] );
      SC->parameters[cp_vz] = 0.5* ( SC->parameters[CellParams::VZ_R] + SC->parameters[CellParams::VZ_V] );
      SC->parameters[cp_rhoq  ] = 0.5* ( SC->parameters[CellParams::RHOQ_R] + SC->parameters[CellParams::RHOQ_V] );
      SC->parameters[cp_p11]   = 0.5* ( SC->parameters[CellParams::P_11_R] + SC->parameters[CellParams::P_11_V] );
      SC->parameters[cp_p22]   = 0.5* ( SC->parameters[CellParams::P_22_R] + SC->parameters[CellParams::P_22_V] );
      SC->parameters[cp_p33]   = 0.5* ( SC->parameters[CellParams::P_33_R] + SC->parameters[CellParams::P_33_V] );

      for (uint popID=0; popID<getObjectWrapper().particleSpecies.size(); ++popID) {
         spatial_cell::Population& pop = SC->get_population(popID);
         pop.RHO = 0.5 * ( pop.RHO_R + pop.RHO_V );
         for(int i=0; i<3; i++) {
            pop.V[i] = 0.5 * ( pop.V_R[i] + pop.V_V[i] );
            pop.P[i]    = 0.5 * ( pop.P_R[i] + pop.P_V[i] );
         }
      }
   }
}

void calculateInitialVelocityMoments(dccrg::Dccrg<SpatialCell,dccrg::Cartesian_Geometry>& mpiGrid) {
   const vector<CellID>& cells = getLocalCells();
   phiprof::start("Calculate moments");

   // Iterate through all local cells (incl. system boundary cells):
   #pragma omp parallel
   {
      // Setting the GPU device inside the moment call itself, because
      // it's being called from so many different projects etc
      #pragma omp for
      for (size_t c=0; c<cells.size(); ++c) {
         const CellID cellID = cells[c];
         SpatialCell* SC = mpiGrid[cellID];
         calculateCellMoments(SC,true,false);
         // WARNING the following is sane as this function is only called by initializeGrid.
         // We need initialized _DT2 values for the dt=0 field propagation done in the beginning.
         // Later these will be set properly.
         SC->parameters[CellParams::RHOM_DT2] = SC->parameters[CellParams::RHOM];
         SC->parameters[CellParams::VX_DT2] = SC->parameters[CellParams::VX];
         SC->parameters[CellParams::VY_DT2] = SC->parameters[CellParams::VY];
         SC->parameters[CellParams::VZ_DT2] = SC->parameters[CellParams::VZ];
         SC->parameters[CellParams::RHOQ_DT2] = SC->parameters[CellParams::RHOQ];
         SC->parameters[CellParams::P_11_DT2] = SC->parameters[CellParams::P_11];
         SC->parameters[CellParams::P_22_DT2] = SC->parameters[CellParams::P_22];
         SC->parameters[CellParams::P_33_DT2] = SC->parameters[CellParams::P_33];
      } // for-loop over spatial cells
   }
   phiprof::stop("Calculate moments");
}
