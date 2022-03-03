/**
 * File: thread-pool.h
 * -------------------
 * Exports a ThreadPool abstraction, which manages a finite pool
 * of worker threads that collaboratively work through a sequence of tasks.
 * As each task is scheduled, the ThreadPool waits for at least
 * one worker thread to be free and then assigns that task to that worker.
 * Threads are scheduled and served in a FIFO manner, and tasks need to
 * take the form of thunks, which are zero-argument thread routines.
 */

#ifndef _thread_pool_
#define _thread_pool_

#include <unistd.h>
#include <cstdlib>
#include <functional>
#include <iostream>
#include <queue>
#include <thread>
#include <vector>
#include <mutex>
#include <semaphore.h>


// place additional #include statements here

namespace develop {

class ThreadPool {
 public:
  /**
   * Constructs a ThreadPool configured to spawn up to the specified
   * number of threads.
   */
  ThreadPool(size_t numThreads);

  /**
   * Destroys the ThreadPool class
   */
  ~ThreadPool();

  /**
   * Schedules the provided thunk (which is something that can
   * be invoked as a zero-argument function without a return value)
   * to be executed by one of the ThreadPool's threads as soon as
   * all previously scheduled thunks have been handled.
   * 
   * Increments the thunk counter.
   */
  void schedule(const std::function<void(void)>& thunk);

  /**
   * Blocks and waits until all previously scheduled thunks
   * have been executed in full.
   */
  void wait();

 private:
  
  typedef struct workerStruct {
    workerStruct() : workerInUse(false){};
    std::thread workerThread; // Self-explanatory.
    std::function<void(void)> workerThunk; // Self-explanatory.
    semaphore thunkToExecute; // Binary coordination case used to track when the thunk should be run.
    bool workerInUse; // Self-explanatory.
  } workerStruct;

  std::vector<workerStruct> workerVector; // Vector of worker structs.
  std::thread dispatcherThread; // Single thread for the dispatcher.

  std::queue<std::function<void(void)>> thunkQueue; // Used to store the thunks waiting for worker assignment.

  int nextSpawnID; // Used to store the ID of the next worker to be spawned.
  int pendingThunks; // Used to store count of remaining thunks.
  bool exitFlag; // Used to indicate that execution is in the destructor.

  std::mutex workersLock; // Used to protect access to the worker vector.
  std::mutex queueLock; // Used to protect access to the queue of thunks.
  std::mutex pendingThunksLock; // Used to protect access to the thunk counter.
  std::mutex nextSpawnIDLock; // Used to protect access to the nexr worker index.

  std::condition_variable_any pendingThunksCondVar; // Used to track if there are any thunks left.

  semaphore workerIsAvailable; // Permits case used to track worker count.
  semaphore newThunkFromScheduler; // Binary coordination case used to track when the scheduler has a new thunk for the dispatcher.

  /**
   * Waits for a thunk to be added to the queue.
   * Waits for a worker thread to become available.
   * Selects an available worker.
   * Marks the worker as unavailable.
   * Removes the thunk from the queue.
   * Pushes the thunk to the worker.
   * Signals the worker to begin execution.
   */
  void dispatcher();

  /**
   * Waits for the dispatcher to signal it to start.
   * Executes the thunk that it holds (updated by the dispatcher!).
   * Marks itself as available once again.
   * Also decrements the thunk counter, signaling end of life for the executed thunk.
   */
  void worker(size_t workerID);

  ThreadPool(const ThreadPool& original) = delete;
  ThreadPool& operator=(const ThreadPool& rhs) = delete;
};

#endif
}
