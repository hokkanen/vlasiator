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

#include <algorithm>
#include <cmath>
#include <utility>
#include <omp.h>

#include <Eigen/Geometry>
#include <Eigen/Core>

#include "cpu_acc_transform.hpp"
#include "cpu_acc_intersections.hpp"
#include "cuda_acc_semilag.hpp"
#include "cuda_acc_map.hpp"

#include "device_launch_parameters.h"
#include "cuda.h"
#include "cuda_runtime.h"

#include "../cuda_context.cuh"

#include "../velocity_mesh_parameters.h"

using namespace std;
using namespace spatial_cell;
using namespace Eigen;

/*!
  prepareAccelerateCell and getAccelerationSybcycles available only through
  cpu_acc_semilag.cpp
*/

/*!
  Propagates the distribution function in velocity space of given real
  space cell. 

  This version prepares the transformation matrix and calculates intersections
  on the CPU. After that, the column construction and actual
  semilagrangian remapping is launched on the GPU.

  Based on SLICE-3D algorithm: Zerroukat, M., and T. Allen. "A
  three‐dimensional monotone and conservative semi‐Lagrangian scheme
  (SLICE‐3D) for transport problems." Quarterly Journal of the Royal
  Meteorological Society 138.667 (2012): 1640-1651.

 * @param spatial_cell Spatial cell containing the accelerated population.
 * @param popID ID of the accelerated particle species.
 * @param map_order Order in which vx,vy,vz mappings are performed. 
 * @param dt Time step of one subcycle.
*/

__global__ void printVBCsizekernel(
   // Quick debug kernel
   //vmesh::VelocityBlockContainer blockContainer
   ) {
   //uint blockDataN = blockContainer.size();
   //Real* parameters = blockContainer.getParameters();
   // const int cudaBlocks = gridDim.x;
   // const int blocki = blockIdx.x;
   const int i = threadIdx.x;
   const int j = threadIdx.y;
   const int k = threadIdx.z;
   const uint ti = k*WID2 + j*WID + i;
   if (ti==0) {
      //printf("device meshwrapper pointer 0x%llx to 0x%llx\n",vmesh::dev_getMeshWrapper(),*vmesh::dev_getMeshWrapper());
      // printf("device meshwrapper pointer 0x%llx to 0x%llx\n",vmesh::getMeshWrapper(),*vmesh::getMeshWrapper());
      // printf(" device blocklength %f %f %f %f \n",(*vmesh::getMeshWrapper()->velocityMeshes)[0].meshMinLimits[0],(*vmesh::getMeshWrapper()->velocityMeshes)[0].meshLimits[0],(*vmesh::getMeshWrapper()->velocityMeshes)[0].meshMaxLimits[0],(*vmesh::getMeshWrapper()->velocityMeshes)[0].meshLimits[1]);
   }
}

void cuda_accelerate_cell(SpatialCell* spatial_cell,
                         const uint popID,     
                         const uint map_order,
                         const Real& dt) {
   double t1 = MPI_Wtime();

   vmesh::VelocityMesh* vmesh    = spatial_cell->get_velocity_mesh(popID);
   vmesh::VelocityBlockContainer* blockContainer = spatial_cell->get_velocity_blocks(popID);

   // Launch cuda transfers
   phiprof::start("CUDA-HtoD");
   const uint thread_id = omp_get_thread_num();
   // Check that enough memory is allocated
   blockContainer->dev_Allocate(vmesh->size());
   blockContainer->dev_prefetchDevice();
   vmesh->dev_prefetchDevice();
   phiprof::stop("CUDA-HtoD");

   // CUDATEST Launch debug kernel?
   // dim3 block(WID,WID,WID);
   // printVBCsizekernel<<<1, block, 0, cuda_getStream()>>> ();
   // HANDLE_ERROR( cudaDeviceSynchronize() );

   // compute transform, forward in time and backward in time
   phiprof::start("compute-transform");

   //compute the transform performed in this acceleration
   Transform<Real,3,Affine> fwd_transform= compute_acceleration_transformation(spatial_cell,popID,dt);
   Transform<Real,3,Affine> bwd_transform= fwd_transform.inverse();
   phiprof::stop("compute-transform");

   const uint8_t refLevel = 0;
   Real intersection_z,intersection_z_di,intersection_z_dj,intersection_z_dk;
   Real intersection_x,intersection_x_di,intersection_x_dj,intersection_x_dk;
   Real intersection_y,intersection_y_di,intersection_y_dj,intersection_y_dk;
   switch(map_order){
       case 0:
          phiprof::start("compute-intersections");
          //Map order XYZ
          compute_intersections_1st(vmesh,bwd_transform, fwd_transform, 0, refLevel,
                                    intersection_x,intersection_x_di,intersection_x_dj,intersection_x_dk);
          compute_intersections_2nd(vmesh,bwd_transform, fwd_transform, 1, refLevel,
                                    intersection_y,intersection_y_di,intersection_y_dj,intersection_y_dk);
          compute_intersections_3rd(vmesh,bwd_transform, fwd_transform, 2, refLevel,
                                    intersection_z,intersection_z_di,intersection_z_dj,intersection_z_dk);
          phiprof::stop("compute-intersections");
          phiprof::start("compute-mapping");
          cuda_acc_map_1d(spatial_cell, popID, 
                          intersection_x,intersection_x_di,intersection_x_dj,intersection_x_dk,
                          0, cudaStreamList[thread_id]); // map along x
          //spatial_cell->adjustSingleCellVelocityBlocks(popID,true);
          cuda_acc_map_1d(spatial_cell, popID, 
                          intersection_y,intersection_y_di,intersection_y_dj,intersection_y_dk,
                          1, cudaStreamList[thread_id]); // map along y
          //spatial_cell->adjustSingleCellVelocityBlocks(popID,true);
          cuda_acc_map_1d(spatial_cell, popID, 
                          intersection_z,intersection_z_di,intersection_z_dj,intersection_z_dk,
                          2, cudaStreamList[thread_id]); // map along z
          phiprof::stop("compute-mapping");
          break;
          
       case 1:
          phiprof::start("compute-intersections");
          //Map order YZX
          compute_intersections_1st(vmesh, bwd_transform, fwd_transform, 1, refLevel,
                                    intersection_y,intersection_y_di,intersection_y_dj,intersection_y_dk);
          compute_intersections_2nd(vmesh, bwd_transform, fwd_transform, 2, refLevel,
                                    intersection_z,intersection_z_di,intersection_z_dj,intersection_z_dk);
          compute_intersections_3rd(vmesh, bwd_transform, fwd_transform, 0, refLevel,
                                    intersection_x,intersection_x_di,intersection_x_dj,intersection_x_dk);
          phiprof::stop("compute-intersections");
          phiprof::start("compute-mapping");
          cuda_acc_map_1d(spatial_cell, popID, 
                          intersection_y,intersection_y_di,intersection_y_dj,intersection_y_dk,
                          1, cudaStreamList[thread_id]); // map along y
          //spatial_cell->adjustSingleCellVelocityBlocks(popID,true);
          cuda_acc_map_1d(spatial_cell, popID, 
                          intersection_z,intersection_z_di,intersection_z_dj,intersection_z_dk,
                          2, cudaStreamList[thread_id]); // map along z
          //spatial_cell->adjustSingleCellVelocityBlocks(popID,true);
          cuda_acc_map_1d(spatial_cell, popID, 
                          intersection_x,intersection_x_di,intersection_x_dj,intersection_x_dk,
                          0, cudaStreamList[thread_id]); // map along x
          phiprof::stop("compute-mapping");
          break;

       case 2:
          phiprof::start("compute-intersections");
          //Map order Z X Y
          compute_intersections_1st(vmesh, bwd_transform, fwd_transform, 2, refLevel,
                                    intersection_z,intersection_z_di,intersection_z_dj,intersection_z_dk);
          compute_intersections_2nd(vmesh, bwd_transform, fwd_transform, 0, refLevel,
                                    intersection_x,intersection_x_di,intersection_x_dj,intersection_x_dk);
          compute_intersections_3rd(vmesh, bwd_transform, fwd_transform, 1, refLevel,
                                    intersection_y,intersection_y_di,intersection_y_dj,intersection_y_dk);
          phiprof::stop("compute-intersections");
          phiprof::start("compute-mapping");
          cuda_acc_map_1d(spatial_cell, popID, 
                          intersection_z,intersection_z_di,intersection_z_dj,intersection_z_dk,
                          2, cudaStreamList[thread_id]); // map along z
          //spatial_cell->adjustSingleCellVelocityBlocks(popID,true);
          cuda_acc_map_1d(spatial_cell, popID, 
                          intersection_x,intersection_x_di,intersection_x_dj,intersection_x_dk,
                          0, cudaStreamList[thread_id]); // map along x
          //spatial_cell->adjustSingleCellVelocityBlocks(popID,true);
          cuda_acc_map_1d(spatial_cell, popID, 
                          intersection_y,intersection_y_di,intersection_y_dj,intersection_y_dk,
                          1, cudaStreamList[thread_id]); // map along y
          phiprof::stop("compute-mapping");
          break;
   }

   // Transfer data back
   phiprof::start("CUDA-DtoH");
   blockContainer->dev_prefetchHost();
   vmesh->dev_prefetchHost();
   cudaStreamSynchronize(cudaStreamList[thread_id]);
   phiprof::stop("CUDA-DtoH");

//   if (Parameters::prepareForRebalance == true) {
//       spatial_cell->parameters[CellParams::LBWEIGHTCOUNTER] += (MPI_Wtime() - t1);
//   }
}
