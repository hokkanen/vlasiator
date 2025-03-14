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
#include <cmath>
#include <vector>
#include <sstream>
#include <ctime>

#ifdef _OPENMP
   #include <omp.h>
#endif

#ifdef USE_GPU
#include "arch/gpu_base.hpp"
#endif

#include <fsgrid.hpp>

#include "vlasovmover.h"
#include "vlasovsolver/vec.h"
#include "definitions.h"
#include "mpiconversion.h"
#include "logger.h"
#include "parameters.h"
#include "readparameters.h"
#include "spatial_cell.hpp"
#include "datareduction/datareducer.h"
#include "sysboundary/sysboundary.h"
#include "fieldtracing/fieldtracing.h"

#include "fieldsolver/fs_common.h"
#include "projects/project.h"
#include "grid.h"
#include "iowrite.h"
#include "ioread.h"

#include "object_wrapper.h"
#include "velocity_mesh_parameters.h"
#include "fieldsolver/gridGlue.hpp"
#include "fieldsolver/derivatives.hpp"

#ifdef CATCH_FPE
#include <fenv.h>
#include <signal.h>
/*! Function used to abort the program upon detecting a floating point exception. Which exceptions are caught is defined using the function feenableexcept.
 */
void fpehandler(int sig_num)
{
   signal(SIGFPE, fpehandler);
   printf("SIGFPE: floating point exception occured, exiting.\n");
   abort();
}
#endif

#include "phiprof.hpp"

Logger logFile, diagnostic;
static dccrg::Dccrg<SpatialCell,dccrg::Cartesian_Geometry> mpiGrid;

using namespace std;

int globalflags::bailingOut = 0;
bool globalflags::writeRestart = 0;
bool globalflags::balanceLoad = 0;
bool globalflags::ionosphereJustSolved = false;

ObjectWrapper objectWrapper;

void addTimedBarrier(string name){
#ifdef NDEBUG
//let's not do  a barrier
   return;
#endif
   int bt=phiprof::initializeTimer(name,"Barriers","MPI");
   phiprof::start(bt);
   MPI_Barrier(MPI_COMM_WORLD);
   phiprof::stop(bt);
}

void computeNewTimeStep(dccrg::Dccrg<SpatialCell,dccrg::Cartesian_Geometry>& mpiGrid,
			FsGrid< fsgrids::technical, FS_STENCIL_WIDTH> & technicalGrid, Real &newDt, bool &isChanged) {

   phiprof::start("compute-timestep");
   // Compute maximum time step. This cannot be done at the first step as the solvers compute the limits for each cell.

   isChanged = false;

   const vector<CellID>& cells = getLocalCells();
   /* Arrays for storing local (per process) and global max dt
      0th position stores ordinary space propagation dt
      1st position stores velocity space propagation dt
      2nd position stores field propagation dt
   */
   Real dtMaxLocal[3];
   Real dtMaxGlobal[3];

   dtMaxLocal[0] = numeric_limits<Real>::max();
   dtMaxLocal[1] = numeric_limits<Real>::max();
   dtMaxLocal[2] = numeric_limits<Real>::max();

   for (vector<CellID>::const_iterator cell_id = cells.begin(); cell_id != cells.end(); ++cell_id) {
      SpatialCell* cell = mpiGrid[*cell_id];
      const Real dx = cell->parameters[CellParams::DX];
      const Real dy = cell->parameters[CellParams::DY];
      const Real dz = cell->parameters[CellParams::DZ];

      cell->parameters[CellParams::MAXRDT] = numeric_limits<Real>::max();

      for (uint popID = 0; popID < getObjectWrapper().particleSpecies.size(); ++popID) {
         cell->set_max_r_dt(popID, numeric_limits<Real>::max());
         const Realf* block_data = cell->get_data(popID);
         const Real *parameters = cell->get_block_parameters(popID);
         const Real HALF = 0.5;
         Real popMin = std::numeric_limits<Real>::max();
#pragma omp parallel
         {
            Real threadMin = std::numeric_limits<Real>::max();
            arch::parallel_reduce<arch::min>({WID, WID, WID, (uint)cell->get_number_of_velocity_blocks(popID)},
                                             ARCH_LOOP_LAMBDA (const uint i, const uint j, const uint k, const uint n, Real *lthreadMin){
                                                const Real VX
                                                   =          parameters[n * BlockParams::N_VELOCITY_BLOCK_PARAMS + BlockParams::VXCRD]
                                                   + (i + HALF)*parameters[n * BlockParams::N_VELOCITY_BLOCK_PARAMS + BlockParams::DVX];
                                                const Real VY
                                                   =          parameters[n * BlockParams::N_VELOCITY_BLOCK_PARAMS + BlockParams::VYCRD]
                                                   + (j + HALF)*parameters[n * BlockParams::N_VELOCITY_BLOCK_PARAMS + BlockParams::DVY];
                                                const Real VZ
                                                   =          parameters[n * BlockParams::N_VELOCITY_BLOCK_PARAMS + BlockParams::VZCRD]
                                                   + (k + HALF)*parameters[n * BlockParams::N_VELOCITY_BLOCK_PARAMS + BlockParams::DVZ];
                                                lthreadMin[0] = min(dx / fabs(VX), lthreadMin[0]);
                                                lthreadMin[0] = min(dy / fabs(VY), lthreadMin[0]);
                                                lthreadMin[0] = min(dz / fabs(VZ), lthreadMin[0]);
                                             }, threadMin);
#pragma omp critical
            {
               popMin = min(threadMin, popMin);
            }
         }
         cell->set_max_r_dt(popID, popMin);
         cell->parameters[CellParams::MAXRDT] = min(popMin, cell->parameters[CellParams::MAXRDT]);
      }

      if (cell->sysBoundaryFlag == sysboundarytype::NOT_SYSBOUNDARY ||
          (cell->sysBoundaryLayer == 1 && cell->sysBoundaryFlag != sysboundarytype::NOT_SYSBOUNDARY)) {
         // spatial fluxes computed also for boundary cells
         dtMaxLocal[0] = min(dtMaxLocal[0], cell->parameters[CellParams::MAXRDT]);
      }

      if (cell->parameters[CellParams::MAXVDT] != 0 &&
          (cell->sysBoundaryFlag == sysboundarytype::NOT_SYSBOUNDARY ||
           (P::vlasovAccelerateMaxwellianBoundaries && cell->sysBoundaryFlag == sysboundarytype::SET_MAXWELLIAN))) {
         // acceleration only done on non-boundary cells
         dtMaxLocal[1] = min(dtMaxLocal[1], cell->parameters[CellParams::MAXVDT]);
      }
   }

   // compute max dt for fieldsolver
   const std::array<int, 3> gridDims(technicalGrid.getLocalSize());
   for (int k = 0; k < gridDims[2]; k++) {
      for (int j = 0; j < gridDims[1]; j++) {
         for (int i = 0; i < gridDims[0]; i++) {
            fsgrids::technical* cell = technicalGrid.get(i, j, k);
            if (cell->sysBoundaryFlag == sysboundarytype::NOT_SYSBOUNDARY ||
               (cell->sysBoundaryLayer == 1 && cell->sysBoundaryFlag != sysboundarytype::NOT_SYSBOUNDARY)) {
               dtMaxLocal[2] = min(dtMaxLocal[2], cell->maxFsDt);
            }
         }
      }
   }

   MPI_Allreduce(&(dtMaxLocal[0]), &(dtMaxGlobal[0]), 3, MPI_Type<Real>(), MPI_MIN, MPI_COMM_WORLD);

   // If any of the solvers are disabled there should be no limits in timespace from it
   if (!P::propagateVlasovTranslation)
      dtMaxGlobal[0] = numeric_limits<Real>::max();
   if (!P::propagateVlasovAcceleration)
      dtMaxGlobal[1] = numeric_limits<Real>::max();
   if (!P::propagateField)
      dtMaxGlobal[2] = numeric_limits<Real>::max();

   creal meanVlasovCFL = 0.5 * (P::vlasovSolverMaxCFL + P::vlasovSolverMinCFL);
   creal meanFieldsCFL = 0.5 * (P::fieldSolverMaxCFL + P::fieldSolverMinCFL);
   Real subcycleDt;

   // reduce/increase dt if it is too high for any of the three propagators or too low for all propagators
   if ((P::dt > dtMaxGlobal[0] * P::vlasovSolverMaxCFL ||
        P::dt > dtMaxGlobal[1] * P::vlasovSolverMaxCFL * P::maxSlAccelerationSubcycles ||
        P::dt > dtMaxGlobal[2] * P::fieldSolverMaxCFL * P::maxFieldSolverSubcycles) ||
       (P::dt < dtMaxGlobal[0] * P::vlasovSolverMinCFL &&
        P::dt < dtMaxGlobal[1] * P::vlasovSolverMinCFL * P::maxSlAccelerationSubcycles &&
        P::dt < dtMaxGlobal[2] * P::fieldSolverMinCFL * P::maxFieldSolverSubcycles)) {

      // new dt computed
      isChanged = true;

      // set new timestep to the lowest one of all interval-midpoints
      newDt = meanVlasovCFL * dtMaxGlobal[0];
      newDt = min(newDt, meanVlasovCFL * dtMaxGlobal[1] * P::maxSlAccelerationSubcycles);
      newDt = min(newDt, meanFieldsCFL * dtMaxGlobal[2] * P::maxFieldSolverSubcycles);

      logFile << "(TIMESTEP) New dt = " << newDt << " computed on step " << P::tstep << " at " << P::t
              << "s   Maximum possible dt (not including  vlasovsolver CFL " << P::vlasovSolverMinCFL << "-"
              << P::vlasovSolverMaxCFL << " or fieldsolver CFL " << P::fieldSolverMinCFL << "-" << P::fieldSolverMaxCFL
              << ") in {r, v, BE} was " << dtMaxGlobal[0] << " " << dtMaxGlobal[1] << " " << dtMaxGlobal[2] << " "
              << " Including subcycling { v, BE}  was " << dtMaxGlobal[1] * P::maxSlAccelerationSubcycles << " "
              << dtMaxGlobal[2] * P::maxFieldSolverSubcycles << " " << endl
              << writeVerbose;

      if (P::dynamicTimestep) {
         subcycleDt = newDt;
      } else {
         logFile << "(TIMESTEP) However, fixed timestep in config overrides dt = " << P::dt << endl << writeVerbose;
         subcycleDt = P::dt;
      }
   } else {
      subcycleDt = P::dt;
   }

   // Subcycle if field solver dt < global dt (including CFL) (new or old dt hence the hassle with subcycleDt
   if (meanFieldsCFL * dtMaxGlobal[2] < subcycleDt && P::propagateField) {
      P::fieldSolverSubcycles =
          min(convert<uint>(ceil(subcycleDt / (meanFieldsCFL * dtMaxGlobal[2]))), P::maxFieldSolverSubcycles);
   } else {
      P::fieldSolverSubcycles = 1;
   }

   phiprof::stop("compute-timestep");
}

ObjectWrapper& getObjectWrapper() {
   return objectWrapper;
}

/** Get local cell IDs. This function creates a cached copy of the
 * cell ID lists to significantly improve performance. The cell ID
 * cache is recalculated every time the mesh partitioning changes.
 * @return Local cell IDs.*/
const std::vector<CellID>& getLocalCells() {
   return Parameters::localCells;
}

void recalculateLocalCellsCache() {
     {
        vector<CellID> dummy;
        dummy.swap(Parameters::localCells);
     }
   Parameters::localCells = mpiGrid.get_cells();
}

int main(int argn,char* args[]) {
   int myRank, doBailout=0;
   const creal DT_EPSILON=1e-12;
   typedef Parameters P;
   Real newDt;
   bool dtIsChanged;

// Init MPI:
   int required=MPI_THREAD_FUNNELED;
   int provided;
   MPI_Init_thread(&argn,&args,required,&provided);
   if (required > provided){
      MPI_Comm_rank(MPI_COMM_WORLD,&myRank);
      if(myRank==MASTER_RANK)
         cerr << "(MAIN): MPI_Init_thread failed! Got " << provided << ", need "<<required <<endl;
      exit(1);
   }

   phiprof::initialize();

   double initialWtime =  MPI_Wtime();
   SysBoundary& sysBoundaryContainer = getObjectWrapper().sysBoundaryContainer;
   MPI_Comm comm = MPI_COMM_WORLD;
   MPI_Comm_rank(comm,&myRank);

   #ifdef CATCH_FPE
   // WARNING FE_INEXACT is too sensitive to be used. See man fenv.
   //feenableexcept(FE_DIVBYZERO|FE_INVALID|FE_OVERFLOW|FE_UNDERFLOW);
   feenableexcept(FE_DIVBYZERO|FE_INVALID|FE_OVERFLOW);
   //feenableexcept(FE_DIVBYZERO|FE_INVALID);
   signal(SIGFPE, fpehandler);
   #endif

   phiprof::start("main");
   phiprof::start("Initialization");

   #ifdef USE_GPU
   // Activate device, create streams
   gpu_init_device();
   #endif

   phiprof::start("Read parameters");
   // Allocate velocity mesh wrapper
   vmesh::allocMeshWrapper();
   //init parameter file reader
   Readparameters readparameters(argn,args);

   P::addParameters();

   // Add parameters for number of populations
   getObjectWrapper().addParameters();
   readparameters.parse();
   P::getParameters();

   getObjectWrapper().addPopulationParameters();
   sysBoundaryContainer.addParameters();
   projects::Project::addParameters();

   Project* project = projects::createProject();
   getObjectWrapper().project = project;
   readparameters.parse(true, false); // 2nd parsing for specific population parameters
   readparameters.helpMessage(); // Call after last parse, exits after printing help if help requested
   getObjectWrapper().getPopulationParameters();
   sysBoundaryContainer.getParameters();
   project->getParameters();

   vmesh::getMeshWrapper()->initVelocityMeshes(getObjectWrapper().particleSpecies.size());
   phiprof::stop("Read parameters");

   // Check for correct application of vectorclass values:
   if ( (VECL<WID) ||
        (VECL*VEC_PER_PLANE != WID2) ||
        (VECL*VEC_PER_BLOCK != WID3) ||
        //(VPREC > VECL) ||
        (VECL != (int)VECL) ||
        (VPREC != (int)VPREC) ||
        (VEC_PER_PLANE != (int)VEC_PER_PLANE) ||
        (VEC_PER_BLOCK != (int)VEC_PER_BLOCK) ) {
      if (myRank == MASTER_RANK) {
         cerr << "(MAIN) ERROR: Vectorclass definition mismatch!" << endl;
         cerr << "VECL " << VECL <<" VEC_PER_PLANE " << VEC_PER_PLANE <<" WID " << WID <<" VEC_PER_BLOCK " << VEC_PER_BLOCK << " VPREC "<< VPREC<<endl;
      }
      exit(1);
   }

   //Get version and config info here
   std::string version;
   std::string config;
   //Only master needs the info
   if (myRank==MASTER_RANK){
      version=readparameters.versionInfo();
      config=readparameters.configInfo();
   }

   // Init parallel logger:
   phiprof::start("open logFile & diagnostic");
   //if restarting we will append to logfiles
   if (logFile.open(MPI_COMM_WORLD,MASTER_RANK,"logfile.txt",P::isRestart) == false) {
      if(myRank == MASTER_RANK) cerr << "(MAIN) ERROR: Logger failed to open logfile!" << endl;
      exit(1);
   }
   if (P::diagnosticInterval != 0) {
      if (diagnostic.open(MPI_COMM_WORLD,MASTER_RANK,"diagnostic.txt",P::isRestart) == false) {
         if(myRank == MASTER_RANK) cerr << "(MAIN) ERROR: Logger failed to open diagnostic file!" << endl;
         exit(1);
      }
   }
   {
      int mpiProcs;
      MPI_Comm_size(MPI_COMM_WORLD,&mpiProcs);
      logFile << "(MAIN) Starting simulation with " << mpiProcs << " MPI processes ";
      #ifdef _OPENMP
         logFile << "and " << omp_get_max_threads();
      #else
         logFile << "and 0";
      #endif
      logFile << " OpenMP threads per process" << endl << writeVerbose;
   }
   phiprof::stop("open logFile & diagnostic");

   // Init project
   phiprof::start("Init project");
   if (project->initialize() == false) {
      if(myRank == MASTER_RANK) cerr << "(MAIN): Project did not initialize correctly!" << endl;
      exit(1);
   }
   if (project->initialized() == false) {
      if (myRank == MASTER_RANK) {
         cerr << "(MAIN): Project base class was not initialized!" << endl;
         cerr << "\t Call Project::initialize() in your project's initialize()-function." << endl;
         exit(1);
      }
   }
   phiprof::stop("Init project");

   // Add VAMR refinement criterias:
#ifdef VAMR
   vamr_ref_criteria::addRefinementCriteria();
#endif

   // Initialize simplified Fieldsolver grids.
   // Needs to be done here already ad the background field will be set right away, before going to initializeGrid even
   phiprof::start("Init fieldsolver grids");

   const std::array<int,3> fsGridDimensions = {convert<int>(P::xcells_ini * pow(2,P::amrMaxSpatialRefLevel)),
							    convert<int>(P::ycells_ini * pow(2,P::amrMaxSpatialRefLevel)),
							    convert<int>(P::zcells_ini * pow(2,P::amrMaxSpatialRefLevel))};

   std::array<bool,3> periodicity{sysBoundaryContainer.isBoundaryPeriodic(0),
                                  sysBoundaryContainer.isBoundaryPeriodic(1),
                                  sysBoundaryContainer.isBoundaryPeriodic(2)};

   FsGridCouplingInformation gridCoupling;
   FsGrid< std::array<Real, fsgrids::bfield::N_BFIELD>, FS_STENCIL_WIDTH> perBGrid(fsGridDimensions, comm, periodicity,gridCoupling);
   FsGrid< std::array<Real, fsgrids::bfield::N_BFIELD>, FS_STENCIL_WIDTH> perBDt2Grid(fsGridDimensions, comm, periodicity,gridCoupling);
   FsGrid< std::array<Real, fsgrids::efield::N_EFIELD>, FS_STENCIL_WIDTH> EGrid(fsGridDimensions, comm, periodicity,gridCoupling);
   FsGrid< std::array<Real, fsgrids::efield::N_EFIELD>, FS_STENCIL_WIDTH> EDt2Grid(fsGridDimensions, comm, periodicity,gridCoupling);
   FsGrid< std::array<Real, fsgrids::ehall::N_EHALL>, FS_STENCIL_WIDTH> EHallGrid(fsGridDimensions, comm, periodicity,gridCoupling);
   FsGrid< std::array<Real, fsgrids::egradpe::N_EGRADPE>, FS_STENCIL_WIDTH> EGradPeGrid(fsGridDimensions, comm, periodicity,gridCoupling);
   FsGrid< std::array<Real, fsgrids::moments::N_MOMENTS>, FS_STENCIL_WIDTH> momentsGrid(fsGridDimensions, comm, periodicity,gridCoupling);
   FsGrid< std::array<Real, fsgrids::moments::N_MOMENTS>, FS_STENCIL_WIDTH> momentsDt2Grid(fsGridDimensions, comm, periodicity,gridCoupling);
   FsGrid< std::array<Real, fsgrids::dperb::N_DPERB>, FS_STENCIL_WIDTH> dPerBGrid(fsGridDimensions, comm, periodicity,gridCoupling);
   FsGrid< std::array<Real, fsgrids::dmoments::N_DMOMENTS>, FS_STENCIL_WIDTH> dMomentsGrid(fsGridDimensions, comm, periodicity,gridCoupling);
   FsGrid< std::array<Real, fsgrids::bgbfield::N_BGB>, FS_STENCIL_WIDTH> BgBGrid(fsGridDimensions, comm, periodicity,gridCoupling);
   FsGrid< std::array<Real, fsgrids::volfields::N_VOL>, FS_STENCIL_WIDTH> volGrid(fsGridDimensions, comm, periodicity,gridCoupling);
   FsGrid< fsgrids::technical, FS_STENCIL_WIDTH> technicalGrid(fsGridDimensions, comm, periodicity,gridCoupling);

   // Set DX, DY and DZ
   // TODO: This is currently just taking the values from cell 1, and assuming them to be
   // constant throughout the simulation.
   perBGrid.DX = perBDt2Grid.DX = EGrid.DX = EDt2Grid.DX = EHallGrid.DX = EGradPeGrid.DX = momentsGrid.DX
      = momentsDt2Grid.DX = dPerBGrid.DX = dMomentsGrid.DX = BgBGrid.DX = volGrid.DX = technicalGrid.DX
      = P::dx_ini / pow(2, P::amrMaxSpatialRefLevel);
   perBGrid.DY = perBDt2Grid.DY = EGrid.DY = EDt2Grid.DY = EHallGrid.DY = EGradPeGrid.DY = momentsGrid.DY
      = momentsDt2Grid.DY = dPerBGrid.DY = dMomentsGrid.DY = BgBGrid.DY = volGrid.DY = technicalGrid.DY
      = P::dy_ini / pow(2, P::amrMaxSpatialRefLevel);
   perBGrid.DZ = perBDt2Grid.DZ = EGrid.DZ = EDt2Grid.DZ = EHallGrid.DZ = EGradPeGrid.DZ = momentsGrid.DZ
      = momentsDt2Grid.DZ = dPerBGrid.DZ = dMomentsGrid.DZ = BgBGrid.DZ = volGrid.DZ = technicalGrid.DZ
      = P::dz_ini / pow(2, P::amrMaxSpatialRefLevel);
   // Set the physical start (lower left corner) X, Y, Z
   perBGrid.physicalGlobalStart = perBDt2Grid.physicalGlobalStart = EGrid.physicalGlobalStart = EDt2Grid.physicalGlobalStart
      = EHallGrid.physicalGlobalStart = EGradPeGrid.physicalGlobalStart = momentsGrid.physicalGlobalStart
      = momentsDt2Grid.physicalGlobalStart = dPerBGrid.physicalGlobalStart = dMomentsGrid.physicalGlobalStart
      = BgBGrid.physicalGlobalStart = volGrid.physicalGlobalStart = technicalGrid.physicalGlobalStart
      = {P::xmin, P::ymin, P::zmin};

   // Checking that spatial cells are cubic, otherwise field solver is incorrect (cf. derivatives in E, Hall term)
   if ((abs((technicalGrid.DX - technicalGrid.DY) / technicalGrid.DX) > 0.001) ||
       (abs((technicalGrid.DX - technicalGrid.DZ) / technicalGrid.DX) > 0.001) ||
       (abs((technicalGrid.DY - technicalGrid.DZ) / technicalGrid.DY) > 0.001)) {
      if (myRank == MASTER_RANK) {
         std::cerr << "WARNING: Your spatial cells seem not to be cubic. However the field solver is assuming them to "
                      "be. Use at your own risk and responsibility!"
                   << std::endl;
      }
   }
   phiprof::stop("Init fieldsolver grids");

   // Initialize grid.  After initializeGrid local cells have dist
   // functions, and B fields set. Cells have also been classified for
   // the various sys boundary conditions.  All remote cells have been
   // created. All spatial date computed this far is up to date for
   // FULL_NEIGHBORHOOD. Block lists up to date for
   // VLASOV_SOLVER_NEIGHBORHOOD (but dist function has not been communicated)
   phiprof::start("Init grids");
   initializeGrids(
      argn,
      args,
      mpiGrid,
      perBGrid,
      BgBGrid,
      momentsGrid,
      momentsDt2Grid,
      EGrid,
      EGradPeGrid,
      volGrid,
      technicalGrid,
      sysBoundaryContainer,
      *project
   );

   const std::vector<CellID>& cells = getLocalCells();

   phiprof::stop("Init grids");

   // Initialize data reduction operators. This should be done elsewhere in order to initialize
   // user-defined operators:
   phiprof::start("Init DROs");
   DataReducer outputReducer, diagnosticReducer;
   initializeDataReducers(&outputReducer, &diagnosticReducer);
   phiprof::stop("Init DROs");

   // Free up memory:
   readparameters.~Readparameters();

   // Run the field solver once with zero dt. This will initialize
   // Fieldsolver dt limits, and also calculate volumetric B-fields.
   // At restart, all we need at this stage has been read from the restart, the rest will be recomputed in due time.
   if(P::isRestart == false) {
      propagateFields(
         perBGrid,
         perBDt2Grid,
         EGrid,
         EDt2Grid,
         EHallGrid,
         EGradPeGrid,
         momentsGrid,
         momentsDt2Grid,
         dPerBGrid,
         dMomentsGrid,
         BgBGrid,
         volGrid,
         technicalGrid,
         sysBoundaryContainer, 0.0, 1.0
      );
   }

   phiprof::start("getFieldsFromFsGrid");
   volGrid.updateGhostCells();
   getFieldsFromFsGrid(volGrid, BgBGrid, EGradPeGrid, technicalGrid, mpiGrid, cells);
   phiprof::stop("getFieldsFromFsGrid");

   // Build communicator for ionosphere solving
   SBC::ionosphereGrid.updateIonosphereCommunicator(mpiGrid, technicalGrid);
   FieldTracing::calculateIonosphereFsgridCoupling(technicalGrid, perBGrid, dPerBGrid, SBC::ionosphereGrid.nodes, SBC::Ionosphere::radius);
   SBC::ionosphereGrid.initSolver(!P::isRestart); // If it is a restart we do not want to zero out everything
   if(SBC::Ionosphere::couplingInterval > 0 && P::isRestart) {
      SBC::Ionosphere::solveCount = floor(P::t / SBC::Ionosphere::couplingInterval)+1;
   } else {
      SBC::Ionosphere::solveCount = 1;
   }

   if (P::isRestart == false) {
      phiprof::start("compute-dt");
      // Run Vlasov solver once with zero dt to initialize
      // per-cell dt limits. In restarts, we read the dt from file.
      calculateSpatialTranslation(mpiGrid,0.0);
      calculateAcceleration(mpiGrid,0.0);
      phiprof::stop("compute-dt");
   }

   // Save restart data
   if (P::writeInitialState) {
      FieldTracing::reduceData(technicalGrid, perBGrid, dPerBGrid, mpiGrid, SBC::ionosphereGrid.nodes); /*!< Call the reductions (e.g. field tracing) */

      phiprof::start("write-initial-state");

      if (myRank == MASTER_RANK)
         logFile << "(IO): Writing initial state to disk, tstep = "  << endl << writeVerbose;
      P::systemWriteDistributionWriteStride.push_back(1);
      P::systemWriteName.push_back("initial-grid");
      P::systemWriteDistributionWriteXlineStride.push_back(0);
      P::systemWriteDistributionWriteYlineStride.push_back(0);
      P::systemWriteDistributionWriteZlineStride.push_back(0);
      P::systemWritePath.push_back("./");
      P::systemWriteFsGrid.push_back(true);

      for(uint si=0; si<P::systemWriteName.size(); si++) {
         P::systemWrites.push_back(0);
      }

      const bool writeGhosts = true;
      if( writeGrid(mpiGrid,
            perBGrid, // TODO: Merge all the fsgrids passed here into one meta-object
            EGrid,
            EHallGrid,
            EGradPeGrid,
            momentsGrid,
            dPerBGrid,
            dMomentsGrid,
            BgBGrid,
            volGrid,
            technicalGrid,
            version,
            config,
            &outputReducer,
            P::systemWriteName.size()-1,
            P::restartStripeFactor,
            writeGhosts
         ) == false
      ) {
         cerr << "FAILED TO WRITE GRID AT " << __FILE__ << " " << __LINE__ << endl;
      }

      P::systemWriteDistributionWriteStride.pop_back();
      P::systemWriteName.pop_back();
      P::systemWriteDistributionWriteXlineStride.pop_back();
      P::systemWriteDistributionWriteYlineStride.pop_back();
      P::systemWriteDistributionWriteZlineStride.pop_back();
      P::systemWritePath.pop_back();
      P::systemWriteFsGrid.pop_back();

      phiprof::stop("write-initial-state");
   }

   if (P::isRestart == false) {
      //compute new dt
      phiprof::start("compute-dt");
      computeNewTimeStep(mpiGrid, technicalGrid, newDt, dtIsChanged);
      if (P::dynamicTimestep == true && dtIsChanged == true) {
         // Only actually update the timestep if dynamicTimestep is on
         P::dt=newDt;
      }
      phiprof::stop("compute-dt");

      //go forward by dt/2 in V, initializes leapfrog split. In restarts the
      //the distribution function is already propagated forward in time by dt/2
      phiprof::start("propagate-velocity-space-dt/2");
      if (P::propagateVlasovAcceleration) {
         calculateAcceleration(mpiGrid, 0.5*P::dt);
      } else {
         //zero step to set up moments _v
         calculateAcceleration(mpiGrid, 0.0);
      }
      phiprof::stop("propagate-velocity-space-dt/2");

      // Apply boundary conditions
      if (P::propagateVlasovTranslation || P::propagateVlasovAcceleration ) {
         phiprof::start("Update system boundaries (Vlasov post-acceleration)");
         sysBoundaryContainer.applySysBoundaryVlasovConditions(mpiGrid, 0.5*P::dt, true);
         phiprof::stop("Update system boundaries (Vlasov post-acceleration)");
         addTimedBarrier("barrier-boundary-conditions");
      }
      // Also update all moments. They won't be transmitted to FSgrid until the field solver is called, though.
      phiprof::start("Compute interp moments");
      calculateInterpolatedVelocityMoments(
         mpiGrid,
         CellParams::RHOM,
         CellParams::VX,
         CellParams::VY,
         CellParams::VZ,
         CellParams::RHOQ,
         CellParams::P_11,
         CellParams::P_22,
         CellParams::P_33
         );
      phiprof::stop("Compute interp moments");
   }

   phiprof::stop("Initialization");

   // ***********************************
   // ***** INITIALIZATION COMPLETE *****
   // ***********************************

   // Main simulation loop:
   if (myRank == MASTER_RANK){
      logFile << "(MAIN): Starting main simulation loop." << endl << writeVerbose;
      //report filtering if we are in an AMR run
      if (P::amrMaxSpatialRefLevel>0){
         logFile<<"Filtering Report: "<<endl;
         for (uint refLevel=0 ; refLevel<= P::amrMaxSpatialRefLevel; refLevel++){
            logFile<<"\tRefinement Level " <<refLevel<<"==> Passes "<<P::numPasses.at(refLevel)<<endl;
         }
            logFile<<endl;
      }
   }

   phiprof::start("report-memory-consumption");
   report_process_memory_consumption();
   phiprof::stop("report-memory-consumption");

   unsigned int computedCells=0;
   unsigned int computedTotalCells=0;
  //Compute here based on time what the file intervals are
   P::systemWrites.clear();
   for(uint i=0;i< P::systemWriteTimeInterval.size();i++){
      int index=(int)(P::t_min/P::systemWriteTimeInterval[i]);
      //if we are already over 1% further than the time interval time that
      //is requested for writing, then jump to next writing index. This is to
      //make sure that at restart we do not write in the middle of
      //the interval.
      if(P::t_min>(index+0.01)*P::systemWriteTimeInterval[i]) {
         index++;
         // Special case for large timesteps
         int index2=(int)((P::t_min+P::dt)/P::systemWriteTimeInterval[i]);
         if (index2>index) index=index2;
      }
      P::systemWrites.push_back(index);
   }

   // Invalidate cached cell lists just to be sure (might not be needed)
   P::meshRepartitioned = true;

   unsigned int wallTimeRestartCounter=1;

   int doNow[2] = {0}; // 0: writeRestartNow, 1: balanceLoadNow ; declared outside main loop
   int writeRestartNow; // declared outside main loop
   bool overrideRebalanceNow = false; // declared outside main loop

   addTimedBarrier("barrier-end-initialization");

   phiprof::start("Simulation");
   double startTime=  MPI_Wtime();
   double beforeTime = MPI_Wtime();
   double beforeSimulationTime=P::t_min;
   double beforeStep=P::tstep_min;

   while(P::tstep <= P::tstep_max  &&
         P::t-P::dt <= P::t_max+DT_EPSILON &&
         wallTimeRestartCounter <= P::exitAfterRestarts) {

      addTimedBarrier("barrier-loop-start");

      phiprof::start("IO");

      phiprof::start("checkExternalCommands");
      if(myRank ==  MASTER_RANK) {
         // check whether STOP or KILL or SAVE has been passed, should be done by MASTER_RANK only as it can reset P::bailout_write_restart
         checkExternalCommands();
      }
      phiprof::stop("checkExternalCommands");

      //write out phiprof profiles and logs with a lower interval than normal
      //diagnostic (every 10 diagnostic intervals).
      phiprof::start("logfile-io");
      logFile << "---------- tstep = " << P::tstep << " t = " << P::t <<" dt = " << P::dt << " FS cycles = " << P::fieldSolverSubcycles << " ----------" << endl;
      if (P::diagnosticInterval != 0 &&
          P::tstep % (P::diagnosticInterval*10) == 0 &&
          P::tstep-P::tstep_min >0) {

         phiprof::print(MPI_COMM_WORLD,"phiprof");

         double currentTime=MPI_Wtime();
         double timePerStep=double(currentTime  - beforeTime) / (P::tstep-beforeStep);
         double timePerSecond=double(currentTime  - beforeTime) / (P::t-beforeSimulationTime + DT_EPSILON);
         double remainingTime=min(timePerStep*(P::tstep_max-P::tstep),timePerSecond*(P::t_max-P::t));
         time_t finalWallTime=time(NULL)+(time_t)remainingTime; //assume time_t is in seconds, as it is almost always
         struct tm *finalWallTimeInfo=localtime(&finalWallTime);
         logFile << "(TIME) current walltime/step " << timePerStep<< " s" <<endl;
         logFile << "(TIME) current walltime/simusecond " << timePerSecond<<" s" <<endl;
         logFile << "(TIME) Estimated completion time is " <<asctime(finalWallTimeInfo)<<endl;
         //reset before values, we want to report speed since last report of speed.
         beforeTime = MPI_Wtime();
         beforeSimulationTime=P::t;
         beforeStep=P::tstep;
         //report_grid_memory_consumption(mpiGrid);
         report_process_memory_consumption();
      }
      logFile << writeVerbose;
      phiprof::stop("logfile-io");

// Check whether diagnostic output has to be produced
      if (P::diagnosticInterval != 0 && P::tstep % P::diagnosticInterval == 0) {

         phiprof::start("diagnostic-io");
         if (writeDiagnostic(mpiGrid, diagnosticReducer) == false) {
            if(myRank == MASTER_RANK)  cerr << "ERROR with diagnostic computation" << endl;

         }
         phiprof::stop("diagnostic-io");
      }

      // write system, loop through write classes
      for (uint i = 0; i < P::systemWriteTimeInterval.size(); i++) {
         if (P::systemWriteTimeInterval[i] >= 0.0 &&
             P::t >= P::systemWrites[i] * P::systemWriteTimeInterval[i] - DT_EPSILON) {
            // If we have only just restarted, the bulk file should already exist from the previous slot.
            if ((P::tstep == P::tstep_min) && (P::tstep>0)) {
               P::systemWrites[i]++;
               // Special case for large timesteps
               int index2=(int)((P::t+P::dt)/P::systemWriteTimeInterval[i]);
               if (index2>P::systemWrites[i]) P::systemWrites[i]=index2;
               continue;
            }

            // Calculate these so refinement parameters can be tuned based on the vlsv
            calculateScaledDeltasSimple(mpiGrid);

            FieldTracing::reduceData(technicalGrid, perBGrid, dPerBGrid, mpiGrid, SBC::ionosphereGrid.nodes); /*!< Call the reductions (e.g. field tracing) */

            phiprof::start("write-system");
            logFile << "(IO): Writing spatial cell and reduced system data to disk, tstep = " << P::tstep << " t = " << P::t << endl << writeVerbose;
            const bool writeGhosts = true;
            if(writeGrid(mpiGrid,
               perBGrid, // TODO: Merge all the fsgrids passed here into one meta-object
               EGrid,
               EHallGrid,
               EGradPeGrid,
               momentsGrid,
               dPerBGrid,
               dMomentsGrid,
               BgBGrid,
               volGrid,
               technicalGrid,
               version,
               config,
               &outputReducer,
               i,
               P::systemStripeFactor,
               writeGhosts
               ) == false
            ) {
               cerr << "FAILED TO WRITE GRID AT" << __FILE__ << " " << __LINE__ << endl;
            }
            P::systemWrites[i]++;
            // Special case for large timesteps
            int index2=(int)((P::t+P::dt)/P::systemWriteTimeInterval[i]);
            if (index2>P::systemWrites[i]) P::systemWrites[i]=index2;
            logFile << "(IO): .... done!" << endl << writeVerbose;
            phiprof::stop("write-system");
         }
      }

      // Reduce globalflags::bailingOut from all processes
      phiprof::start("Bailout-allreduce");
      MPI_Allreduce(&(globalflags::bailingOut), &(doBailout), 1, MPI_INT, MPI_SUM, MPI_COMM_WORLD);
      phiprof::stop("Bailout-allreduce");

      // Write restart data if needed
      // Combined with checking of additional load balancing to have only one collective call.
      phiprof::start("compute-is-restart-written-and-extra-LB");
      if (myRank == MASTER_RANK) {
         if (  (P::saveRestartWalltimeInterval >= 0.0
            && (P::saveRestartWalltimeInterval*wallTimeRestartCounter <=  MPI_Wtime()-initialWtime
               || P::tstep == P::tstep_max
               || P::t >= P::t_max))
            || (doBailout > 0 && P::bailout_write_restart)
            || globalflags::writeRestart
         ) {
            doNow[0] = 1;
            if (globalflags::writeRestart == true) {
               doNow[0] = 2; // Setting to 2 so as to not increment the restart count below.
               globalflags::writeRestart = false; // This flag is only used by MASTER_RANK here and it needs to be reset after a restart write has been issued.
            }
         }
         else {
            doNow[0] = 0;
         }
         if (globalflags::balanceLoad == true) {
            doNow[1] = 1;
            globalflags::balanceLoad = false;
         }
      }
      MPI_Bcast( &doNow, 2 , MPI_INT , MASTER_RANK ,MPI_COMM_WORLD);
      writeRestartNow = doNow[0];
      doNow[0] = 0;
      if (doNow[1] == 1) {
         P::prepareForRebalance = true;
         doNow[1] = 0;
      }
      phiprof::stop("compute-is-restart-written-and-extra-LB");

      if (writeRestartNow >= 1){
         phiprof::start("write-restart");
         if (writeRestartNow == 1) {
            wallTimeRestartCounter++;
         }

         if (myRank == MASTER_RANK)
            logFile << "(IO): Writing restart data to disk, tstep = " << P::tstep << " t = " << P::t << endl << writeVerbose;
         //Write the restart:
         if( writeRestart(mpiGrid,
                  perBGrid, // TODO: Merge all the fsgrids passed here into one meta-object
                  EGrid,
                  EHallGrid,
                  EGradPeGrid,
                  momentsGrid,
                  dPerBGrid,
                  dMomentsGrid,
                  BgBGrid,
                  volGrid,
                  technicalGrid,
                  version,
                  config,
                  outputReducer,"restart",(uint)P::t,P::restartStripeFactor) == false ) {
            logFile << "(IO): ERROR Failed to write restart!" << endl << writeVerbose;
            cerr << "FAILED TO WRITE RESTART" << endl;
         }
         if (myRank == MASTER_RANK)
            logFile << "(IO): .... done!"<< endl << writeVerbose;
         phiprof::stop("write-restart");
      }

      phiprof::stop("IO");
      addTimedBarrier("barrier-end-io");

      //no need to propagate if we are on the final step, we just
      //wanted to make sure all IO is done even for final step
      if(P::tstep == P::tstep_max ||
         P::t >= P::t_max ||
         doBailout > 0) {
         break;
      }

      //Re-loadbalance if needed
      //TODO - add LB measure and do LB if it exceeds threshold
      if(((P::tstep % P::rebalanceInterval == 0 && P::tstep > P::tstep_min) || overrideRebalanceNow)) {
         logFile << "(LB): Start load balance, tstep = " << P::tstep << " t = " << P::t << endl << writeVerbose;
         // Refinement includes LB
         if (!dtIsChanged && P::adaptRefinement && P::tstep % (P::rebalanceInterval * P::refineMultiplier) == 0 && P::t > P::refineAfter) {
            logFile << "(AMR): Adapting refinement!"  << endl << writeVerbose;
            if (!adaptRefinement(mpiGrid, technicalGrid, sysBoundaryContainer, *project))
               continue;   // Refinement failed and we're bailing out

            // Calculate new dt limits since we might break CFL when refining
            phiprof::start("compute-dt");
            calculateSpatialTranslation(mpiGrid,0.0);
            calculateAcceleration(mpiGrid,0.0);
            phiprof::stop("compute-dt");
         }
         balanceLoad(mpiGrid, sysBoundaryContainer);
         addTimedBarrier("barrier-end-load-balance");
         phiprof::start("Shrink_to_fit");
         // * shrink to fit after LB * //
         shrink_to_fit_grid_data(mpiGrid);
         phiprof::stop("Shrink_to_fit");
         logFile << "(LB): ... done!"  << endl << writeVerbose;
         P::prepareForRebalance = false;

         overrideRebalanceNow = false;

         // Make sure the ionosphere communicator is up-to-date, in case inner boundary cells
         // moved.
         SBC::ionosphereGrid.updateIonosphereCommunicator(mpiGrid, technicalGrid);
      }

      //get local cells
      const vector<CellID>& cells = getLocalCells();

      //compute how many spatial cells we solve for this step
      computedCells=0;
      for(size_t i=0; i<cells.size(); i++) {
         for (uint popID=0; popID<getObjectWrapper().particleSpecies.size(); ++popID)
            computedCells += mpiGrid[cells[i]]->get_number_of_velocity_blocks(popID)*WID3;
      }
      computedTotalCells+=computedCells;

      //Check if dt needs to be changed, and propagate V back a half-step to change dt and set up new situation
      //do not compute new dt on first step (in restarts dt comes from file, otherwise it was initialized before we entered
      //simulation loop
      // FIXME what if dt changes at a restart??
      if(P::dynamicTimestep  && P::tstep > P::tstep_min) {
         computeNewTimeStep(mpiGrid, technicalGrid, newDt, dtIsChanged);
         addTimedBarrier("barrier-check-dt");
         if(dtIsChanged) {
            phiprof::start("update-dt");
            //propagate velocity space back to real-time
            if( P::propagateVlasovAcceleration ) {
               // Back half dt to real time, forward by new half dt
               calculateAcceleration(mpiGrid,-0.5*P::dt + 0.5*newDt);
            }
            else {
               //zero step to set up moments _v
               calculateAcceleration(mpiGrid, 0.0);
            }

            P::dt=newDt;

            logFile <<" dt changed to "<<P::dt <<"s, distribution function was half-stepped to real-time and back"<<endl<<writeVerbose;
            phiprof::stop("update-dt");
            continue; //
            //addTimedBarrier("barrier-new-dt-set");
         }
      }

      if (P::tstep % P::rebalanceInterval == P::rebalanceInterval-1 || P::prepareForRebalance == true) {
         if(P::prepareForRebalance == true) {
            overrideRebalanceNow = true;
         } else {
            P::prepareForRebalance = true;
         }
         #pragma omp parallel for
         for (size_t c=0; c<cells.size(); ++c) {
            mpiGrid[cells[c]]->get_cell_parameters()[CellParams::LBWEIGHTCOUNTER] = 0;
         }
      }

      phiprof::start("Propagate");
      //Propagate the state of simulation forward in time by dt:

      phiprof::start("Spatial-space");
      if( P::propagateVlasovTranslation) {
         calculateSpatialTranslation(mpiGrid,P::dt);
      } else {
         calculateSpatialTranslation(mpiGrid,0.0);
      }
      phiprof::stop("Spatial-space",computedCells,"Cells");

      // Apply boundary conditions
      if (P::propagateVlasovTranslation || P::propagateVlasovAcceleration ) {
         phiprof::start("Update system boundaries (Vlasov post-translation)");
         sysBoundaryContainer.applySysBoundaryVlasovConditions(mpiGrid, P::t+0.5*P::dt, false);
         phiprof::stop("Update system boundaries (Vlasov post-translation)");
         addTimedBarrier("barrier-boundary-conditions");
      }

      phiprof::start("Compute interp moments");
      calculateInterpolatedVelocityMoments(
         mpiGrid,
         CellParams::RHOM_DT2,
         CellParams::VX_DT2,
         CellParams::VY_DT2,
         CellParams::VZ_DT2,
         CellParams::RHOQ_DT2,
         CellParams::P_11_DT2,
         CellParams::P_22_DT2,
         CellParams::P_33_DT2
      );
      phiprof::stop("Compute interp moments");

      // Propagate fields forward in time by dt. This needs to be done before the
      // moments for t + dt are computed (field uses t and t+0.5dt)
      if (P::propagateField) {
         phiprof::start("Propagate Fields");

         phiprof::start("fsgrid-coupling-in");
         // Copy moments over into the fsgrid.
         //setupTechnicalFsGrid(mpiGrid, cells, technicalGrid);
         feedMomentsIntoFsGrid(mpiGrid, cells, momentsGrid, technicalGrid, false);
         feedMomentsIntoFsGrid(mpiGrid, cells, momentsDt2Grid, technicalGrid, true);
         phiprof::stop("fsgrid-coupling-in");

         propagateFields(
            perBGrid,
            perBDt2Grid,
            EGrid,
            EDt2Grid,
            EHallGrid,
            EGradPeGrid,
            momentsGrid,
            momentsDt2Grid,
            dPerBGrid,
            dMomentsGrid,
            BgBGrid,
            volGrid,
            technicalGrid,
            sysBoundaryContainer,
            P::dt,
            P::fieldSolverSubcycles
         );

         phiprof::start("getFieldsFromFsGrid");
         // Copy results back from fsgrid.
         volGrid.updateGhostCells();
         technicalGrid.updateGhostCells();
         getFieldsFromFsGrid(volGrid, BgBGrid, EGradPeGrid, technicalGrid, mpiGrid, cells);
         phiprof::stop("getFieldsFromFsGrid");
         phiprof::stop("Propagate Fields",cells.size(),"SpatialCells");
         addTimedBarrier("barrier-after-field-solver");
      }

      if(FieldTracing::fieldTracingParameters.useCache) {
         FieldTracing::resetReconstructionCoefficientsCache();
      }

      // Map current data down into the ionosphere
      // TODO check: have we set perBGrid correctly here, or is it possibly perBDt2Grid in some cases??
      if(SBC::ionosphereGrid.nodes.size() > 0 && ((P::t > SBC::Ionosphere::solveCount * SBC::Ionosphere::couplingInterval && SBC::Ionosphere::couplingInterval > 0) || SBC::Ionosphere::couplingInterval == 0)) {
         FieldTracing::calculateIonosphereFsgridCoupling(technicalGrid, perBGrid, dPerBGrid, SBC::ionosphereGrid.nodes, SBC::Ionosphere::radius);
         SBC::ionosphereGrid.mapDownBoundaryData(perBGrid, dPerBGrid, momentsGrid, volGrid, technicalGrid);
         SBC::ionosphereGrid.calculateConductivityTensor(SBC::Ionosphere::F10_7, SBC::Ionosphere::recombAlpha, SBC::Ionosphere::backgroundIonisation);

         // Solve ionosphere
         int nIterations, nRestarts;
         Real residual, minPotentialN, maxPotentialN, minPotentialS, maxPotentialS;
         SBC::ionosphereGrid.solve(nIterations, nRestarts, residual, minPotentialN, maxPotentialN, minPotentialS, maxPotentialS);
         logFile << "tstep = " << P::tstep
         << " t = " << P::t
         << " ionosphere iterations = " << nIterations
         << " restarts = " << nRestarts
         << " residual = " << std::scientific << residual << std::defaultfloat
         << " N potential min " << minPotentialN
         << " max " << maxPotentialN
         << " difference " << maxPotentialN - minPotentialN
         << " S potential min " << minPotentialS
         << " max " << maxPotentialS
         << " difference " << maxPotentialS - minPotentialS
         << endl;
         SBC::Ionosphere::solveCount++;
         globalflags::ionosphereJustSolved = true;
         // Reset flag in all cells
         #pragma omp parallel for
         for(size_t i=0; i<cells.size(); i++) {
            if(mpiGrid[cells[i]]->parameters[CellParams::FORCING_CELL_NUM] == 1) {
               mpiGrid[cells[i]]->parameters[CellParams::FORCING_CELL_NUM] = 0;
            }
         }
      }

      phiprof::start("Velocity-space");
      if ( P::propagateVlasovAcceleration ) {
         calculateAcceleration(mpiGrid,P::dt);
         addTimedBarrier("barrier-after-ad just-blocks");
      } else {
         //zero step to set up moments _v
         calculateAcceleration(mpiGrid, 0.0);
      }
      phiprof::stop("Velocity-space",computedCells,"Cells");
      addTimedBarrier("barrier-after-acceleration");

      if (P::propagateVlasovTranslation || P::propagateVlasovAcceleration ) {
         phiprof::start("Update system boundaries (Vlasov post-acceleration)");
         sysBoundaryContainer.applySysBoundaryVlasovConditions(mpiGrid, P::t+0.5*P::dt, true);
         phiprof::stop("Update system boundaries (Vlasov post-acceleration)");
         addTimedBarrier("barrier-boundary-conditions");
      }

      phiprof::start("Compute interp moments");
      // *here we compute rho and rho_v for timestep t + dt, so next
      // timestep * //
      calculateInterpolatedVelocityMoments(
         mpiGrid,
         CellParams::RHOM,
         CellParams::VX,
         CellParams::VY,
         CellParams::VZ,
         CellParams::RHOQ,
         CellParams::P_11,
         CellParams::P_22,
         CellParams::P_33
      );
      phiprof::stop("Compute interp moments");

      phiprof::stop("Propagate",computedCells,"Cells");

      phiprof::start("Project endTimeStep");
      project->hook(hook::END_OF_TIME_STEP, mpiGrid, perBGrid);
      phiprof::stop("Project endTimeStep");

      // Check timestep
      if (P::dt < P::bailout_min_dt) {
         stringstream s;
         s << "The timestep dt=" << P::dt << " went below bailout.bailout_min_dt (" << to_string(P::bailout_min_dt) << ")." << endl;
         bailout(true, s.str(), __FILE__, __LINE__);
      }
      //Move forward in time
      P::meshRepartitioned = false;
      globalflags::ionosphereJustSolved = false;
      ++P::tstep;
      P::t += P::dt;

   }

   double after = MPI_Wtime();

   phiprof::stop("Simulation");
   phiprof::start("Finalization");
   if (P::propagateField ) {
      finalizeFieldPropagator();
   }
   if (myRank == MASTER_RANK) {
      if (doBailout > 0) {
         logFile << "(BAILOUT): Bailing out, see error log for details." << endl;
      }

      double timePerStep;
      if (P::tstep == P::tstep_min) {
         timePerStep=0.0;
      } else {
         timePerStep=double(after  - startTime) / (P::tstep-P::tstep_min);
      }
      double timePerSecond=double(after  - startTime) / (P::t-P::t_min+DT_EPSILON);
      logFile << "(MAIN): All timesteps calculated." << endl;
      logFile << "\t (TIME) total run time " << after - startTime << " s, total simulated time " << P::t -P::t_min<< " s" << endl;
      if(P::t != 0.0) {
         logFile << "\t (TIME) seconds per timestep " << timePerStep  <<
         ", seconds per simulated second " <<  timePerSecond << endl;
      }
      logFile << writeVerbose;
   }

   phiprof::stop("Finalization");
   phiprof::stop("main");

   phiprof::print(MPI_COMM_WORLD,"phiprof");

   if (myRank == MASTER_RANK) logFile << "(MAIN): Exiting." << endl << writeVerbose;
   logFile.close();
   if (P::diagnosticInterval != 0) diagnostic.close();

   perBGrid.finalize();
   perBDt2Grid.finalize();
   EGrid.finalize();
   EDt2Grid.finalize();
   EHallGrid.finalize();
   EGradPeGrid.finalize();
   momentsGrid.finalize();
   momentsDt2Grid.finalize();
   dPerBGrid.finalize();
   dMomentsGrid.finalize();
   BgBGrid.finalize();
   volGrid.finalize();
   technicalGrid.finalize();

   MPI_Finalize();

   #ifdef USE_GPU
   gpu_clear_device();
   #endif
   return 0;
}
