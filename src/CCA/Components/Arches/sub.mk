#
#  The MIT License
#
#  Copyright (c) 1997-2016 The University of Utah
#
#  Permission is hereby granted, free of charge, to any person obtaining a copy
#  of this software and associated documentation files (the "Software"), to
#  deal in the Software without restriction, including without limitation the
#  rights to use, copy, modify, merge, publish, distribute, sublicense, and/or
#  sell copies of the Software, and to permit persons to whom the Software is
#  furnished to do so, subject to the following conditions:
#
#  The above copyright notice and this permission notice shall be included in
#  all copies or substantial portions of the Software.
#
#  THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
#  IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
#  FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
#  AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
#  LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
#  FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS
#  IN THE SOFTWARE.
#
#
# Makefile fragment for this subdirectory

include $(SCIRUN_SCRIPTS)/smallso_prologue.mk

SRCDIR := CCA/Components/Arches

########################################################################
#
# CUDA_ENABLED_SRCS are files that if CUDA is enabled (via configure), must be
# compiled using the nvcc compiler.
#
# WARNING: If you add a file to the list of CUDA_SRCS, you must add a
# corresponding rule at the end of this file!
#
# Also, do not put the .cc on this list of files as the .cc or .cu
# will be added automatically as needed.
#
CUDA_ENABLED_SRCS :=         \
       Arches                \
       BoundaryCond_new      \
       BoundaryCondition     \
       CQMOM                 \
       Discretization        \
       DQMOM                 \
       ExplicitSolver        \
       KokkosSolver          \
       Properties            

ifeq ($(HAVE_CUDA),yes)
   # CUDA enabled files, listed here (and with a rule at the end of
   # this sub.mk) are copied to the binary side and renamed with a .cu
   # extension (.cc replaced with .cu) so that they can be compiled
   # using the nvcc compiler.
   SRCS += $(foreach var,$(CUDA_ENABLED_SRCS),$(OBJTOP_ABS)/$(SRCDIR)/$(var).cu)
   DLINK_FILES := $(DLINK_FILES) $(foreach var,$(CUDA_ENABLED_SRCS),$(SRCDIR)/$(var).o)
else
   SRCS += $(foreach var,$(CUDA_ENABLED_SRCS),$(SRCDIR)/$(var).cc)
endif

########################################################################

SRCS += $(SRCDIR)/ArchesConstVariables.cc      \
        $(SRCDIR)/ArchesLabel.cc               \
        $(SRCDIR)/ArchesMaterial.cc            \
        $(SRCDIR)/ArchesParticlesHelper.cc     \
        $(SRCDIR)/ArchesBCHelper.cc            \
        $(SRCDIR)/ArchesVariables.cc           \
        $(SRCDIR)/CellInformation.cc           \
        $(SRCDIR)/CompDynamicProcedure.cc      \
        $(SRCDIR)/ExplicitTimeInt.cc           \
        $(SRCDIR)/IncDynamicProcedure.cc       \
        $(SRCDIR)/IntrusionBC.cc               \
        $(SRCDIR)/LU.cc                        \
        $(SRCDIR)/MomentumSolver.cc            \
        $(SRCDIR)/NonlinearSolver.cc           \
        $(SRCDIR)/PhysicalConstants.cc         \
        $(SRCDIR)/PressureSolverV2.cc          \
        $(SRCDIR)/RHSSolver.cc                 \
        $(SRCDIR)/ScaleSimilarityModel.cc      \
        $(SRCDIR)/SmagorinskyModel.cc          \
        $(SRCDIR)/Source.cc                    \
        $(SRCDIR)/TurbulenceModel.cc           \
        $(SRCDIR)/WBCHelper.cc                 \
        $(SRCDIR)/TurbulenceModelPlaceholder.cc

ifeq ($(HAVE_CUDA),yes)
  SRCS += $(SRCDIR)/constructLinearSystemKernel.cu
  DLINK_FILES := $(DLINK_FILES) $(SRCDIR)/constructLinearSystemKernel.o
endif

PSELIBS := \
        CCA/Components/Wasatch          \
        \
        CCA/Components/Arches/fortran   \
        CCA/Components/Models           \
        CCA/Components/OnTheFlyAnalysis \
        CCA/Ports                       \
        Core/Containers                 \
        Core/Datatypes                  \
        Core/Disclosure                 \
        Core/Exceptions                 \
        Core/Geometry                   \
        Core/GeometryPiece              \
        Core/Grid                       \
        Core/IO                         \
        Core/Math                       \
        Core/Parallel                   \
        Core/ProblemSpec                \
        Core/Util

ifeq ($(HAVE_PETSC),yes)
   SRCS += $(SRCDIR)/Filter.cc      \
           $(SRCDIR)/PetscCommon.cc 
   LIBS := $(LIBS) $(PETSC_LIBRARY)
   INCLUDES += $(PETSC_INCLUDE)
endif

ifeq ($(HAVE_HYPRE),yes)
   LIBS := $(LIBS) $(HYPRE_LIBRARY)
   INCLUDES += $(HYPRE_INCLUDE)
endif

LIBS := $(LIBS) $(XML2_LIBRARY) $(F_LIBRARY) $(MPI_LIBRARY) $(M_LIBRARY) \
        $(LAPACK_LIBRARY) $(BLAS_LIBRARY) $(THREAD_LIBRARY) \
        $(RADPROPS_LIBRARY) $(TABPROPS_LIBRARY) \
        $(BOOST_LIBRARY) $(Z_LIBRARY) \
        $(SPATIALOPS_LIBRARY)

INCLUDES := $(INCLUDES) $(BOOST_INCLUDE) \
            $(EXPRLIB_INCLUDE) $(TABPROPS_INCLUDE) $(RADPROPS_INCLUDE) $(SPATIALOPS_INCLUDE)

$(SRCDIR)/BoundaryCondition.$(OBJEXT) : $(SRCDIR)/fortran/mmbcvelocity_fort.h
$(SRCDIR)/BoundaryCondition.$(OBJEXT) : $(SRCDIR)/fortran/mm_computevel_fort.h
$(SRCDIR)/BoundaryCondition.$(OBJEXT) : $(SRCDIR)/fortran/mm_explicit_fort.h
$(SRCDIR)/BoundaryCondition.$(OBJEXT) : $(SRCDIR)/fortran/mm_explicit_oldvalue_fort.h
$(SRCDIR)/BoundaryCondition.$(OBJEXT) : $(SRCDIR)/fortran/mm_explicit_vel_fort.h
$(SRCDIR)/CellInformation.$(OBJEXT)   : $(SRCDIR)/fortran/cellg_fort.h
$(SRCDIR)/Discretization.$(OBJEXT)    : $(SRCDIR)/fortran/prescoef_var_fort.h
$(SRCDIR)/Discretization.$(OBJEXT)    : $(SRCDIR)/fortran/uvelcoef_fort.h
$(SRCDIR)/Discretization.$(OBJEXT)    : $(SRCDIR)/fortran/uvelcoef_central_fort.h
$(SRCDIR)/Discretization.$(OBJEXT)    : $(SRCDIR)/fortran/uvelcoef_upwind_fort.h
$(SRCDIR)/Discretization.$(OBJEXT)    : $(SRCDIR)/fortran/uvelcoef_mixed_fort.h
$(SRCDIR)/Discretization.$(OBJEXT)    : $(SRCDIR)/fortran/uvelcoef_hybrid_fort.h
$(SRCDIR)/Discretization.$(OBJEXT)    : $(SRCDIR)/fortran/vvelcoef_fort.h
$(SRCDIR)/Discretization.$(OBJEXT)    : $(SRCDIR)/fortran/vvelcoef_central_fort.h
$(SRCDIR)/Discretization.$(OBJEXT)    : $(SRCDIR)/fortran/vvelcoef_upwind_fort.h
$(SRCDIR)/Discretization.$(OBJEXT)    : $(SRCDIR)/fortran/vvelcoef_mixed_fort.h
$(SRCDIR)/Discretization.$(OBJEXT)    : $(SRCDIR)/fortran/vvelcoef_hybrid_fort.h
$(SRCDIR)/Discretization.$(OBJEXT)    : $(SRCDIR)/fortran/wvelcoef_fort.h
$(SRCDIR)/Discretization.$(OBJEXT)    : $(SRCDIR)/fortran/wvelcoef_central_fort.h
$(SRCDIR)/Discretization.$(OBJEXT)    : $(SRCDIR)/fortran/wvelcoef_upwind_fort.h
$(SRCDIR)/Discretization.$(OBJEXT)    : $(SRCDIR)/fortran/wvelcoef_mixed_fort.h
$(SRCDIR)/Discretization.$(OBJEXT)    : $(SRCDIR)/fortran/wvelcoef_hybrid_fort.h
$(SRCDIR)/SmagorinskyModel.$(OBJEXT)  : $(SRCDIR)/fortran/smagmodel_fort.h
$(SRCDIR)/Source.$(OBJEXT)            : $(SRCDIR)/fortran/pressrcpred_fort.h
$(SRCDIR)/Source.$(OBJEXT)            : $(SRCDIR)/fortran/pressrcpred_var_fort.h
$(SRCDIR)/Source.$(OBJEXT)            : $(SRCDIR)/fortran/uvelsrc_fort.h
$(SRCDIR)/Source.$(OBJEXT)            : $(SRCDIR)/fortran/vvelsrc_fort.h
$(SRCDIR)/Source.$(OBJEXT)            : $(SRCDIR)/fortran/wvelsrc_fort.h

########################################################################
# DigitalFilterGenerator
#
# See build specification in .../src/StandAlone/sub.mk

########################################################################
#
# Rules to copy CUDA enabled source (.cc) files to the binary build tree
# and rename with a .cu extension.
#

ifeq ($(HAVE_CUDA),yes)
  # Copy the 'original' .cc files into the binary tree and rename as .cu
  $(OBJTOP_ABS)/$(SRCDIR)/Arches.cu : $(SRCTOP_ABS)/$(SRCDIR)/Arches.cc
	cp $< $@
  $(OBJTOP_ABS)/$(SRCDIR)/BoundaryCondition.cu : $(SRCTOP_ABS)/$(SRCDIR)/BoundaryCondition.cc
	cp $< $@
  $(OBJTOP_ABS)/$(SRCDIR)/BoundaryCond_new.cu : $(SRCTOP_ABS)/$(SRCDIR)/BoundaryCond_new.cc
	cp $< $@
  $(OBJTOP_ABS)/$(SRCDIR)/CQMOM.cu : $(SRCTOP_ABS)/$(SRCDIR)/CQMOM.cc
	cp $< $@
  $(OBJTOP_ABS)/$(SRCDIR)/Discretization.cu : $(SRCTOP_ABS)/$(SRCDIR)/Discretization.cc
	cp $< $@
  $(OBJTOP_ABS)/$(SRCDIR)/DQMOM.cu : $(SRCTOP_ABS)/$(SRCDIR)/DQMOM.cc
	cp $< $@
  $(OBJTOP_ABS)/$(SRCDIR)/ExplicitSolver.cu : $(SRCTOP_ABS)/$(SRCDIR)/ExplicitSolver.cc
	cp $< $@
  $(OBJTOP_ABS)/$(SRCDIR)/KokkosSolver.cu : $(SRCTOP_ABS)/$(SRCDIR)/KokkosSolver.cc
	cp $< $@
  $(OBJTOP_ABS)/$(SRCDIR)/Properties.cu : $(SRCTOP_ABS)/$(SRCDIR)/Properties.cc
	cp $< $@
endif

########################################################################
# Handle subdirs (These files are just 'included' into the build of libCCA_Components_Arches.so)

SUBDIRS := $(SRCDIR)/ChemMix             \
           $(SRCDIR)/CoalModels          \
           $(SRCDIR)/CoalModels/fortran  \
           $(SRCDIR)/DigitalFilter       \
           $(SRCDIR)/LagrangianParticles \
           $(SRCDIR)/Operators           \
           $(SRCDIR)/ParticleModels      \
           $(SRCDIR)/PropertyModels      \
           $(SRCDIR)/PropertyModelsV2    \
           $(SRCDIR)/Radiation           \
           $(SRCDIR)/Radiation/fortran   \
           $(SRCDIR)/SourceTerms         \
           $(SRCDIR)/Task                \
           $(SRCDIR)/Transport           \
           $(SRCDIR)/TransportEqns       \
           $(SRCDIR)/Utility             \
           $(SRCDIR)/WallHTModels


include $(SCIRUN_SCRIPTS)/recurse.mk
include $(SCIRUN_SCRIPTS)/smallso_epilogue.mk

#### Handle subdirs that are their OWN SHARED LIBRARIES
# I don't know of any reason that these are actually made into separate libraries...
# perhaps just for historical reasons.  It would be just as easy to fold them into
# libArches if there ever was a reason to...
SUBDIRS := $(SRCDIR)/fortran

include $(SCIRUN_SCRIPTS)/recurse.mk
#### End handle subdirs

