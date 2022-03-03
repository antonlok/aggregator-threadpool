/**
 * File: thread-pool.cc
 * --------------------
 * Presents the implementation of the ThreadPool class.
 */

#include "thread-pool.h"
#include "ostreamlock.h"

using namespace std;
using develop::ThreadPool;


ThreadPool::ThreadPool(size_t numThreads) : workerVector(numThreads), nextSpawnID(0), pendingThunks(0), exitFlag(false), workerIsAvailable(numThreads) {
  dispatcherThread = thread([this]() { dispatcher(); });
}

void ThreadPool::schedule(const function<void(void)>& thunk) {
  queueLock.lock();
  thunkQueue.push(thunk);
  queueLock.unlock();

  pendingThunksLock.lock();
  pendingThunks++;
  pendingThunksLock.unlock();

  newThunkFromScheduler.signal();
}

void ThreadPool::dispatcher() {
  while (true) {
    newThunkFromScheduler.wait();
    workerIsAvailable.wait();

    if (exitFlag) {
      break;
    }

    int availableWorkerID = -1;
    workersLock.lock();
    for (size_t candidateWorkerID = 0; candidateWorkerID < workerVector.size(); candidateWorkerID++) {
      // If we hit the next Spawn ID, all existing workers were busy. Fire up a new one.
      if (candidateWorkerID == nextSpawnID) {
        nextSpawnIDLock.lock();
        workerVector[nextSpawnID].workerThread = thread(&ThreadPool::worker, this, nextSpawnID);
        nextSpawnID++;
        nextSpawnIDLock.unlock();
      }
      // Select a free worker, which may or may not have just been spawned.
      if (!workerVector[candidateWorkerID].workerInUse) {
        workerVector[candidateWorkerID].workerInUse = true;
        availableWorkerID = candidateWorkerID;
        workersLock.unlock();
        break;
      }
    }

    queueLock.lock();
    function<void(void)> currentThunk = thunkQueue.front();
    thunkQueue.pop();
    queueLock.unlock();

    workersLock.lock();
    workerVector[availableWorkerID].workerThunk = currentThunk;
    workerVector[availableWorkerID].thunkToExecute.signal();
    workersLock.unlock();
  }
}

void ThreadPool::worker(size_t workerID) {
  while (true) {
    workerVector[workerID].thunkToExecute.wait();

    if (exitFlag) {
      break;
    }

    workerVector[workerID].workerThunk();

    pendingThunksLock.lock();
    pendingThunks--;
    if (pendingThunks == 0) { // Notify wait() if there are no thunks left.
      pendingThunksCondVar.notify_all();
    }
    pendingThunksLock.unlock();

    workersLock.lock();
    workerVector[workerID].workerInUse = false;
    workersLock.unlock();

    workerIsAvailable.signal();
  }
}

void ThreadPool::wait() {
  // Wait for a signal from the worker that all thunks are complete, 
  // but also verify, as more may have been added since the signal.
  unique_lock<mutex> pendingThunksLockAdapter(pendingThunksLock);
  pendingThunksCondVar.wait(pendingThunksLock, [this] { return pendingThunks == 0; });
}

ThreadPool::~ThreadPool() {
  wait();
  exitFlag = true;

  for (size_t workerID = 0; workerID < nextSpawnID; workerID++) {
    workerVector[workerID].thunkToExecute.signal();
    workerVector[workerID].workerThread.join();
  }

  newThunkFromScheduler.signal(); 
  workerIsAvailable.signal();
  dispatcherThread.join();
}
