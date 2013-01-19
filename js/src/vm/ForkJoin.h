/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 4 -*-
 * vim: set ts=8 sw=4 et tw=78:
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef ForkJoin_h__
#define ForkJoin_h__

#include "jscntxt.h"
#include "vm/ThreadPool.h"

// ForkJoin
//
// This is the building block for executing multi-threaded JavaScript with
// shared memory (as distinct from Web Workers).  The idea is that you have
// some (typically data-parallel) operation which you wish to execute in
// parallel across as many threads as you have available.  An example might be
// applying |map()| to a vector in parallel. To implement such a thing, you
// would define a subclass of |ForkJoinOp| to implement the operation and then
// invoke |ExecuteForkJoinOp()|, as follows:
//
//     class MyForkJoinOp {
//       ... define callbacks as appropriate for your operation ...
//     };
//     MyForkJoinOp op;
//     ExecuteForkJoinOp(cx, op);
//
// |ExecuteForkJoinOp()| will fire up the workers in the runtime's
// thread pool, have them execute the callback |parallel()| defined in
// the |ForkJoinOp| class, and then return once all the workers have
// completed.  You will receive |N| calls to the |parallel()|
// callback, where |N| is the value returned by |ForkJoinSlice()|.
// Each callback will be supplied with a |ForkJoinSlice| instance
// providing some context.
//
// Typically there will be one call to |parallel()| from each worker thread,
// but that is not something you should rely upon---if we implement
// work-stealing, for example, then it could be that a single worker thread
// winds up handling multiple slices.
//
// Operation callback:
//
// During parallel execution, you should periodically invoke |slice.check()|,
// which will handle the operation callback.  If the operation callback is
// necessary, |slice.check()| will arrange a rendezvous---that is, as each
// active worker invokes |check()|, it will come to a halt until everyone is
// blocked (Stop The World).  At this point, we perform the callback on the
// main thread, and then resume execution.  If a worker thread terminates
// before calling |check()|, that's fine too.  We assume that you do not do
// unbounded work without invoking |check()|.
//
// For more details on how operation callbacks and so forth are signaled,
// see the section below on Signaling Abort and Interrupts.
//
// Sequential Fallback:
//
// It is assumed that anyone using this API must be prepared for a sequential
// fallback.  Therefore, the |ExecuteForkJoinOp()| returns a status code
// indicating whether a fatal error occurred (in which case you should just
// stop) or whether you should retry the operation, but executing
// sequentially.  An example of where the fallback would be useful is if the
// parallel code encountered an unexpected path that cannot safely be executed
// in parallel (writes to shared state, say).
//
// Garbage collection and allocation:
//
// Code which executes on these parallel threads must be very careful
// with respect to garbage collection and allocation.  Currently, we
// do not permit GC to occur when executing in parallel.  Furthermore,
// the typical allocation paths are UNSAFE in parallel code because
// they access shared state (the compartment's arena lists and so
// forth) without any synchronization.
//
// To deal with this, the forkjoin code creates a distinct |Allocator|
// object for each slice.  You can access the appropriate object via
// the |ForkJoinSlice| object that is provided to the callbacks.  Once
// the execution is complete, all the objects found in these distinct
// |Allocator| is merged back into the main compartment lists and
// things proceed normally.
//
// In Ion-generated code, we will do allocation through the |Allocator|
// found in |ForkJoinSlice| (which is obtained via TLS).  Also, no
// write barriers are emitted.  Conceptually, we should never need a
// write barrier because we only permit writes to objects that are
// newly allocated, and such objects are always black (to use inc. GC
// terminology).  However, to be safe, we also block upon entering a
// parallel section to ensure that any concurrent marking or inc. GC
// has completed.
//
// In the future, it should be possible to lift the restriction that
// we must block until inc. GC has completed and also to permit GC
// during parallel execution. But we're not there yet.
//
// Signaling Aborts and Interrupts:
//
// Parallel execution needs to periodically "check in" to determine
// whether an interrupt has been signaled or whether one of the other
// threads has requested an abort.  This is done by checking two flags
// on the runtime (runtime->parallelAbort and runtime->interrupt).  If
// either flag is true, then the check() method is invoked. This design
// seems to be non-ideal---as it would be nice to check a single flag, and
// it would be nice if the parallelAbort flag were a member of ForkJoinShared
// rather than the runtime---but there are several constraining factors:
//
// - We need to be able to distinguish a user-requested interrupt from an
//   internal parallel abort. I considered setting the interrupt flag for both,
//   but that would potentially lead to extra calls to the operation callback.
//   Moreover, because the user requests an interrupt asynchronously, we must be
//   prepared for the situation that both an interrupt *and* an abort have been
//   requested.
//
// - Placing the flags in the JSRuntime* means that we can bake the
//   pointer into the generated code, which is more efficient than
//   dereferencing the per-thread-data (only one load).
//
// - In normal ion code, on entry to a function we check the stack
//   limit and on backedges we check the interrupt flag.  This is
//   sufficient because when an interrupt is signaled we clear the
//   stack limit.  But this doesn't work in the parallel setting
//   because we'd have to clear the stack limits for all threads.
//   This is not possible since the interrupt code runs asynchronously
//   and doesn't have access to all the stacks.  Moreover, the other
//   threads may be in the process of terminating, etc, so that would
//   be tricky.  Instead we just check the interrupt and parallelAbort
//   flags on entry to the function as well.
//
// - Anyway, perhaps the details of this design will change in the future.
//
// Current Limitations:
//
// - The API does not support recursive or nested use.  That is, the
//   |parallel()| callback of a |ForkJoinOp| may not itself invoke
//   |ExecuteForkJoinOp()|.  We may lift this limitation in the future.
//
// - No load balancing is performed between worker threads.  That means that
//   the fork-join system is best suited for problems that can be slice into
//   uniform bits.


namespace js {

// Parallel operations in general can have one of three states.  They may
// succeed, fail, or "bail", where bail indicates that the code encountered an
// unexpected condition and should be re-run sequentially.
// Different subcategories of the "bail" state are encoded as variants of
// TP_RETRY_*.
enum ParallelResult { TP_SUCCESS, TP_RETRY_SEQUENTIALLY, TP_RETRY_AFTER_GC, TP_FATAL };

struct ForkJoinOp;

// Returns the number of slices that a fork-join op will have when
// executed.
uint32_t ForkJoinSlices(JSContext *cx);

// Executes the given |TaskSet| in parallel using the runtime's |ThreadPool|,
// returning upon completion.  In general, if there are |N| workers in the
// threadpool, the problem will be divided into |N+1| slices, as the main
// thread will also execute one slice.
ParallelResult ExecuteForkJoinOp(JSContext *cx, ForkJoinOp &op);

class PerThreadData;
class ForkJoinShared;
class AutoRendezvous;
class AutoSetForkJoinSlice;

#ifdef DEBUG
struct IonTraceData {
    uint32_t bblock;
    uint32_t lir;
    uint32_t execModeInt;
    const char *lirOpName;
    const char *mirOpName;
    JSScript *script;
    jsbytecode *pc;
};
#endif

struct ForkJoinSlice
{
  public:
    // PerThreadData corresponding to the current worker thread.
    PerThreadData *perThreadData;

    // Which slice should you process? Ranges from 0 to |numSlices|.
    const uint32_t sliceId;

    // How many slices are there in total?
    const uint32_t numSlices;

    // Allocator to use when allocating on this thread.  See
    // |ion::ParFunctions::ParNewGCThing()|.  This should move into
    // |perThreadData|.
    Allocator *const allocator;

    // If we took a parallel bailout, the script that bailed out is stored here.
    JSScript *abortedScript;

    // Records the last instr. to execute on this thread.
#ifdef DEBUG
    IonTraceData traceData;
#endif

    ForkJoinSlice(PerThreadData *perThreadData, uint32_t sliceId, uint32_t numSlices,
                  Allocator *arenaLists, ForkJoinShared *shared);

    // True if this is the main thread, false if it is one of the parallel workers.
    bool isMainThread();

    // When the code would normally trigger a GC, we don't trigger it
    // immediately but instead record that request here.  This will
    // cause |ExecuteForkJoinOp()| to invoke |TriggerGC()| or
    // |TriggerCompartmentGC()| as appropriate once the par. sec. is
    // complete. This is done because those routines do various
    // preparations that are not thread-safe, and because the full set
    // of arenas is not available until the end of the par. sec.
    void requestGC(gcreason::Reason reason);
    void requestCompartmentGC(JSCompartment *compartment, gcreason::Reason reason);

    // During the parallel phase, this method should be invoked
    // periodically, for example on every backedge, similar to the
    // interrupt check.  If it returns false, then the parallel phase
    // has been aborted and so you should bailout.  The function may
    // also rendesvous to perform GC or do other similar things.
    //
    // This function is guaranteed to have no effect if both
    // runtime()->parallelAbort and runtime()->interrupt are zero.
    // Ion-generated code takes advantage of this by inlining the
    // checks on those flags before actually calling this function.
    bool check();

    // Be wary, the runtime is shared between all threads!
    JSRuntime *runtime();

    // Acquire and release the JSContext from the runtime.
    JSContext *acquireContext();
    void releaseContext();

    // Check the current state of parallel execution.
    static inline ForkJoinSlice *Current();
    static inline bool InParallelSection();

    static bool Initialize();

  private:
    friend class AutoRendezvous;
    friend class AutoSetForkJoinSlice;

    bool checkOutOfLine();

#ifdef JS_THREADSAFE
    // Initialized by Initialize()
    static unsigned ThreadPrivateIndex;
#endif

#ifdef JS_THREADSAFE
    // Sets the abort flag and adjusts ionStackLimit so as to cause
    // the overrun check to fail.  This should lead to the operation
    // as a whole aborting.
    void triggerAbort();
#endif

    ForkJoinShared *const shared;
};

// Generic interface for specifying divisible operations that can be
// executed in a fork-join fashion.
struct ForkJoinOp
{
  public:
    // Invoked from each parallel thread to process one slice.  The
    // |ForkJoinSlice| which is supplied will also be available using TLS.
    //
    // Returns true on success, false to halt parallel execution.
    virtual bool parallel(ForkJoinSlice &slice) = 0;
};

// Locks a JSContext for its scope.
class LockedJSContext
{
    ForkJoinSlice *slice_;
    JSContext *cx_;
    uint8_t *savedIonTop_;

  public:
    LockedJSContext(ForkJoinSlice *slice)
      : slice_(slice),
        cx_(slice->acquireContext()),
        savedIonTop_(cx_->runtime->mainThread.ionTop)
    {
        // Switch out main thread data for the local thread data.
        cx_->runtime->mainThread.ionTop = slice_->perThreadData->ionTop;
    }

    ~LockedJSContext() {
        slice_->releaseContext();

        // Restore saved main thread data.
        cx_->runtime->mainThread.ionTop = savedIonTop_;
    }

    operator JSContext *() { return cx_; }
    JSContext *operator->() { return cx_; }
};

} // namespace js

/* static */ inline js::ForkJoinSlice *
js::ForkJoinSlice::Current()
{
#ifdef JS_THREADSAFE
    return (ForkJoinSlice*) PR_GetThreadPrivate(ThreadPrivateIndex);
#else
    return NULL;
#endif
}

/* static */ inline bool
js::ForkJoinSlice::InParallelSection()
{
    return Current() != NULL;
}

#endif // ForkJoin_h__
