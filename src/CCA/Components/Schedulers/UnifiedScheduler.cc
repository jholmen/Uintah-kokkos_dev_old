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

#include <CCA/Components/Schedulers/UnifiedScheduler.h>
#include <CCA/Components/Schedulers/OnDemandDataWarehouse.h>
#include <CCA/Components/Schedulers/TaskGraph.h>
#include <CCA/Ports/Output.h>

#include <Core/Exceptions/ProblemSetupException.h>
#include <Core/Grid/Variables/CCVariable.h>
#include <Core/Grid/Variables/NCVariable.h>
#include <Core/Grid/Variables/SFCXVariable.h>
#include <Core/Grid/Variables/SFCYVariable.h>
#include <Core/Grid/Variables/SFCZVariable.h>
#include <Core/Parallel/CrowdMonitor.hpp>
#include <Core/Util/Time.h>

#ifdef HAVE_CUDA
#include <CCA/Components/Schedulers/GPUDataWarehouse.h>
#include <Core/Grid/Variables/GPUGridVariable.h>
#include <Core/Grid/Variables/GPUStencil7.h>
#endif

#include <sci_defs/cuda_defs.h>

#include <cstring>
#include <iomanip>
#include <mutex>

#define USE_PACKING

using namespace Uintah;

// sync cerr so they are readable when output by multiple threads
extern std::mutex cerrLock;

extern DebugStream taskdbg;
extern DebugStream waitout;
extern DebugStream execout;
extern DebugStream taskorder;
extern DebugStream taskLevel_dbg;

extern std::map<std::string, double> waittimes;
extern std::map<std::string, double> exectimes;

static double Unified_CurrentWaitTime = 0;

static DebugStream unified_dbg(             "Unified_DBG",             false);
static DebugStream unified_timeout(         "Unified_TimingsOut",      false);
static DebugStream unified_queuelength(     "Unified_QueueLength",     false);

#ifdef HAVE_CUDA

  //TODO, should be deallocated
  std::map <unsigned int, queue<cudaStream_t*> > * UnifiedScheduler::idleStreams = new std::map <unsigned int, queue<cudaStream_t*> >;

  namespace {

  struct idle_streams_tag{};
  using  idle_streams_monitor = Uintah::CrowdMonitor<idle_streams_tag>;

  }

#endif


//______________________________________________________________________
//
namespace {

std::mutex      g_main_io_mutex{};
std::mutex      g_worker_io_mutex{};
std::mutex      g_scheduler_mutex{}; // scheduler lock (acquire and release quickly)

} // namespace


//______________________________________________________________________
//
namespace Uintah { namespace Impl {

namespace {

__thread int       t_tid = 0;

}

namespace {

enum class ThreadState : int
{
    Inactive
  , Active
  , Exit
};

UnifiedSchedulerWorker * g_runners[MAX_THREADS]        = {};
volatile ThreadState     g_thread_states[MAX_THREADS]  = {};
int                      g_cpu_affinities[MAX_THREADS] = {};
int                      g_num_threads                 = 0;

volatile int g_run_tasks{0};


//______________________________________________________________________
//
void set_affinity( const int proc_unit )
{
#ifndef __APPLE__
  //disable affinity on OSX since sched_setaffinity() is not available in OSX API
  cpu_set_t mask;
  unsigned int len = sizeof(mask);
  CPU_ZERO(&mask);
  CPU_SET(proc_unit, &mask);
  sched_setaffinity(0, len, &mask);
#endif
}


//______________________________________________________________________
//
void thread_driver( const int tid )
{
  // t_tid is __thread variable, unique to each std::thread spawned below
  t_tid = tid;

  // set each TaskWorker thread's affinity
  set_affinity( g_cpu_affinities[tid] );

  try {
    // wait until main thread sets function and changes states
    g_thread_states[tid] = ThreadState::Inactive;
    while (g_thread_states[tid] == ThreadState::Inactive) {
      std::this_thread::yield();
    }

    while (g_thread_states[tid] == ThreadState::Active) {

      // run the function and wait for main thread to reset state
      g_runners[tid]->run();

      g_thread_states[tid] = ThreadState::Inactive;
      while (g_thread_states[tid] == ThreadState::Inactive) {
        std::this_thread::yield();
      }
    }
  } catch (const std::exception & e) {
    std::cerr << "Exception thrown from worker thread: " << e.what() << std::endl;
    std::cerr.flush();
    std::abort();
  } catch (...) {
    std::cerr << "Unknown Exception thrown from worker thread" << std::endl;
    std::cerr.flush();
    std::abort();
  }
}


//______________________________________________________________________
// only called by thread 0 (main thread)
void thread_fence()
{
  // main thread tid is at [0]
  g_thread_states[0] = ThreadState::Inactive;

  // TaskRunner threads start at [1]
  for (int i = 1; i < g_num_threads; ++i) {
    while (g_thread_states[i] == ThreadState::Active) {
      std::this_thread::yield();
    }
  }
  std::atomic_thread_fence(std::memory_order_seq_cst);
}


//______________________________________________________________________
// only called by main thread
void init_threads( UnifiedScheduler * sched, int num_threads )
{
  g_num_threads = num_threads;
  for (int i = 0; i < g_num_threads; ++i) {
    g_thread_states[i]  = ThreadState::Active;
    g_cpu_affinities[i] = i;
  }

  // set main thread's affinity - core 0
  set_affinity(g_cpu_affinities[0]);
  t_tid = 0;

  // TaskRunner threads start at g_runners[1]
  for (int i = 1; i < g_num_threads; ++i) {
    g_runners[i] = new UnifiedSchedulerWorker(sched);
  }

  // spawn worker threads
  // TaskRunner threads start at [1]
  for (int i = 1; i < g_num_threads; ++i) {
    std::thread(thread_driver, i).detach();
  }

  thread_fence();
}

} // namespace
}} // namespace Uintah::Impl


//______________________________________________________________________
//
UnifiedScheduler::UnifiedScheduler( const ProcessorGroup * myworld
                                  , const Output         * oport
                                  , UnifiedScheduler     * parentScheduler
                                  )
  : MPIScheduler(myworld, oport, parentScheduler)
{
#ifdef HAVE_CUDA
  if (Uintah::Parallel::usingDevice()) {
    gpuInitialize();

    // we need one of these for each GPU, as each device will have it's own CUDA context
    for (int i = 0; i < numDevices_; ++i) {
      getCudaStreamFromPool(i);
    }

    // disable memory windowing on variables.  This will ensure that
    // each variable is allocated its own memory on each patch,
    // precluding memory blocks being defined across multiple patches.
    Uintah::OnDemandDataWarehouse::d_combineMemory = false;

  }

  int numThreads = Uintah::Parallel::getNumThreads();
  if (numThreads == -1) {
    numThreads = 1;
  }
  //get the true numDevices (in case we have the simulation turned on)
  int numDevices;
  CUDA_RT_SAFE_CALL(cudaGetDeviceCount(&numDevices));
  int can_access = 0;
  for (int i = 0; i < numDevices; ++i) {
    CUDA_RT_SAFE_CALL( cudaSetDevice(i) );
    for (int j = 0; j < numDevices; ++j) {
      if (i != j) {
        cudaDeviceCanAccessPeer(&can_access, i, j);
        if (can_access) {
          printf("GOOD\n GPU device #%d can access GPU device #%d\n", i, j);
          cudaDeviceEnablePeerAccess(j, 0);
        } else {
          printf("ERROR\n GPU device #%d cannot access GPU device #%d\n.  Uintah is not yet configured to work with multiple GPUs in different NUMA regions.  For now, use the environment variable CUDA_VISIBLE_DEVICES and don't list GPU device #%d\n." , i, j, j);
          SCI_THROW( InternalError("** GPUs in multiple NUMA regions are currently unsupported.", __FILE__, __LINE__));
        }
      }
    }
  }

#endif

  if (unified_timeout.active()) {
    char filename[64];
    sprintf(filename, "timingStats.%d", d_myworld->myrank());
    timingStats.open(filename);
    if (d_myworld->myrank() == 0) {
      sprintf(filename, "timingStats.%d.max", d_myworld->size());
      maxStats.open(filename);
      sprintf(filename, "timingStats.%d.avg", d_myworld->size());
      avgStats.open(filename);
    }
  }
}


//______________________________________________________________________
//
UnifiedScheduler::~UnifiedScheduler()
{
  if (unified_timeout.active()) {
    timingStats.close();
    if (d_myworld->myrank() == 0) {
      maxStats.close();
      avgStats.close();
    }
  }

#ifdef HAVE_CUDA
  //freeCudaStreams();
#endif

}


//______________________________________________________________________
//
int UnifiedScheduler::verifyAnyGpuActive() {

#ifdef HAVE_CUDA
  //Attempt to access the zeroth GPU
  cudaError_t errorCode =  cudaSetDevice(0);
  if (errorCode == cudaSuccess) {
    return 1; //let 1 be a good error code
  }
#endif
  return 2;
}


//______________________________________________________________________
//
void
UnifiedScheduler::problemSetup( const ProblemSpecP&     prob_spec,
                                      SimulationStateP& state )
{
  // Default taskReadyQueueAlg
  taskQueueAlg_ = MostMessages;
  std::string taskQueueAlg = "MostMessages";

  ProblemSpecP params = prob_spec->findBlock("Scheduler");
  if (params) {
    params->get("taskReadyQueueAlg", taskQueueAlg);
    if (taskQueueAlg == "FCFS") {
      taskQueueAlg_ = FCFS;
    }
    else if (taskQueueAlg == "Random") {
      taskQueueAlg_ = Random;
    }
    else if (taskQueueAlg == "Stack") {
      taskQueueAlg_ = Stack;
    }
    else if (taskQueueAlg == "MostChildren") {
      taskQueueAlg_ = MostChildren;
    }
    else if (taskQueueAlg == "LeastChildren") {
      taskQueueAlg_ = LeastChildren;
    }
    else if (taskQueueAlg == "MostAllChildren") {
      taskQueueAlg_ = MostChildren;
    }
    else if (taskQueueAlg == "LeastAllChildren") {
      taskQueueAlg_ = LeastChildren;
    }
    else if (taskQueueAlg == "MostL2Children") {
      taskQueueAlg_ = MostL2Children;
    }
    else if (taskQueueAlg == "LeastL2Children") {
      taskQueueAlg_ = LeastL2Children;
    }
    else if (taskQueueAlg == "MostMessages") {
      taskQueueAlg_ = MostMessages;
    }
    else if (taskQueueAlg == "LeastMessages") {
      taskQueueAlg_ = LeastMessages;
    }
    else if (taskQueueAlg == "PatchOrder") {
      taskQueueAlg_ = PatchOrder;
    }
    else if (taskQueueAlg == "PatchOrderRandom") {
      taskQueueAlg_ = PatchOrderRandom;
    }
  }

  proc0cout << "   Using \"" << taskQueueAlg
      << "\" task queue priority algorithm" << std::endl;

  numThreads_ = Uintah::Parallel::getNumThreads() - 1;
  if (numThreads_ < 1
      && (Uintah::Parallel::usingMPI() || Uintah::Parallel::usingDevice())) {
    if (d_myworld->myrank() == 0) {
      std::cerr << "Error: no thread number specified for Unified Scheduler"
          << std::endl;
      throw ProblemSetupException(
          "This scheduler requires number of threads to be in the range [2, 64],\n.... please use -nthreads <num>, and -gpu if using GPUs",
          __FILE__, __LINE__);
    }
  } else if (numThreads_ > MAX_THREADS) {
    if (d_myworld->myrank() == 0) {
      std::cerr << "Error: Number of threads too large..." << std::endl;
      throw ProblemSetupException(
          "Too many threads. Reduce MAX_THREADS and recompile.", __FILE__,
          __LINE__);
    }
  }

  if (d_myworld->myrank() == 0) {
    std::string plural = (numThreads_ == 1) ? " thread" : " threads";
    std::cout
        << "   WARNING: Multi-threaded Unified scheduler is EXPERIMENTAL, not all tasks are thread safe yet.\n"
        << "   Creating " << numThreads_ << " additional "
        << plural + " for task execution (total task execution threads = "
        << numThreads_ + 1 << ")." << std::endl;

#ifdef HAVE_CUDA
    if (Uintah::Parallel::usingDevice()) {
      cudaError_t retVal;
      int availableDevices;
      CUDA_RT_SAFE_CALL(retVal = cudaGetDeviceCount(&availableDevices));
      std::cout << "   Using " << numDevices_ << "/" << availableDevices
          << " available GPU(s)" << std::endl;

      for (int device_id = 0; device_id < availableDevices; device_id++) {
        cudaDeviceProp device_prop;
        CUDA_RT_SAFE_CALL(
            retVal = cudaGetDeviceProperties(&device_prop, device_id));
        printf("   GPU Device %d: \"%s\" with compute capability %d.%d\n",
            device_id, device_prop.name, device_prop.major, device_prop.minor);
      }
    }
#endif
  }

  SchedulerCommon::problemSetup(prob_spec, state);

#ifdef HAVE_CUDA
  //Now pick out the materials out of the file.  This is done with an assumption that there
  //will only be ICE or MPM problems, and no problem will have both ICE and MPM materials in it.
  //I am unsure if this assumption is correct.
  //TODO: Add in MPM material support, just needs to look for an MPM block instead of an ICE block.
  ProblemSpecP mp = prob_spec->findBlockWithOutAttribute("MaterialProperties");
  if (mp) {
    ProblemSpecP group = mp->findBlock("ICE");
    if (group) {
      for (ProblemSpecP child = group->findBlock("material"); child != 0;
          child = child->findNextBlock("material")) {
        ProblemSpecP EOS_ps = child->findBlock("EOS");
        if (!EOS_ps) {
          throw ProblemSetupException("ERROR ICE: Cannot find EOS tag",
              __FILE__, __LINE__);
        }

        std::string EOS;
        if (!EOS_ps->getAttribute("type", EOS)) {
          throw ProblemSetupException("ERROR ICE: Cannot find EOS 'type' tag",
              __FILE__, __LINE__);
        }

        //add this material to the collection of materials
        materialsNames.push_back(EOS);
      }
    }
  }
#endif

  // this spawns threads, sets affinity, etc
  init_threads(this, numThreads_);

}


//______________________________________________________________________
//
SchedulerP
UnifiedScheduler::createSubScheduler()
{
  UintahParallelPort * lbp      = getPort("load balancer");
  UnifiedScheduler   * subsched = scinew UnifiedScheduler( d_myworld, m_outPort_, this );

  subsched->attachPort( "load balancer", lbp );
  subsched->d_sharedState = d_sharedState;
  subsched->numThreads_ = Uintah::Parallel::getNumThreads() - 1;

  return subsched;

}


//______________________________________________________________________
//
void
UnifiedScheduler::runTask( DetailedTask*         task,
                           int                   iteration,
                           int                   thread_id /* = 0 */,
                           Task::CallBackEvent   event )
{

  if (waitout.active()) {
    waittimesLock.lock();
    {
      waittimes[task->getTask()->getName()] += Unified_CurrentWaitTime;
      Unified_CurrentWaitTime = 0;
    }
    waittimesLock.unlock();
  }

  //Only execute CPU or GPU tasks.  Don't execute postGPU tasks a second time.
  if ( event == Task::CPU || event == Task::GPU) {
    // -------------------------< begin task execution timing >-------------------------
    double task_start_time = Time::currentSeconds();

    if (trackingVarsPrintLocation_ & SchedulerCommon::PRINT_BEFORE_EXEC) {
      printTrackedVars(task, SchedulerCommon::PRINT_BEFORE_EXEC);
    }

    std::vector<DataWarehouseP> plain_old_dws(dws.size());
    for (int i = 0; i < (int)dws.size(); ++i) {
      plain_old_dws[i] = dws[i].get_rep();
    }

    task->doit(d_myworld, dws, plain_old_dws, event);


    if (trackingVarsPrintLocation_ & SchedulerCommon::PRINT_AFTER_EXEC) {
      printTrackedVars(task, SchedulerCommon::PRINT_AFTER_EXEC);
    }

    double total_task_time = Time::currentSeconds() - task_start_time;
    // -------------------------< end task execution timing >-------------------------

    dlbLock.lock();
    {
      if (execout.active()) {
        exectimes[task->getTask()->getName()] += total_task_time;
      }

      // If I do not have a sub scheduler
      if (!task->getTask()->getHasSubScheduler()) {
        //add my task time to the total time
        mpi_info_[TotalTask] += total_task_time;
        if (!d_sharedState->isCopyDataTimestep() && task->getTask()->getType() != Task::Output) {
          // add contribution of task execution time to load balancer
          getLoadBalancer()->addContribution(task, total_task_time);
        }
      }
    }
    dlbLock.unlock();
  }

  // For CPU and postGPU task runs, post MPI sends and call task->done;
  if (event == Task::CPU || event == Task::postGPU) {

#ifdef HAVE_CUDA
    if (Uintah::Parallel::usingDevice()) {

      //TODO: Don't make every task run through this
      //TODO: Verify that it only looks for data that's valid in the GPU, and not assuming it's valid.
      findIntAndExtGpuDependencies( task, iteration, thread_id);
      //The ghost cell destinations indicate which devices we're using,
      //and which ones we'll need streams for.
      assignDevicesAndStreamsFromGhostVars(task);
      createTaskGpuDWs(task);

      //place everything in the GPU data warehouses
      prepareDeviceVars(task);
      prepareTaskVarsIntoTaskDW(task);
      prepareGhostCellsIntoTaskDW(task);
      syncTaskGpuDWs(task);

      //get these ghost cells to contiguous arrays so they can be copied to host.
      performInternalGhostCellCopies(task);  //TODO: Fix for multiple GPUs
      //Now that we've done internal ghost cell copies, we can mark the staging vars as being valid.
      //TODO: Sync required?  We shouldn't mark data as valid until it has copied.
      markDeviceRequiresDataAsValid(task);

      copyAllGpuToGpuDependences(task);
      //TODO: Mark destination staging vars as valid.


      //copy all dependencies to arrays
      copyAllExtGpuDependenciesToHost(task);

      //In order to help copy values to another on-node GPU or another MPI rank, ghost cell data
      //was placed in a var in the patch it is *going to*.  It helps reuse gpu dw engine code this way.
      //But soon, after this task is done, we are likely going to receive a different region of that patch
      //from a neighboring on-node GPU or neighboring MPI rank.  So we need to remove this foreign variable
      //now so it can be used again.
      //clearForeignGpuVars(deviceVars);
    }
#endif
    if (Uintah::Parallel::usingMPI()) {
      postMPISends(task, iteration, thread_id);
#ifdef HAVE_CUDA

#endif
    }
#ifdef HAVE_CUDA

    if (Uintah::Parallel::usingDevice()) {
      task->deleteTaskGpuDataWarehouses();
    }
#endif
    task->done(dws);  // should this be timed with taskstart? - BJW

    // -------------------------< begin MPI test timing >-------------------------
    double test_start_time = Time::currentSeconds();

    if (Uintah::Parallel::usingMPI()) {
      // This is per thread, no lock needed.
      sends_[thread_id].testsome(d_myworld);
    }

    mpi_info_[TotalTestMPI] += Time::currentSeconds() - test_start_time;
    // -------------------------< end MPI test timing >-------------------------

    // Add subscheduler timings to the parent scheduler and reset subscheduler timings
    if (parentScheduler_) {
      for (size_t i = 0; i < mpi_info_.size(); ++i) {
        MPIScheduler::TimingStat e = (MPIScheduler::TimingStat)i;
        parentScheduler_->mpi_info_[e] += mpi_info_[e];
      }
      mpi_info_.reset(0);
    }
  }
}  // end runTask()


//______________________________________________________________________
//
void
UnifiedScheduler::execute( int tgnum     /* = 0 */,
                           int iteration /* = 0 */ )
{
  // copy data timestep must be single threaded for now
  if (Uintah::Parallel::usingMPI() && d_sharedState->isCopyDataTimestep()) {
    MPIScheduler::execute( tgnum, iteration );
    return;
  }

  ASSERTRANGE(tgnum, 0, (int )graphs.size());
  TaskGraph* tg = graphs[tgnum];
  tg->setIteration(iteration);
  currentTG_ = tgnum;

  if (graphs.size() > 1) {
    // tg model is the multi TG model, where each graph is going to need to
    // have its dwmap reset here (even with the same tgnum)
    tg->remapTaskDWs(dwmap);
  }

  dts = tg->getDetailedTasks();

  if (dts == 0) {
    proc0cout << "UnifiedScheduler skipping execute, no tasks\n";
    return;
  }

  dts->initializeScrubs(dws, dwmap);
  dts->initTimestep();

  ntasks = dts->numLocalTasks();
  for (int i = 0; i < ntasks; ++i) {
    dts->localTask(i)->resetDependencyCounts();
  }

  bool emit_timings = unified_timeout.active();
  if (emit_timings) {
    d_labels.clear();
    d_times.clear();
  }

  //  // TODO - determine if this TG output code is even working correctly (APH - 09/16/15)
  //  makeTaskGraphDoc(dts, d_myworld->myrank());
  //  if (useInternalDeps() && emit_timings) {
  //    emitTime("taskGraph output");
  //  }

  mpi_info_.reset( 0 );

  numTasksDone = 0;
  abort = false;
  abort_point = 987654;

  if (reloc_new_posLabel_ && dws[dwmap[Task::OldDW]] != 0) {
    dws[dwmap[Task::OldDW]]->exchangeParticleQuantities(dts, getLoadBalancer(), reloc_new_posLabel_, iteration);
  }

  currentIteration = iteration;
  currphase = 0;
  numPhases = tg->getNumTaskPhases();
  phaseTasks.clear();
  phaseTasks.resize(numPhases, 0);
  phaseTasksDone.clear();
  phaseTasksDone.resize(numPhases, 0);
  phaseSyncTask.clear();
  phaseSyncTask.resize(numPhases, NULL);
  dts->setTaskPriorityAlg(taskQueueAlg_);

  // get the number of tasks in each task phase
  for (int i = 0; i < ntasks; ++i) {
    phaseTasks[dts->localTask(i)->getTask()->d_phase]++;
  }

  if (unified_dbg.active()) {
    std::lock_guard<std::mutex> lock(g_main_io_mutex);
    {
      unified_dbg << "\n"
                  << "Rank-" << d_myworld->myrank() << " Executing " << dts->numTasks() << " tasks (" << ntasks << " local)\n"
                  << "Total task phases: " << numPhases
                  << "\n";
      for (size_t phase = 0; phase < phaseTasks.size(); ++phase) {
        unified_dbg << "Phase: " << phase << " has " << phaseTasks[phase] << " total tasks\n";
      }
      unified_dbg << std::endl;
    }
  }

  static int totaltasks;

  if (taskdbg.active()) {
    std::lock_guard<std::mutex> lock(g_main_io_mutex);
    {
      taskdbg << myRankThread() << " starting task phase " << currphase << ", total phase " << currphase << " tasks = "
              << phaseTasks[currphase] << std::endl;
    }
  }



  //------------------------------------------------------------------------------------------------
  // activate TaskRunners
  //------------------------------------------------------------------------------------------------
  if (!d_sharedState->isCopyDataTimestep()) {
    Impl::g_run_tasks = 1;
    for (int i = 1; i < numThreads_; ++i) {
      Impl::g_thread_states[i] = Impl::ThreadState::Active;
    }
  }
  //------------------------------------------------------------------------------------------------


  // main thread also executes tasks
  runTasks(Impl::t_tid);


  //------------------------------------------------------------------------------------------------
  // deactivate TaskRunners
  //------------------------------------------------------------------------------------------------
  if (!d_sharedState->isCopyDataTimestep()) {
    Impl::g_run_tasks = 0;

    Impl::thread_fence();

    for (int i = 1; i < numThreads_; ++i) {
      Impl::g_thread_states[i] = Impl::ThreadState::Inactive;
    }
  }
  //------------------------------------------------------------------------------------------------




  if (unified_queuelength.active()) {
    float lengthsum = 0;
    totaltasks += ntasks;
    for (unsigned int i = 1; i < histogram.size(); ++i) {
      lengthsum = lengthsum + i * histogram[i];
    }

    float queuelength = lengthsum / totaltasks;
    float allqueuelength = 0;
    MPI_Reduce(&queuelength, &allqueuelength, 1, MPI_FLOAT, MPI_SUM, 0, d_myworld->getComm());

    proc0cout << "average queue length:" << allqueuelength / d_myworld->size() << std::endl;
  }

  emitTime("MPI Send time", mpi_info_[TotalSendMPI]);
  emitTime("MPI Recv time", mpi_info_[TotalRecvMPI]);
  emitTime("MPI TestSome time", mpi_info_[TotalTestMPI]);
  emitTime("MPI Wait time", mpi_info_[TotalWaitMPI]);
  emitTime("MPI reduce time", mpi_info_[TotalReduceMPI]);
  emitTime("Total send time", mpi_info_[TotalSend] - mpi_info_[TotalSendMPI] - mpi_info_[TotalTestMPI]);
  emitTime("Total recv time", mpi_info_[TotalRecv] - mpi_info_[TotalRecvMPI] - mpi_info_[TotalWaitMPI]);
  emitTime("Total task time", mpi_info_[TotalTask]);
  emitTime("Total reduction time", mpi_info_[TotalReduce] - mpi_info_[TotalReduceMPI]);
  emitTime("Total comm time", mpi_info_[TotalRecv] + mpi_info_[TotalSend] + mpi_info_[TotalReduce]);

  double time = Time::currentSeconds();
  double totalexec = time - d_lasttime;
  d_lasttime = time;

  emitTime("Other excution time", totalexec - mpi_info_[TotalSend] - mpi_info_[TotalRecv] - mpi_info_[TotalTask] - mpi_info_[TotalReduce]);

  // compute the net timings
  if (d_sharedState != 0) {

    computeNetRunTimeStats(d_sharedState->d_runTimeStats);

    // TaskRunner threads start at g_runners[1]
    for (int i = 1; i < numThreads_; ++i) {
      d_sharedState->d_runTimeStats[SimulationState::TaskWaitThreadTime] += Impl::g_runners[i]->getWaittime();
    }
  }

  if (restartable && tgnum == (int)graphs.size() - 1) {
    // Copy the restart flag to all processors
    int myrestart = dws[dws.size() - 1]->timestepRestarted();
    int netrestart;

    MPI_Allreduce(&myrestart, &netrestart, 1, MPI_INT, MPI_LOR, d_myworld->getComm());

    if (netrestart) {
      dws[dws.size() - 1]->restartTimestep();
      if (dws[0]) {
        dws[0]->setRestarted();
      }
    }
  }

  finalizeTimestep();

  if ( (execout.active() || emit_timings) && !parentScheduler_) {  // only do on toplevel scheduler
    outputTimingStats("UnifiedScheduler");
  }

  if (unified_dbg.active()) {
    unified_dbg << "Rank-" << d_myworld->myrank() << " - UnifiedScheduler finished" << std::endl;
  }
} // end execute()


//______________________________________________________________________
//
void UnifiedScheduler::markTaskConsumed(int& numTasksDone, int& currphase, int numPhases, DetailedTask* dtask) {

  //Update the count of tasks consumed by the scheduler.
  numTasksDone++;
  //cerrLock.lock();
  //{
  //cout << myRankThread() << " Consumed task: " << dtask->getName() << " tasks done is: " << numTasksDone << " of " << ntasks << endl;
  //}
  //cerrLock.unlock();
  if (taskorder.active()) {
    if (d_myworld->myrank() == d_myworld->size() / 2) {
      std::lock_guard<std::mutex> lock(g_main_io_mutex);
      {
        taskorder << myRankThread() << " Running task static order: " << dtask->getStaticOrder() << ", scheduled order: "
                  << numTasksDone << std::endl;
      }
    }
  }

  //Update the count of this phase consumed.
  phaseTasksDone[dtask->getTask()->d_phase]++;

  //See if we've consumed all tasks on this phase, if so, go to the next phase.
  while (phaseTasks[currphase] == phaseTasksDone[currphase] && currphase + 1 < numPhases) {
    currphase++;
    if (taskdbg.active()) {
      std::lock_guard<std::mutex> lock(g_main_io_mutex);
      {
        taskdbg << myRankThread() << " switched to task phase " << currphase << ", total phase " << currphase << " tasks = "
                << phaseTasks[currphase] << std::endl;
      }
    }
  }
}


//______________________________________________________________________
//
void
UnifiedScheduler::runTasks( int thread_id )
{

  while( numTasksDone < ntasks ) {

    DetailedTask* readyTask = NULL;
    DetailedTask* initTask = NULL;

    int pendingMPIMsgs = 0;
    bool havework = false;

#ifdef HAVE_CUDA
    bool usingDevice = Uintah::Parallel::usingDevice();
    bool gpuInitReady = false;
    bool gpuVerifyDataTransferCompletion = false;
    bool gpuFinalizeDevicePreparation = false;
    bool gpuRunReady = false;
    bool gpuPending = false;
    bool cpuInitReady = false;
    bool cpuFinalizeHostPreparation = false;
    bool cpuRunReady = false;

#endif

    // ----------------------------------------------------------------------------------
    // Part 1:
    //    Check if anything this thread can do concurrently.
    //    If so, then update the various scheduler counters.
    // ----------------------------------------------------------------------------------
    g_scheduler_mutex.lock();
    while (!havework) {
      /*
       * (1.1)
       *
       * If it is time to setup for a reduction task, then do so.
       *
       */

      if ((phaseSyncTask[currphase] != NULL) && (phaseTasksDone[currphase] == phaseTasks[currphase] - 1)) {
        readyTask = phaseSyncTask[currphase];
        havework = true;
        markTaskConsumed(numTasksDone, currphase, numPhases, readyTask);
#ifdef HAVE_CUDA
        cpuRunReady = true;
#endif

        break;
      }

      /*
       * (1.2)
       *
       * Run a CPU task that has its MPI communication complete. These tasks get in the external
       * ready queue automatically when their receive count hits 0 in DependencyBatch::received,
       * which is called when a MPI message is delivered.
       *
       * NOTE: This is also where a GPU-enabled task gets into the GPU initially-ready queue
       *
       */
      else if (dts->numExternalReadyTasks() > 0) {
        readyTask = dts->getNextExternalReadyTask();
        if (readyTask != NULL) {
          havework = true;
#ifdef HAVE_CUDA
          /*
           * (1.2.1)
           *
           * If it's a GPU-enabled task, assign it to a device (patches were assigned devices previously)
           * and initiate its H2D computes and requires data copies. This is where the
           * execution cycle begins for each GPU-enabled Task.
           *
           * gpuInitReady = true
           */
          if (readyTask->getTask()->usesDevice()) {
            //These tasks can't start unless we copy and/or verify all data into GPU memory
            gpuInitReady = true;
          } else if (usingDevice == false || readyTask->getPatches() == NULL) {
            //These tasks won't ever have anything to pull out of the device
            //so go ahead and mark the task "done" and say that it's ready
            //to start running as a CPU task.

            markTaskConsumed(numTasksDone, currphase, numPhases, readyTask);
            cpuRunReady = true;
          } else if (!readyTask->getTask()->usesDevice() && usingDevice) {
            //These tasks can't start unless we copy and/or verify all data into host memory
            cpuInitReady = true;
          } else {

            markTaskConsumed(numTasksDone, currphase, numPhases, readyTask);
            cpuRunReady = true;
          }
#else
          //if NOT compiled with device support, then this is a CPU task and we can mark the task consumed
          markTaskConsumed(numTasksDone, currphase, numPhases, readyTask);
#endif
          break;
        }
      }

      /*
       * (1.3)
       *
       * If we have an internally-ready CPU task, initiate its MPI receives, preparing it for
       * CPU external ready queue. The task is moved to the CPU external-ready queue in the
       * call to task->checkExternalDepCount().
       *
       */
      else if (dts->numInternalReadyTasks() > 0) {
        initTask = dts->getNextInternalReadyTask();
        if (initTask != NULL) {
          if (initTask->getTask()->getType() == Task::Reduction || initTask->getTask()->usesMPI()) {
            if (taskdbg.active()) {
              std::lock_guard<std::mutex> lock(g_main_io_mutex);
              {
                taskdbg << myRankThread() << " Task internal ready 1 " << *initTask << std::endl;
              }
            }
            phaseSyncTask[initTask->getTask()->d_phase] = initTask;
            ASSERT(initTask->getRequires().size() == 0)
            initTask = NULL;
          }
          else if (initTask->getRequires().size() == 0) {  // no ext. dependencies, then skip MPI sends
            initTask->markInitiated();
            initTask->checkExternalDepCount();  // where tasks get added to external ready queue
            initTask = NULL;
          }
          else {
            havework = true;
            break;
          }
        }
      }
#ifdef HAVE_CUDA
      /*
       * (1.4)
       *
       * Check if highest priority GPU task's asynchronous H2D copies are completed. If so,
       * then reclaim the streams and events it used for these operations, and mark as valid
       * the vars for which this task was responsible.  (If the vars are awaiting ghost cells
       * then those vars will be updated with a status to reflect they aren't quite valid yet)
       *
       * gpuVerifyDataTransferCompletion = true
       */
      else if (usingDevice == true
          && dts->numVerifyDataTransferCompletion() > 0
          && dts->peekNextVerifyDataTransferCompletionTask()->checkAllCudaStreamsDoneForThisTask()) {
          readyTask = dts->getNextVerifyDataTransferCompletionTask();
          gpuVerifyDataTransferCompletion = true;
          havework = true;
          break;
      }
      /*
       * (1.4.1)
       *
       * Check if all GPU variables for the task are either valid or valid and awaiting ghost cells.
       * If any aren't yet at that state (due to another task having not copied it in yet), then
       * repeat this step.  If all variables have been copied in and some need ghost cells, then
       * process that.  If no variables need to have their ghost cells processed,
       * the GPU to GPU ghost cell copies.  Also make GPU data as being valid as it is now
       * copied into the device.
       *
       * gpuFinalizeDevicePreparation = true
       */
      else if (usingDevice == true
          && dts->numFinalizeDevicePreparation() > 0
          && dts->peekNextFinalizeDevicePreparationTask()->checkAllCudaStreamsDoneForThisTask()) {
          readyTask = dts->getNextFinalizeDevicePreparationTask();
          gpuFinalizeDevicePreparation = true;
          havework = true;
          break;
      }
      /*
       * (1.4.2)
       *
       * Check if highest priority GPU task's asynchronous device to device ghost cell copies are
       * finished. If so, then reclaim the streams and events it used for these operations, execute
       * the task and then put it into the GPU completion-pending queue.
       *
       * gpuRunReady = true
       */
      else if (usingDevice == true
          && dts->numInitiallyReadyDeviceTasks() > 0
          && dts->peekNextInitiallyReadyDeviceTask()->checkAllCudaStreamsDoneForThisTask()) {

        // printf("Ghost cell copies done...\n");
        // All of this task's h2d copies is complete, so add it to the completion
        // pending GPU task queue and prepare to run.
        readyTask = dts->getNextInitiallyReadyDeviceTask();
        gpuRunReady = true;
        havework = true;

        break;

      }
      /*
       * (1.4.3)
       *
       * Check if a CPU task needs data into host memory from GPU memory
       * If so, copies data D2H.  Also checks if all data has arrived and is ready to process.
       *
       * cpuFinalizeHostPreparation = true
       */
      else if (usingDevice == true
          && dts->numFinalizeHostPreparation() > 0
          && dts->peekNextFinalizeHostPreparationTask()->checkAllCudaStreamsDoneForThisTask()) {
          readyTask = dts->getNextFinalizeHostPreparationTask();
          cpuFinalizeHostPreparation = true;
          havework = true;
          break;
      }
      /*
       * (1.4.4)
       *
       * Check if highest priority GPU task's asynchronous D2H copies are completed. If so,
       * then reclaim the streams and events it used for these operations, execute the task and
       * then put it into the CPU completion-pending queue.
       *
       * cpuRunReady = true
       */
      else if (usingDevice == true
               && dts->numInitiallyReadyHostTasks() > 0
               && dts->peekNextInitiallyReadyHostTask()->checkAllCudaStreamsDoneForThisTask()) {

        readyTask = dts->getNextInitiallyReadyHostTask();

        markTaskConsumed(numTasksDone, currphase, numPhases, readyTask);

        reclaimCudaStreamsIntoPool(readyTask);

        cpuRunReady = true;
        havework = true;
        break;
      }
      /*
       * (1.5)
       *
       * Check to see if any GPU tasks have been completed. This means the kernel(s)
       * have executed (which prevents out of order kernels, and also keeps tasks that depend on
       * its data to wait until the async kernel call is done).
       * This task's MPI sends can then be posted and done() can be called.
       *
       * gpuPending = true
       */
      else if (usingDevice == true
          && dts->numCompletionPendingDeviceTasks() > 0
          && dts->peekNextCompletionPendingDeviceTask()->checkAllCudaStreamsDoneForThisTask()) {

        readyTask = dts->getNextCompletionPendingDeviceTask();
        havework = true;
        gpuPending = true;
        markTaskConsumed(numTasksDone, currphase, numPhases, readyTask);

        break;

      }
#endif
      /*
       * (1.6)
       *
       * Otherwise there's nothing to do but process MPI recvs.
       */
      else {
        pendingMPIMsgs = pendingMPIRecvs();
        if (pendingMPIMsgs > 0) {
          havework = true;
          break;
        }
      }
      if (numTasksDone == ntasks) {
        break;
      }
    } // end while (!havework)

    g_scheduler_mutex.unlock();

    // ----------------------------------------------------------------------------------
    // Part 2
    //    Concurrent Part:
    //      Each thread does its own thing here... modify this code with caution
    // ----------------------------------------------------------------------------------

    if (initTask != NULL) {
      initiateTask(initTask, abort, abort_point, currentIteration);
      if (taskdbg.active()) {
        std::lock_guard<std::mutex> lock(g_main_io_mutex);
        {
          taskdbg << myRankThread() << " Task internal ready 2 " << *initTask << " deps needed: " << initTask->getExternalDepCount()
                  << std::endl;
        }
      }
      initTask->markInitiated();
      initTask->checkExternalDepCount();
    }
    else if (readyTask != NULL) {
      if (taskdbg.active()) {
        std::lock_guard<std::mutex> lock(g_main_io_mutex);
        {
          taskdbg << myRankThread() << " Task external ready " << *readyTask << std::endl;
        }
      }
      if (readyTask->getTask()->getType() == Task::Reduction) {
        initiateReduction(readyTask);
      }
#ifdef HAVE_CUDA
      else if (gpuInitReady) {
        //prepare to run a GPU task.

        //Ghost cells from CPU same device to variable not yet on GPU -> Managed already by getGridVar()
        //Ghost cells from CPU same device to variable already on GPU -> Managed in initiateH2DCopies(), then copied with performInternalGhostCellCopies()
        //Ghost cells from GPU other device to variable not yet on GPU -> new MPI code and getGridVar()
        //Ghost cells from GPU other device to variable already on GPU -> new MPI code, then initiateH2DCopies(), and copied with performInternalGhostCellCopies()
        //Ghost cells from GPU same device to variable not yet on GPU -> managed in initiateH2DCopies(), and copied with performInternalGhostCellCopies()
        //Ghost cells from GPU same device to variable already on GPU -> Managed in initiateH2DCopies()?
        assignDevicesAndStreams(readyTask);
        initiateH2DCopies(readyTask);
        syncTaskGpuDWs(readyTask);

        dts->addVerifyDataTransferCompletion(readyTask);

      } else if (gpuVerifyDataTransferCompletion) {
        //The requires data is either already there or has just been copied in so mark it as valid.
        //Go through the tasks's deviceVars collection.  Any requires in it should be marked as either
        //valid (if no ghost cells) or valid without ghost cells (if ghost cells are needed).
        markDeviceRequiresDataAsValid(readyTask);

        //Tasks own label/patch/matl/level vars (nonstaging) for which it must process and validate ghost cells.
        //For those vars,
        //It does so by processing a list of ghost cells to copy in this task's GPU_DW's d_varDB.
        //Only process this if the task has ghostVars (as it was the one who is responsible for the processing).

        if (!ghostCellsProcessingReady(readyTask)) {
          //it wasn't ready, so check again.
          dts->addVerifyDataTransferCompletion(readyTask);
        } else {
          performInternalGhostCellCopies(readyTask);
          dts->addFinalizeDevicePreparation(readyTask);
        }
      } else if (gpuFinalizeDevicePreparation) {
        //Go through the tasks's deviceVars collection.  Any valid without ghost cells are marked as valid.
        markDeviceGhostsAsValid(readyTask);
        //Check if all of the tasks variables are valid (if no ghost cells)
        //or valid with ghost cells (if it needed ghost cells)
        if (!allGPUVarsProcessingReady(readyTask)) {
          //Some vars aren't valid and ready, or some ghost vars don't have ghost cell data copied in.
          //Just repeat this step.  We must be waiting on another task to finish copying in some of the variables we need.
          dts->addFinalizeDevicePreparation(readyTask);
        } else {
          //This task had no ghost variables to process.  Or they have all been processed.
          //So jump to the gpuRunReady step.
          dts->addInitiallyReadyDeviceTask(readyTask);
        }
      } else if (gpuRunReady) {

        //Run the task on the GPU!
        runTask(readyTask, currentIteration, thread_id, Task::GPU);

        //See if we're dealing with 32768 ghost cells per patch.  If so,
        //it's easier to manage them on the host for now than on the GPU.  We can issue
        //these on the same stream as runTask, and it won't process until after the GPU
        //kernel completed.
        //initiateD2HForHugeGhostCells(readyTask);

        dts->addCompletionPendingDeviceTask(readyTask);

      } else if (gpuPending) {
        //The GPU task has completed. All of the computes data is now valid and should be marked as such.

        //Go through all computes for the task. Mark them as valid.
        markDeviceComputesDataAsValid(readyTask);

        //The Task GPU Datawarehouses are no longer needed.  Delete them on the host and device.
        readyTask->deleteTaskGpuDataWarehouses();

        // Run post GPU part of task.  It won't actually rerun the task
        // But it will run post computation management logic, which includes
        // marking the task as done.
        runTask(readyTask, currentIteration, thread_id, Task::postGPU);

        // recycle this task's stream
        reclaimCudaStreamsIntoPool(readyTask);
      }
#endif
      else {
        //prepare to run a CPU task.
#ifdef HAVE_CUDA
        if (cpuInitReady) {
          //If the task graph has scheduled to output variables, make sure that we are
          //going to actually output variables before pulling data out of the GPU.
          //(It would be nice if the task graph didn't have this OutputVariables task if
          //it wasn't going to output data, but that would require more task graph recompilations,
          //which can be even costlier overall.  So we do the check here.)
          //So check everything, except for ouputVariables tasks when it's not an output timestep.

          if ((m_outPort_->isOutputTimestep())
              || ((readyTask->getTask()->getName() != "DataArchiver::outputVariables")
                  && (readyTask->getTask()->getName() != "DataArchiver::outputVariables(checkpoint)"))) {
            assignDevicesAndStreams(readyTask);

            initiateD2H(readyTask);

          }
          dts->addFinalizeHostPreparation(readyTask);
        } else if (cpuFinalizeHostPreparation) {
          markHostRequiresDataAsValid(readyTask);
          if (!allHostVarsProcessingReady(readyTask)) {
            //Some vars aren't valid and ready,  We must be waiting on another task to finish
            //copying in some of the variables we need.
            dts->addFinalizeHostPreparation(readyTask);
          }  else {
            reclaimCudaStreamsIntoPool(readyTask);
            dts->addInitiallyReadyHostTask(readyTask);
          }


        } else if (cpuRunReady) {
#endif
          //run CPU task.
          runTask(readyTask, currentIteration, thread_id, Task::CPU);
          printTaskLevels(d_myworld, taskLevel_dbg, readyTask);
#ifdef HAVE_CUDA
        }
#endif
      }
    }
    else if (pendingMPIMsgs > 0) {
      processMPIRecvs(TEST);
    }
    else {
      // This can only happen when all tasks have finished.
      ASSERT(numTasksDone == ntasks);
    }
  }  //end while (numTasksDone < ntasks)
}


//______________________________________________________________________
//
struct CompareDep {

  bool operator()( DependencyBatch* a,
                   DependencyBatch* b )
  {
    return a->messageTag < b->messageTag;
  }
};


#ifdef HAVE_CUDA

//______________________________________________________________________
//
void UnifiedScheduler::prepareGpuDependencies(DetailedTask* dtask, DependencyBatch* batch, const VarLabel* pos_var,
    OnDemandDataWarehouse* dw, OnDemandDataWarehouse* old_dw,
    const DetailedDep* dep, LoadBalancer* lb,
    GpuUtilities::DeviceVarDestination dest) {

  //This should handle the following scenarios:
  //GPU -> different GPU same node  (write to GPU array, move to other device memory, copy in via copyGPUGhostCellsBetweenDevices)
  //GPU -> different GPU another node (write to GPU array, move to host memory, copy via MPI)
  //GPU -> CPU another node (write to GPU array, move to host memory, copy via MPI)
  //It should not handle
  //GPU -> CPU same node (handled in initateH2D)
  //GPU -> same GPU same node (handled in initateH2D)

  //This method therefore indicates that staging/contiguous arrays are needed, and what ghost cell copies
  //need to occur in the GPU.

  if (dep->isNonDataDependency()) {
    return;
  }

  const VarLabel* label = dep->req->var;
  const Patch* fromPatch = dep->fromPatch;
  const int matlIndx = dep->matl;
  const Level* level = fromPatch->getLevel();
  const int levelID = level->getID();

  //TODO: Ask Alan about everything in the dep object.
  //The toTasks (will there be more than one?)
  //the dep->comp (computes?)
  //the dep->req (requires?)

  DetailedTask* toTask = NULL;
  //Go through all toTasks
  for (list<DetailedTask*>::const_iterator iter = dep->toTasks.begin();
      iter != dep->toTasks.end(); ++iter) {
    toTask = (*iter);

    const Patch* toPatch = toTask->getPatches()->get(0);
    if (toTask->getPatches()->size() > 1) {
      printf("ERROR:\nUnifiedScheduler::prepareGpuDependencies() does not yet support a dependency that has multiple destination patches\n");
      SCI_THROW( InternalError("UnifiedScheduler::prepareGpuDependencies() does not yet support a dependency that has multiple destination patches", __FILE__, __LINE__));
    }
    const int fromresource = dtask->getAssignedResourceIndex();
    const int toresource = toTask->getAssignedResourceIndex();

    const int fromDeviceIndex = GpuUtilities::getGpuIndexForPatch(fromPatch);
    //For now, assume that task will only work on one device
    const int toDeviceIndex = GpuUtilities::getGpuIndexForPatch(toTask->getPatches()->get(0));

    //const size_t elementSize = OnDemandDataWarehouse::getTypeDescriptionSize(
    //    dep->req->var->typeDescription()->getSubType()->getType());

    //printf("In OnDemandDataWarehouse::prepareGPUDependencies from patch %d to patch %d, from task %p to task %p\n", fromPatch->getID(), toTask->getPatches()->get(0)->getID(), task, toTask);

    if ((fromresource == toresource) && (fromDeviceIndex == toDeviceIndex)) {
      //don't handle GPU -> same GPU same node here
      continue;
    }

    GPUDataWarehouse* gpudw = NULL;
    if (fromDeviceIndex != -1) {
      gpudw = dw->getGPUDW(fromDeviceIndex);
      if (!gpudw->isValidOnGPU(label->d_name.c_str(), fromPatch->getID(), matlIndx, levelID)) {
        continue;
      }
    } else {
      SCI_THROW(
          InternalError("Device index not found for "+label->getFullName(matlIndx, fromPatch), __FILE__, __LINE__));
    }
    /*
    if (toTask->getTask()->usesDevice()) {
      //The above tells us that it going to a GPU.
      //For now, when the receiving end calls getGridVar, it will piece together
      //the ghost cells, and move it into the GPU if needed.
      //Therefore, we don't at this moment need to know if it's going to a GPU.
      //But in the future, if we can manage direct GPU->GPU communication avoiding
      //the CPU then this is important to know
      toGPU = true;
    }*/

    switch (label->typeDescription()->getType()) {
    case TypeDescription::ParticleVariable: {
    }
      break;
    case TypeDescription::NCVariable:
    case TypeDescription::CCVariable:
    case TypeDescription::SFCXVariable:
    case TypeDescription::SFCYVariable:
    case TypeDescription::SFCZVariable: {

      //
      //var->getSizes(host_low, host_high, host_offset, host_size, host_strides);
      //TODO, This compiles a list of regions we need to copy into contiguous arrays.
      //We don't yet handle a scenario where the ghost cell region is exactly the same
      //size as the variable, meaning we don't need to create an array and copy to it.

      //if (gpu_stats.active()) {
      //  gpu_stats << myRankThread()
      //      << " prepareGpuDependencies - preparing dependency for task: " << *dep
      //      << ", ghost type: " << dep->req->gtype << ", number of ghost cells: " << dep->req->numGhostCells
      //      << " from dw ID " << dw->getID() << '\n';
      //}

      //We're going to copy the ghost vars from the source variable (already in the GPU) to a destination
      //array (not yet in the GPU).  So make sure there is a destination.

      //See if we're already planning on making this exact copy.  If so, don't do it again.
      IntVector host_low, host_high, host_offset, host_size;
      host_low = dep->low;
      host_high = dep->high;
      host_offset = dep->low;
      host_size = dep->high - dep->low;
      size_t elementDataSize = OnDemandDataWarehouse::getTypeDescriptionSize(dep->req->var->typeDescription()->getSubType()->getType());

      //If this staging var already exists, then assume the full ghost cell copying information
      //has already been set up previously.  (Duplicate dependencies show up by this point, so
      //just ignore the duplicate).

      //NOTE: On the CPU, a ghost cell face may be sent from patch A to patch B, while a ghost cell
      //edge/line may be sent from patch A to patch C, and the line of data for C is wholly within
      //the face data for B.
      //For the sake of preparing for cuda aware MPI, we still want to create two staging vars here,
      //a contiguous face for B, and a contiguous edge/line for C.
      if (!(dtask->getDeviceVars().stagingVarAlreadyExists(dep->req->var, fromPatch, matlIndx, levelID, host_low, host_size, dep->req->mapDataWarehouse()))) {


        //TODO: This host var really should be created last minute only if it's copying data to host.  Not here.
        GridVariableBase* tempGhostVar = dynamic_cast<GridVariableBase*>(label->typeDescription()->createInstance());
        tempGhostVar->allocate(dep->low, dep->high);

        //tempGhostVar->getSizes(host_low, host_high, host_offset, host_size,
        //    host_strides);

        //Indicate we want a staging array in the device.
        dtask->getDeviceVars().add(fromPatch, matlIndx, levelID, true, host_size,
            tempGhostVar->getDataSize(), elementDataSize, host_offset,
             dep->req, Ghost::None, 0, fromDeviceIndex, tempGhostVar, dest);
        //let this Task GPU DW know about this staging array
        dtask->getTaskVars().addTaskGpuDWStagingVar(fromPatch, matlIndx, levelID, host_offset, host_size, elementDataSize, dep->req, fromDeviceIndex);

        //Now make sure the Task DW knows about the non-staging variable where the
        //staging variable's data will come from.
        //Scenarios occur in which the same source region is listed to send to two different patches.
        //This task doesn't need to know about the same source twice.
        if (!(dtask->getTaskVars().varAlreadyExists(dep->req->var, fromPatch, matlIndx, levelID, dep->req->mapDataWarehouse()))) {
          //let this Task GPU DW know about the source location.
          dtask->getTaskVars().addTaskGpuDWVar(fromPatch, matlIndx, levelID, elementDataSize, dep->req, fromDeviceIndex);
        } else {
          if (gpu_stats.active()) {
            cerrLock.lock();
            gpu_stats << myRankThread()
            << " prepareGpuDependencies - Already had a task GPUDW Var for label " << dep->req->var->getName()
            << " patch " << fromPatch->getID()
            << " matl " << matlIndx
            << " level " << levelID
            << endl;

            cerrLock.unlock();
          }
        }

        //Handle a GPU-another GPU same device transfer.  We have already queued up the staging array on
        //source GPU.  Now queue up the staging array on the destination GPU.
        if (dest == GpuUtilities::anotherDeviceSameMpiRank) {
          //Indicate we want a staging array in the device.
          //TODO: We don't need a host array, it's going GPU->GPU.  So get rid of tempGhostVar here.
          dtask->getDeviceVars().add(toPatch, matlIndx, levelID, true, host_size,
              tempGhostVar->getDataSize(), elementDataSize, host_offset,
               dep->req, Ghost::None, 0, toDeviceIndex, tempGhostVar, dest);

          //And the task should know of this staging array.
          dtask->getTaskVars().addTaskGpuDWStagingVar(toPatch, matlIndx, levelID, host_offset, host_size, elementDataSize, dep->req, toDeviceIndex);

        }

        if (gpu_stats.active()) {
          cerrLock.lock();
          gpu_stats << myRankThread()
              << " prepareGpuDependencies - Preparing a GPU contiguous ghost cell array ";
          if (dest == GpuUtilities::anotherMpiRank) {
            gpu_stats << "to prepare for a later copy from MPI Rank " << fromresource << " to MPI Rank " << toresource;
          } else if (dest == GpuUtilities::anotherDeviceSameMpiRank) {
            gpu_stats << "to prepare for a later GPU to GPU copy from on-node device # " << fromDeviceIndex << " to on-node device # " << toDeviceIndex;
          } else {
            gpu_stats << "to UNKNOWN ";
          }
          gpu_stats << " for " << dep->req->var->getName().c_str()
              << " from patch " << fromPatch->getID()
              << " to patch " << toPatch->getID()
              << " between shared low (" << dep->low.x() << ", " << dep->low.y() << ", " << dep->low.z() << ")"
              << " and shared high (" << dep->high.x() << ", " << dep->high.y() << ", " << dep->high.z() << ")"
              << " and host offset (" << host_offset.x() << ", " << host_offset.y() << ", " << host_offset.z() << ")"
              << endl;
          cerrLock.unlock();
        }

        //we always write this to a "foreign" staging variable. We are going to
        //copying it from the foreign = false var to the foreign = true var.  Thus the
        //patch source and destination are the same.  And it's staying on device.
        IntVector temp(0,0,0);
        dtask->getGhostVars().add(dep->req->var, fromPatch, fromPatch,
            matlIndx, levelID, false, true, host_offset, host_size, dep->low, dep->high,
            OnDemandDataWarehouse::getTypeDescriptionSize(dep->req->var->typeDescription()->getSubType()->getType()),
            temp,
            fromDeviceIndex, toDeviceIndex, fromresource, toresource,
            (Task::WhichDW) dep->req->mapDataWarehouse(), GpuUtilities::sameDeviceSameMpiRank);



        if (dest == GpuUtilities::anotherDeviceSameMpiRank) {
          //GPU to GPU copies needs another entry indicating a peer to peer transfer.

          if (gpu_stats.active()) {
            cerrLock.lock();
            gpu_stats << myRankThread()
                << " prepareGpuDependencies - Preparing a GPU to GPU peer copy "
                << " for " << dep->req->var->getName().c_str()
                << " from patch " << fromPatch->getID()
                << " to patch " << toPatch->getID()
                << " between shared low (" << dep->low.x() << ", " << dep->low.y() << ", " << dep->low.z() << ")"
                << " and shared high (" << dep->high.x() << ", " << dep->high.y() << ", " << dep->high.z() << ")"
                << " and host offset (" << host_offset.x() << ", " << host_offset.y() << ", " << host_offset.z() << ")"
                << endl;
            cerrLock.unlock();
          }

          dtask->getGhostVars().add(dep->req->var, fromPatch, toPatch,
             matlIndx, levelID, true, true, host_offset, host_size, dep->low, dep->high,
             OnDemandDataWarehouse::getTypeDescriptionSize(dep->req->var->typeDescription()->getSubType()->getType()),
             temp,
             fromDeviceIndex, toDeviceIndex, fromresource, toresource,
             (Task::WhichDW) dep->req->mapDataWarehouse(), GpuUtilities::anotherDeviceSameMpiRank);

        } else if (dest == GpuUtilities::anotherMpiRank)  {
          if (gpu_stats.active()) {
            cerrLock.lock();
            gpu_stats << myRankThread()
                << " prepareGpuDependencies - Preparing a GPU to host ghost cell copy"
                << " for " << dep->req->var->getName().c_str()
                << " from patch " << fromPatch->getID()
                << " to patch " << toPatch->getID()
                << " between shared low (" << dep->low.x() << ", " << dep->low.y() << ", " << dep->low.z() << ")"
                << " and shared high (" << dep->high.x() << ", " << dep->high.y() << ", " << dep->high.z() << ")"
                << " and host offset (" << host_offset.x() << ", " << host_offset.y() << ", " << host_offset.z() << ")"
                << endl;
            cerrLock.unlock();
          }
          dtask->getGhostVars().add(dep->req->var, fromPatch, toPatch,
             matlIndx, levelID, true, true, host_offset, host_size, dep->low, dep->high,
             OnDemandDataWarehouse::getTypeDescriptionSize(dep->req->var->typeDescription()->getSubType()->getType()),
             temp,
             fromDeviceIndex, toDeviceIndex, fromresource, toresource,
             (Task::WhichDW) dep->req->mapDataWarehouse(), GpuUtilities::anotherMpiRank);

        }
      }
    }
      break;
    default: {
      cerr
          << "UnifiedScheduler::prepareGPUDependencies(), unsupported variable type"
          << endl;
    }

    }
  }
}


//______________________________________________________________________
//
void
UnifiedScheduler::gpuInitialize( bool reset ) {

  cudaError_t retVal;
  if (simulate_multiple_gpus.active()) {
    printf("SimulateMultipleGPUs is on, simulating 3 GPUs\n");
    numDevices_ = 3;
  } else if (use_single_device.active()) {
    numDevices_ = 1;
  } else {
    int numDevices = 0;
    CUDA_RT_SAFE_CALL(retVal = cudaGetDeviceCount(&numDevices));
    numDevices_ = numDevices;
  }

  if (simulate_multiple_gpus.active()) {

    //we're simulating many, but we only will use one.
    CUDA_RT_SAFE_CALL(retVal = cudaSetDevice(0));
    if (reset) {
      CUDA_RT_SAFE_CALL(retVal = cudaDeviceReset());
    }
  } else {
    for (int i = 0; i < numDevices_; ++i) {
      if (reset) {
        CUDA_RT_SAFE_CALL(retVal = cudaSetDevice(i));
        CUDA_RT_SAFE_CALL(retVal = cudaDeviceReset());
      }
    }
    //set it back to the 0th device
    CUDA_RT_SAFE_CALL(retVal = cudaSetDevice(0));
    currentDevice_ = 0;
  }

}


//______________________________________________________________________
//
void
UnifiedScheduler::initiateH2DCopies( DetailedTask * dtask ) {

  const Task* task = dtask->getTask();

  dtask->clearPreparationCollections();
  //Gather up all possible dependents from requires and computes and remove duplicates (we don't want to
  //transfer some variables twice).
  //Note: A task can only run on one level at a time.  It could run multiple patches and multiple
  //materials, but a single task will never run multiple levels.
  std::map<labelPatchMatlDependency, const Task::Dependency*> vars;
  for (const Task::Dependency* dependantVar = task->getRequires(); dependantVar != 0; dependantVar = dependantVar->next) {
    constHandle<PatchSubset> patches = dependantVar->getPatchesUnderDomain(dtask->getPatches());
    constHandle<MaterialSubset> matls = dependantVar->getMaterialsUnderDomain(dtask->getMaterials());
    const int numPatches = patches->size();
    const int numMatls = matls->size();
    for (int i = 0; i < numPatches; ++i) {
      for (int j = 0; j < numMatls; ++j) {
        labelPatchMatlDependency lpmd(dependantVar->var->getName().c_str(), patches->get(i)->getID(), matls->get(j), Task::Requires);
        if (vars.find(lpmd) == vars.end()) {
          vars.insert(
              std::map<labelPatchMatlDependency, const Task::Dependency*>::value_type(lpmd, dependantVar));
        }
      }
    }
  }
  for (const Task::Dependency* dependantVar = task->getComputes(); dependantVar != 0; dependantVar = dependantVar->next) {
    constHandle<PatchSubset> patches = dependantVar->getPatchesUnderDomain(dtask->getPatches());
    constHandle<MaterialSubset> matls = dependantVar->getMaterialsUnderDomain(dtask->getMaterials());
    const int numPatches = patches->size();
    const int numMatls = matls->size();
    for (int i = 0; i < numPatches; ++i) {
      for (int j = 0; j < numMatls; ++j) {
        labelPatchMatlDependency lpmd(dependantVar->var->getName().c_str(), patches->get(i)->getID(), matls->get(j), Task::Computes);
        if (vars.find(lpmd) == vars.end()) {
          vars.insert(
              std::map<labelPatchMatlDependency, const Task::Dependency*>::value_type(lpmd, dependantVar));
        }
      }
    }
  }

  unsigned int device_id = -1;
  //The task runs on one device.  The first patch we see can be used to tell us
  //which device we should be on.
  std::map<labelPatchMatlDependency, const Task::Dependency*>::iterator varIter;
  varIter = vars.begin();
  if (varIter != vars.end()) {
    device_id = GpuUtilities::getGpuIndexForPatch(varIter->second->getPatchesUnderDomain(dtask->getPatches())->get(0));
    OnDemandDataWarehouse::uintahSetCudaDevice(device_id);
  }

  //Go through each unique dependent var and see if we should allocate space and/or
  //queue it to be copied H2D.

  for (varIter = vars.begin(); varIter != vars.end(); ++varIter) {
    const Task::Dependency* curDependency = varIter->second;

    constHandle<PatchSubset> patches = curDependency->getPatchesUnderDomain(dtask->getPatches());
    constHandle<MaterialSubset> matls = curDependency->getMaterialsUnderDomain(dtask->getMaterials());
    const int numPatches = patches->size();
    //const int numMatls = matls->size();
    // for now, we're only interested in grid variables
    const TypeDescription::Type type = curDependency->var->typeDescription()->getType();

    const int patchID = varIter->first.patchID;
    const Patch * patch = NULL;
    //const Patch * patch = grid->getPatchByID(patchID, levelID);
    for (int i = 0; i < numPatches; ++i) {
      if (patches->get(i)->getID() == patchID) {
        patch = patches->get(i);
      }
    }
    if (!patch) {
      printf("ERROR:\nUnifiedScheduler::initiateD2H() patch not found.\n");
      SCI_THROW( InternalError("UnifiedScheduler::initiateD2H() patch not found.", __FILE__, __LINE__));
    }
    const int matlID = varIter->first.matlIndex;
    const Level* level = getLevel(patches.get_rep());
    int levelID = level->getID();
    if (curDependency->var->typeDescription()->getType() == TypeDescription::ReductionVariable) {
      levelID = -1;
    }
    const int deviceIndex = GpuUtilities::getGpuIndexForPatch(patch);

    // a fix for when INF ghost cells are requested such as in RMCRT
    //  e.g. tsk->requires(abskg_dw, d_abskgLabel, gac, SHRT_MAX);
    bool uses_SHRT_MAX = (curDependency->numGhostCells == SHRT_MAX);

    if (type == TypeDescription::CCVariable
        || type == TypeDescription::NCVariable
        || type == TypeDescription::SFCXVariable
        || type == TypeDescription::SFCYVariable
        || type == TypeDescription::SFCZVariable
        || type == TypeDescription::PerPatch
        || type == TypeDescription::ReductionVariable) {

      //For this dependency, get its CPU Data Warehouse and GPU Datawarehouse.
      const int dwIndex = curDependency->mapDataWarehouse();
      OnDemandDataWarehouseP dw = dws[dwIndex];
      GPUDataWarehouse* gpudw = dw->getGPUDW(deviceIndex);

      //Get all size information about this dependency.
      IntVector low, high, lowOffset, highOffset;
      Patch::VariableBasis basis = Patch::translateTypeToBasis(type, false);
      Patch::getGhostOffsets(type, curDependency->gtype, curDependency->numGhostCells, lowOffset, highOffset);
      patch->computeExtents(basis, curDependency->var->getBoundaryLayer(), lowOffset, highOffset, low, high);
      const IntVector host_size = high - low;
      const size_t elementDataSize =
          OnDemandDataWarehouse::getTypeDescriptionSize(curDependency->var->typeDescription()->getSubType()->getType());

      if (gpu_stats.active()) {
        cerrLock.lock();
        {
          gpu_stats << myRankThread()
              << " InitiateH2D - Handling this task's dependency for "
              << curDependency->var->getName() << " for patch: " << patchID
              << " material: " << matlID << " level: " << levelID;
          if (curDependency->deptype == Task::Requires) {
            gpu_stats << " - A REQUIRES dependency" << endl;
          } else if (curDependency->deptype == Task::Computes) {
            gpu_stats << " - A COMPUTES dependency" << endl;
          }
        }
        cerrLock.unlock();
      }

      if (curDependency->deptype == Task::Requires) {

        //See if this variable still exists on the device from some prior task
        //and if it's valid.  If so, use it.  If not, queue it to be possibly allocated
        //and then copy it in H2D.
        const bool allocated = gpudw->isAllocatedOnGPU( curDependency->var->getName().c_str(), patchID, matlID, levelID);

        bool allocatedCorrectSize = allocated;
        if (type == TypeDescription::CCVariable
            || type == TypeDescription::NCVariable
            || type == TypeDescription::SFCXVariable
            || type == TypeDescription::SFCYVariable
            || type == TypeDescription::SFCZVariable) {
          allocatedCorrectSize = gpudw->isAllocatedOnGPU( curDependency->var->getName().c_str(), patchID, matlID, levelID,
                                                          make_int3(low.x(), low.y(), low.z()),
                                                          make_int3(host_size.x(), host_size.y(), host_size.z()));
        }

        //For any var, only ONE task should manage all ghost cells for it.
        //It is a giant mess to try and have two tasks simultaneously managing ghost cells for a single var.
        //So if ghost cells are required, attempt to claim that we're the ones going to manage ghost cells
        //This changes a var's status to valid awaiting ghost data if this task claims ownership of managing ghost cells
        //Otherwise the var's status is left alone (perhaps the ghost cells were already processed by another task a while ago)
        bool gatherGhostCells = false;
        if (curDependency->gtype != Ghost::None && curDependency->numGhostCells > 0) {

          //If an entry doesn't already exist, make an empty unallocated variable on the GPU.
          //We will also mark it as awaiting ghost cells.
          //(If it was a uses_SHRT_MAX variable, we'll handle that later.  For these gathering will happen on the *CPU*
          //as it's currently much more efficient to do it there.)
          gpudw->putUnallocatedIfNotExists(curDependency->var->getName().c_str(), patchID, matlID, levelID,
                                  false, make_int3(low.x(), low.y(), low.z()), make_int3(host_size.x(), host_size.y(), host_size.z()));
          gatherGhostCells = gpudw->testAndSetAwaitingGhostDataOnGPU(curDependency->var->getName().c_str(), patchID, matlID, levelID);

        }
        const bool validOnGpu = gpudw->isValidOnGPU(curDependency->var->getName().c_str(), patchID, matlID, levelID);

        if (allocated && allocatedCorrectSize && validOnGpu) {

          //This requires variable already exists on the GPU.  Queue it to be added to this tasks's TaskDW.
          dtask->getTaskVars().addTaskGpuDWVar(patch, matlID, levelID, elementDataSize, curDependency, deviceIndex);

          if (gpu_stats.active()) {
            cerrLock.lock();
            {
              gpu_stats << myRankThread()
                  << " InitiateH2D() - GridVariable: "
                  << curDependency->var->getName().c_str()
                  << " already exists, skipping H2D copy..." << std::endl;
            }
            cerrLock.unlock();
          }

          if (gatherGhostCells) {
            dtask->getVarsToBeGhostReady().addVarToBeGhostReady(dtask->getName(), patch, matlID, levelID,
                                                                curDependency,
                                                                deviceIndex);
            //The variable's space already exists on the GPU, and it's valid on the GPU.  So we copy in any ghost
            //cells into the GPU and let the GPU handle the ghost cell copying logic.

            vector<OnDemandDataWarehouse::ValidNeighbors> validNeighbors;
            dw->getValidNeighbors(curDependency->var, matlID, patch, curDependency->gtype, curDependency->numGhostCells, validNeighbors);
            for (vector<OnDemandDataWarehouse::ValidNeighbors>::iterator iter = validNeighbors.begin(); iter != validNeighbors.end(); ++iter) {

              const Patch* sourcePatch = NULL;
              if (iter->neighborPatch->getID() >= 0) {
                sourcePatch = iter->neighborPatch;
              } else {
                //This occurs on virtual patches.  They can be "wrap around" patches, meaning if you go to one end of a domain
                //you will show up on the other side.  Virtual patches have negative patch IDs, but they know what real patch they
                //are referring to.
                sourcePatch = iter->neighborPatch->getRealPatch();
              }

              int sourceDeviceNum = GpuUtilities::getGpuIndexForPatch(sourcePatch);
              int destDeviceNum = deviceIndex;

              IntVector ghost_host_low, ghost_host_high, ghost_host_offset, ghost_host_size, ghost_host_strides;
              iter->validNeighbor->getSizes(ghost_host_low, ghost_host_high, ghost_host_offset, ghost_host_size, ghost_host_strides);

              IntVector virtualOffset = iter->neighborPatch->getVirtualOffset();

              bool useCpuGhostCells = false;
              bool useGpuGhostCells = false;
              bool useGpuStaging = false;

              //Assume for now that the ghost cell region is also the exact same size as the
              //staging var.  (If in the future ghost cell data is managed a bit better as
              //it currently does on the CPU, then some ghost cell regions will be found
              //*within* an existing staging var.  This is known to happen with Wasatch
              //computations involving periodic boundary scenarios.)
              //Note: The line below is potentially risky.  The var may exist in the database but we don't know
              //if the data in it is valid.  I'm not sure the correct fix here yet.
              useGpuStaging = dw->getGPUDW(destDeviceNum)->stagingVarExists(
                                    curDependency->var->getName().c_str(),
                                    patchID, matlID, levelID,
                                    make_int3(iter->low.x(), iter->low.y(), iter->low.z()),
                                    make_int3(iter->high.x() - iter->low.x(), iter->high.y()- iter->low.y(), iter->high.z()- iter->low.z()));

              if (gpu_stats.active()) {
                cerrLock.lock();
                {
                  gpu_stats << myRankThread()
                     << " InitiateH2D() - ";
                  if (useGpuStaging) {
                    gpu_stats << " Found a GPU staging var for ";
                  } else {
                    gpu_stats << " Didn't find a GPU staging var for ";
                  }
                  gpu_stats << curDependency->var->getName().c_str() << " patch "
                    << patchID << " material "
                    << matlID << " level "
                    << levelID
                    << " with low/offset (" << iter->low.x()
                    << ", " << iter->low.y()
                    << ", " << iter->low.z()
                    << ") with size (" << iter->high.x() - iter->low.x()
                    << ", " << iter->high.y() - iter->low.y()
                    << ", " << iter->high.z() - iter->low.z() << ")"
                    << endl;
                }
                cerrLock.unlock();
              }

              if (useGpuStaging
                  || (sourceDeviceNum == destDeviceNum)
                  || (dw->getGPUDW(sourceDeviceNum)->isValidOnCPU(curDependency->var->getName().c_str(), sourcePatch->getID(), matlID, levelID))) {

                //See if we need to push into the device ghost cell data from the CPU.
                //This will happen one for one of two reasons.
                //First, if the ghost cell source data isn't valid on the GPU, then we assume it must be
                //on the CPU.  (This can happen from a prior MPI transfer from another node).
                //Second, if the ghost cell source data is valid on the GPU *and* CPU *and*
                //the ghost cell is from one GPU to another GPU on the same machine
                //then we are going to use the CPU data.  (This happens after an output task, when
                //the data in a GPU is copied to the CPU to be copied on the hard drive.)
                //In either case, we copy the ghost cell from the CPU to the appropriate GPU
                //then a kernel can merge these ghost cells into the GPU grid var.
                //Note: In the future when modifies support is included, then
                //it would be possible to be valid on the CPU and not valid on the GPU
                //We can run into a tricky situation where a GPU datawarehouse may try to bring in
                //a patch multiple times for different ghost cells (e.g. patch 1 needs 0's ghost cells
                //and patch 2 needs 0's ghost cells.  It will try to bring in patch 0 twice).  The fix
                //is to see if patch 0 in the GPUDW exists, is queued up, but not valid.
                //TODO: Update if statements to handle both staging and non staging GPU variables.

                if (!useGpuStaging) {
                  if (!(dw->getGPUDW(sourceDeviceNum)->isAllocatedOnGPU(
                      curDependency->var->getName().c_str(),
                      sourcePatch->getID(), matlID, levelID))
                      || !(dw->getGPUDW(sourceDeviceNum)->isValidOnGPU(
                          curDependency->var->getName().c_str(),
                          sourcePatch->getID(), matlID, levelID))) {
                    useCpuGhostCells = true;
                  } else if (dw->getGPUDW(sourceDeviceNum)->isValidOnCPU(
                      curDependency->var->getName().c_str(),
                      sourcePatch->getID(), matlID, levelID)
                      && sourceDeviceNum != destDeviceNum) {
                    useCpuGhostCells = true;
                  } else if (!(dw->getGPUDW(sourceDeviceNum)->isValidOnCPU(
                      curDependency->var->getName().c_str(),
                      sourcePatch->getID(), matlID, levelID))
                      && !(dw->getGPUDW(sourceDeviceNum)->isValidOnGPU(
                          curDependency->var->getName().c_str(),
                          sourcePatch->getID(), matlID, levelID))) {
                    printf(
                        "ERROR: Needed ghost cell data not found on the CPU or a GPU\n");
                    exit(-1);
                  } else {
                    useGpuGhostCells = true;
                  }
                }

                if (useCpuGhostCells) {

                  GridVariableBase* srcvar = iter->validNeighbor->cloneType();
                  srcvar->copyPointer(*(iter->validNeighbor));
                  if (gpu_stats.active()) {
                    cerrLock.lock();
                    {
                      gpu_stats << myRankThread()
                          << " InitiateH2D() - The CPU has ghost cells needed for "
                          << curDependency->var->getName()
                          << " from patch "
                          << sourcePatch->getID() << " to "
                          << patchID << " within device " << destDeviceNum;
                      if (iter->validNeighbor->isForeign()) {
                        gpu_stats << ".  The ghost cells data is foreign.  ";
                      } else {
                        gpu_stats << ".  The ghost cells data is not foreign.  ";
                      }
                        gpu_stats << "The ghost variable is at (" << ghost_host_low.x()
                          << ", " << ghost_host_low.y()
                          << ", " << ghost_host_low.z()
                          << ") with size (" << ghost_host_size.x()
                          << ", " << ghost_host_size.y()
                          << ", " << ghost_host_size.z()
                          << ") with offset (" << ghost_host_offset.x()
                          << ", " << ghost_host_offset.y()
                          << ", " << ghost_host_offset.z() << ")"
                          << ".  The iter low is (" << iter->low.x()
                          << ", " << iter->low.y()
                          << ", " << iter->low.z()
                          << ") and iter high is (" << iter->high.x()
                          << ", " << iter->high.y()
                          << ", " << iter->high.z()
                          << ") the patch ID is " << patchID
                          << " and the neighbor variable has a virtual offset (" << virtualOffset.x()
                          << ", " << virtualOffset.y()
                          << ", " << virtualOffset.z() << ")"
                          << " and is at host address " << iter->validNeighbor->getBasePointer()
                          << endl;
                    }
                    cerrLock.unlock();
                  }

                  //If they came in via MPI, then these neighbors are foreign.
                  if (iter->validNeighbor->isForeign()) {


                    /*double * phi_data = new double[iter->validNeighbor->getDataSize()/sizeof(double)];
                     iter->validNeighbor->copyOut(phi_data);
                     int zlow = iter->low.z();
                     int ylow = iter->low.y();
                     int xlow = iter->low.x();
                     int zhigh = iter->high.z();
                     int yhigh = iter->high.y();
                     int xhigh = iter->high.x();
                     int ystride = ghost_host_size.y();
                     int xstride = ghost_host_size.x();

                     printf(" - initiateH2D Going to copy data between (%d, %d, %d) and (%d, %d, %d)\n", xlow, ylow, zlow, xhigh, yhigh, zhigh);
                     for (int k = zlow; k < zhigh; ++k) {
                       for (int j = ylow; j < yhigh; ++j) {
                         for (int i = xlow; i < xhigh; ++i) {
                           //cout << "(x,y,z): " << k << "," << j << "," << i << endl;
                           // For an array of [ A ][ B ][ C ], we can index it thus:
                           // (a * B * C) + (b * C) + (c * 1)
                           int idx = ((i-xlow) + ((j-ylow) * xstride) + ((k-zlow) * xstride * ystride));
                           printf(" -  initiateH2D phi(%d, %d, %d) is %1.6lf ptr %p base pointer %p idx %d\n", i, j, k, phi_data[idx], phi_data + idx, phi_data, idx);
                         }
                       }
                     }
                     delete[] phi_data;
                     */


                    //Prepare to tell the host-side GPU DW to allocated space for this variable.
                    dtask->getDeviceVars().add(sourcePatch, matlID, levelID, true,
                        ghost_host_size, srcvar->getDataSize(),
                        ghost_host_strides.x(), ghost_host_offset,
                        curDependency, Ghost::None, 0, destDeviceNum,
                        srcvar, GpuUtilities::sameDeviceSameMpiRank);

                    //let this Task GPU DW know about this staging array
                    dtask->getTaskVars().addTaskGpuDWStagingVar(sourcePatch, matlID, levelID, ghost_host_offset, ghost_host_size, ghost_host_strides.x(),
                        curDependency, sourceDeviceNum);

                    //See NOTE: above.  Scenarios occur in which the same source region is listed to send to two different patches.
                    //This task doesn't need to know about the same source twice.
                    /*if (!(dtask->getTaskVars().varAlreadyExists(curDependency->var, sourcePatch, matlID, levelID, curDependency->mapDataWarehouse()))) {
                      //let this Task GPU DW know about the source location.
                      dtask->getTaskVars().addTaskGpuDWVar(sourcePatch, matlID, levelID, ghost_host_strides.x(), curDependency, sourceDeviceNum);
                    } else {
                      if (gpu_stats.active()) {
                        cerrLock.lock();
                        gpu_stats << myRankThread()
                        << " InitiateH2D - Already had a task GPUDW Var for label " << curDependency->var->getName()
                        << " patch " << sourcePatch->getID()
                        << " matl " << matlID
                        << " level " << levelID
                        << endl;

                        cerrLock.unlock();
                      }
                    }*/

                    dtask->getGhostVars().add(curDependency->var,
                                   sourcePatch, patch, matlID, levelID,
                                   iter->validNeighbor->isForeign(), false,
                                   ghost_host_offset, ghost_host_size,
                                   iter->low, iter->high,
                                   elementDataSize, virtualOffset,
                                   destDeviceNum, destDeviceNum, -1, -1,   /* we're copying within a device, so destDeviceNum -> destDeviceNum */
                                   (Task::WhichDW) curDependency->mapDataWarehouse(),
                                   GpuUtilities::sameDeviceSameMpiRank);
                  } else {

                    //Copy this patch into the GPU. (If it came from another node
                    //via MPI, then we won't have the entire neighbor patch
                    //just the contiguous array(s) containing ghost cells data.
                    //If there are periodic boundary conditions, the source region of a patch
                    //could show up multiple times.)

                    //TODO: Instead of copying the entire patch for a ghost cell, we should just create a foreign var, copy
                    //a contiguous array of ghost cell data into that foreign var, then copy in that foreign var.
                    if (!dtask->getDeviceVars().varAlreadyExists(curDependency->var, sourcePatch, matlID, levelID, curDependency->mapDataWarehouse())) {

                      if (gpu_stats.active()) {
                         cerrLock.lock();
                         {
                           gpu_stats << myRankThread()
                               << " InitiateH2D() -  The CPU has ghost cells needed, use it.  Patch "
                               << sourcePatch->getID() << " to "
                               << patchID
                               << " with size (" << host_size.x()
                               << ", " << host_size.y()
                               << ", " << host_size.z()
                               << ") with offset (" << ghost_host_offset.x()
                               << ", " << ghost_host_offset.y()
                               << ", " << ghost_host_offset.z() << ")"
                               << ".  The iter low is (" << iter->low.x()
                               << ", " << iter->low.y()
                               << ", " << iter->low.z()
                               << ") and iter high is (" << iter->high.x()
                               << ", " << iter->high.y()
                               << ", " << iter->high.z()
                               << ") and the neighbor variable has a virtual offset (" << virtualOffset.x()
                               << ", " << virtualOffset.y()
                               << ", " << virtualOffset.z() << ")"
                               << " with pointer " << srcvar->getBasePointer()
                               << endl;
                         }
                         cerrLock.unlock();
                       }
                      //Prepare to tell the host-side GPU DW to allocated space for this variable.
                      dtask->getDeviceVars().add(sourcePatch, matlID, levelID, false,
                          ghost_host_size, srcvar->getDataSize(),
                          ghost_host_strides.x(), ghost_host_offset,
                          curDependency, Ghost::None, 0, destDeviceNum,
                          srcvar, GpuUtilities::sameDeviceSameMpiRank);


                      //Prepare this task GPU DW for knowing about this variable on the GPU.
                      dtask->getTaskVars().addTaskGpuDWVar(sourcePatch, matlID, levelID,
                          ghost_host_strides.x(), curDependency, destDeviceNum);

                    } else {  //else the variable is not already in deviceVars
                      if (gpu_stats.active()) {
                        cerrLock.lock();
                        {
                          gpu_stats << myRankThread()
                                << " InitiateH2D() - The CPU has ghost cells needed but it's already been queued to go into the GPU.  Patch "
                                << sourcePatch->getID() << " to "
                                << patchID << " from device "
                                << sourceDeviceNum << " to device " << destDeviceNum
                                << ".  The ghost variable is at (" << ghost_host_low.x()
                                << ", " << ghost_host_low.y()
                                << ", " << ghost_host_low.z()
                                << ") with size (" << ghost_host_size.x()
                                << ", " << ghost_host_size.y()
                                << ", " << ghost_host_size.z()
                                << ") with offset (" << ghost_host_offset.x()
                                << ", " << ghost_host_offset.y()
                                << ", " << ghost_host_offset.z() << ")"
                                << ".  The iter low is (" << iter->low.x()
                                << ", " << iter->low.y()
                                << ", " << iter->low.z()
                                << ") and iter high is *" << iter->high.x()
                                << ", " << iter->high.y()
                                << ", " << iter->high.z()
                                << ") the patch ID is " << patchID
                                << " and the neighbor variable has a virtual offset (" << virtualOffset.x()
                                << ", " << virtualOffset.y()
                                << ", " << virtualOffset.z() << ")"
                                << endl;
                        }
                        cerrLock.unlock();
                      }
                    }

                    //Add in the upcoming GPU ghost cell copy.
                    dtask->getGhostVars().add(curDependency->var,
                                              sourcePatch, patch, matlID, levelID,
                                              false, false,
                                              ghost_host_offset, ghost_host_size,
                                              iter->low, iter->high,
                                              elementDataSize, virtualOffset,
                                              destDeviceNum, destDeviceNum, -1, -1,   /* we're copying within a device, so destDeviceNum -> destDeviceNum */
                                              (Task::WhichDW) curDependency->mapDataWarehouse(),
                                              GpuUtilities::sameDeviceSameMpiRank);

                  }
                } else if (useGpuGhostCells) {
                  if (gpu_stats.active()) {
                    cerrLock.lock();
                    {
                      gpu_stats << myRankThread()
                          << " InitiateH2D() - The CPU does not need to supply ghost cells from patch "
                          << sourcePatch->getID() << " to "
                          << patchID << " from device "
                          << sourceDeviceNum << " to device " << destDeviceNum
                          << endl;
                    }
                    cerrLock.unlock();
                  }

                  //If this task doesn't own this source patch, then we need to make sure
                  //the upcoming task data warehouse at least has knowledge of this GPU variable that
                  //already exists in the GPU.  So queue up to load the neighbor patch metadata into the
                  //task datawarehouse.
                  if (!patches->contains(sourcePatch)) {
                    if (iter->validNeighbor->isForeign()) {
                      dtask->getTaskVars().addTaskGpuDWStagingVar(sourcePatch, matlID, levelID,  ghost_host_offset, ghost_host_size, ghost_host_strides.x(),
                                                curDependency, sourceDeviceNum);
                    } else {
                      if (!(dtask->getTaskVars().varAlreadyExists(curDependency->var, sourcePatch, matlID, levelID,
                                                                  (Task::WhichDW) curDependency->mapDataWarehouse()))) {
                        dtask->getTaskVars().addTaskGpuDWVar(sourcePatch, matlID, levelID,
                                                             ghost_host_strides.x(), curDependency, sourceDeviceNum);
                      }
                    }
                  }

                  //Store the source and destination patch, and the range of the ghost cells
                  //A GPU kernel will use this collection to do all internal GPU ghost cell copies for
                  //that one specific GPU.
                  dtask->getGhostVars().add(curDependency->var,
                      sourcePatch, patch, matlID, levelID,
                      iter->validNeighbor->isForeign(), false,
                      ghost_host_offset, ghost_host_size,
                      iter->low, iter->high,
                      elementDataSize, virtualOffset,
                      destDeviceNum, destDeviceNum, -1, -1,   /* we're copying within a device, so destDeviceNum -> destDeviceNum */
                      (Task::WhichDW) curDependency->mapDataWarehouse(),
                      GpuUtilities::sameDeviceSameMpiRank);
                  if (gpu_stats.active()) {
                    cerrLock.lock();
                    {
                      gpu_stats << myRankThread()
                          << " InitaiteH2D() - Internal GPU ghost cell copy queued for "
                          << curDependency->var->getName().c_str() << " from patch "
                          << sourcePatch->getID() << " to patch " << patchID
                          << " using a variable starting at ("
                          << ghost_host_offset.x() << ", " << ghost_host_offset.y() << ", "
                          << ghost_host_offset.z() << ") and size ("
                          << ghost_host_size.x() << ", " << ghost_host_size.y() << ", "
                          << ghost_host_size.z() << ")"
                          << " copying from ("
                          << iter->low.x() << ", " << iter->low.y() << ", "
                          << iter->low.z() << ")" << " to (" << iter->high.x()
                          << ", " << iter->high.y() << ", " << iter->high.z()
                          << ")" << " with virtual patch offset ("
                          << virtualOffset.x() << ", " << virtualOffset.y()
                          << ", " << virtualOffset.z() << ")"
                          << "." << endl;
                    }
                    cerrLock.unlock();
                  }
                } else if (useGpuStaging) {
                  if (gpu_stats.active()) {
                    cerrLock.lock();
                    {
                      gpu_stats << myRankThread()
                          << " InitiateH2D() - Using source staging variable in the GPU "
                          << sourcePatch->getID() << " to "
                          << patchID << " from device "
                          << destDeviceNum << " to device " << destDeviceNum
                          << endl;
                    }
                    cerrLock.unlock();
                  }

                  //We checked previously and we know the staging var exists in the host-side GPU DW
                  //Make sure this task GPU DW knows about the staging var as well.
                  dtask->getTaskVars().addTaskGpuDWStagingVar(patch, matlID, levelID,
                      iter->low, iter->high - iter->low,
                      ghost_host_strides.x(), curDependency, destDeviceNum);

                  //Assume for now that the ghost cell region is also the exact same size as the
                  //staging var.  (If in the future ghost cell data is managed a bit better as
                  //it currently does on the CPU, then some ghost cell regions will be found
                  //*within* an existing staging var.  This is known to happen with Wasatch
                  //computations involving periodic boundary scenarios.)
                  dtask->getGhostVars().add(curDependency->var,
                      patch, patch,   /*We're merging the staging variable on in*/
                      matlID, levelID,
                      true, false,
                      iter->low,              /*Assuming ghost cell region is the variable size */
                      IntVector(iter->high.x() - iter->low.x(), iter->high.y() - iter->low.y(), iter->high.z() - iter->low.z()),
                      iter->low,
                      iter->high,
                      elementDataSize, virtualOffset,
                      destDeviceNum, destDeviceNum, -1, -1,   /* we're copying within a device, so destDeviceNum -> destDeviceNum */
                      (Task::WhichDW) curDependency->mapDataWarehouse(),
                      GpuUtilities::sameDeviceSameMpiRank);
                  if (gpu_stats.active()) {
                    cerrLock.lock();
                    {
                      gpu_stats << myRankThread()
                          << " InitaiteH2D() - Internal GPU ghost cell copy queued for "
                          << curDependency->var->getName().c_str() << " from patch "
                          << patchID << " staging true to patch " << patchID
                          << " staging false using a variable starting at ("
                          << iter->low.x() << ", " << iter->low.y() << ", "
                          << iter->low.z() << ") and size ("
                          << (iter->high.x() - iter->low.x()) << ", " << (iter->high.y() - iter->low.y()) << ", "
                          << (iter->high.z() - iter->low.z()) << ")"
                          << " copying from ("
                          << iter->low.x() << ", " << iter->low.y() << ", "
                          << iter->low.z() << ")" << " to (" << iter->high.x()
                          << ", " << iter->high.y() << ", " << iter->high.z()
                          << ")" << " with virtual patch offset ("
                          << virtualOffset.x() << ", " << virtualOffset.y()
                          << ", " << virtualOffset.z() << ")"
                          << "." << endl;
                    }
                    cerrLock.unlock();
                  }
                }
              }
            }
          }

        } else if (allocated && !allocatedCorrectSize) {
          //At the moment this isn't allowed. So it does an exit(-1).  There are two reasons for this.
          //First, the current CPU system always needs to "resize" variables when ghost cells are required.
          //Essentially the variables weren't created with room for ghost cells, and so room  needs to be created.
          //This step can be somewhat costly (I've seen a benchmark where it took 5% of the total computation time).
          //And at the moment this hasn't been coded to resize on the GPU.  It would require an additional step and
          //synchronization to make it work.
          //The second reason is with concurrency.  Suppose a patch that CPU thread A own needs
          //ghost cells from a patch that CPU thread B owns.
          //A can recognize that B's data is valid on the GPU, and so it stores for the future to copy B's
          //data on in.  Meanwhile B notices it needs to resize.  So A could start trying to copy in B's
          //ghost cell data while B is resizing its own data.
          //I believe both issues can be fixed with proper checkpoints.  But in reality
          //we shouldn't be resizing variables on the GPU, so this event should never happenn.
          gpudw->remove(curDependency->var->getName().c_str(), patchID, matlID, levelID);
          cerr
              << "Resizing of GPU grid vars not implemented at this time.  "
              << "For the GPU, computes need to be declared with scratch computes to have room for ghost cells."
              << "Requested var of size (" << host_size.x() << ", " << host_size.y() << ", " << host_size.z() << ") "
              << "with offset (" << low.x() << ", " << low.y() << ", " << low.z() << ")" << endl;
          exit(-1);

        } else if (!allocated
            || (allocated && allocatedCorrectSize && !validOnGpu)) {

          //It's either not on the GPU, or space exists on the GPU for it but it is invalid.
          //Either way, gather all ghost cells host side (if needed), then queue the data to be
          //copied in H2D.  If the data doesn't exist in the GPU, then the upcoming allocateAndPut
          //will allocate space for it.  Otherwise if it does exist on the GPU, the upcoming
          //allocateAndPut will notice that and simply configure it to reuse the pointer.

          if (type == TypeDescription::CCVariable
              || type == TypeDescription::NCVariable
              || type == TypeDescription::SFCXVariable
              || type == TypeDescription::SFCYVariable
              || type == TypeDescription::SFCZVariable) {

            // Since the var is on the host, this will manage ghost cells by
            // creating a host var, rewindowing it, then copying in the regions needed for the ghost cells.
            // If this is the case, then ghost cells for this specific instance of this var is completed.
            //Note: Unhandled scenario:  If the adjacent patch is only in the GPU, this code doesn't gather it.

            GridVariableBase* gridVar = dynamic_cast<GridVariableBase*>(curDependency->var->typeDescription()->createInstance());
            if (uses_SHRT_MAX) {
              IntVector domainLo_EC, domainHi_EC;
              level->findCellIndexRange(domainLo_EC, domainHi_EC); // including extraCells
              dw->getRegion(*gridVar, curDependency->var, matlID, level, domainLo_EC, domainHi_EC, true);
            } else {
              dw->getGridVar(*gridVar, curDependency->var, matlID, patch, curDependency->gtype, curDependency->numGhostCells);
            }
            IntVector host_low, host_high, host_offset, host_size, host_strides;
            gridVar->getSizes(host_low, host_high, host_offset, host_size, host_strides);
            //Queue this CPU var to go into the host-side GPU DW.
            //Also queue that this GPU DW var should also be found in this tasks's Task DW.
            dtask->getDeviceVars().add(patch, matlID, levelID, false, host_size,
                gridVar->getDataSize(), host_strides.x(), host_offset,
                curDependency, curDependency->gtype, curDependency->numGhostCells,
                deviceIndex,
                gridVar, GpuUtilities::sameDeviceSameMpiRank);
            dtask->getTaskVars().addTaskGpuDWVar(patch, matlID, levelID,
                host_strides.x(), curDependency,
                deviceIndex);
            if (gatherGhostCells) {
              dtask->getVarsToBeGhostReady().addVarToBeGhostReady(dtask->getName(), patch, matlID, levelID,
                                                                  curDependency,
                                                                  deviceIndex);
            }
          } else if (type == TypeDescription::PerPatch) {
            PerPatchBase* patchVar = dynamic_cast<PerPatchBase*>(curDependency->var->typeDescription()->createInstance());
            dw->get(*patchVar, curDependency->var, matlID, patch);
            dtask->getDeviceVars().add(patch, matlID, levelID, patchVar->getDataSize(),
                elementDataSize, curDependency,
                deviceIndex,
                patchVar, GpuUtilities::sameDeviceSameMpiRank);
            dtask->getTaskVars().addTaskGpuDWVar(patch, matlID, levelID,
                elementDataSize, curDependency,
                deviceIndex);
          } else if (type == TypeDescription::ReductionVariable) {
            levelID = -1;
            ReductionVariableBase* reductionVar = dynamic_cast<ReductionVariableBase*>(curDependency->var->typeDescription()->createInstance());
            dw->get(*reductionVar, curDependency->var, patch->getLevel(), matlID);
            dtask->getDeviceVars().add(patch, matlID, levelID, reductionVar->getDataSize(),
                elementDataSize, curDependency,
                deviceIndex,
                reductionVar, GpuUtilities::sameDeviceSameMpiRank);
            dtask->getTaskVars().addTaskGpuDWVar(patch, matlID, levelID,
                elementDataSize, curDependency,
                deviceIndex);
          }
          else {
            cerr << "UnifiedScheduler::initiateH2D(), unsupported variable type for computes variable "
                               << curDependency->var->getName() << endl;
          }
        }

      } else if (curDependency->deptype == Task::Computes) {
        //compute the amount of space the host needs to reserve on the GPU for this variable.

        if (gpu_stats.active()) {
          cerrLock.lock();
          {
            gpu_stats << myRankThread()
                << " InitiateH2D() - The CPU is allocating computes space"
                << " for " << curDependency->var->getName()
                << " patch " << patchID
                << " material " << matlID
                << " level " << levelID
                << " on device "
                << deviceIndex
                << endl;
          }
          cerrLock.unlock();
        }

        if (type == TypeDescription::PerPatch) {
          //For PerPatch, it's not a mesh of variables, it's just a single variable, so elementDataSize is the memSize.
          size_t memSize = elementDataSize;

          PerPatchBase* patchVar = dynamic_cast<PerPatchBase*>(curDependency->var->typeDescription()->createInstance());
          dw->put(*patchVar, curDependency->var, matlID, patch);
          delete patchVar;
          patchVar = NULL;
          dtask->getDeviceVars().add(patch, matlID, levelID,
              memSize, elementDataSize, curDependency,
              deviceIndex,
              NULL, GpuUtilities::sameDeviceSameMpiRank);
          dtask->getTaskVars().addTaskGpuDWVar(patch, matlID, levelID,
              elementDataSize, curDependency,
              deviceIndex);
        } else if (type == TypeDescription::ReductionVariable) {
          //For ReductionVariable, it's not a mesh of variables, it's just a single variable, so elementDataSize is the memSize.
          size_t memSize = elementDataSize;
          //ReductionVariableBase* reductionVar = dynamic_cast<ReductionVariableBase*>(curDependency->var->typeDescription()->createInstance());
          //dw->put(*reductionVar, curDependency->var, patch->getLevel(), matlID);
          // delete reductionVar;
          //reductionVar = NULL;
          dtask->getDeviceVars().add(patch, matlID, levelID,
              memSize, elementDataSize, curDependency,
              deviceIndex,
              NULL, GpuUtilities::sameDeviceSameMpiRank);
          dtask->getTaskVars().addTaskGpuDWVar(patch, matlID, levelID,
              elementDataSize, curDependency,
              deviceIndex);

        } else if (type == TypeDescription::CCVariable
            || type == TypeDescription::NCVariable
            || type == TypeDescription::SFCXVariable
            || type == TypeDescription::SFCYVariable
            || type == TypeDescription::SFCZVariable) {

          //TODO, not correct if scratch ghost is used?

          GridVariableBase* gridVar =
              dynamic_cast<GridVariableBase*>(curDependency->var->typeDescription()->createInstance());

          //Get variable size. Scratch computes means we need to factor that in when computing the size.
          Patch::VariableBasis basis = Patch::translateTypeToBasis(
              curDependency->var->typeDescription()->getType(), false);
          IntVector lowIndex, highIndex;
          IntVector lowOffset, highOffset;

          Patch::getGhostOffsets(
              gridVar->virtualGetTypeDescription()->getType(),
              curDependency->gtype, curDependency->numGhostCells, lowOffset,
              highOffset);
          patch->computeExtents(basis,
              curDependency->var->getBoundaryLayer(), lowOffset, highOffset,
              lowIndex, highIndex);
          size_t memSize = (highIndex.x() - lowIndex.x())
              * (highIndex.y() - lowIndex.y())
              * (highIndex.z() - lowIndex.z()) * elementDataSize;

          //Even though it's only on the device, we to create space for the var on the host.
          //This makes it easy in case we ever need to perform any D2Hs.
          //TODO: Is this even needed?  InitiateD2H seems to create the var when it's needed host side.
          //So what happens if we remove this?
          //I know it crashes if this is removed, but it seems like that crash should be fixed
    const bool finalized = dw->isFinalized();
    if (finalized) {
      dw->unfinalize();
    }

          dw->allocateAndPut(*gridVar, curDependency->var, matlID,
              patch, curDependency->gtype,
              curDependency->numGhostCells);


          if (finalized) {
            dw->refinalize();
          }


          delete gridVar;
          gridVar = NULL;
          dtask->getDeviceVars().add(patch, matlID, levelID, false, host_size,
              memSize, elementDataSize, low, curDependency,
              curDependency->gtype, curDependency->numGhostCells,
              deviceIndex,
              NULL, GpuUtilities::sameDeviceSameMpiRank);
          dtask->getTaskVars().addTaskGpuDWVar(patch, matlID, levelID,
              elementDataSize, curDependency,
              deviceIndex);
        } else {
          cerr << "UnifiedScheduler::initiateH2D(), unsupported variable type for computes variable "
               << curDependency->var->getName() << endl;
        }
      }
    }
  }

  // We've now gathered up all possible things that need to go on the device.  Copy it over.

  //gpu_stats << myRankThread() << " Calling createTaskGpuDWs for " << dtask->getName() << endl;
  createTaskGpuDWs(dtask);

  //gpu_stats << myRankThread() << " Calling prepareDeviceVars for " << dtask->getName() << endl;
  prepareDeviceVars(dtask);

  //gpu_stats << myRankThread() << " Calling prepareTaskVarsIntoTaskDW for " << dtask->getName() << endl;
  prepareTaskVarsIntoTaskDW(dtask);

  //gpu_stats << myRankThread() << " Calling prepareGhostCellsIntoTaskDW for " << dtask->getName() << endl;
  prepareGhostCellsIntoTaskDW(dtask);

}


//______________________________________________________________________
//
void
UnifiedScheduler::prepareDeviceVars( DetailedTask * dtask ) {

  bool isStaging = false;

  string taskID = dtask->getName();
  //std::set<unsigned int> deviceNums = dtask->getDeviceNums();
  //for (std::set<unsigned int>::const_iterator deviceNums_it = deviceNums.begin(); deviceNums_it != deviceNums.end(); ++deviceNums_it) {
    isStaging = false;
    //Because maps are unordered, it is possible a staging var could be inserted before the regular
    //var exists.  So  just loop twice, once when all staging is false, then loop again when all
    //staging is true
    for (int i = 0; i < 2; ++i) {
      //Get all data in the GPU, and store it on the GPU Data Warehouse on the host, as only it
      //is responsible for management of data.  So this processes the previously collected deviceVars.
      multimap<GpuUtilities::LabelPatchMatlLevelDw, DeviceGridVariableInfo> & varMap = dtask->getDeviceVars().getMap();

      for (multimap<GpuUtilities::LabelPatchMatlLevelDw,
          DeviceGridVariableInfo>::iterator it = varMap.begin();
          it != varMap.end(); ++it) {
        int whichGPU = it->second.whichGPU;
        int dwIndex = it->second.dep->mapDataWarehouse();
        GPUDataWarehouse* gpudw = dws[dwIndex]->getGPUDW(whichGPU);

        if (it->second.staging == isStaging) {

          if (dtask->getDeviceVars().getTotalVars(whichGPU, dwIndex) >= 0) {
            //No contiguous arrays section

            void* device_ptr = NULL;  // device base pointer to raw data


            IntVector offset = it->second.offset;
            IntVector size = it->second.sizeVector;
            IntVector low = offset;
            IntVector high = offset + size;

            //Allocate the vars if needed.  If they've already been allocated, then
            //this simply sets the var to reuse the existing pointer.
            switch (it->second.dep->var->typeDescription()->getType()) {
            case TypeDescription::PerPatch: {
              GPUPerPatchBase* patchVar = OnDemandDataWarehouse::createGPUPerPatch(it->second.sizeOfDataType);
              gpudw->allocateAndPut(*patchVar,
                  it->second.dep->var->getName().c_str(), it->first.patchID,
                  it->first.matlIndx, it->first.levelIndx,
                  it->second.sizeOfDataType);
              device_ptr = patchVar->getVoidPointer();
              delete patchVar;
              break;
            }
            case TypeDescription::ReductionVariable: {

              GPUReductionVariableBase* reductionVar = OnDemandDataWarehouse::createGPUReductionVariable(it->second.sizeOfDataType);
              gpudw->allocateAndPut(*reductionVar,
                  it->second.dep->var->getName().c_str(), it->first.patchID,
                  it->first.matlIndx, it->first.levelIndx,
                  it->second.sizeOfDataType);
              device_ptr = reductionVar->getVoidPointer();
              delete reductionVar;
              break;
            }
            case TypeDescription::CCVariable:
            case TypeDescription::NCVariable:
            case TypeDescription::SFCXVariable:
            case TypeDescription::SFCYVariable:
            case TypeDescription::SFCZVariable: {
              GridVariableBase* tempGhostvar =
                  dynamic_cast<GridVariableBase*>(it->second.dep->var->typeDescription()->createInstance());
              tempGhostvar->allocate(low, high);
              delete tempGhostvar;

              GPUGridVariableBase* device_var = OnDemandDataWarehouse::createGPUGridVariable(it->second.sizeOfDataType);
              if (gpudw) {

                gpudw->allocateAndPut(
                    *device_var, it->second.dep->var->getName().c_str(),
                    it->first.patchID, it->first.matlIndx, it->first.levelIndx, it->second.staging,
                    make_int3(low.x(), low.y(), low.z()),
                    make_int3(high.x(), high.y(), high.z()),
                    it->second.sizeOfDataType,
                    (GPUDataWarehouse::GhostType) (it->second.gtype),
                    it->second.numGhostCells);


              } else {
                if (gpu_stats.active()) {
                  cerrLock.lock();
                  gpu_stats << myRankThread()
                      << " prepareDeviceVars() - ERROR - No task data warehouse found for device #" << it->second.whichGPU
                      << " and dwindex " <<  dwIndex << endl;
                  cerrLock.unlock();
                }
                SCI_THROW(InternalError("No gpu data warehouse found\n",__FILE__, __LINE__));
              }
              device_ptr = device_var->getVoidPointer();
              delete device_var;
              break;
            }
            default: {
              cerrLock.lock();
              cerr << "This variable's type is not supported." << endl;
              cerrLock.unlock();
            }
            }

            //If it's a requires, copy the data on over.  If it's a computes, leave it as allocated but unused space.
            if (it->second.dep->deptype == Task::Requires) {
              if (!device_ptr) {
                std::cerr << "ERROR: GPU variable's device pointer was NULL"
                    << std::endl;
                throw ProblemSetupException(
                    "ERROR: GPU variable's device pointer was NULL",
                    __FILE__, __LINE__);
              }
              cudaStream_t* stream = dtask->getCudaStreamForThisTask(whichGPU);
              OnDemandDataWarehouse::uintahSetCudaDevice(whichGPU);
              switch (it->second.dep->var->typeDescription()->getType()) {
              case TypeDescription::PerPatch: {
                if (it->second.dest == GpuUtilities::sameDeviceSameMpiRank) {

                  CUDA_RT_SAFE_CALL(
                      cudaMemcpyAsync(device_ptr,
                          dynamic_cast<PerPatchBase*>(it->second.var)->getBasePointer(),
                          it->second.varMemSize, cudaMemcpyHostToDevice, *stream));
                  delete it->second.var;
                }
                break;
              }
              case TypeDescription::ReductionVariable: {
                if (it->second.dest == GpuUtilities::sameDeviceSameMpiRank) {

                  CUDA_RT_SAFE_CALL(
                      cudaMemcpyAsync(device_ptr,
                          dynamic_cast<ReductionVariableBase*>(it->second.var)->getBasePointer(),
                          it->second.varMemSize, cudaMemcpyHostToDevice, *stream));
                  delete it->second.var;
                }
                break;
              }
              case TypeDescription::CCVariable:
              case TypeDescription::NCVariable:
              case TypeDescription::SFCXVariable:
              case TypeDescription::SFCYVariable:
              case TypeDescription::SFCZVariable: {
                if (gpu_stats.active()) {
                  cerrLock.lock();
                  gpu_stats << myRankThread()
                      << " prepareDeviceVars() - Checking for copy" << whichGPU
                      << " data for variable " << it->first.label
                      << " patch: " << it->first.patchID
                      << " material: " << it->first.matlIndx
                      << " level: " << it->first.levelIndx
                      << " staging: "  << it->second.staging;
                      if (it->second.staging) {
                        gpu_stats << " offset (" << low.x()
                            << ", " << low.y()
                            << ", " << low.z()
                            << ") and size (" << size.x()
                            << ", " << size.y()
                            << ", " << size.z();
                      }
                      gpu_stats << " destination enum is " << it->second.dest << endl;
                  cerrLock.unlock();
                }
                if (it->second.dest == GpuUtilities::sameDeviceSameMpiRank) {
                  bool performCopy = false;
                  if (!it->second.staging) {
                    performCopy = gpudw->testAndSetCopyingIntoGPU(it->second.dep->var->getName().c_str(),
                                        it->first.patchID, it->first.matlIndx, it->first.levelIndx);
                  } else {
                    performCopy = gpudw->testAndSetCopyingIntoGPUStaging(it->second.dep->var->getName().c_str(),
                                                            it->first.patchID, it->first.matlIndx, it->first.levelIndx,
                                                            make_int3(low.x(), low.y(), low.z()),
                                                            make_int3(size.x(), size.y(), size.z()));
                  }
                  if (performCopy) {
                    if (gpu_stats.active()) {
                      cerrLock.lock();
                      gpu_stats << myRankThread()
                          << " prepareDeviceVars() - Copying into GPU #" << whichGPU
                          << " data for variable " << it->first.label
                          << " patch: " << it->first.patchID
                          << " material: " << it->first.matlIndx
                          << " level: " << it->first.levelIndx
                          << " staging: "  << it->second.staging;
                          if (it->second.staging) {
                            gpu_stats << " offset (" << low.x()
                                << ", " << low.y()
                                << ", " << low.z()
                                << ") and size (" << size.x()
                                << ", " << size.y()
                                << ", " << size.z();
                          }
                          gpu_stats << " from host address " << dynamic_cast<GridVariableBase*>(it->second.var)->getBasePointer()
                          << " to device address " << device_ptr
                          << " into REQUIRES GPUDW " << endl;
                      cerrLock.unlock();
                    }
                    CUDA_RT_SAFE_CALL(cudaMemcpyAsync(device_ptr,
                                              dynamic_cast<GridVariableBase*>(it->second.var)->getBasePointer(),
                                              it->second.varMemSize, cudaMemcpyHostToDevice, *stream));

                    //Tell this task that we're managing the copies for this variable.
                    dtask->getVarsBeingCopiedByTask().getMap().insert(
                        std::pair<GpuUtilities::LabelPatchMatlLevelDw, DeviceGridVariableInfo>(it->first, it->second));
                  }

                  //Now that this requires grid variable is copied onto the device,
                  //we no longer need to hold onto this particular reference of the host side
                  //version of it.  So go ahead and remove our reference to it.
                  delete it->second.var;
                } else if (it->second.dest == GpuUtilities::anotherDeviceSameMpiRank ||
                           it->second.dest == GpuUtilities::anotherMpiRank) {
                  //We're not performing a host to GPU copy.  This is just prepare a staging var.
                  //So it is a a gpu normal var to gpu staging var copy.
                  //It is to prepare for upcoming GPU to host (MPI) or GPU to GPU copies.
                  //Tell this task that we're managing the copies for this variable.
                  dtask->getVarsBeingCopiedByTask().getMap().insert(
                      std::pair<GpuUtilities::LabelPatchMatlLevelDw, DeviceGridVariableInfo>(it->first, it->second));
                }
                }
                break;
              default:
                {
                  cerrLock.lock();
                  cerr << "Variable " << it->second.dep->var->getName() << " is of a type that is not supported on GPUs yet." << endl;
                  cerrLock.unlock();
                }
              }

            }

            //TODO: Materials have to go on as well.  Should they go on contiguously?
            //put materials on the datawarehouse
            //for (int dwIndex = 0; dwIndex < (int)dws.size(); dwIndex++) {
            //  if (dtask->getDeviceVars().getSizeForDataWarehouse(dwIndex) > 0) {
            //    dws[dwIndex]->getGPUDW()->putMaterials(materialsNames);
            //  }
            //}

          } else {

            /*
             //Create a large array on the host.  We'll copy this one array over, instead of copying
             //many little individual arrays for each label/material/patch.
             //Those arrays can copy fast, but the API call overhead to prepare a copy is huge.
             //So we're trying to reduce the amount of device API calls.

             //This array will also contain blank space for computes variables.

             //TODO: Support perPatch variables here?

             //Get a task ID.  All grid variables that are about to be placed in a device array belong to
             //a task/patch/timestep tuple ID.  So we'll use this ID to let the GPU DW scheduler know which
             //array to work with
             //TODO: If there are two threads and two patches, they share the same TaskID.
             string taskID = dtask->getName();
             for (int dwIndex = 0; dwIndex < (int)dws.size(); dwIndex++) {
             if (dtask->getDeviceVars().getSizeForDataWarehouse(dwIndex) > 0) {
             //create a contiguous array on the host and on the device for this datawarehouse.
             //TODO: Make it work for multiple GPUS/multiple
             dws[dwIndex]->getGPUDW()->allocate(taskID.c_str(), dtask->getDeviceVars().getSizeForDataWarehouse(dwIndex));

             if (gpu_stats.active()) {
             cerrLock.lock();
             {
             gpu_stats << "Allocated buffer of size " << dtask->getDeviceVars().getSizeForDataWarehouse(dwIndex) << endl;
             }
             cerrLock.unlock();
             }

             //put materials on the datawarehouse
             dws[dwIndex]->getGPUDW()->putMaterials(materialsNames);
             }
             }

             //first loop through all data that needs to be copied to the device.
             for (unsigned int i = 0; i < dtask->getDeviceVars().numItems(); ++i ) {
             if (dtask->getDeviceVars().getDependency(i)->deptype == Task::Requires) {
             int dwIndex =  dtask->getDeviceVars().getDependency(i)->mapDataWarehouse();
             OnDemandDataWarehouseP dw = dws[dwIndex];
             const Task::Dependency* dep = dtask->getDeviceVars().getDependency(i);
             IntVector offset = dtask->getDeviceVars().getOffset(i);
             IntVector size = dtask->getDeviceVars().getSizeVector(i);
             IntVector low = offset;
             IntVector high = offset + size;


             switch (dtask->getDeviceVars().getSizeOfDataType(i)) {
             case sizeof(int) : {
             GPUGridVariable<int> device_var;
             dws[dwIndex]->getGPUDW()->putContiguous(device_var, taskID.c_str(), dep->var->getName().c_str(),
             dtask->getDeviceVars().getPatchPointer(i)->getID(), dtask->getDeviceVars().getMatlIndx(i), 0,
             make_int3(low.x(), low.y(), low.z()), make_int3(high.x(), high.y(), high.z()), dtask->getDeviceVars().getSizeOfDataType(i), dynamic_cast<GridVariableBase*>(dtask->getDeviceVars().getVar(i)), true);

             break;
             } case sizeof(double) : {
             GPUGridVariable<double> device_var;
             dws[dwIndex]->getGPUDW()->putContiguous(device_var, taskID.c_str(), dep->var->getName().c_str(),
             dtask->getDeviceVars().getPatchPointer(i)->getID(), dtask->getDeviceVars().getMatlIndx(i), 0,
             make_int3(low.x(), low.y(), low.z()), make_int3(high.x(), high.y(), high.z()), dtask->getDeviceVars().getSizeOfDataType(i), dynamic_cast<GridVariableBase*>(dtask->getDeviceVars().getVar(i)), true);

             break;
             } case sizeof(GPUStencil7) : {
             GPUGridVariable<GPUStencil7> device_var;
             dws[dwIndex]->getGPUDW()->putContiguous(device_var, taskID.c_str(), dep->var->getName().c_str(),
             dtask->getDeviceVars().getPatchPointer(i)->getID(), dtask->getDeviceVars().getMatlIndx(i), 0,
             make_int3(low.x(), low.y(), low.z()), make_int3(high.x(), high.y(), high.z()), dtask->getDeviceVars().getSizeOfDataType(i), dynamic_cast<GridVariableBase*>(dtask->getDeviceVars().getVar(i)), true);

             break;
             } default : {
             SCI_THROW(InternalError("Unsupported GPUGridVariable type: " + dep->var->getName(), __FILE__, __LINE__));
             }
             }

             //Now that this requires grid variable is copied onto the device,
             //we no longer need to hold onto this particular reference of the host side
             //version of it.  So go ahead and remove our reference to it. (Note: this was removed
             //as in some situations it was the last remaining reference to it, so it would remove it
             //for good, but the data warehouse was not aware that it was gone and tried to delete
             //it again when scrubbing.
             delete dtask->getDeviceVars().getVar(i);
             }
             }

             //now loop through the computes data where we just need to assign space (but not copy) on the device.
             for (unsigned int i = 0; i < dtask->getDeviceVars().numItems(); ++i ) {
             if (dtask->getDeviceVars().getDependency(i)->deptype != Task::Requires) {
             int dwIndex =  dtask->getDeviceVars().getDependency(i)->mapDataWarehouse();
             OnDemandDataWarehouseP dw = dws[dwIndex];
             const Task::Dependency* dep = dtask->getDeviceVars().getDependency(i);
             IntVector offset = dtask->getDeviceVars().getOffset(i);
             IntVector size = dtask->getDeviceVars().getSizeVector(i);
             IntVector low = offset;
             IntVector high = offset + size;
             switch (dtask->getDeviceVars().getSizeOfDataType(i)) {
             case sizeof(int) : {
             GPUGridVariable<int> device_var;
             dws[dwIndex]->getGPUDW()->putContiguous(device_var, taskID.c_str(), dep->var->getName().c_str(),
             dtask->getDeviceVars().getPatchPointer(i)->getID(), dtask->getDeviceVars().getMatlIndx(i), 0,
             make_int3(low.x(), low.y(), low.z()), make_int3(high.x(), high.y(), high.z()), dtask->getDeviceVars().getSizeOfDataType(i), NULL, false);

             break;
             } case sizeof(double) : {
             GPUGridVariable<double> device_var;
             dws[dwIndex]->getGPUDW()->putContiguous(device_var, taskID.c_str(), dep->var->getName().c_str(),
             dtask->getDeviceVars().getPatchPointer(i)->getID(), dtask->getDeviceVars().getMatlIndx(i), 0,
             make_int3(low.x(), low.y(), low.z()), make_int3(high.x(), high.y(), high.z()), dtask->getDeviceVars().getSizeOfDataType(i), NULL, false);

             break;
             } case sizeof(GPUStencil7) : {
             GPUGridVariable<GPUStencil7> device_var;
             dws[dwIndex]->getGPUDW()->putContiguous(device_var, taskID.c_str(), dep->var->getName().c_str(),
             dtask->getDeviceVars().getPatchPointer(i)->getID(), dtask->getDeviceVars().getMatlIndx(i), 0,
             make_int3(low.x(), low.y(), low.z()), make_int3(high.x(), high.y(), high.z()), dtask->getDeviceVars().getSizeOfDataType(i), NULL, false);

             break;
             } default : {
             SCI_THROW(InternalError("Unsupported GPUGridVariable type: " + dep->var->getName(), __FILE__, __LINE__));
             }
             }
             //At this point, space is still allocated on the host side for this computes grid
             //variable.  Do not delete dtask->getDeviceVars().getGridVar(i) yet, we want to leave a
             //reference open and space open until we copy it back.  In processD2HCopies() we
             //remove the reference to it, possibly deleting it if the reference count is zero.
             }
             }

             for (int dwIndex = 0; dwIndex < (int)dws.size(); dwIndex++) {
             if (dtask->getDeviceVars().getSizeForDataWarehouse(dwIndex) > 0) {
             //copy it to the device.  Any data warehouse that has requires should be doing copies here.
             //copyDataHostToDevice knows how to copy only the initialized portion of the array, and not
             //everything including the uninitialized garbage data in the array.
             cudaError_t retVal = dws[dwIndex]->getGPUDW()->copyDataHostToDevice(taskID.c_str(), dtask->getCudaStreamForThisTask());
             if (retVal != cudaSuccess) {
             SCI_THROW(InternalError("Detected CUDA kernel execution failure on Task: "+ dtask->getName(), __FILE__, __LINE__));
             }
             }
             }
             */
          } //end if for contiguous or not if block > 0
        }
      }
      isStaging = !isStaging;
    }
  //}
}


//______________________________________________________________________
//
void
UnifiedScheduler::prepareTaskVarsIntoTaskDW( DetailedTask * dtask ) {
  //Copy all task variables metadata into the Task GPU DW.
  //All necessary metadata information must already exist in the host-side GPU DWs.

  multimap<GpuUtilities::LabelPatchMatlLevelDw, DeviceGridVariableInfo> & taskVarMap = dtask->getTaskVars().getMap();

  //Because maps are unordered, it is possible a staging var could be inserted before the regular
  //var exists.  So just loop twice, once when all staging is false, then loop again when all
  //staging is true
  bool isStaging = false;

  for (int i = 0; i < 2; ++i) {
    for (multimap<GpuUtilities::LabelPatchMatlLevelDw,
        DeviceGridVariableInfo>::const_iterator it = taskVarMap.begin();
        it != taskVarMap.end(); ++it) {
      //if isStaging is false, do the non-staging vars
      //then if isStaging is true, then do the staging vars.
      //isStaging is flipped after the first iteration of the i for loop.
      if (it->second.staging == isStaging) {
        switch (it->second.dep->var->typeDescription()->getType()) {
        case TypeDescription::PerPatch:
        case TypeDescription::ReductionVariable:
        case TypeDescription::CCVariable:
        case TypeDescription::NCVariable:
        case TypeDescription::SFCXVariable:
        case TypeDescription::SFCYVariable:
        case TypeDescription::SFCZVariable: {

          int dwIndex = it->second.dep->mapDataWarehouse();
          GPUDataWarehouse* gpudw = dws[dwIndex]->getGPUDW(it->second.whichGPU);
          int patchID = it->first.patchID;
          int matlIndx = it->first.matlIndx;
          int levelIndx = it->first.levelIndx;

          int3 offset;
          int3 size;
          if (it->second.staging) {
            offset = make_int3(it->second.offset.x(), it->second.offset.y(), it->second.offset.z());
            size = make_int3(it->second.sizeVector.x(), it->second.sizeVector.y(), it->second.sizeVector.z());
          } else {
            offset = make_int3(0,0,0);
            size = make_int3(0,0,0);
            if (gpu_stats.active()) {
              cerrLock.lock();
              gpu_stats << myRankThread()
                  << " prepareTaskVarsIntoTaskDW() - data for variable " << it->second.dep->var->getName()
                  << " patch: " << patchID
                  << " material: " << matlIndx
                  << " level: " << levelIndx
                  << endl;
              cerrLock.unlock();
            }
          }

          GPUDataWarehouse* taskgpudw = dtask->getTaskGpuDataWarehouse(it->second.whichGPU, (Task::WhichDW) dwIndex);
          if (taskgpudw) {
            taskgpudw->copyItemIntoTaskDW(gpudw, it->second.dep->var->getName().c_str(), patchID, matlIndx, levelIndx,
                                             it->second.staging, offset, size );

          } else {
            if (gpu_stats.active()) {
              cerrLock.lock();
              gpu_stats << myRankThread()
                  << " prepareTaskVarsIntoTaskDW() - ERROR - No task data warehouse found for device #" << it->second.whichGPU
                  << " and dwindex " <<  dwIndex << endl;
              cerrLock.unlock();
            }
            SCI_THROW(InternalError("No task data warehouse found\n",__FILE__, __LINE__));
          }

        }
        break;
        default: {
          cerrLock.lock();
          cerr << "Variable " << it->second.dep->var->getName() << " is of a type that is not supported on GPUs yet." << endl;
          cerrLock.unlock();
        }
        }
      }
    }
    isStaging = !isStaging;
  }

}


//______________________________________________________________________
//
void
UnifiedScheduler::prepareGhostCellsIntoTaskDW( DetailedTask * dtask ) {

  //Tell the Task DWs about any ghost cells they will need to process.
  //This adds in entries into the task DW's d_varDB which isn't a var, but is instead
  //metadata describing how to copy ghost cells between two vars listed in d_varDB.

  const map<GpuUtilities::GhostVarsTuple, DeviceGhostCellsInfo> & ghostVarMap = dtask->getGhostVars().getMap();
  for (map<GpuUtilities::GhostVarsTuple, DeviceGhostCellsInfo>::const_iterator it=ghostVarMap.begin(); it!=ghostVarMap.end(); ++it) {
    //If the neighbor is valid on the GPU, we just send in from and to coordinates
    //and call a kernel to copy those coordinates
    //If it's not valid on the GPU, we copy in the grid var and send in from and to coordinates
    //and call a kernel to copy those coordinates.

    //Peer to peer GPU copies will be handled elsewhere.
    //GPU to another MPI ranks will be handled elsewhere.
    if (it->second.dest != GpuUtilities::anotherDeviceSameMpiRank && it->second.dest != GpuUtilities::anotherMpiRank) {
      int dwIndex = it->first.dataWarehouse;

      //We can copy it manually internally within the device via a kernel.
      //This apparently goes faster overall
      IntVector varOffset = it->second.varOffset;
      IntVector varSize = it->second.varSize;
      IntVector ghost_low = it->first.sharedLowCoordinates;
      IntVector ghost_high = it->first.sharedHighCoordinates;
      IntVector virtualOffset = it->second.virtualOffset;
      if (gpu_stats.active()) {
        cerrLock.lock();
        {
          gpu_stats << myRankThread()
              << " prepareGhostCellsIntoTaskDW() - Preparing ghost cell upcoming copy for " << it->first.label
              << " matl " << it->first.matlIndx << " level " << it->first.levelIndx
              << " from patch " << it->second.sourcePatchPointer->getID() << " staging "  << it->second.sourceStaging
              << " to patch " << it->second.destPatchPointer->getID() << " staging "  << it->second.destStaging
              << " from device #" << it->second.sourceDeviceNum
              << " to device #" << it->second.destDeviceNum
              << " in the Task GPU DW " << dwIndex << endl;
        }
        cerrLock.unlock();
      }

      //Add in an entry into this Task DW's d_varDB which isn't a var, but is instead
      //metadata describing how to copy ghost cells between two vars listed in d_varDB.
      dtask->getTaskGpuDataWarehouse(it->second.sourceDeviceNum, (Task::WhichDW) dwIndex)->putGhostCell(
          it->first.label.c_str(), it->second.sourcePatchPointer->getID(),
          it->second.destPatchPointer->getID(), it->first.matlIndx, it->first.levelIndx,
          it->second.sourceStaging, it->second.destStaging,
          make_int3(varOffset.x(), varOffset.y(), varOffset.z()),
          make_int3(varSize.x(), varSize.y(), varSize.z()),
          make_int3(ghost_low.x(), ghost_low.y(), ghost_low.z()),
          make_int3(ghost_high.x(), ghost_high.y(), ghost_high.z()),
          make_int3(virtualOffset.x(), virtualOffset.y(), virtualOffset.z()));

    }
  }
}


//______________________________________________________________________
//
bool
UnifiedScheduler::ghostCellsProcessingReady( DetailedTask * dtask ) {

  const Task* task = dtask->getTask();

  //Gather up all possible dependents from requires and computes and remove duplicates (we don't want to
  //transfer some variables twice).
  //Note: A task can only run on one level at a time.  It could run multiple patches and multiple
  //materials, but a single task will never run multiple levels.
  std::map<labelPatchMatlDependency, const Task::Dependency*> vars;
  for (const Task::Dependency* dependantVar = task->getRequires(); dependantVar != 0; dependantVar = dependantVar->next) {
    constHandle<PatchSubset> patches = dependantVar->getPatchesUnderDomain(dtask->getPatches());
    constHandle<MaterialSubset> matls = dependantVar->getMaterialsUnderDomain(dtask->getMaterials());
    const int numPatches = patches->size();
    const int numMatls = matls->size();
    for (int i = 0; i < numPatches; ++i) {
      for (int j = 0; j < numMatls; ++j) {
        labelPatchMatlDependency lpmd(dependantVar->var->getName().c_str(), patches->get(i)->getID(), matls->get(j), Task::Requires);
        if (vars.find(lpmd) == vars.end()) {
          vars.insert(
              std::map<labelPatchMatlDependency, const Task::Dependency*>::value_type(lpmd, dependantVar));
        }
      }
    }
  }

  //No need to look up computes here.  Computes should never need ghost cells from neighbors.  That's contradictory to the idea of computes.
  /*
  for (const Task::Dependency* dependantVar = task->getComputes(); dependantVar != 0; dependantVar = dependantVar->next) {
    constHandle<PatchSubset> patches = dependantVar->getPatchesUnderDomain(dtask->getPatches());
    constHandle<MaterialSubset> matls = dependantVar->getMaterialsUnderDomain(dtask->getMaterials());
    const int numPatches = patches->size();
    const int numMatls = matls->size();
    for (int i = 0; i < numPatches; ++i) {
      for (int j = 0; j < numMatls; ++j) {
        labelPatchMatlDependency lpmd(dependantVar->var->getName().c_str(), patches->get(i)->getID(), matls->get(j), Task::Computes);
        if (vars.find(lpmd) == vars.end()) {
          vars.insert(
              std::map<labelPatchMatlDependency, const Task::Dependency*>::value_type(lpmd, dependantVar));
        }
      }
    }
  }*/

  //Go through each var, see if it's valid or valid with ghosts.
  std::map<labelPatchMatlDependency, const Task::Dependency*>::iterator varIter;
  for (varIter = vars.begin(); varIter != vars.end(); ++varIter) {
    const Task::Dependency* curDependency = varIter->second;

    constHandle<PatchSubset> patches = curDependency->getPatchesUnderDomain(dtask->getPatches());
    constHandle<MaterialSubset> matls = curDependency->getMaterialsUnderDomain(dtask->getMaterials());
    const int numPatches = patches->size();
    const int patchID = varIter->first.patchID;
    const Patch * patch = NULL;
    //const Patch * patch = grid->getPatchByID(patchID, levelID);
    for (int i = 0; i < numPatches; ++i) {
      if (patches->get(i)->getID() == patchID) {
        patch = patches->get(i);
      }
    }
    const Level* level = getLevel(patches.get_rep());
    int levelID = level->getID();
    if (curDependency->var->typeDescription()->getType() == TypeDescription::ReductionVariable) {
      levelID = -1;
    }
    const int matlID = varIter->first.matlIndex;
    const int dwIndex = curDependency->mapDataWarehouse();
    OnDemandDataWarehouseP dw = dws[dwIndex];
    GPUDataWarehouse* gpudw = dw->getGPUDW(GpuUtilities::getGpuIndexForPatch(patch));
    if (curDependency->deptype == Task::Requires) {
      if (curDependency->gtype != Ghost::None && curDependency->numGhostCells > 0) {
        if (!(gpudw->areAllStagingVarsValid(curDependency->var->getName().c_str(),patchID, matlID, levelID))) {
          if (gpu_stats.active()) {
            cerrLock.lock();
            {
              gpu_stats << myRankThread() << " UnifiedScheduler::ghostCellsProcessingReady() -"
                  // Task: " << dtask->getName()
                  << " Not all staging vars were ready for "
                  << curDependency->var->getName() << " patch " << patchID
                  << " material " << matlID << " level " << levelID << endl;
            }
            cerrLock.unlock();
          }
          return false;
        }
      }
    }
  }

  //if we got there, then everything must be ready to go.
  return true;
}


//______________________________________________________________________
//
bool
UnifiedScheduler::allHostVarsProcessingReady( DetailedTask * dtask ) {

  const Task* task = dtask->getTask();

  dtask->clearPreparationCollections();

  //Gather up all possible dependents from requires and computes and remove duplicates (we don't want to
  //transfer some variables twice).
  //Note: A task can only run on one level at a time.  It could run multiple patches and multiple
  //materials, but a single task will never run multiple levels.
  std::map<labelPatchMatlDependency, const Task::Dependency*> vars;
  for (const Task::Dependency* dependantVar = task->getRequires(); dependantVar != 0; dependantVar = dependantVar->next) {
    constHandle<PatchSubset> patches = dependantVar->getPatchesUnderDomain(dtask->getPatches());
    if (patches) {
      constHandle<MaterialSubset> matls = dependantVar->getMaterialsUnderDomain(dtask->getMaterials());
      const int numPatches = patches->size();
      const int numMatls = matls->size();
      for (int i = 0; i < numPatches; ++i) {
        for (int j = 0; j < numMatls; ++j) {
          labelPatchMatlDependency lpmd(dependantVar->var->getName().c_str(), patches->get(i)->getID(), matls->get(j), Task::Requires);
          if (vars.find(lpmd) == vars.end()) {
            vars.insert(
                std::map<labelPatchMatlDependency, const Task::Dependency*>::value_type(lpmd, dependantVar));
          }
        }
      }
    } else {
      //cout << myRankThread() << " In allHostVarsProcessingReady, no patches, task is " << dtask->getName() << endl;
    }
  }

  //No need to look up computes here.  Computes should never need ghost cells from neighbors.  That's contradictory to the idea of computes.
  /*
  for (const Task::Dependency* dependantVar = task->getComputes(); dependantVar != 0; dependantVar = dependantVar->next) {
    constHandle<PatchSubset> patches = dependantVar->getPatchesUnderDomain(dtask->getPatches());
    constHandle<MaterialSubset> matls = dependantVar->getMaterialsUnderDomain(dtask->getMaterials());
    const int numPatches = patches->size();
    const int numMatls = matls->size();
    for (int i = 0; i < numPatches; ++i) {
      for (int j = 0; j < numMatls; ++j) {
        labelPatchMatlDependency lpmd(dependantVar->var->getName().c_str(), patches->get(i)->getID(), matls->get(j), Task::Computes);
        if (vars.find(lpmd) == vars.end()) {
          vars.insert(
              std::map<labelPatchMatlDependency, const Task::Dependency*>::value_type(lpmd, dependantVar));
        }
      }
    }
  }*/

  //Go through each var, see if it's valid or valid with ghosts.
  std::map<labelPatchMatlDependency, const Task::Dependency*>::iterator varIter;
  for (varIter = vars.begin(); varIter != vars.end(); ++varIter) {
    const Task::Dependency* curDependency = varIter->second;

    constHandle<PatchSubset> patches = curDependency->getPatchesUnderDomain(dtask->getPatches());
    constHandle<MaterialSubset> matls = curDependency->getMaterialsUnderDomain(dtask->getMaterials());
    const int numPatches = patches->size();
    const int patchID = varIter->first.patchID;
    const Patch * patch = NULL;
    //const Patch * patch = grid->getPatchByID(patchID, levelID);
    for (int i = 0; i < numPatches; ++i) {
      if (patches->get(i)->getID() == patchID) {
        patch = patches->get(i);
      }
    }
    const Level* level = getLevel(patches.get_rep());
    int levelID = level->getID();
    if (curDependency->var->typeDescription()->getType() == TypeDescription::ReductionVariable) {
      levelID = -1;
    }
    const int matlID = varIter->first.matlIndex;
    const int dwIndex = curDependency->mapDataWarehouse();
    OnDemandDataWarehouseP dw = dws[dwIndex];
    GPUDataWarehouse* gpudw = dw->getGPUDW(GpuUtilities::getGpuIndexForPatch(patch));
    if (curDependency->deptype == Task::Requires) {
      if (gpudw->dwEntryExistsOnCPU(curDependency->var->getName().c_str(),patchID, matlID, levelID)) {
        //if (curDependency->gtype != Ghost::None && curDependency->numGhostCells > 0) {
        //  //it has ghost cells.
        //  if (!(gpudw->isValidWithGhostsOnGPU(curDependency->var->getName().c_str(),patchID, matlID, levelID))) {
        //    cout << "In allGPUVarsProcessingReady, it wasn't valid with ghosts for " << curDependency->var->getName().c_str() << " patch " << patchID << endl;
        //    return false;
        //  }
        //} else {
        //  //it doesn't have ghost cells
          if (!(gpudw->isValidOnCPU(curDependency->var->getName().c_str(),patchID, matlID, levelID))) {
            if (gpu_stats.active()) {
              cerrLock.lock();
              {
                gpu_stats << myRankThread() << " UnifiedScheduler::allHostVarsProcessingReady() -"
                    // Task: " << dtask->getName()
                    << " CPU Task: " << dtask->getName()
                    << " is not ready because this var isn't valid in host memory.  Var "
                    << curDependency->var->getName() << " patch " << patchID
                    << " material " << matlID << " level " << levelID << endl;
              }
              cerrLock.unlock();
            }
            return false;
          }
        //}
      }
    }

  }

  //if we got there, then everything must be ready to go.
  return true;
}


//______________________________________________________________________
//
bool
UnifiedScheduler::allGPUVarsProcessingReady( DetailedTask * dtask ) {

  const Task* task = dtask->getTask();

  dtask->clearPreparationCollections();

  //Gather up all possible dependents from requires and computes and remove duplicates (we don't want to
  //transfer some variables twice).
  //Note: A task can only run on one level at a time.  It could run multiple patches and multiple
  //materials, but a single task will never run multiple levels.
  std::map<labelPatchMatlDependency, const Task::Dependency*> vars;
  for (const Task::Dependency* dependantVar = task->getRequires(); dependantVar != 0; dependantVar = dependantVar->next) {
    constHandle<PatchSubset> patches = dependantVar->getPatchesUnderDomain(dtask->getPatches());
    constHandle<MaterialSubset> matls = dependantVar->getMaterialsUnderDomain(dtask->getMaterials());
    const int numPatches = patches->size();
    const int numMatls = matls->size();
    for (int i = 0; i < numPatches; ++i) {
      for (int j = 0; j < numMatls; ++j) {
        labelPatchMatlDependency lpmd(dependantVar->var->getName().c_str(), patches->get(i)->getID(), matls->get(j), Task::Requires);
        if (vars.find(lpmd) == vars.end()) {
          vars.insert(
              std::map<labelPatchMatlDependency, const Task::Dependency*>::value_type(lpmd, dependantVar));
        }
      }
    }
  }

  //No need to look up computes here.  Computes should never need ghost cells from neighbors.  That's contradictory to the idea of computes.
  /*
  for (const Task::Dependency* dependantVar = task->getComputes(); dependantVar != 0; dependantVar = dependantVar->next) {
    constHandle<PatchSubset> patches = dependantVar->getPatchesUnderDomain(dtask->getPatches());
    constHandle<MaterialSubset> matls = dependantVar->getMaterialsUnderDomain(dtask->getMaterials());
    const int numPatches = patches->size();
    const int numMatls = matls->size();
    for (int i = 0; i < numPatches; ++i) {
      for (int j = 0; j < numMatls; ++j) {
        labelPatchMatlDependency lpmd(dependantVar->var->getName().c_str(), patches->get(i)->getID(), matls->get(j), Task::Computes);
        if (vars.find(lpmd) == vars.end()) {
          vars.insert(
              std::map<labelPatchMatlDependency, const Task::Dependency*>::value_type(lpmd, dependantVar));
        }
      }
    }
  }*/

  //Go through each var, see if it's valid or valid with ghosts.
  std::map<labelPatchMatlDependency, const Task::Dependency*>::iterator varIter;
  for (varIter = vars.begin(); varIter != vars.end(); ++varIter) {
    const Task::Dependency* curDependency = varIter->second;

    constHandle<PatchSubset> patches = curDependency->getPatchesUnderDomain(dtask->getPatches());
    constHandle<MaterialSubset> matls = curDependency->getMaterialsUnderDomain(dtask->getMaterials());
    const int numPatches = patches->size();
    const int patchID = varIter->first.patchID;
    const Patch * patch = NULL;
    //const Patch * patch = grid->getPatchByID(patchID, levelID);
    for (int i = 0; i < numPatches; ++i) {
      if (patches->get(i)->getID() == patchID) {
        patch = patches->get(i);
      }
    }
    const Level* level = getLevel(patches.get_rep());
    int levelID = level->getID();
    if (curDependency->var->typeDescription()->getType() == TypeDescription::ReductionVariable) {
      levelID = -1;
    }
    const int matlID = varIter->first.matlIndex;
    const int dwIndex = curDependency->mapDataWarehouse();
    OnDemandDataWarehouseP dw = dws[dwIndex];
    GPUDataWarehouse* gpudw = dw->getGPUDW(GpuUtilities::getGpuIndexForPatch(patch));
    if (curDependency->deptype == Task::Requires) {
      if (curDependency->gtype != Ghost::None && curDependency->numGhostCells > 0) {
        //it has ghost cells.
        if (!(gpudw->isValidWithGhostsOnGPU(curDependency->var->getName().c_str(),patchID, matlID, levelID))) {
          return false;
        }
      } else {
        //it doesn't have ghost cells
        if (!(gpudw->isValidOnGPU(curDependency->var->getName().c_str(),patchID, matlID, levelID))) {
          return false;
        }
      }
    }

  }

  //if we got there, then everything must be ready to go.
  return true;
}


//______________________________________________________________________
//
void
UnifiedScheduler::markDeviceRequiresDataAsValid( DetailedTask * dtask ) {

  //This marks any Requires variable as valid that wasn't in the GPU but is now in the GPU.
  //If they were already in the GPU due to being computes from a previous time step, it was already
  //marked as valid.  So there is no need to do anything extra for them.
  //If they weren't in the GPU yet, this task or another task copied it in.
  //If it's another task that copied it in, we let that task manage it.
  //If it was this task, then those variables which this task copied in are found in varsBeingCopiedByTask.
  //By the conclusion of this method, some variables will be valid and awaiting ghost cells, some will
  //just be valid if they had no ghost cells, and some variables will be undetermined if they're being managed
  ///by another task.
  //After this method, a kernel is invoked to process ghost cells.


  //Go through device requires vars and mark them as valid on the device.  They are either already
  //valid because they were there previously.  Or they just got copied in and the stream completed.
  multimap<GpuUtilities::LabelPatchMatlLevelDw, DeviceGridVariableInfo> & varMap = dtask->getVarsBeingCopiedByTask().getMap();
  for (multimap<GpuUtilities::LabelPatchMatlLevelDw, DeviceGridVariableInfo>::iterator it = varMap.begin();
            it != varMap.end(); ++it) {
    int whichGPU = it->second.whichGPU;
    int dwIndex = it->second.dep->mapDataWarehouse();
    GPUDataWarehouse* gpudw = dws[dwIndex]->getGPUDW(whichGPU);
    if (it->second.dep->deptype == Task::Requires) {
      if (!it->second.staging) {
        //A regular/non staging variable.
        //If it has a ghost cell
        //if (it->second.gtype == Ghost::None || it->second.numGhostCells == 0) {
        if (gpu_stats.active()) {
          cerrLock.lock();
          {
            gpu_stats << myRankThread() << " markDeviceRequiresDataAsValid() -"
                << " Marking GPU memory as valid for " << it->second.dep->var->getName().c_str() << " patch " << it->first.patchID << endl;
          }
          cerrLock.unlock();
        }

        gpudw->setValidOnGPU(it->second.dep->var->getName().c_str(), it->first.patchID, it->first.matlIndx, it->first.levelIndx);
        //}
      } else {
        if (gpu_stats.active()) {
          cerrLock.lock();
          {
            gpu_stats << myRankThread() << " markDeviceRequiresDataAsValid() -"
                << " Marking GPU memory as valid for " << it->second.dep->var->getName().c_str() << " patch " << it->first.patchID
                << " offset(" << it->second.offset.x() << ", " << it->second.offset.y() << ", " << it->second.offset.z()
                << ") size (" << it->second.sizeVector.x() << ", " << it->second.sizeVector.y() << ", " << it->second.sizeVector.z() << ")" << endl;
          }
          cerrLock.unlock();
        }
        gpudw->setValidOnGPUStaging(it->second.dep->var->getName().c_str(), it->first.patchID, it->first.matlIndx, it->first.levelIndx,
                                    make_int3(it->second.offset.x(),it->second.offset.y(),it->second.offset.z()),
                                    make_int3(it->second.sizeVector.x(), it->second.sizeVector.y(), it->second.sizeVector.z()));
      }
    }
  }
}


//______________________________________________________________________
//
void
UnifiedScheduler::markDeviceGhostsAsValid( DetailedTask * dtask ) {
  //Go through requires vars and mark them as valid on the device.  They are either already
  //valid because they were there previously.  Or they just got copied in and the stream completed.
  //Now go through the varsToBeGhostReady collection.  Any in there should be marked as valid with ghost cells
  multimap<GpuUtilities::LabelPatchMatlLevelDw, DeviceGridVariableInfo> & varMap = dtask->getVarsToBeGhostReady().getMap();
  for (multimap<GpuUtilities::LabelPatchMatlLevelDw, DeviceGridVariableInfo>::iterator it = varMap.begin();
            it != varMap.end(); ++it) {
    int whichGPU = it->second.whichGPU;
    int dwIndex = it->second.dep->mapDataWarehouse();
    GPUDataWarehouse* gpudw = dws[dwIndex]->getGPUDW(whichGPU);

    //A regular/non staging variable.
    //If it has a ghost cell
    gpudw->setValidWithGhostsOnGPU(it->second.dep->var->getName().c_str(), it->first.patchID, it->first.matlIndx, it->first.levelIndx);

  }
}


//______________________________________________________________________
//
void
UnifiedScheduler::markDeviceComputesDataAsValid( DetailedTask * dtask ) {
  //Go through device computes vars and mark them as valid on the device.

  //The only thing we need to process is the requires.
  const Task* task = dtask->getTask();
  for (const Task::Dependency* comp = task->getComputes(); comp != 0; comp = comp->next) {
    constHandle<PatchSubset> patches = comp->getPatchesUnderDomain(dtask->getPatches());
    constHandle<MaterialSubset> matls = comp->getMaterialsUnderDomain(dtask->getMaterials());
    // this is so we can allocate persistent events and streams to distribute when needed
    //   one stream and one event per variable per H2D copy (numPatches * numMatls)
    int numPatches = patches->size();
    int numMatls = matls->size();
    int dwIndex = comp->mapDataWarehouse();
    OnDemandDataWarehouseP dw = dws[dwIndex];

    for (int i = 0; i < numPatches; ++i) {
      GPUDataWarehouse * gpudw = dw->getGPUDW(
          GpuUtilities::getGpuIndexForPatch(patches->get(i)));
      if (gpudw != NULL) {
        for (int j = 0; j < numMatls; ++j) {
          int patchID = patches->get(i)->getID();
          int matlID = matls->get(j);
          const Level* level = getLevel(patches.get_rep());
          int levelID = level->getID();
          if (gpudw->isAllocatedOnGPU(comp->var->getName().c_str(), patchID, matlID, levelID)) {
            gpudw->setValidOnGPU(comp->var->getName().c_str(), patchID, matlID, levelID);
          }
        }
      }
    }
  }
}


//______________________________________________________________________
//
void
UnifiedScheduler::markHostRequiresDataAsValid( DetailedTask * dtask ) {
  //Data has been copied from the device to the host.  The stream has completed.
  //Go through all variables that this CPU task depends on and mark them as valid on the CPU

  //The only thing we need to process is the requires.
  multimap<GpuUtilities::LabelPatchMatlLevelDw, DeviceGridVariableInfo> & varMap = dtask->getVarsBeingCopiedByTask().getMap();
  for (multimap<GpuUtilities::LabelPatchMatlLevelDw, DeviceGridVariableInfo>::iterator it = varMap.begin();
            it != varMap.end(); ++it) {
    int whichGPU = it->second.whichGPU;
    int dwIndex = it->second.dep->mapDataWarehouse();
    GPUDataWarehouse* gpudw = dws[dwIndex]->getGPUDW(whichGPU);
    if (it->second.dep->deptype == Task::Requires) {
      if (!it->second.staging) {
        if (gpu_stats.active()) {
          cerrLock.lock();
          {
            gpu_stats << myRankThread() << " markHostRequiresDataAsValid() -"
                << " Marking host memory as valid for " << it->second.dep->var->getName().c_str() << " patch " << it->first.patchID << endl;
          }
          cerrLock.unlock();
        }

        gpudw->setValidOnCPU(it->second.dep->var->getName().c_str(), it->first.patchID, it->first.matlIndx, it->first.levelIndx);
        //}
      }/* else {
        if (gpu_stats.active()) {
          cerrLock.lock();
          {
            gpu_stats << myRankThread() << " markDeviceRequiresDataAsValid() -"
                << " Marking host memory as valid for " << it->second.dep->var->getName().c_str() << " patch " << it->first.patchID
                << " offset(" << it->second.offset.x() << ", " << it->second.offset.y() << ", " << it->second.offset.z()
                << ") size (" << it->second.sizeVector.x() << ", " << it->second.sizeVector.y() << ", " << it->second.sizeVector.z() << ")" << endl;
          }
          cerrLock.unlock();
        }
        gpudw->setValidOnHostStaging(it->second.dep->var->getName().c_str(), it->first.patchID, it->first.matlIndx, it->first.levelIndx,
                                    make_int3(it->second.offset.x(),it->second.offset.y(),it->second.offset.z()),
                                    make_int3(it->second.sizeVector.x(), it->second.sizeVector.y(), it->second.sizeVector.z()));
      }*/
    }
  }
}


//______________________________________________________________________
//
void
UnifiedScheduler::initiateD2HForHugeGhostCells( DetailedTask * dtask ) {
  //RMCRT problems use 32768 ghost cells as a way to force an "all to all" transmission of ghost cells
  //It is much easier to manage these ghost cells in host memory instead of GPU memory.  So for such
  //variables, after they are done computing, we will copy them D2H.  For RMCRT, this overhead
  //only adds about 1% or less to the overall computation time.  '
  //This only works with COMPUTES, it is not configured to work with requires.

  const Task* task = dtask->getTask();

  // determine which computes variables to copy back to the host
  for (const Task::Dependency* comp = task->getComputes(); comp != 0; comp = comp->next) {
    //Only process large number of ghost cells.
    if (comp->numGhostCells == SHRT_MAX) {
      constHandle<PatchSubset> patches = comp->getPatchesUnderDomain(dtask->getPatches());
      constHandle<MaterialSubset> matls = comp->getMaterialsUnderDomain(dtask->getMaterials());

      int dwIndex = comp->mapDataWarehouse();
      OnDemandDataWarehouseP dw = dws[dwIndex];

      void* host_ptr   = NULL;    // host base pointer to raw data
      void* device_ptr = NULL;    // device base pointer to raw data
      size_t host_bytes = 0;      // raw byte count to copy to the device
      size_t device_bytes = 0;    // raw byte count to copy to the host
      IntVector host_low, host_high, host_offset, host_size, host_strides;

      int numPatches = patches->size();
      int numMatls = matls->size();
      //__________________________________
      //
      for (int i = 0; i < numPatches; ++i) {
        for (int j = 0; j < numMatls; ++j) {
          const int patchID = patches->get(i)->getID();
          const int matlID  = matls->get(j);
          const Level* level = getLevel(patches.get_rep());
          const int levelID = level->getID();
          const std::string compVarName = comp->var->getName();

          const Patch * patch = NULL;
          //const Patch * patch = grid->getPatchByID(patchID, levelID);
          for (int i = 0; i < numPatches; ++i) {
            if (patches->get(i)->getID() == patchID) {
             patch = patches->get(i);
            }
          }
          if (!patch) {
           printf("ERROR:\nUnifiedScheduler::initiateD2HForHugeGhostCells() patch not found.\n");
           SCI_THROW( InternalError("UnifiedScheduler::initiateD2HForHugeGhostCells() patch not found.", __FILE__, __LINE__));
          }

          const unsigned int deviceNum = GpuUtilities::getGpuIndexForPatch(patch);
          GPUDataWarehouse * gpudw = dw->getGPUDW(deviceNum);
          OnDemandDataWarehouse::uintahSetCudaDevice(deviceNum);
          cudaStream_t* stream = dtask->getCudaStreamForThisTask(deviceNum);

          if (gpudw != NULL) {

            //It's not valid on the CPU but it is on the GPU.  Copy it on over.
            if (!gpudw->isValidOnCPU( compVarName.c_str(), patchID, matlID, levelID)) {
              TypeDescription::Type type = comp->var->typeDescription()->getType();
              switch (type) {
                case TypeDescription::CCVariable:
                case TypeDescription::NCVariable:
                case TypeDescription::SFCXVariable:
                case TypeDescription::SFCYVariable:
                case TypeDescription::SFCZVariable: {

                  if (gpu_stats.active()) {
                    cerrLock.lock();
                    {
                      gpu_stats << myRankThread() << " initiateD2HForHugeGhostCells() -"
                          // Task: " << dtask->getName()
                          << " Checking if we should copy of \""
                          << compVarName << "\" Patch " << patchID
                          << " Material " << matlID << endl;
                    }
                    cerrLock.unlock();
                  }
                  bool performCopy = gpudw->testAndSetCopyingIntoCPU(compVarName.c_str(), patchID, matlID, levelID);
                  if (performCopy) {

                    //    std::pair<GpuUtilities::LabelPatchMatlLevelDw, DeviceGridVariableInfo>(it->first, it->second));

                    //size the host var to be able to fit all r::oom needed.
                    IntVector host_low, host_high, host_lowOffset, host_highOffset, host_offset, host_size, host_strides;
                    Patch::VariableBasis basis = Patch::translateTypeToBasis(type, false);


                    Patch::getGhostOffsets(type, comp->gtype, comp->numGhostCells, host_lowOffset, host_highOffset);
                    patch->computeExtents(basis,
                        comp->var->getBoundaryLayer(), host_lowOffset,
                        host_highOffset, host_low, host_high);
                    int dwIndex = comp->mapDataWarehouse();
                    OnDemandDataWarehouseP dw = dws[dwIndex];


                    //It's possible the computes data may contain ghost cells.  But a task needing to get the data
                    //out of the GPU may not know this.  It may just want the var data.
                    //This creates a dilemma, as the GPU var is sized differently than the CPU var.
                    //So ask the GPU what size it has for the var.  Size the CPU var to match so it can copy all GPU data in.
                    //When the GPU->CPU copy is done, then we need to resize the CPU var if needed to match
                    //what the CPU is expecting it to be.
                    //GPUGridVariableBase* gpuGridVar;

                    //gpudw->get(*gpuGridVar, dependantVar->var->getName().c_str(), patchID, matlID);
                    int3 low;
                    int3 high;
                    int3 size;
                    GPUDataWarehouse::GhostType tempgtype;
                    Ghost::GhostType gtype;
                    int numGhostCells;
                    gpudw->getSizes(low, high, size, tempgtype, numGhostCells, compVarName.c_str(), patchID, matlID, levelID);

                    gtype = (Ghost::GhostType) tempgtype;


                    if (gpu_stats.active()) {
                      cerrLock.lock();
                      {
                        gpu_stats << myRankThread() << " initiateD2HForHugeGhostCells() -"
                            // Task: " << dtask->getName()
                            << " Yes, we are copying \""
                            << compVarName << "\" patch" << patchID
                            << " material " << matlID
                            << " number of ghost cells " << numGhostCells << " from device to host" << endl;
                      }
                      cerrLock.unlock();
                    }


                    GridVariableBase* gridVar = dynamic_cast<GridVariableBase*>(comp->var->typeDescription()->createInstance());

                    bool finalized = dw->isFinalized();
                    if (finalized) {
                      dw->unfinalize();
                    }
                    dw->allocateAndPut(*gridVar, comp->var, matlID, patch, gtype, numGhostCells);
                    if (finalized) {
                      dw->refinalize();
                    }
                    if (gpu_stats.active()) {
                      cerrLock.lock();
                      {
                        gpu_stats << myRankThread() << " InitiateD2H() -"
                            // Task: " << dtask->getName()
                            << " allocateAndPut for "
                            << compVarName << " patch" << patchID
                            << " material " << matlID
                            << " number of ghost cells " << numGhostCells << " from device to host" << endl;
                      }
                      cerrLock.unlock();
                    }

                    gridVar->getSizes(host_low, host_high, host_offset, host_size, host_strides);
                    host_ptr = gridVar->getBasePointer();
                    host_bytes = gridVar->getDataSize();

                    int3 device_offset;
                    int3 device_size;
                    GPUGridVariableBase* device_var = OnDemandDataWarehouse::createGPUGridVariable(host_strides.x());
                    gpudw->get(*device_var, compVarName.c_str(), patchID, matlID, levelID);
                    device_var->getArray3(device_offset, device_size, device_ptr);
                    delete device_var;

                    // if offset and size is equal to CPU DW, directly copy back to CPU var memory;
                    if (device_offset.x == host_low.x()
                        && device_offset.y == host_low.y()
                        && device_offset.z == host_low.z()
                        && device_size.x == host_size.x()
                        && device_size.y == host_size.y()
                        && device_size.z == host_size.z()) {

                      if (gpu_stats.active()) {
                        cerrLock.lock();
                        {
                          gpu_stats << myRankThread() << " initiateD2HForHugeGhostCells - Copy of \""
                              << compVarName << "\""
                              << " patch " << patchID
                              << " material " << matlID
                              << " level " << levelID
                              << ", size = "
                              << std::dec << host_bytes << " to " << std::hex
                              << host_ptr << " from " << std::hex << device_ptr
                              << ", using stream " << std::hex
                              << stream << std::dec << endl;
                        }
                        cerrLock.unlock();
                      }
                      cudaError_t retVal;
                      CUDA_RT_SAFE_CALL(retVal = cudaMemcpyAsync(host_ptr, device_ptr, host_bytes, cudaMemcpyDeviceToHost, *stream));


                      IntVector temp(0,0,0);
                      dtask->getVarsBeingCopiedByTask().add(patch, matlID, levelID,
                                                            false,
                                                            IntVector(device_size.x, device_size.y, device_size.z),
                                                            host_strides.x(), host_bytes,
                                                            IntVector(device_offset.x, device_offset.y, device_offset.z),
                                                            comp,
                                                            gtype, numGhostCells,  deviceNum,
                                                            gridVar, GpuUtilities::sameDeviceSameMpiRank);


                      if (retVal == cudaErrorLaunchFailure) {
                        SCI_THROW(
                            InternalError("Detected CUDA kernel execution failure on Task: "+ dtask->getName(), __FILE__, __LINE__));
                      } else {
                        CUDA_RT_SAFE_CALL(retVal);
                      }

                    }
                    delete gridVar;

                  }
                  break;
                }
              }
            }
          }
        }
      }
    }
  }
}


//______________________________________________________________________
//
void
UnifiedScheduler::initiateD2H( DetailedTask * dtask ) {
  //Request that all contiguous device arrays from the device be sent to their contiguous host array counterparts.
  //We only copy back the data needed for an upcoming task.  If data isn't needed, it can stay on the device and
  //potentially even die on the device

  //Returns true if no device data is required, thus allowing a CPU task to immediately proceed.

  void* host_ptr = NULL;    // host base pointer to raw data
  void* device_ptr = NULL;    // host base pointer to raw data
  size_t host_bytes = 0;    // raw byte count to copy to the device

  const Task* task = dtask->getTask();
  dtask->clearPreparationCollections();
  //The only thing we need to process is the requires.

  //Gather up all possible dependents and remove duplicate (we don't want to transfer some variables twice)
  std::map<labelPatchMatlDependency, const Task::Dependency*> vars;
  for (const Task::Dependency* dependantVar = task->getRequires(); dependantVar != 0; dependantVar = dependantVar->next) {
    constHandle<PatchSubset> patches = dependantVar->getPatchesUnderDomain(dtask->getPatches());
    constHandle<MaterialSubset> matls = dependantVar->getMaterialsUnderDomain(dtask->getMaterials());
    const int numPatches = patches->size();
    const int numMatls = matls->size();
    for (int i = 0; i < numPatches; ++i) {
      for (int j = 0; j < numMatls; ++j) {
        labelPatchMatlDependency lpmd(dependantVar->var->getName().c_str(), patches->get(i)->getID(), matls->get(j), Task::Requires);
        if (vars.find(lpmd) == vars.end()) {
          if (gpu_stats.active()) {
            cerrLock.lock();
            {
              gpu_stats << myRankThread() << " InitiateD2H - For task "
                  << dtask->getName() << " checking on requires \""
                  << dependantVar->var->getName() << "\""
                  << " patch " <<  patches->get(i)->getID()
                  << " material " << matls->get(j)
                  << endl;
            }
            cerrLock.unlock();
          }
          vars.insert(
              std::map<labelPatchMatlDependency, const Task::Dependency*>::value_type(lpmd, dependantVar));
        }
      }
    }
  }

  for (const Task::Dependency* dependantVar = task->getComputes(); dependantVar != 0; dependantVar = dependantVar->next) {
    constHandle<PatchSubset> patches = dependantVar->getPatchesUnderDomain(dtask->getPatches());
    constHandle<MaterialSubset> matls = dependantVar->getMaterialsUnderDomain(dtask->getMaterials());
    const int numPatches = patches->size();
    const int numMatls = matls->size();
    for (int i = 0; i < numPatches; ++i) {
      for (int j = 0; j < numMatls; ++j) {
        labelPatchMatlDependency lpmd(dependantVar->var->getName().c_str(), patches->get(i)->getID(), matls->get(j), Task::Computes);
        if (vars.find(lpmd) == vars.end()) {
          if (gpu_stats.active()) {
            cerrLock.lock();
            {
              gpu_stats << myRankThread() << " InitiateD2H - For task "
                  << dtask->getName() << " checking on computes \""
                  << dependantVar->var->getName() << "\""
                  << " patch " <<  patches->get(i)->getID()
                  << " material " << matls->get(j)
                  << endl;
            }
            cerrLock.unlock();
          }
          //vars.insert(
          //    std::map<labelPatchMatlDependency, const Task::Dependency*>::value_type(lpmd, dependantVar));
          //printf("There were computes!\n");
        }
      }
    }
  }

  //Go through each unique dependent var and see if we should queue up a D2H copy
  std::map<labelPatchMatlDependency, const Task::Dependency*>::iterator varIter;
  for (varIter = vars.begin(); varIter != vars.end(); ++varIter) {
    const Task::Dependency* dependantVar = varIter->second;
    constHandle<PatchSubset> patches = dependantVar->getPatchesUnderDomain(dtask->getPatches());
    constHandle<MaterialSubset> matls = dependantVar->getMaterialsUnderDomain(dtask->getMaterials());


    // this is so we can allocate persistent events and streams to distribute when needed
    //   one stream and one event per variable per H2D copy (numPatches * numMatls)

    int numPatches = patches->size();
    int dwIndex = dependantVar->mapDataWarehouse();
    OnDemandDataWarehouseP dw = dws[dwIndex];

    //GPUDataWarehouse * baseGpuDW = dw->getGPUDW(0); //A temporary hack, the 0th GPU DW holds the host variable information status.
    const int patchID = varIter->first.patchID;
    const Level* level = getLevel(patches.get_rep());
    int levelID = level->getID();
    if (dependantVar->var->typeDescription()->getType() == TypeDescription::ReductionVariable) {
      levelID = -1;
    }
    const Patch * patch = NULL;
    //const Patch * patch = grid->getPatchByID(patchID, levelID);
    for (int i = 0; i < numPatches; ++i) {
      if (patches->get(i)->getID() == patchID) {
        patch = patches->get(i);
      }
    }
    if (!patch) {
      printf("ERROR:\nUnifiedScheduler::initiateD2H() patch not found.\n");
      SCI_THROW( InternalError("UnifiedScheduler::initiateD2H() patch not found.", __FILE__, __LINE__));
    }
    const int matlID = varIter->first.matlIndex;

    unsigned int deviceNum = GpuUtilities::getGpuIndexForPatch(patch);
    GPUDataWarehouse * gpudw = dw->getGPUDW(deviceNum);
    OnDemandDataWarehouse::uintahSetCudaDevice(deviceNum);
    cudaStream_t* stream = dtask->getCudaStreamForThisTask(deviceNum);

    const string varName = dependantVar->var->getName();

    if (gpudw != NULL) {
      //gpu_stats << myRankThread() << "Checking status for var " << varName << " patchID " << patchID
      //                       << " material " << matlID << " levelID " << levelID << endl;

      //It's not valid on the CPU but it is on the GPU.  Copy it on over.
      if (!gpudw->isValidOnCPU( varName.c_str(), patchID, matlID, levelID) &&
          gpudw->isAllocatedOnGPU( varName.c_str(), patchID, matlID, levelID) &&
          gpudw->isValidOnGPU( varName.c_str(), patchID, matlID, levelID)) {

        TypeDescription::Type type = dependantVar->var->typeDescription()->getType();
        switch (type) {
        case TypeDescription::CCVariable:
        case TypeDescription::NCVariable:
        case TypeDescription::SFCXVariable:
        case TypeDescription::SFCYVariable:
        case TypeDescription::SFCZVariable: {

          if (gpu_stats.active()) {
            cerrLock.lock();
            {
              gpu_stats << myRankThread() << " InitiateD2H() -"
                  // Task: " << dtask->getName()
                  << " Checking if we should copy of \""
                  << varName << "\" Patch " << patchID
                  << " Material " << matlID << endl;
            }
            cerrLock.unlock();
          }
          bool performCopy = gpudw->testAndSetCopyingIntoCPU(varName.c_str(), patchID, matlID, levelID);
          if (performCopy) {

            if (gpu_stats.active()) {
              cerrLock.lock();
              {
                gpu_stats << myRankThread() << " InitiateD2H() -"
                    // Task: " << dtask->getName()
                    << " Yes, we are copying \""
                    << varName << "\" patch" << patchID
                    << " material " << matlID
                    << " number of ghost cells " << dependantVar->numGhostCells << " from device to host" << endl;
              }
              cerrLock.unlock();
            }

            //It's possible the computes data may contain ghost cells.  But a task needing to get the data
            //out of the GPU may not know this.  It may just want the var data.
            //This creates a dilemma, as the GPU var is sized differently than the CPU var.
            //So ask the GPU what size it has for the var.  Size the CPU var to match so it can copy all GPU data in.
            //When the GPU->CPU copy is done, then we need to resize the CPU var if needed to match
            //what the CPU is expecting it to be.

            //Get the host var variable
            GridVariableBase* gridVar = dynamic_cast<GridVariableBase*>(dependantVar->var->typeDescription()->createInstance());
            const size_t elementDataSize =
                OnDemandDataWarehouse::getTypeDescriptionSize(dependantVar->var->typeDescription()->getSubType()->getType());

            //The device will have our best knowledge of the exact dimensions/ghost cells of the variable.
            //So lets get those values.
            int3 device_low;
            int3 device_offset;
            int3 device_high;
            int3 device_size;
            GPUDataWarehouse::GhostType tempgtype;
            Ghost::GhostType gtype;
            int numGhostCells;
            gpudw->getSizes(device_low, device_high, device_size, tempgtype, numGhostCells, varName.c_str(), patchID, matlID, levelID);
            gtype = (Ghost::GhostType) tempgtype;
            device_offset = device_low;

            //Now get dimensions for the host variable.
            bool uses_SHRT_MAX = (numGhostCells == SHRT_MAX);
            Patch::VariableBasis basis = Patch::translateTypeToBasis(type, false);
            //Get the size/offset of what the host var would be with ghost cells.
            IntVector host_low, host_high, host_lowOffset, host_highOffset, host_offset, host_size;
            if (uses_SHRT_MAX) {
              level->findCellIndexRange(host_low, host_high); // including extraCells
            } else {
              Patch::getGhostOffsets(type, gtype, numGhostCells, host_lowOffset, host_highOffset);
              patch->computeExtents(basis,
                  dependantVar->var->getBoundaryLayer(), host_lowOffset,
                  host_highOffset, host_low, host_high);
            }
            host_size = host_high - host_low;
            int dwIndex = dependantVar->mapDataWarehouse();
            OnDemandDataWarehouseP dw = dws[dwIndex];


            //const bool finalized = dw->isFinalized();
            //if (finalized) {
            //  dw->unfinalize();
            //}

            //Get/make the host var
            if (gpu_stats.active()) {
              cerrLock.lock();
               {
                gpu_stats << myRankThread() << " InitiateD2H() -"
                    << " calling allocateAndPut for "
                    << varName << " patch" << patchID
                    << " material " << matlID
                    << " level " << levelID
                    << " number of ghost cells " << numGhostCells << " from device to host" << endl;
               }
              cerrLock.unlock();
            }

            //get the device var so we can get the pointer.
            GPUGridVariableBase* device_var = OnDemandDataWarehouse::createGPUGridVariable(elementDataSize);
            gpudw->get(*device_var, varName.c_str(), patchID, matlID, levelID);
            device_var->getArray3(device_offset, device_size, device_ptr);
            delete device_var;
            //host_low = IntVector(device_offset.x, device_offset.y, device_offset.z)
            //gridVar->rewindow(IntVector(device_offset.x, device_offset.y, device_offset.z), IntVector(device_size.x+device_offset.x, device_size.y+device_offset.y, device_size.z+device_offset.z));


            bool proceedWithCopy = false;
            // See if the size of the host var and the device var match.

            if (device_offset.x == host_low.x()
                && device_offset.y == host_low.y()
                && device_offset.z == host_low.z()
                && device_size.x == host_size.x()
                && device_size.y == host_size.y()
                && device_size.z == host_size.z()) {
              proceedWithCopy = true;
              //Note, race condition possible here
              bool finalized = dw->isFinalized();
              if (finalized) {
                dw->unfinalize();
              }
              if (uses_SHRT_MAX) {
                //dw->allocateAndPut(*gridVar, dependantVar->var, matlID, patch, gtype, numGhostCells);
                //dw->getLevel(*gridVar, dependantVar->var, matlID, level);
                dw->getRegion(*gridVar, dependantVar->var, matlID, level, host_low, host_high, true, true);
              } else {
                dw->allocateAndPut(*gridVar, dependantVar->var, matlID, patch, gtype, numGhostCells);
              }
              if (finalized) {
                dw->refinalize();
              }

            } else {
              //They didn't match.  Lets see if the device var doesn't have ghost cells.
              //This can happen prior to the first timestep during initial computations when no variables
              //had room for ghost cells.
              Patch::getGhostOffsets(type, Ghost::None, 0, host_lowOffset, host_highOffset);
              patch->computeExtents(basis,
                  dependantVar->var->getBoundaryLayer(), host_lowOffset,
                  host_highOffset, host_low, host_high);
              host_size = host_high - host_low;
              if (device_offset.x == host_low.x()
                  && device_offset.y == host_low.y()
                  && device_offset.z == host_low.z()
                  && device_size.x == host_size.x()
                  && device_size.y == host_size.y()
                  && device_size.z == host_size.z()) {
                proceedWithCopy = true;
                //Note, race condition possible here
                bool finalized = dw->isFinalized();
                if (finalized) {
                  dw->unfinalize();
                }
                dw->allocateAndPut(*gridVar, dependantVar->var, matlID, patch, Ghost::None, 0);
                if (finalized) {
                  dw->refinalize();
                }
              } else {
                //The sizes STILL don't match.
                //One more last ditch effort.  Assume it was using up to 32768 ghost cells.
                level->findCellIndexRange(host_low, host_high);
                host_size = host_high - host_low;
                if (device_offset.x == host_low.x()
                     && device_offset.y == host_low.y()
                     && device_offset.z == host_low.z()
                     && device_size.x == host_size.x()
                     && device_size.y == host_size.y()
                     && device_size.z == host_size.z()) {
                  //ok, this worked.  Allocate it the large ghost cell way with getRegion

                  //Note, race condition possible here
                  bool finalized = dw->isFinalized();
                  if (finalized) {
                    dw->unfinalize();
                  }
                  dw->getRegion(*gridVar, dependantVar->var, matlID, level, host_low, host_high, true);
                  if (finalized) {
                    dw->refinalize();
                  }
                  proceedWithCopy = true;
                } else {
                  printf("ERROR:\nUnifiedScheduler::initiateD2H() - Device and host sizes didn't match.  Device size is (%d, %d, %d), and host size is (%d, %d, %d)\n", device_size.x, device_size.y, device_size.y,host_size.x(), host_size.y(),host_size.z());
                  SCI_THROW( InternalError("UnifiedScheduler::initiateD2H() - Device and host sizes didn't match.", __FILE__, __LINE__));
                }
              }

            }



            // if offset and size is equal to CPU DW, directly copy back to CPU var memory;
            if (proceedWithCopy) {

              host_ptr = gridVar->getBasePointer();
              host_bytes = gridVar->getDataSize();
              if (gpu_stats.active()) {
                cerrLock.lock();
                {
                  gpu_stats << myRankThread() << " InitiateD2H() - Copy of \""
                      << varName << "\""
                      << " patch " << patchID
                      << " material " << matlID
                      << " level " << levelID
                      << ", size = "
                      << std::dec << host_bytes
                      << " offset (" << device_offset.x << ", " << device_offset.y << ", " << device_offset.z << ")"
                      << " size (" << device_size.x << ", " << device_size.y << ", " << device_size.z << ")"
                      << " to " << std::hex << host_ptr << " from " << std::hex << device_ptr
                      << ", using stream " << std::hex
                      << stream << std::dec << endl;
                }
                cerrLock.unlock();
              }
              cudaError_t retVal;


              if (host_bytes == 0) {
                printf("ERROR:\nUnifiedScheduler::initiateD2H() - Transfer bytes is listed as zero.\n");
                SCI_THROW( InternalError("UnifiedScheduler::initiateD2H() - Transfer bytes is listed as zero.", __FILE__, __LINE__));
              }
              if (!host_ptr) {
                printf("ERROR:\nUnifiedScheduler::initiateD2H() - Invalid host pointer, it was NULL.\n");
                SCI_THROW( InternalError("UnifiedScheduler::initiateD2H() - Invalid host pointer, it was NULL.", __FILE__, __LINE__));
              }

              CUDA_RT_SAFE_CALL(retVal = cudaMemcpyAsync(host_ptr, device_ptr, host_bytes, cudaMemcpyDeviceToHost, *stream));


              IntVector temp(0,0,0);
              dtask->getVarsBeingCopiedByTask().add(patch, matlID, levelID,
                                                    false,
                                                    IntVector(device_size.x, device_size.y, device_size.z),
                                                    elementDataSize, host_bytes,
                                                    IntVector(device_offset.x, device_offset.y, device_offset.z),
                                                    dependantVar,
                                                    gtype, numGhostCells,  deviceNum,
                                                    gridVar, GpuUtilities::sameDeviceSameMpiRank);


              if (retVal == cudaErrorLaunchFailure) {
                SCI_THROW(
                    InternalError("Detected CUDA kernel execution failure on Task: "+ dtask->getName(), __FILE__, __LINE__));
              } else {
                CUDA_RT_SAFE_CALL(retVal);
              }


//              if (varName == "abskg") {
//                cudaDeviceSynchronize();
//                double * vardata = new double[gridVar->getDataSize()/sizeof(double)];
//                gridVar->copyOut(vardata);
//                printf("Just got data for %s from device address %p data is %1.6lf\n", varName.c_str(), device_ptr, vardata[0]);
//                int zhigh = host_high.z();
//                int yhigh = host_high.y();
//                int xhigh = host_high.x();
//                int ghostLayers = numGhostCells;
//                int ystride = host_size.y();
//                int xstride = host_size.x();
//
//                printf("Going to loop between (%d, %d, %d) to (%d, %d, %d) with host_size (%d, %d, %d)\n", host_low.x(), host_low.y(), host_low.z(), xhigh, yhigh, zhigh, host_size.x(), host_size.y(), host_size.z());
//                for (int k = 0; k < host_size.z(); ++k) {
//                  for (int j = 0; j < host_size.y(); ++j) {
//                    for (int i = 0; i < host_size.x(); ++i) {
//                      //IntVector temp2(i,j,k);
//
//                      // For an array of [ A ][ B ][ C ], we can index it thus:
//                      // (a * B * C) + (b * C) + (c * 1)
//                      int idx = i + ((j) * xstride) + ((k) * xstride * ystride);
//                      printf(" - var(%d, %d, %d) is %1.6lf at offset %d\n", i + host_low.x(), j + host_low.y(), k + host_low.z(), vardata[idx], idx);
//                    }
//                  }
//                }
//              }

            }
            delete gridVar;

          }
          break;
        }
        case TypeDescription::PerPatch: {
          bool performCopy = gpudw->testAndSetCopyingIntoCPU(varName.c_str(), patchID, matlID, levelID);
          if (performCopy) {

            PerPatchBase* hostPerPatchVar = dynamic_cast<PerPatchBase*>(dependantVar->var->typeDescription()->createInstance());
            const bool finalized = dw->isFinalized();
            if (finalized) {
              dw->unfinalize();
            }
            dw->put(*hostPerPatchVar, dependantVar->var, matlID, patch);
            if (finalized) {
              dw->refinalize();
            }
            host_ptr = hostPerPatchVar->getBasePointer();
            host_bytes = hostPerPatchVar->getDataSize();

            GPUPerPatchBase* gpuPerPatchVar = OnDemandDataWarehouse::createGPUPerPatch(host_bytes);
            gpudw->get(*gpuPerPatchVar, varName.c_str(), patchID, matlID, levelID);
            device_ptr = gpuPerPatchVar->getVoidPointer();
            size_t device_bytes = gpuPerPatchVar->getMemSize();
            delete gpuPerPatchVar;

            if (gpu_stats.active()) {
              cerrLock.lock();
              {
                gpu_stats << myRankThread() << "initiateD2H copy of \""
                    << varName << "\", size = "
                    << std::dec << host_bytes << " to " << std::hex
                    << host_ptr << " from " << std::hex << device_ptr
                    << ", using stream " << std::hex << stream
                    << std::dec << endl;
              }
              cerrLock.unlock();
            }
            //TODO: Verify no memory leaks
            if (host_bytes == device_bytes) {
              CUDA_RT_SAFE_CALL(cudaMemcpyAsync(host_ptr, device_ptr, host_bytes, cudaMemcpyDeviceToHost, *stream));
              dtask->getVarsBeingCopiedByTask().add(patch, matlID, levelID,
                                                    host_bytes, host_bytes,
                                                    dependantVar,
                                                    deviceNum,
                                                    hostPerPatchVar,
                                                    GpuUtilities::sameDeviceSameMpiRank);
            } else {
              printf("InitiateD2H - PerPatch variable memory sizes didn't match\n");
              SCI_THROW(InternalError("InitiateD2H - PerPatch variable memory sizes didn't match", __FILE__, __LINE__));
            }
            delete hostPerPatchVar;
          }

          break;
        }
        case TypeDescription::ReductionVariable: {
          bool performCopy = gpudw->testAndSetCopyingIntoCPU(varName.c_str(), patchID, matlID, levelID);
          if (performCopy) {
            ReductionVariableBase* hostReductionVar = dynamic_cast<ReductionVariableBase*>(dependantVar->var->typeDescription()->createInstance());
            const bool finalized = dw->isFinalized();
            if (finalized) {
              dw->unfinalize();
            }
            dw->put(*hostReductionVar, dependantVar->var, patch->getLevel(), matlID);
            if (finalized) {
              dw->refinalize();
            }
            host_ptr   = hostReductionVar->getBasePointer();
            host_bytes = hostReductionVar->getDataSize();

            GPUReductionVariableBase* gpuReductionVar = OnDemandDataWarehouse::createGPUReductionVariable(host_bytes);
            gpudw->get(*gpuReductionVar, varName.c_str(), patchID, matlID, levelID);
            device_ptr = gpuReductionVar->getVoidPointer();
            size_t device_bytes = gpuReductionVar->getMemSize();
            delete gpuReductionVar;

            if (gpu_stats.active()) {
              cerrLock.lock();
              {
                gpu_stats << myRankThread() << "initiateD2H copy of \""
                    << varName << "\", size = "
                    << std::dec << host_bytes << " to " << std::hex
                    << host_ptr << " from " << std::hex << device_ptr
                    << ", using stream " << std::hex << stream
                    << std::dec << endl;
              }
              cerrLock.unlock();
            }

            if (host_bytes == device_bytes) {
              CUDA_RT_SAFE_CALL(cudaMemcpyAsync(host_ptr, device_ptr, host_bytes, cudaMemcpyDeviceToHost, *stream));
              dtask->getVarsBeingCopiedByTask().add(patch, matlID, levelID,
                                                    host_bytes, host_bytes,
                                                    dependantVar,
                                                    deviceNum,
                                                    hostReductionVar,
                                                    GpuUtilities::sameDeviceSameMpiRank);
            } else {
              printf("InitiateD2H - Reduction variable memory sizes didn't match\n");
              SCI_THROW(InternalError("InitiateD2H - Reduction variable memory sizes didn't match", __FILE__, __LINE__));
            }
            delete hostReductionVar;
          }
          break;
        }
        default: {
          cerrLock.lock();
          cerr << "Variable " << varName << " is of a type that is not supported on GPUs yet." << endl;
          cerrLock.unlock();
        }
        }
      }
    }
  }
  //TODO, figure out contiguous data in this model
  //Now do any contiguous data.
  //cudaError_t retVal;
  //int device = dtask->getDeviceNum();
  //CUDA_RT_SAFE_CALL(retVal = OnDemandDataWarehouse::uintahSetCudaDevice(device);

  //string taskID = dtask->getName();
  //retVal = dws[dwmap[Task::NewDW]]->getGPUDW()->copyDataDeviceToHost(taskID.c_str(), dtask->getCudaStreamForThisTask());
  //if (retVal != cudaSuccess) {
  //   printf("Error code is %d\n", retVal);
  //   SCI_THROW(InternalError("Detected CUDA kernel execution failure on Task: "+ dtask->getName(), __FILE__, __LINE__));
  //}

}


//______________________________________________________________________
//
void UnifiedScheduler::freeCudaStreamsFromPool() {
  cudaError_t retVal;

  idle_streams_monitor idle_streams_write_lock{ Uintah::CrowdMonitor<idle_streams_tag>::WRITER };
  {
    if (gpu_stats.active()) {
      cerrLock.lock();
      gpu_stats << myRankThread() << " locking freeCudaStreams" << std::endl;
      cerrLock.unlock();
    }

    unsigned int totalStreams = 0;
    for (map<unsigned int, queue<cudaStream_t*> >::const_iterator it = idleStreams->begin(); it != idleStreams->end(); ++it) {
      totalStreams += it->second.size();
      if (gpu_stats.active()) {
        cerrLock.lock();
        {
          gpu_stats << myRankThread() << " Preparing to deallocate " << it->second.size() << " CUDA stream(s) for device #"
                    << it->first << std::endl;
        }
        cerrLock.unlock();
      }
    }

    for (map<unsigned int, queue<cudaStream_t*> >::const_iterator it = idleStreams->begin(); it != idleStreams->end(); ++it) {
      unsigned int device = it->first;
      OnDemandDataWarehouse::uintahSetCudaDevice(device);

      while (!idleStreams->operator[](device).empty()) {
        cudaStream_t* stream = idleStreams->operator[](device).front();
        idleStreams->operator[](device).pop();
        if (gpu_stats.active()) {
          cerrLock.lock();
          gpu_stats << myRankThread() << " Performing cudaStreamDestroy for stream " << stream << " on device " << device
                    << std::endl;
          cerrLock.unlock();
        }
        CUDA_RT_SAFE_CALL(retVal = cudaStreamDestroy(*stream));
        free(stream);
      }
    }

    cerrLock.lock();
    gpu_stats << myRankThread() << " unlocking freeCudaStreams " << std::endl;
    cerrLock.unlock();
  }
}


//______________________________________________________________________
//
cudaStream_t *
UnifiedScheduler::getCudaStreamFromPool( int device ) {
  cudaError_t retVal;
  cudaStream_t* stream;

  idle_streams_monitor idle_streams_write_lock{ Uintah::CrowdMonitor<idle_streams_tag>::WRITER };
  {
    if (idleStreams->operator[](device).size() > 0) {
      stream = idleStreams->operator[](device).front();
      idleStreams->operator[](device).pop();
      if (gpu_stats.active()) {
        cerrLock.lock();
        {
          gpu_stats << myRankThread() << " Issued CUDA stream " << std::hex
              << stream << " on device " << std::dec << device << std::endl;
        }
        cerrLock.unlock();
      }
    } else {  // shouldn't need any more than the queue capacity, but in case
      OnDemandDataWarehouse::uintahSetCudaDevice(device);
      // this will get put into idle stream queue and ultimately deallocated after final timestep
      stream = ((cudaStream_t*) malloc(sizeof(cudaStream_t)));
      CUDA_RT_SAFE_CALL(retVal = cudaStreamCreate(&(*stream)));

      if (gpu_stats.active()) {
        cerrLock.lock();
        {
          gpu_stats << myRankThread()
              << " Needed to create 1 additional CUDA stream " << std::hex
              << stream << " for device " << std::dec << device << std::endl;
        }
        cerrLock.unlock();
      }
    }
  }

  return stream;
}


//______________________________________________________________________
//
void
UnifiedScheduler::reclaimCudaStreamsIntoPool( DetailedTask * dtask ) {


  if (gpu_stats.active()) {
    cerrLock.lock();
    {
      gpu_stats << myRankThread()
                << " Seeing if we need to reclaim any CUDA streams for task " << dtask->getName() << " at " << &dtask << std::endl;
    }
    cerrLock.unlock();
  }

  // reclaim DetailedTask streams
  std::set<unsigned int> deviceNums = dtask->getDeviceNums();
  for (std::set<unsigned int>::iterator iter = deviceNums.begin();
      iter != deviceNums.end(); ++iter) {
    //printf("For task %s reclaiming stream for deviceNum %d\n", dtask->getName().c_str(), *iter);
    cudaStream_t* stream = dtask->getCudaStreamForThisTask(*iter);
    if (stream != NULL) {

      idle_streams_monitor idle_streams_write_lock{ Uintah::CrowdMonitor<idle_streams_tag>::WRITER };
      {
        idleStreams->operator[](*iter).push(stream);
      }

      if (gpu_stats.active()) {
        cerrLock.lock();
        {
          gpu_stats << myRankThread() << " Reclaimed CUDA stream " << std::hex
              << stream << " on device " << std::dec << *iter
              << " for task " << dtask->getName() << " at " << &dtask << std::endl;
        }
        cerrLock.unlock();
      }

      //It seems that task objects persist between timesteps.  So make sure we remove
      //all knowledge of any formerly used streams.
      dtask->clearCudaStreamsForThisTask();
    }
  }
}


////______________________________________________________________________
////
//cudaError_t
//UnifiedScheduler::unregisterPageLockedHostMem()
//{
//  cudaError_t retVal;
//  std::set<void*>::iterator iter;
//
//  // unregister the page-locked host memory
//  for (iter = pinnedHostPtrs.begin(); iter != pinnedHostPtrs.end(); iter++) {
//    CUDA_RT_SAFE_CALL(retVal = cudaHostUnregister(*iter));
//  }
//  pinnedHostPtrs.clear();
//
//  return retVal;
//}


//______________________________________________________________________
//
void
UnifiedScheduler::createTaskGpuDWs( DetailedTask * dtask ) {
  //Create GPU datawarehouses for this specific task only.  They will get copied into the GPU.
  //This is sizing these datawarehouses dynamically and doing it all in only one alloc per datawarehouse.
  //See the bottom of the GPUDataWarehouse.h for more information.

  std::set<unsigned int> deviceNums = dtask->getDeviceNums();
  for (std::set<unsigned int>::const_iterator deviceNums_it = deviceNums.begin(); deviceNums_it != deviceNums.end(); ++deviceNums_it) {
    const unsigned int currentDevice = *deviceNums_it;
    unsigned int numItemsInDW = dtask->getTaskVars().getTotalVars(currentDevice, Task::OldDW) + dtask->getGhostVars().getNumGhostCellCopies(currentDevice, Task::OldDW);
    if (numItemsInDW > 0) {

      size_t objectSizeInBytes = sizeof(GPUDataWarehouse)
          - sizeof(GPUDataWarehouse::dataItem) * MAX_VARDB_ITEMS
          + sizeof(GPUDataWarehouse::dataItem) * numItemsInDW;
      //void *placementNewBuffer = malloc(objectSizeInBytes);
      //GPUDataWarehouse* old_taskGpuDW = new (placementNewBuffer) GPUDataWarehouse( currentDevice, placementNewBuffer);

      GPUDataWarehouse* old_taskGpuDW = (GPUDataWarehouse *) malloc(objectSizeInBytes);
      std::ostringstream out;
      out << "Old task GPU DW"
          << " MPIRank: " << Uintah::Parallel::getMPIRank();
          //<< " Task: " << task->getTask()->getName();
      old_taskGpuDW->init( currentDevice, out.str());
      old_taskGpuDW->setDebug(gpudbg.active());

      old_taskGpuDW->init_device(objectSizeInBytes, numItemsInDW);
      dtask->setTaskGpuDataWarehouse(currentDevice, Task::OldDW, old_taskGpuDW);

      if (gpu_stats.active()) {
        cerrLock.lock();
        {
          gpu_stats << myRankThread()
             << " UnifiedScheduler::createTaskGpuDWs() - Created an old Task GPU DW for task " <<  dtask->getName()
             << " for device #" << currentDevice
             << " at host address " << old_taskGpuDW
             << " to contain " << dtask->getTaskVars().getTotalVars(currentDevice, Task::OldDW)
             << " task variables and " << dtask->getGhostVars().getNumGhostCellCopies(currentDevice, Task::OldDW)
             << " ghost cell copies." << endl;
        }
        cerrLock.unlock();
      }

    }

    numItemsInDW = dtask->getTaskVars().getTotalVars(currentDevice, Task::NewDW) + dtask->getGhostVars().getNumGhostCellCopies(currentDevice, Task::NewDW);
    if (numItemsInDW > 0) {

      size_t objectSizeInBytes = sizeof(GPUDataWarehouse)
          - sizeof(GPUDataWarehouse::dataItem) * MAX_VARDB_ITEMS
          + sizeof(GPUDataWarehouse::dataItem) * numItemsInDW;
      //void *placementNewBuffer = malloc(objectSizeInBytes);
      //GPUDataWarehouse* new_taskGpuDW = new (placementNewBuffer) GPUDataWarehouse(currentDevice, placementNewBuffer);
      GPUDataWarehouse* new_taskGpuDW = (GPUDataWarehouse *) malloc(objectSizeInBytes);
      std::ostringstream out;
      out << "New task GPU DW"
          << " MPIRank: " << Uintah::Parallel::getMPIRank()
          << " Thread:" << Impl::t_tid;
          //<< " Task: " << task->getName();
      new_taskGpuDW->init(currentDevice, out.str());
      new_taskGpuDW->setDebug(gpudbg.active());
      new_taskGpuDW->init_device(objectSizeInBytes, numItemsInDW);

      dtask->setTaskGpuDataWarehouse(currentDevice, Task::NewDW, new_taskGpuDW);

      if (gpu_stats.active()) {
        cerrLock.lock();
        {
          gpu_stats << myRankThread()
             << " UnifiedScheduler::createTaskGpuDWs() - Created a new Task GPU DW for task " <<  dtask->getName()
             << " for device #" << currentDevice
             << " at host address " << new_taskGpuDW
             << " to contain " << dtask->getTaskVars().getTotalVars(currentDevice, Task::NewDW)
             << " task variables and " << dtask->getGhostVars().getNumGhostCellCopies(currentDevice, Task::NewDW)
             << " ghost cell copies." << endl;
        }
        cerrLock.unlock();
      }
    }
    //Determine the sizes of each GPU Datawarehouse.  We want these DWs sized as small as possible to
    //minimize copy times to the GPU.
  }
}


//______________________________________________________________________
//
void
UnifiedScheduler::assignDevicesAndStreams( DetailedTask * dtask ) {

  // Figure out which device this patch was assigned to.
  // If a task has multiple patches, then assign all.  Most tasks should
  // only end up on one device.  Only tasks like data archiver's output variables
  // work on multiple patches which can be on multiple devices.
  std::map<const Patch *, int>::iterator it;
  for (int i = 0; i < dtask->getPatches()->size(); ++i) {
    const Patch* patch = dtask->getPatches()->get(i);
    int index = GpuUtilities::getGpuIndexForPatch(patch);
    if (index >= 0) {
      //See if this task doesn't yet have a stream for this GPU device.
      if (dtask->getCudaStreamForThisTask(index) == NULL) {
        dtask->assignDevice(index);
        cudaStream_t* stream = getCudaStreamFromPool(index);
        cerrLock.lock();
        {
          gpu_stats << myRankThread() << " Assigning for CPU task " << dtask->getName() << " at " << &dtask
                  << " stream " << std::hex << stream << std::dec
                  << " for device " << index
                  << std::endl;
        }
        cerrLock.unlock();
        dtask->setCudaStreamForThisTask(index, stream);
      }
    } else {
      cerrLock.lock();
      cerr << "ERROR: Could not find the assigned GPU for this patch." << endl;
      cerrLock.unlock();
      exit(-1);
      //For some reason, this patch wasn't assigned a GPU.  Give it the zeroth GPU.
      //readyTask->assignDevice(0);
    }
  }
}


void UnifiedScheduler::assignDevicesAndStreamsFromGhostVars(DetailedTask* dtask) {
  //Go through the ghostVars collection and look at the patch where all ghost cells are going.
  set<unsigned int> & destinationDevices = dtask->getGhostVars().getDestinationDevices();
  for (set<unsigned int>::iterator iter=destinationDevices.begin(); iter != destinationDevices.end(); ++iter) {
    //see if this task was already assigned a stream.
    if (dtask->getCudaStreamForThisTask(*iter) == NULL) {
      dtask->assignDevice(*iter);
      dtask->setCudaStreamForThisTask(*iter, getCudaStreamFromPool(*iter));
    }
  }
}


//______________________________________________________________________
//
void
UnifiedScheduler::findIntAndExtGpuDependencies( DetailedTask * dtask, int iteration, int t_id ) {
  MALLOC_TRACE_TAG_SCOPE("UnifiedScheduler::findIntAndExtGpuDependencies");

  if (gpu_stats.active()) {
    cerrLock.lock();
    gpu_stats << myRankThread() << " findIntAndExtGpuDependencies - task "
        << *dtask << '\n';
    cerrLock.unlock();
  }

  dtask->clearPreparationCollections();
  // Prepare internal dependencies.  Only makes sense if we have GPUs that we are using.
//    if (task->getTask()->usesDevice()) {
  if (Uintah::Parallel::usingDevice()) {
    //If we have ghost cells coming from a GPU to another GPU on the same node
    //This does not cover going to another node (the loop below handles external
    //dependencies) That is handled in initiateH2D()
    //This does not handle preparing from a CPU to a same node GPU.  That is handled in initiateH2D()
    for (DependencyBatch* batch = dtask->getInternalComputes(); batch != 0; batch = batch->comp_next) {
      for (DetailedDep* req = batch->head; req != 0; req = req->next) {
        if ((req->condition == DetailedDep::FirstIteration && iteration > 0)
            || (req->condition == DetailedDep::SubsequentIterations
                && iteration == 0)
            || (notCopyDataVars_.count(req->req->var->getName()) > 0)) {
          // See comment in DetailedDep about CommCondition
          if (gpu_stats.active()) {
            cerrLock.lock();
            gpu_stats << myRankThread()
                << "   Preparing GPU dependencies, ignoring conditional send for "
                << *req << std::endl;
            cerrLock.unlock();
          }
          continue;
        }
        // if we send/recv to an output task, don't send/recv if not an output timestep
        if (req->toTasks.front()->getTask()->getType() == Task::Output
            && !oport_->isOutputTimestep() && !oport_->isCheckpointTimestep()) {
          if (gpu_stats.active()) {
            cerrLock.lock();
            gpu_stats << myRankThread()
                << "   Preparing GPU dependencies, ignoring non-output-timestep send for "
                << *req << std::endl;
            cerrLock.unlock();
          }
          continue;
        }
        OnDemandDataWarehouse* dw = dws[req->req->mapDataWarehouse()].get_rep();

        const VarLabel* posLabel;
        OnDemandDataWarehouse* posDW;

        // the load balancer is used to determine where data was in the old dw on the prev timestep -
        // pass it in if the particle data is on the old dw
        LoadBalancer* lb = 0;

        if (!reloc_new_posLabel_ && parentScheduler_) {
          posDW =
              dws[req->req->task->mapDataWarehouse(Task::ParentOldDW)].get_rep();
          posLabel = parentScheduler_->reloc_new_posLabel_;
        } else {
          // on an output task (and only on one) we require particle variables from the NewDW
          if (req->toTasks.front()->getTask()->getType() == Task::Output) {
            posDW =
                dws[req->req->task->mapDataWarehouse(Task::NewDW)].get_rep();
          } else {
            posDW =
                dws[req->req->task->mapDataWarehouse(Task::OldDW)].get_rep();
            lb = getLoadBalancer();
          }
          posLabel = reloc_new_posLabel_;
        }
        //Invoke Kernel to copy this range out of the GPU.
        prepareGpuDependencies(dtask, batch, posLabel, dw, posDW, req, lb, GpuUtilities::anotherDeviceSameMpiRank);
      }
    }  // end for (DependencyBatch * batch = task->getInteranlComputes() )

    // Prepare external dependencies.  The only thing that needs to be prepared is
    // getting ghost cell data from a GPU into a flat array and copied to host memory
    // so that the MPI engine can treat it normally.
    // That means this handles GPU->other node GPU and GPU->other node CPU.
    //

    for (DependencyBatch* batch = dtask->getComputes(); batch != 0;
        batch = batch->comp_next) {
      for (DetailedDep* req = batch->head; req != 0; req = req->next) {
        if ((req->condition == DetailedDep::FirstIteration && iteration > 0)
            || (req->condition == DetailedDep::SubsequentIterations
                && iteration == 0)
            || (notCopyDataVars_.count(req->req->var->getName()) > 0)) {
          // See comment in DetailedDep about CommCondition
          if (gpu_stats.active()) {
            cerrLock.lock();
            gpu_stats << myRankThread()
                << "   Preparing GPU dependencies, ignoring conditional send for "
                << *req << std::endl;
            cerrLock.unlock();
          }
          continue;
        }
        // if we send/recv to an output task, don't send/recv if not an output timestep
        if (req->toTasks.front()->getTask()->getType() == Task::Output
            && !oport_->isOutputTimestep() && !oport_->isCheckpointTimestep()) {
          if (gpu_stats.active()) {
            cerrLock.lock();
            gpu_stats << myRankThread()
                << "   Preparing GPU dependencies, ignoring non-output-timestep send for "
                << *req << std::endl;
            cerrLock.unlock();
          }
          continue;
        }
        OnDemandDataWarehouse* dw = dws[req->req->mapDataWarehouse()].get_rep();

        if (gpu_stats.active()) {
          cerrLock.lock();
          gpu_stats << myRankThread()
              << " --> Preparing GPU dependencies for sending " << *req
              << ", ghosttype: " << req->req->gtype << ", number of ghost cells: "
              << req->req->numGhostCells << " from dw " << dw->getID() << '\n';
          cerrLock.unlock();
        }
        const VarLabel* posLabel;
        OnDemandDataWarehouse* posDW;

        // the load balancer is used to determine where data was in the old dw on the prev timestep -
        // pass it in if the particle data is on the old dw
        LoadBalancer* lb = 0;

        if (!reloc_new_posLabel_ && parentScheduler_) {
          posDW =
              dws[req->req->task->mapDataWarehouse(Task::ParentOldDW)].get_rep();
          posLabel = parentScheduler_->reloc_new_posLabel_;
        } else {
          // on an output task (and only on one) we require particle variables from the NewDW
          if (req->toTasks.front()->getTask()->getType() == Task::Output) {
            posDW =
                dws[req->req->task->mapDataWarehouse(Task::NewDW)].get_rep();
          } else {
            posDW =
                dws[req->req->task->mapDataWarehouse(Task::OldDW)].get_rep();
            lb = getLoadBalancer();
          }
          posLabel = reloc_new_posLabel_;
        }
        //Invoke Kernel to copy this range out of the GPU.
        prepareGpuDependencies(dtask, batch, posLabel, dw, posDW, req, lb, GpuUtilities::anotherMpiRank);
      }
    }  // end for (DependencyBatch * batch = task->getComputes() )
  }
}


//______________________________________________________________________
//
void
UnifiedScheduler::syncTaskGpuDWs( DetailedTask * dtask ) {

  //For each GPU datawarehouse, see if there are ghost cells listed to be copied
  //if so, launch a kernel that copies them.
  std::set<unsigned int> deviceNums = dtask->getDeviceNums();
  GPUDataWarehouse *taskgpudw;
  for (std::set<unsigned int>::const_iterator deviceNums_it = deviceNums.begin(); deviceNums_it != deviceNums.end(); ++deviceNums_it) {
    const unsigned int currentDevice = *deviceNums_it;
    taskgpudw = dtask->getTaskGpuDataWarehouse(currentDevice,Task::OldDW);
    if (taskgpudw) {
      taskgpudw->syncto_device(dtask->getCudaStreamForThisTask(currentDevice));
    }

    taskgpudw = dtask->getTaskGpuDataWarehouse(currentDevice,Task::NewDW);
    if (taskgpudw) {
      taskgpudw->syncto_device(dtask->getCudaStreamForThisTask(currentDevice));
    }
  }
}


//______________________________________________________________________
//
void
UnifiedScheduler::performInternalGhostCellCopies( DetailedTask * dtask ) {

  //For each GPU datawarehouse, see if there are ghost cells listed to be copied
  //if so, launch a kernel that copies them.
  std::set<unsigned int> deviceNums = dtask->getDeviceNums();
  for (std::set<unsigned int>::const_iterator deviceNums_it = deviceNums.begin(); deviceNums_it != deviceNums.end(); ++deviceNums_it) {
    const unsigned int currentDevice = *deviceNums_it;
    if (dtask->getTaskGpuDataWarehouse(currentDevice, Task::OldDW) != NULL
        && dtask->getTaskGpuDataWarehouse(currentDevice, Task::OldDW)->ghostCellCopiesNeeded()) {
      dtask->getTaskGpuDataWarehouse(currentDevice, Task::OldDW)->copyGpuGhostCellsToGpuVarsInvoker(dtask->getCudaStreamForThisTask(currentDevice));
    } else {
      if (gpu_stats.active()) {
        cerrLock.lock();
        {
          gpu_stats << myRankThread()
              << " A No internal ghost cell copies needed for this task \""
              << dtask->getName() << "\"\'s old DW"<< endl;
        }
        cerrLock.unlock();
      }
    }
    if (dtask->getTaskGpuDataWarehouse(currentDevice, Task::NewDW) != NULL
        && dtask->getTaskGpuDataWarehouse(currentDevice, Task::NewDW)->ghostCellCopiesNeeded()) {
      dtask->getTaskGpuDataWarehouse(currentDevice, Task::NewDW)->copyGpuGhostCellsToGpuVarsInvoker(dtask->getCudaStreamForThisTask(currentDevice));
    } else {
      if (gpu_stats.active()) {
        cerrLock.lock();
        {
          gpu_stats << myRankThread()
              << " B No internal ghost cell copies needed for this task \""
              << dtask->getName() << "\"\'s new DW"<< endl;
        }
        cerrLock.unlock();
      }
    }
  }
}


//______________________________________________________________________
//
void
UnifiedScheduler::copyAllGpuToGpuDependences( DetailedTask * dtask ) {
  //Iterate through the ghostVars, find all whose destination is another GPU same MPI rank
  //Get the destination device, the size
  //And do a straight GPU to GPU copy.
  const map<GpuUtilities::GhostVarsTuple, DeviceGhostCellsInfo> & ghostVarMap = dtask->getGhostVars().getMap();
  for (map<GpuUtilities::GhostVarsTuple, DeviceGhostCellsInfo>::const_iterator it=ghostVarMap.begin(); it!=ghostVarMap.end(); ++it) {
    if (it->second.dest == GpuUtilities::anotherDeviceSameMpiRank) {
      //TODO: Needs a particle section

      IntVector ghostLow = it->first.sharedLowCoordinates;
      IntVector ghostHigh = it->first.sharedHighCoordinates;
      IntVector ghostSize(ghostHigh.x() - ghostLow.x(), ghostHigh.y() - ghostLow.y(), ghostHigh.z() - ghostLow.z());
      int3 device_source_offset;
      int3 device_source_size;

      //get the source variable from the source GPU DW
      void *device_source_ptr;
      size_t elementDataSize = it->second.xstride;
      size_t memSize = ghostSize.x() * ghostSize.y() * ghostSize.z() * elementDataSize;
      GPUGridVariableBase* device_source_var = OnDemandDataWarehouse::createGPUGridVariable(elementDataSize);
      OnDemandDataWarehouseP dw = dws[it->first.dataWarehouse];
      GPUDataWarehouse* gpudw = dw->getGPUDW(it->second.sourceDeviceNum);
      gpudw->getStagingVar(*device_source_var,
                 it->first.label.c_str(),
                 it->second.sourcePatchPointer->getID(),
                 it->first.matlIndx,
                 it->first.levelIndx,
                 make_int3(ghostLow.x(),ghostLow.y(), ghostLow.z()),
                 make_int3(ghostSize.x(), ghostSize.y(), ghostSize.z()));
      device_source_var->getArray3(device_source_offset, device_source_size, device_source_ptr);

      //Get the destination variable from the destination GPU DW
      gpudw = dw->getGPUDW(it->second.destDeviceNum);
      int3 device_dest_offset;
      int3 device_dest_size;
      void *device_dest_ptr;
      GPUGridVariableBase* device_dest_var = OnDemandDataWarehouse::createGPUGridVariable(elementDataSize);
      gpudw->getStagingVar(*device_dest_var,
                     it->first.label.c_str(),
                     it->second.destPatchPointer->getID(),
                     it->first.matlIndx,
                     it->first.levelIndx,
                     make_int3(ghostLow.x(),ghostLow.y(), ghostLow.z()),
                     make_int3(ghostSize.x(), ghostSize.y(), ghostSize.z()));
        device_dest_var->getArray3(device_dest_offset, device_dest_size, device_dest_ptr);


      if (gpu_stats.active()) {
        cerrLock.lock();
        {
          gpu_stats << myRankThread()
             << " GpuDependenciesToHost()  - \""
             << "GPU to GPU peer transfer from GPU #"
             << it->second.sourceDeviceNum << " to GPU #"
             << it->second.destDeviceNum << " for label "
             << it->first.label << " from patch "
             << it->second.sourcePatchPointer->getID() << " to patch "
             << it->second.destPatchPointer->getID() << " matl "
             << it->first.matlIndx << " level "
             << it->first.levelIndx << " size = "
             << std::dec << memSize << " from ptr " << std::hex
             << device_source_ptr << " to ptr " << std::hex << device_dest_ptr
             << ", using stream " << std::hex
             << dtask->getCudaStreamForThisTask(it->second.sourceDeviceNum) << std::dec << endl;
        }
        cerrLock.unlock();
      }

      //We can run peer copies from the source or the device stream.  While running it
      //from the device technically is said to be a bit slower, it's likely just
      //to an extra event being created to manage blocking the destination stream.
      //By putting it on the device we are able to not need a synchronize step after
      //all the copies, because any upcoming API call will use the streams and be
      //naturally queued anyway.  When a copy completes, anything placed in the
      //destiantion stream can then process.
      //Note: If we move to UVA, then we could just do a straight memcpy

      cudaStream_t* stream = dtask->getCudaStreamForThisTask(it->second.destDeviceNum);
      OnDemandDataWarehouse::uintahSetCudaDevice(it->second.destDeviceNum);

      if (simulate_multiple_gpus.active()) {
         CUDA_RT_SAFE_CALL( cudaMemcpyPeerAsync  (device_dest_ptr,
                     0,
                     device_source_ptr,
                     0,
                     memSize,
                     *stream) );

       } else {

         CUDA_RT_SAFE_CALL( cudaMemcpyPeerAsync  (device_dest_ptr,
           it->second.destDeviceNum,
           device_source_ptr,
           it->second.sourceDeviceNum,
           memSize,
           *stream) );
      }
    }
  }
}

//______________________________________________________________________
//
void
UnifiedScheduler::copyAllExtGpuDependenciesToHost( DetailedTask * dtask ) {

  bool copiesExist = false;

  //If we put it in ghostVars, then we copied it to an array on the GPU (D2D).  Go through the ones that indicate
  //they are going to another MPI rank.  Copy them out to the host (D2H).  To make the engine cleaner for now,
  //we'll then do a H2H copy step into the variable.  In the future, to be more efficient, we could skip the
  //host to host copy and instead have sendMPI() send the array we get from the device instead.  To be even more efficient
  //than that, if everything is pinned, unified addressing set up, and CUDA aware MPI used, then we could pull
  //everything out via MPI that way and avoid the manual D2H copy and the H2H copy.
  const map<GpuUtilities::GhostVarsTuple, DeviceGhostCellsInfo> & ghostVarMap = dtask->getGhostVars().getMap();
  for (map<GpuUtilities::GhostVarsTuple, DeviceGhostCellsInfo>::const_iterator it=ghostVarMap.begin(); it!=ghostVarMap.end(); ++it) {
    //TODO: Needs a particle section
    if (it->second.dest == GpuUtilities::anotherMpiRank) {
      void* host_ptr    = NULL;    // host base pointer to raw data
      void* device_ptr  = NULL;  // device base pointer to raw data
      size_t host_bytes = 0;
      IntVector host_low, host_high, host_offset, host_size, host_strides;
      int3 device_offset;
      int3 device_size;


      //We created a temporary host variable for this earlier,
      //and the deviceVars collection knows about it.  It's set as a foreign var.
      IntVector ghostLow = it->first.sharedLowCoordinates;
      IntVector ghostHigh = it->first.sharedHighCoordinates;
      IntVector ghostSize(ghostHigh.x() - ghostLow.x(), ghostHigh.y() - ghostLow.y(), ghostHigh.z() - ghostLow.z());
      DeviceGridVariableInfo item = dtask->getDeviceVars().getStagingItem(it->first.label,
                 it->second.sourcePatchPointer,
                 it->first.matlIndx,
                 it->first.levelIndx,
                 ghostLow,
                 ghostSize,
                 (const int)it->first.dataWarehouse);
      GridVariableBase* tempGhostVar = (GridVariableBase*)item.var;

      tempGhostVar->getSizes(host_low, host_high, host_offset, host_size, host_strides);

      host_ptr = tempGhostVar->getBasePointer();
      host_bytes = tempGhostVar->getDataSize();

      // copy the computes data back to the host
      //d2hComputesLock_.writeLock();
      //{

        GPUGridVariableBase* device_var = OnDemandDataWarehouse::createGPUGridVariable(host_strides.x());
        OnDemandDataWarehouseP dw = dws[it->first.dataWarehouse];
        GPUDataWarehouse* gpudw = dw->getGPUDW(it->second.sourceDeviceNum);
        gpudw->getStagingVar(*device_var,
                   it->first.label.c_str(),
                   it->second.sourcePatchPointer->getID(),
                   it->first.matlIndx,
                   it->first.levelIndx,
                   make_int3(ghostLow.x(),ghostLow.y(), ghostLow.z()),
                   make_int3(ghostSize.x(), ghostSize.y(), ghostSize.z()));
        device_var->getArray3(device_offset, device_size, device_ptr);

        // if offset and size is equal to CPU DW, directly copy back to CPU var memory;
        if (device_offset.x == host_low.x()
            && device_offset.y == host_low.y()
            && device_offset.z == host_low.z()
            && device_size.x == host_size.x()
            && device_size.y == host_size.y()
            && device_size.z == host_size.z()) {

          //Since we know we need a stream, obtain one.

          cudaStream_t* stream = dtask->getCudaStreamForThisTask(it->second.sourceDeviceNum);
          OnDemandDataWarehouse::uintahSetCudaDevice(it->second.sourceDeviceNum);
          if (gpu_stats.active()) {
            cerrLock.lock();
            {
              gpu_stats << myRankThread()
                  << " copyAllExtGpuDependenciesToHost()  - \""
                  << it->first.label << "\", size = "
                  << std::dec << host_bytes << " to " << std::hex
                  << host_ptr << " from " << std::hex << device_ptr
                  << ", using stream " << std::hex
                  << dtask->getCudaStreamForThisTask(it->second.sourceDeviceNum) << std::dec << endl;
            }
            cerrLock.unlock();
          }

          CUDA_RT_SAFE_CALL(cudaMemcpyAsync(host_ptr, device_ptr, host_bytes, cudaMemcpyDeviceToHost, *stream));
          //CUDA_RT_SAFE_CALL(cudaMemcpy(host_ptr, device_ptr, host_bytes, cudaMemcpyDeviceToHost));

          /*cudaDeviceSynchronize();
          double * dst = new double[tempGhostVar->getDataSize()/sizeof(double)];
          tempGhostVar->copyOut(dst);
          for (int i = 0; i < tempGhostVar->getDataSize()/sizeof(double); ++i) {
            printf("for patch %d at tempGhostVar[%d], value is %1.6lf\n",it->second.sourcePatchPointer->getID(),i, dst[i]);
          }
          delete[] dst;
          */
          copiesExist = true;
        } else {
          cerr << "unifiedSCheduler::GpuDependenciesToHost() - Error - The host and device variable sizes did not match.  Cannot copy D2H." <<endl;
          SCI_THROW(InternalError("Error - The host and device variable sizes did not match.  Cannot copy D2H", __FILE__, __LINE__));
        }

      //}
      //d2hComputesLock_.writeUnlock();
      delete device_var;
    }
  }

  if (copiesExist) {

    //Wait until all streams are done
    //Further optimization could be to check each stream one by one and make copies before waiting for other streams to complete.
    //TODO: There's got to be a better way to do this.
    while (!dtask->checkAllCudaStreamsDoneForThisTask()) {
      //sleep?
      //printf("Sleeping\n");
    }


    for (map<GpuUtilities::GhostVarsTuple, DeviceGhostCellsInfo>::const_iterator it=ghostVarMap.begin(); it!=ghostVarMap.end(); ++it) {

      if (it->second.dest == GpuUtilities::anotherMpiRank) {
        //TODO: Needs a particle section
        IntVector host_low, host_high, host_offset, host_size, host_strides;

        //We created a temporary host variable for this earlier,
        //and the deviceVars collection knows about it.
        IntVector ghostLow = it->first.sharedLowCoordinates;
        IntVector ghostHigh = it->first.sharedHighCoordinates;
        IntVector ghostSize(ghostHigh.x() - ghostLow.x(), ghostHigh.y() - ghostLow.y(), ghostHigh.z() - ghostLow.z());
        DeviceGridVariableInfo item = dtask->getDeviceVars().getStagingItem(it->first.label,
                   it->second.sourcePatchPointer,
                   it->first.matlIndx,
                   it->first.levelIndx,
                   ghostLow,
                   ghostSize,
                   (const int)it->first.dataWarehouse);

        //We created a temporary host variable for this earlier,
        //and the deviceVars collection knows about it.

        GridVariableBase* tempGhostVar = (GridVariableBase*)item.var;

        OnDemandDataWarehouseP dw = dws[(const int)it->first.dataWarehouse];


        //Also get the existing host copy
        GridVariableBase* gridVar = dynamic_cast<GridVariableBase*>(it->second.label->typeDescription()->createInstance());

        //Get variable size. Scratch computes means we need to factor that in when computing the size.
        Patch::VariableBasis basis = Patch::translateTypeToBasis(it->second.label->typeDescription()->getType(), false);
        IntVector lowIndex, highIndex;
        IntVector lowOffset, highOffset;

        Patch::getGhostOffsets(
            gridVar->virtualGetTypeDescription()->getType(),
            item.dep->gtype, item.dep->numGhostCells, lowOffset,
            highOffset);
        it->second.sourcePatchPointer->computeExtents(basis,
            item.dep->var->getBoundaryLayer(), lowOffset, highOffset,
            lowIndex, highIndex);

        //size_t memSize = (highIndex.x() - lowIndex.x())
        //    * (highIndex.y() - lowIndex.y())
        //    * (highIndex.z() - lowIndex.z()) * host_strides.x();

        //If it doesn't exist yet on the host, create it.  If it does exist on the host, then
        //if we got here that meant the host data was invalid and the device data was valid, so
        //nuke the old contents and create a new one.  (Should we just get a mutable var instead
        //as it should be the size we already need?)  This process is admittedly a bit hacky,
        //as now the var will be both partially valid and invalid.  The ghost cell region is now
        //valid on the host, while the rest of the host var would be invalid.
        //Since we are writing to an old data warehouse (from device to host), we need to
        //temporarily unfinalize it.
        const bool finalized = dw->isFinalized();
        if (finalized) {
          dw->unfinalize();
        }

        if (!dw->exists(item.dep->var, it->first.matlIndx, it->second.sourcePatchPointer)) {
          dw->allocateAndPut(*gridVar, item.dep->var, it->first.matlIndx,
              it->second.sourcePatchPointer, item.dep->gtype,
              item.dep->numGhostCells);
        } else {
          //Get a const variable in a non-constant way.
          //This assumes the variable has already been resized properly, which is why ghost cells are set to zero.
          //TODO: Check sizes anyway just to be safe.
          dw->getModifiable(*gridVar, item.dep->var, it->first.matlIndx, it->second.sourcePatchPointer, Ghost::None, 0);

        }
        //Do a host-to-host copy to bring the device data now on the host into the host-side variable so
        //that sendMPI can easily find the data as if no GPU were involved at all.
        gridVar->copyPatch(tempGhostVar, ghostLow, ghostHigh );
        if(finalized) {
          dw->refinalize();
        }


        //If there's ever a need to manually verify what was copied host-to-host, use this code below.
        //if (/*it->second.sourcePatchPointer->getID() == 1 && */ item.dep->var->d_name == "phi") {
        /*  double * phi_data = new double[gridVar->getDataSize()/sizeof(double)];
          tempGhostVar->copyOut(phi_data);
          IntVector l = ghostLow;
          IntVector h = ghostHigh;
          int zlow = l.z();
          int ylow = l.y();
          int xlow = l.x();
          int zhigh = h.z();
          int yhigh = h.y();
          int xhigh = h.x();
          //int ghostLayers = 0;
          //int ystride = yhigh + ghostLayers;
          //int xstride = xhigh + ghostLayers;
          int ystride = h.y() - l.y();
          int xstride = h.x() - l.x();

          printf(" - Going to copy data between (%d, %d, %d) and (%d, %d, %d)\n", l.x(), l.y(), l.z(), h.x(), h.y(), h.z());
          for (int k = l.z(); k < zhigh; ++k) {
            for (int j = l.y(); j < yhigh; ++j) {
              for (int i = l.x(); i < xhigh; ++i) {
                //cout << "(x,y,z): " << k << "," << j << "," << i << endl;
                // For an array of [ A ][ B ][ C ], we can index it thus:
                // (a * B * C) + (b * C) + (c * 1)
                int idx = ((i-xlow) + ((j-ylow) * xstride) + ((k-zlow) * xstride * ystride));
                printf(" - phi(%d, %d, %d) is %1.6lf ptr %p base pointer %p idx %d\n", i, j, k, phi_data[idx], phi_data + idx, phi_data, idx);
              }
            }
          } */
        //}

        //let go of our reference counters.
        delete gridVar;
        delete tempGhostVar;
      }
    }
  }
}

#endif


//______________________________________________________________________
//  generate string   <MPI rank>.<Thread ID>
//  useful to see who running what    
std::string
UnifiedScheduler::myRankThread()
{
  std::ostringstream out;
  out << Uintah::Parallel::getMPIRank()<< "." << Impl::t_tid;
  return out.str();
}

//______________________________________________________________________
//
void
UnifiedScheduler::init_threads(UnifiedScheduler * sched, int numThreads_ )
{
  Impl::init_threads(sched, numThreads_);
}

//------------------------------------------
// UnifiedSchedulerWorker Thread Methods
//------------------------------------------
UnifiedSchedulerWorker::UnifiedSchedulerWorker( UnifiedScheduler * scheduler )
  : d_scheduler{scheduler }
  , d_rank{ scheduler->getProcessorGroup()->myrank() }
{
}


//______________________________________________________________________
//
void
UnifiedSchedulerWorker::run()
{
  while( Impl::g_run_tasks ) {
    d_waittime += Time::currentSeconds() - d_waitstart;

    try {
      d_scheduler->runTasks(Impl::t_tid);
    }
    catch (Exception& e) {
      std::lock_guard<std::mutex> lock(g_worker_io_mutex);
      {
        std::cerr << "Worker " << d_rank << "-" << Impl::t_tid << ": Caught exception: " << e.message() << "\n";
        if (e.stackTrace()) {
          std::cerr << "Stack trace: " << e.stackTrace() << '\n';
        }
      }
    }
  }
}


//______________________________________________________________________
//
double
UnifiedSchedulerWorker::getWaittime()
{
  return d_waittime;
}


//______________________________________________________________________
//
void
UnifiedSchedulerWorker::resetWaittime( double start )
{
  d_waitstart = start;
  d_waittime  = 0.0;
}
