/*
 * This file is part of Vlasiator.
 * Copyright 2010-2016 Finnish Meteorological Institute
 *
 * For details of usage, see the COPYING file and read the "Rules of the Road"
 * at http://vlasiator.fmi.fi/
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

#ifndef DRO_POPULATIONS_H
#define	DRO_POPULATIONS_H

#include "datareductionoperator.h"
#include "../object_wrapper.h"

namespace DRO {
   
   class DataReductionOperatorPopulations: public DataReductionOperator {
   public:
      DataReductionOperatorPopulations(const std::string& name,const unsigned int popID, const unsigned int byteOffset,const unsigned int vectorSize);
      virtual ~DataReductionOperatorPopulations();
      
      virtual bool getDataVectorInfo(std::string& dataType,unsigned int& dataSize,unsigned int& vectorSize) const;
      virtual std::string getName() const;
      virtual bool reduceData(const spatial_cell::SpatialCell* cell,char* buffer);
      virtual bool reduceData(const spatial_cell::SpatialCell* cell,Real* result);
      virtual bool setSpatialCell(const spatial_cell::SpatialCell* cell);
      
   protected:
      uint _byteOffset;
      uint _vectorSize;
      uint _popID;
      std::string _name;
      const Real *_data;
   };
   
} // namespace DRO

#endif	/* DRO_POPULATIONS_H */

