/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 4 -*-
 * vim: set ts=8 sw=4 et tw=78:
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "jscntxt.h"
#include "jscompartment.h"

#include "vm/ForkJoin.h"
#include "vm/Monitor.h"
#include "gc/Marking.h"

#include "jsinferinlines.h"

#ifdef JS_THREADSAFE
#  include "prthread.h"
#endif

// For extracting stack extent for each thread.
#include "jsnativestack.h"

// For representing stack event for each thread.
#include "StackExtents.h"

using namespace js;

#ifdef JS_THREADSAFE

class js::ForkJoinShared : public TaskExecutor, public Monitor
{
    /////////////////////////////////////////////////////////////////////////
    // Constant fields

    JSContext *const cx_;          // Current context
    ThreadPool *const threadPool_; // The thread pool.
    ForkJoinOp &op_;               // User-defined operations to be perf. in par.
    const uint32_t numSlices_;     // Total number of threads.
    PRCondVar *rendezvousEnd_;     // Cond. var used to signal end of rendezvous.
    PRLock *cxLock_;               // Locks cx_ for parallel VM calls.

    /////////////////////////////////////////////////////////////////////////
    // Per-thread arenas
    //
    // Each worker thread gets an arena to use when allocating.

    Vector<Allocator *, 16> allocators_;

    // Each worker thread has an associated StackExtent instance.
    Vector<gc::StackExtent, 16> stackExtents_;

    // Each worker thread is responsible for storing a pointer to itself here.
    Vector<ForkJoinSlice *, 16> slices_;

    /////////////////////////////////////////////////////////////////////////
    // Locked Fields
    //
    // Only to be accessed while holding the lock.

    uint32_t uncompleted_;         // Number of uncompleted worker threads
    uint32_t blocked_;             // Number of threads that have joined rendezvous
    uint32_t rendezvousIndex_;     // Number of rendezvous attempts

    // Fields related to asynchronously-read gcRequested_ flag
    gcreason::Reason gcReason_;    // Reason given to request GC
    Zone *gcZone_; // Zone for GC, or NULL for full

    /////////////////////////////////////////////////////////////////////////
    // Asynchronous Flags
    //
    // These can be read without the lock (hence the |volatile| declaration).
    // All fields should be *written with the lock*, however.

    // Set to true when parallel execution should abort.
    volatile bool abort_;

    // Set to true when a worker bails for a fatal reason.
    volatile bool fatal_;

    // The main thread has requested a rendezvous.
    volatile bool rendezvous_;

    // True if a worker requested a GC
    volatile bool gcRequested_;

    // True if all non-main threads have stopped for the main thread to GC
    volatile bool worldStoppedForGC_;

    // Invoked only from the main thread:
    void executeFromMainThread();

    // Executes slice #threadId of the work, either from a worker or
    // the main thread.
    void executePortion(PerThreadData *perThread, uint32_t threadId);

    // Rendezvous protocol:
    //
    // Use AutoRendezvous rather than invoking initiateRendezvous() and
    // endRendezvous() directly.

    friend class AutoRendezvous;
    friend class AutoMarkWorldStoppedForGC;

    // Requests that the other threads stop.  Must be invoked from the main
    // thread.
    void initiateRendezvous(ForkJoinSlice &threadCx);

    // If a rendezvous has been requested, blocks until the main thread says
    // we may continue.
    void joinRendezvous(ForkJoinSlice &threadCx);

    // Permits other threads to resume execution.  Must be invoked from the
    // main thread after a call to initiateRendezvous().
    void endRendezvous(ForkJoinSlice &threadCx);

  public:
    ForkJoinShared(JSContext *cx,
                   ThreadPool *threadPool,
                   ForkJoinOp &op,
                   uint32_t numSlices,
                   uint32_t uncompleted);
    ~ForkJoinShared();

    bool init();

    ParallelResult execute();

    // Invoked from parallel worker threads:
    virtual void executeFromWorker(uint32_t threadId, uintptr_t stackLimit);

    // Moves all the per-thread arenas into the main zone and
    // processes any pending requests for a GC.  This can only safely
    // be invoked on the main thread after the workers have completed.
    void transferArenasToZone();

    void triggerGCIfRequested();

    // Invoked during processing by worker threads to "check in".
    bool check(ForkJoinSlice &threadCx);

    // Requests a GC, either full or specific to a zone.
    void requestGC(gcreason::Reason reason);
    void requestZoneGC(Zone *zone, gcreason::Reason reason);

    // Requests that computation abort.
    void setAbortFlag(bool fatal);

    JSRuntime *runtime() { return cx_->runtime; }

    JSContext *acquireContext() { PR_Lock(cxLock_); return cx_; }
    void releaseContext() { PR_Unlock(cxLock_); }

    gc::StackExtent &stackExtent(uint32_t i) { return stackExtents_[i]; }

    bool isWorldStoppedForGC() { return worldStoppedForGC_; }

    void addSlice(ForkJoinSlice *slice);
    void removeSlice(ForkJoinSlice *slice);
};

class js::AutoRendezvous
{
  private:
    ForkJoinSlice &threadCx;

  public:
    AutoRendezvous(ForkJoinSlice &threadCx)
        : threadCx(threadCx)
    {
        threadCx.shared->initiateRendezvous(threadCx);
    }

    ~AutoRendezvous() {
        threadCx.shared->endRendezvous(threadCx);
    }
};

unsigned ForkJoinSlice::ThreadPrivateIndex;
bool ForkJoinSlice::TLSInitialized;

class js::AutoSetForkJoinSlice
{
  public:
    AutoSetForkJoinSlice(ForkJoinSlice *threadCx) {
        PR_SetThreadPrivate(ForkJoinSlice::ThreadPrivateIndex, threadCx);
    }

    ~AutoSetForkJoinSlice() {
        PR_SetThreadPrivate(ForkJoinSlice::ThreadPrivateIndex, NULL);
    }
};

class js::AutoMarkWorldStoppedForGC
{
  private:
    ForkJoinSlice &threadCx;

  public:
    AutoMarkWorldStoppedForGC(ForkJoinSlice &threadCx)
        : threadCx(threadCx)
    {
        threadCx.shared->worldStoppedForGC_ = true;
        threadCx.shared->cx_->mainThread().suppressGC--;
        JS_ASSERT(!threadCx.shared->cx_->runtime->preserveCodeDueToParallelDo);
        threadCx.shared->cx_->runtime->preserveCodeDueToParallelDo = true;
    }

    ~AutoMarkWorldStoppedForGC()
    {
        threadCx.shared->worldStoppedForGC_ = false;
        threadCx.shared->cx_->mainThread().suppressGC++;
        threadCx.shared->cx_->runtime->preserveCodeDueToParallelDo = false;
    }

};

/////////////////////////////////////////////////////////////////////////////
// ForkJoinShared
//

ForkJoinShared::ForkJoinShared(JSContext *cx,
                               ThreadPool *threadPool,
                               ForkJoinOp &op,
                               uint32_t numSlices,
                               uint32_t uncompleted)
  : cx_(cx),
    threadPool_(threadPool),
    op_(op),
    numSlices_(numSlices),
    allocators_(cx),
    stackExtents_(cx),
    slices_(cx),
    uncompleted_(uncompleted),
    blocked_(0),
    rendezvousIndex_(0),
    gcReason_(gcreason::NUM_REASONS),
    gcZone_(NULL),
    abort_(false),
    fatal_(false),
    rendezvous_(false),
    gcRequested_(false),
    worldStoppedForGC_(false)
{
}

bool
ForkJoinShared::init()
{
    // Create temporary arenas to hold the data allocated during the
    // parallel code.
    //
    // Note: you might think (as I did, initially) that we could use
    // zone |Allocator| for the main thread.  This is not true,
    // because when executing parallel code we sometimes check what
    // arena list an object is in to decide if it is writable.  If we
    // used the zone |Allocator| for the main thread, then the
    // main thread would be permitted to write to any object it wants.

    if (!Monitor::init())
        return false;

    rendezvousEnd_ = PR_NewCondVar(lock_);
    if (!rendezvousEnd_)
        return false;

    cxLock_ = PR_NewLock();
    if (!cxLock_)
        return false;

    if (!stackExtents_.resize(numSlices_))
        return false;
    for (unsigned i = 0; i < numSlices_; i++) {
        Allocator *allocator = cx_->runtime->new_<Allocator>(cx_->zone());
        if (!allocator)
            return false;

        if (!allocators_.append(allocator)) {
            js_delete(allocator);
            return false;
        }

        if (!slices_.append((ForkJoinSlice*)NULL))
            return false;

        if (i > 0) {
            gc::StackExtent *prev = &stackExtents_[i-1];
            prev->setNext(&stackExtents_[i]);
        }
    }

    // If we ever have other clients of StackExtents, then we will
    // need to link them all together (and likewise unlink them
    // properly).  For now ForkJoin is sole StackExtents client, and
    // currently it constructs only one instance of them at a time.
    JS_ASSERT(cx_->runtime->extraExtents == NULL);

    return true;
}

ForkJoinShared::~ForkJoinShared()
{
    PR_DestroyCondVar(rendezvousEnd_);

    while (allocators_.length() > 0)
        js_delete(allocators_.popCopy());
}

ParallelResult
ForkJoinShared::execute()
{
    // Sometimes a GC request occurs *just before* we enter into the
    // parallel section.  Rather than enter into the parallel section
    // and then abort, we just check here and abort early.
    if (cx_->runtime->interrupt)
        return TP_RETRY_SEQUENTIALLY;

    AutoLockMonitor lock(*this);

    // Notify workers to start and execute one portion on this thread.
    {
        gc::AutoSuppressGC gc(cx_);
        AutoUnlockMonitor unlock(*this);
        if (!threadPool_->submitAll(cx_, this))
            return TP_FATAL;
        executeFromMainThread();
    }

    // Wait for workers to complete.
    while (uncompleted_ > 0)
        lock.wait();

    bool gcWasRequested = gcRequested_; // transfer clears gcRequested_ flag.
    transferArenasToZone();
    triggerGCIfRequested();

    // Check if any of the workers failed.
    if (abort_) {
        if (fatal_)
            return TP_FATAL;
        else if (gcWasRequested)
            return TP_RETRY_AFTER_GC;
        else
            return TP_RETRY_SEQUENTIALLY;
    }

    // Everything went swimmingly. Give yourself a pat on the back.
    return TP_SUCCESS;
}

void
ForkJoinShared::transferArenasToZone()
{
    JS_ASSERT(ForkJoinSlice::Current() == NULL);

    // stop-the-world GC may still be sweeping; let that finish so
    // that we do not upset the state of compartments being swept.
    cx_->runtime->gcHelperThread.waitBackgroundSweepEnd();

    Zone *zone = cx_->zone();
    for (unsigned i = 0; i < numSlices_; i++)
        zone->adoptWorkerAllocator(allocators_[i]);

    triggerGCIfRequested();
}

void
ForkJoinShared::triggerGCIfRequested() {
    // this function either executes after the fork-join section ends
    // or when the world is stopped:
    JS_ASSERT(!ParallelJSActive());

    if (gcRequested_) {
        if (gcZone_ == NULL)
            js::TriggerGC(cx_->runtime, gcReason_);
        else
            js::TriggerZoneGC(gcZone_, gcReason_);
        gcRequested_ = false;
        gcZone_ = NULL;
    }
}

void
ForkJoinShared::executeFromWorker(uint32_t workerId, uintptr_t stackLimit)
{
    JS_ASSERT(workerId < numSlices_ - 1);

    PerThreadData thisThread(cx_->runtime);
    TlsPerThreadData.set(&thisThread);
    thisThread.ionStackLimit = stackLimit;
    executePortion(&thisThread, workerId);
    TlsPerThreadData.set(NULL);

    AutoLockMonitor lock(*this);
    uncompleted_ -= 1;
    if (blocked_ == uncompleted_) {
        // Signal the main thread that we have terminated.  It will be either
        // working, arranging a rendezvous, or waiting for workers to
        // complete.
        lock.notify();
    }
}

void
ForkJoinShared::executeFromMainThread()
{
    executePortion(&cx_->mainThread(), numSlices_ - 1);
}

void
ForkJoinShared::executePortion(PerThreadData *perThread,
                               uint32_t threadId)
{
    Allocator *allocator = allocators_[threadId];
    ForkJoinSlice slice(perThread, threadId, numSlices_, allocator, this);
    AutoSetForkJoinSlice autoContext(&slice);

    if (!op_.parallel(slice))
        setAbortFlag(false);
}

struct AutoInstallForkJoinStackExtents : public gc::StackExtents
{
    AutoInstallForkJoinStackExtents(JSRuntime *rt,
                                    gc::StackExtent *head)
        : StackExtents(head), rt(rt)
    {
        rt->extraExtents = this;
        JS_ASSERT(wellFormed());
    }

    ~AutoInstallForkJoinStackExtents() {
        rt->extraExtents = NULL;
    }

    bool wellFormed() {
        for (gc::StackExtent *l = head; l != NULL; l = l->next) {
            if (l->stackMin > l->stackEnd)
                return false;
        }
        return true;
    }

    JSRuntime *rt;
};

bool
ForkJoinShared::check(ForkJoinSlice &slice)
{
    JS_ASSERT(cx_->runtime->interrupt);

    if (abort_)
        return false;

    if (slice.isMainThread()) {
        // We are the main thread: therefore we must
        // (1) initiate the rendezvous;
        // (2) if GC was requested, reinvoke trigger
        //     which will do various non-thread-safe
        //     preparatory steps.  We then invoke
        //     a non-incremental GC manually.
        // (3) run the operation callback, which
        //     would normally run the GC but
        //     incrementally, which we do not want.
        JSRuntime *rt = cx_->runtime;

        // Calls to js::TriggerGC() should have been redirected to
        // requestGC(), and thus the gcIsNeeded flag is not set yet.
        JS_ASSERT(!rt->gcIsNeeded);

        if (gcRequested_ && rt->isHeapBusy()) {
            // Cannot call GCSlice when heap busy, so abort.  Easier
            // right now to abort rather than prove it cannot arise,
            // and safer for short-term than asserting !isHeapBusy.
            setAbortFlag(false);
            return false;
        }

        // (1). Initiaize the rendezvous and record stack extents.
        AutoRendezvous autoRendezvous(slice);
        AutoMarkWorldStoppedForGC autoMarkSTWFlag(slice);
        slice.recordStackExtent();
        AutoInstallForkJoinStackExtents extents(rt, &stackExtents_[0]);

        // (2).  Note that because we are in a STW section, calls to
        // js::TriggerGC() etc will not re-invoke
        // ForkJoinSlice::requestGC().
        triggerGCIfRequested();

        // (2b) Run the GC if it is required.  This would occur as
        // part of js_InvokeOperationCallback(), but we want to avoid
        // an incremental GC.
        if (rt->gcIsNeeded) {
            GC(rt, GC_NORMAL, gcReason_);
        }

        // (3). Invoke the callback and abort if it returns false.
        if (!js_InvokeOperationCallback(cx_)) {
            setAbortFlag(true);
            return false;
        }

        return true;
    } else if (rendezvous_) {
        slice.recordStackExtent();
        joinRendezvous(slice);
    }

    return true;
}

void
ForkJoinShared::initiateRendezvous(ForkJoinSlice &slice)
{
    // The rendezvous protocol is always initiated by the main thread.  The
    // main thread sets the rendezvous flag to true.  Seeing this flag, other
    // threads will invoke |joinRendezvous()|, which causes them to (1) read
    // |rendezvousIndex| and (2) increment the |blocked| counter.  Once the
    // |blocked| counter is equal to |uncompleted|, all parallel threads have
    // joined the rendezvous, and so the main thread is signaled.  That will
    // cause this function to return.
    //
    // Some subtle points:
    //
    // - Worker threads may potentially terminate their work before they see
    //   the rendezvous flag.  In this case, they would decrement
    //   |uncompleted| rather than incrementing |blocked|.  Either way, if the
    //   two variables become equal, the main thread will be notified
    //
    // - The |rendezvousIndex| counter is used to detect the case where the
    //   main thread signals the end of the rendezvous and then starts another
    //   rendezvous before the workers have a chance to exit.  We circumvent
    //   this by having the workers read the |rendezvousIndex| counter as they
    //   enter the rendezvous, and then they only block until that counter is
    //   incremented.  Another alternative would be for the main thread to
    //   block in |endRendezvous()| until all workers have exited, but that
    //   would be slower and involve unnecessary synchronization.
    //
    //   Note that the main thread cannot ever get more than one rendezvous
    //   ahead of the workers, because it must wait for all of them to enter
    //   the rendezvous before it can end it, so the solution of using a
    //   counter is perfectly general and we need not fear rollover.

    JS_ASSERT(slice.isMainThread());
    JS_ASSERT(!rendezvous_ && blocked_ == 0);
    JS_ASSERT(cx_->runtime->interrupt);

    AutoLockMonitor lock(*this);

    // Signal other threads we want to start a rendezvous.
    rendezvous_ = true;

    // Wait until all the other threads blocked themselves.
    while (blocked_ != uncompleted_)
        lock.wait();
}

void
ForkJoinShared::joinRendezvous(ForkJoinSlice &slice)
{
    JS_ASSERT(!slice.isMainThread());
    JS_ASSERT(rendezvous_);

    AutoLockMonitor lock(*this);
    const uint32_t index = rendezvousIndex_;
    blocked_ += 1;

    // If we're the last to arrive, let the main thread know about it.
    if (blocked_ == uncompleted_)
        lock.notify();

    // Wait until the main thread terminates the rendezvous.  We use a
    // separate condition variable here to distinguish between workers
    // notifying the main thread that they have completed and the main
    // thread notifying the workers to resume.
    while (rendezvousIndex_ == index)
        PR_WaitCondVar(rendezvousEnd_, PR_INTERVAL_NO_TIMEOUT);
}

void
ForkJoinShared::endRendezvous(ForkJoinSlice &slice)
{
    JS_ASSERT(slice.isMainThread());

    AutoLockMonitor lock(*this);
    rendezvous_ = false;
    blocked_ = 0;
    rendezvousIndex_++;

    // Signal other threads that rendezvous is over.
    PR_NotifyAllCondVar(rendezvousEnd_);
}

void
ForkJoinShared::setAbortFlag(bool fatal)
{
    AutoLockMonitor lock(*this);

    abort_ = true;
    fatal_ = fatal_ || fatal;

    cx_->runtime->triggerOperationCallback();
}

void
ForkJoinShared::requestGC(gcreason::Reason reason)
{
    // Remember the details of the GC that was required for later,
    // then trigger an interrupt.

    AutoLockMonitor lock(*this);

    gcZone_ = NULL;
    gcReason_ = reason;
    gcRequested_ = true;

    cx_->runtime->triggerOperationCallback();
}

void
ForkJoinShared::requestZoneGC(Zone *zone,
                              gcreason::Reason reason)
{
    // Remember the details of the GC that was required for later,
    // then trigger an interrupt.  If more than one zone is requested,
    // fallback to full GC.

    AutoLockMonitor lock(*this);

    if (gcRequested_ && gcZone_ != zone) {
        // If a full GC has been requested, or a GC for another zone,
        // issue a request for a full GC.
        gcZone_ = NULL;
        gcReason_ = reason;
        gcRequested_ = true;
    } else {
        // Otherwise, just GC this zone.
        gcZone_ = zone;
        gcReason_ = reason;
        gcRequested_ = true;
    }

    cx_->runtime->triggerOperationCallback();
}

#endif // JS_THREADSAFE

/////////////////////////////////////////////////////////////////////////////
// ForkJoinSlice
//

ForkJoinSlice::ForkJoinSlice(PerThreadData *perThreadData,
                             uint32_t sliceId, uint32_t numSlices,
                             Allocator *allocator, ForkJoinShared *shared)
    : perThreadData(perThreadData),
      sliceId(sliceId),
      numSlices(numSlices),
      allocator(allocator),
      abortedScript(NULL),
      shared(shared),
      extent(&shared->stackExtent(sliceId))
{
    shared->addSlice(this);
}

ForkJoinSlice::~ForkJoinSlice()
{
    shared->removeSlice(this);
    extent->clearStackExtent();
}

void
ForkJoinShared::addSlice(ForkJoinSlice *slice)
{
    slices_[slice->sliceId] = slice;
}

void
ForkJoinShared::removeSlice(ForkJoinSlice *slice)
{
    slices_[slice->sliceId] = NULL;
}

bool
ForkJoinSlice::isMainThread()
{
#ifdef JS_THREADSAFE
    return perThreadData == &shared->runtime()->mainThread;
#else
    return true;
#endif
}

JSRuntime *
ForkJoinSlice::runtime()
{
#ifdef JS_THREADSAFE
    return shared->runtime();
#else
    return NULL;
#endif
}

JSContext *
ForkJoinSlice::acquireContext()
{
#ifdef JS_THREADSAFE
    return shared->acquireContext();
#else
    return NULL;
#endif
}

void
ForkJoinSlice::releaseContext()
{
#ifdef JS_THREADSAFE
    return shared->releaseContext();
#endif
}

bool
ForkJoinSlice::check()
{
#ifdef JS_THREADSAFE
    if (runtime()->interrupt)
        return shared->check(*this);
    else
        return true;
#else
    return false;
#endif
}

bool
ForkJoinSlice::InitializeTLS()
{
#ifdef JS_THREADSAFE
    if (!TLSInitialized) {
        TLSInitialized = true;
        PRStatus status = PR_NewThreadPrivateIndex(&ThreadPrivateIndex, NULL);
        return status == PR_SUCCESS;
    }
    return true;
#else
    return true;
#endif
}

bool
ForkJoinSlice::InWorldStoppedForGCSection()
{
    return shared->isWorldStoppedForGC();
}

void
ForkJoinSlice::recordStackExtent()
{
    uintptr_t dummy;
    uintptr_t *myStackTop = &dummy;

    gc::StackExtent &extent = shared->stackExtent(sliceId);

    // This establishes the tip, and ParallelDo::parallel the base,
    // of the stack address-range of this thread for the GC to scan.
#if JS_STACK_GROWTH_DIRECTION > 0
    extent.stackEnd = reinterpret_cast<uintptr_t *>(myStackTop);
#else
    extent.stackMin = reinterpret_cast<uintptr_t *>(myStackTop + 1);
#endif

    JS_ASSERT(extent.stackMin <= extent.stackEnd);

    PerThreadData *ptd = perThreadData;
    // PerThreadData *ptd = TlsPerThreadData.get();
    extent.ionTop        = ptd->ionTop;
    extent.ionActivation = ptd->ionActivation;
}


void ForkJoinSlice::recordStackBase(uintptr_t *baseAddr)
{
    // This establishes the base, and ForkJoinSlice::recordStackExtent the tip,
    // of the stack address-range of this thread for the GC to scan.
#if JS_STACK_GROWTH_DIRECTION > 0
        this->extent->stackMin = baseAddr;
#else
        this->extent->stackEnd = baseAddr;
#endif
}

void
ForkJoinSlice::requestGC(gcreason::Reason reason)
{
#ifdef JS_THREADSAFE
    shared->requestGC(reason);
#endif
}

void
ForkJoinSlice::requestZoneGC(Zone *zone,
                             gcreason::Reason reason)
{
#ifdef JS_THREADSAFE
    shared->requestZoneGC(zone, reason);
#endif
}

#ifdef JS_THREADSAFE
void
ForkJoinSlice::triggerAbort()
{
    shared->setAbortFlag(false);

    // set iontracklimit to -1 so that on next entry to a function,
    // the thread will trigger the overrecursedcheck.  If the thread
    // is in a loop, then it will be calling ForkJoinSlice::check(),
    // in which case it will notice the shared abort_ flag.
    //
    // In principle, we probably ought to set the ionStackLimit's for
    // the other threads too, but right now the various slice objects
    // are not on a central list so that's not possible.
    perThreadData->ionStackLimit = -1;
}
#endif

/////////////////////////////////////////////////////////////////////////////

namespace js {
class AutoEnterParallelSection
{
  private:
    JSContext *cx_;
    uint8_t *prevIonTop_;

  public:
    AutoEnterParallelSection(JSContext *cx)
      : cx_(cx),
        prevIonTop_(cx->mainThread().ionTop)
    {
        // Note: we do not allow GC during parallel sections.
        // Moreover, we do not wish to worry about making
        // write barriers thread-safe.  Therefore, we guarantee
        // that there is no incremental GC in progress:

        if (IsIncrementalGCInProgress(cx->runtime)) {
            PrepareForIncrementalGC(cx->runtime);
            FinishIncrementalGC(cx->runtime, gcreason::API);
        }

        cx->runtime->gcHelperThread.waitBackgroundSweepEnd();
    }

    ~AutoEnterParallelSection() {
        cx_->mainThread().ionTop = prevIonTop_;
    }
};
}

uint32_t
js::ForkJoinSlices(JSContext *cx)
{
#ifndef JS_THREADSAFE
    return 1;
#else
    // Parallel workers plus this main thread.
    return cx->runtime->threadPool.numWorkers() + 1;
#endif
}

ParallelResult
js::ExecuteForkJoinOp(JSContext *cx, ForkJoinOp &op)
{
#ifdef JS_THREADSAFE
    // Recursive use of the ThreadPool is not supported.
    if (ForkJoinSlice::Current() != NULL)
        return TP_RETRY_SEQUENTIALLY;

    AutoEnterParallelSection enter(cx);

    ThreadPool *threadPool = &cx->runtime->threadPool;
    uint32_t numSlices = ForkJoinSlices(cx);

    ForkJoinShared shared(cx, threadPool, op, numSlices, numSlices - 1);
    if (!shared.init())
        return TP_RETRY_SEQUENTIALLY;

    return shared.execute();
#else
    return TP_RETRY_SEQUENTIALLY;
#endif
}
