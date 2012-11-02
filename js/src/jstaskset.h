/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 4 -*-
 * vim: set ts=8 sw=4 et tw=78:
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef jstaskset_h___
#define jstaskset_h___

#include "jsthreadpool.h"

namespace js {

// Parallel operations in general can have one of three states.  They
// may succeed, fail, or "bail", where bail indicates that the code
// encountered an unexpected condition and should be re-run
// sequentially.
enum ParallelResult { TP_SUCCESS, TP_RETRY_SEQUENTIALLY, TP_FATAL };

struct TaskSet;

// Executes the given |TaskSet| in parallel using the runtime's
// |ThreadPool|, returning upon completion.  In general, if there are
// |N| workers in the threadpool, the problem will be divided into
// |N+1| slices, as the main thread will also execute one slice.
ParallelResult ExecuteTaskSet(JSContext *cx, TaskSet &taskSet);

class TaskSetSharedContext;
class AutoRendezvous;
class AutoSetThreadContext;
namespace gc { struct ArenaLists; }

struct ThreadContext
{
public:
    JS::PerThreadData *perThreadData;
    const size_t threadId;
    const size_t numThreads;
    uintptr_t ionStackLimit;

    // Arenas to use when allocating on this thread.
    // See |js::ion::ParFunctions::ParNewGCThing()|.
    gc::ArenaLists *const arenaLists;

    ThreadContext(JS::PerThreadData *perThreadData, size_t threadId, size_t numThreads,
                  uintptr_t stackLimit, js::gc::ArenaLists *arenaLists,
                  TaskSetSharedContext *shared);

    // True if this is the main thread, false if it is one of the parallel workers
    bool isMainThread();

    // Generally speaking, if a thread returns false, that is
    // interpreted as a "bailout"---meaning, a recoverable error.  If
    // however you call this function before returning false, then the
    // error will be interpreted as *fatal*.  This doesn't strike me
    // as the most elegant solution here but I don't know what'd be better.
    //
    // For convenience, *always* returns false.
    bool setFatal();

    // During the parallel phase, this method should be invoked
    // periodically, for example on every backedge, similar to the
    // interrupt check.  If it returns false, then the parallel phase
    // has been aborted and so you should bailout.  The function may
    // also rendesvous to perform GC or do other similar things.
    //
    // The |ThreadContext| passed as argument should be equal to
    // |ThreadContext::current()|.
    bool check();

    // Returns the runtime.  Be wary, this is shared between all threads!
    JSRuntime *runtime();

    // Access current context using thread-local data.
    static inline ThreadContext *current();
    static bool Initialize();

private:
    friend class AutoRendezvous;
    friend class AutoSetThreadContext;

    static PRUintn ThreadPrivateIndex; // initialized by Initialize()

    TaskSetSharedContext *const shared;
};

// Generic interface for specifying parallel operations.
struct TaskSet
{
public:
    // Invoked before parallel phase begins; informs the task set how
    // many worker threads there will be and gives it a chance to
    // initialize per-thread data structures
    //
    // Returns true on success, false to halt parallel execution.
    virtual bool pre(size_t numThreads) = 0;

    // Invoked from each parallel thread.  The |ThreadContext| which
    // is supplied will also be available using TLS.
    //
    // Returns true on success, false to halt parallel execution.
    virtual bool parallel(ThreadContext &taskSetCx) = 0;

    // Invoked after parallel phase ends if execution was successful
    // (not aborted)
    //
    // Returns true on success, false to halt parallel execution.
    virtual bool post(size_t numThreads) = 0;
};

/* True if this thread is currently executing a ParallelArray
   operation across multiple threads. */
static inline bool InParallelSection() {
#   ifdef JS_THREADSAFE_ION
    return ThreadContext::current() != NULL;
#   else
    return false;
#   endif
}

#endif

}
