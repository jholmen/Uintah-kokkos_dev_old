/*
 * The MIT License
 *
 * Copyright (c) 1997-2016 The University of Utah
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to
 * deal in the Software without restriction, including without limitation the
 * rights to use, copy, modify, merge, publish, distribute, sublicense, and/or
 * sell copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
 * FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS
 * IN THE SOFTWARE.
 */

#ifndef __SDINTERFACEMODEL_H__
#define __SDINTERFACEMODEL_H__

#include <CCA/Components/MPM/MPMFlags.h>
#include <CCA/Components/MPM/ConstitutiveModel/MPMMaterial.h>
#include <CCA/Ports/Scheduler.h>
#include <CCA/Ports/SchedulerP.h>
#include <CCA/Ports/DataWarehouse.h>
#include <Core/Grid/SimulationStateP.h>
#include <Core/Grid/SimulationState.h>
#include <Core/Grid/Task.h>
#include <Core/Grid/Variables/ComputeSet.h>
#include <Core/Labels/MPMLabel.h>
#include <Core/Parallel/ProcessorGroup.h>
#include <Core/ProblemSpec/ProblemSpecP.h>

namespace Uintah {

  class SDInterfaceModel {
  public:
    
    SDInterfaceModel(ProblemSpecP& ps, SimulationStateP& sS, MPMFlags* Mflag);
    virtual ~SDInterfaceModel();

    virtual void addComputesAndRequiresInterpolated(SchedulerP & sched,
                                          const PatchSet* patches,
                                          const MaterialSet* matls);

    virtual void sdInterfaceInterpolated(const ProcessorGroup*,
                                         const PatchSubset* patches,
                                         const MaterialSubset* matls,
                                         DataWarehouse* old_dw,
                                         DataWarehouse* new_dw);

    virtual void addComputesAndRequiresDivergence(SchedulerP & sched,
                                                  const PatchSet* patches,
                                                  const MaterialSet* matls);

    virtual void sdInterfaceDivergence(const ProcessorGroup*,
                                       const PatchSubset* patches,
                                       const MaterialSubset* matls,
                                       DataWarehouse* old_dw,
                                       DataWarehouse* new_dw);

    virtual void outputProblemSpec(ProblemSpecP& ps);

  protected:
    MPMLabel* d_lb;
    MPMFlags* d_Mflag;
    SimulationStateP d_sharedState;

    int NGP, NGN;
    int numMPMmatls;

    SDInterfaceModel(const SDInterfaceModel&);
    SDInterfaceModel& operator=(const SDInterfaceModel&);
    
  };
  
} // end namespace Uintah
#endif
