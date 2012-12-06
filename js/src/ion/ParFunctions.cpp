/* -*- Mode: C++; tab-width: 4; indent-tabs-mode: nil; c-basic-offset: 4 -*-
 * vim: set ts=4 sw=4 et tw=99:
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "jsinterp.h"
#include "ParFunctions.h"
#include "IonSpewer.h"

#include "jsinterpinlines.h"
#include "jscompartmentinlines.h"
#include "jsarrayinlines.h"

#include "vm/ForkJoin-inl.h"

using namespace js;
using namespace ion;

// Load the current thread context.
ForkJoinSlice *
ion::ParForkJoinSlice()
{
    return ForkJoinSlice::current();
}

// ParNewGCThing() is called in place of NewGCThing() when executing
// parallel code.  It uses the ArenaLists for the current thread and
// allocates from there.
JSObject *
ion::ParNewGCThing(ForkJoinSlice *slice, gc::AllocKind allocKind, uint32_t thingSize)
{
    JS_ASSERT(ForkJoinSlice::current() == slice);
    void *t = slice->allocator->parallelNewGCThing(allocKind, thingSize);
    return static_cast<JSObject *>(t);
}

// Check that the object was created by the current thread
// (and hence is writable).
bool
ion::ParWriteGuard(ForkJoinSlice *slice, JSObject *object)
{
    JS_ASSERT(ForkJoinSlice::current() == slice);
    return slice->allocator->arenas.containsArena(slice->runtime(),
                                                  object->arenaHeader());
}

#ifdef DEBUG
static void
printTrace(const char *prefix, struct IonTraceData *cached)
{
    fprintf(stderr, "%s / Block %3u / LIR %3u / Mode %u / Opcode %s\n",
            prefix,
            cached->bblock, cached->lir,
            cached->execModeInt, cached->opcode);
}
#endif

#ifdef DEBUG
struct IonTraceData seqTraceData;
#endif

void
ion::Trace(uint32_t bblock, uint32_t lir, uint32_t execModeInt,
      const char *opcode)
{
#ifdef DEBUG
    static enum { NotSet, All, Bailouts } traceMode;

    /*
       If you set IONFLAGS=trace, this function will be invoked before every LIR.

       You can either modify it to do whatever you like, or use gdb scripting.
       For example:

       break ParTrace
       commands
       continue
       exit
     */

    if (traceMode == NotSet) {
        // Racy, but that's ok.
        const char *env = getenv("IONFLAGS");
        if (strstr(env, "trace-all")) {
            traceMode = All;
        } else {
            traceMode = Bailouts;
        }
    }

    IonTraceData *cached;
    if (execModeInt == 0) {
        cached = &seqTraceData;
    } else {
        cached = &ForkJoinSlice::current()->traceData;
    }

    if (bblock == 0xDEADBEEF) {
        printTrace("BAILOUT", cached);
    }

    cached->bblock = bblock;
    cached->lir = lir;
    cached->execModeInt = execModeInt;
    cached->opcode = opcode;

    if (traceMode == All)
        printTrace("Exec", cached);
#endif
}

bool
ion::ParCheckOverRecursed(ForkJoinSlice *slice)
{
    JS_ASSERT(ForkJoinSlice::current() == slice);

    // When an interrupt is triggered, we currently overwrite the
    // stack limit with a sentinel value that brings us here.
    // Therefore, we must check whether this is really a stack overrun
    // and, if not, check whether an interrupt is needed.
    if (slice->isMainThread()) {
        int stackDummy_;
        if (!JS_CHECK_STACK_SIZE(js::GetNativeStackLimit(slice->runtime()), &stackDummy_)) {
            return false;
        } else {
            return ParCheckInterrupt(slice);
        }
    } else {
        // FIXME---we don't ovewrite the stack limit for worker
        // threads, which means that technically they can recurse
        // forever---or at least a long time---without ever checking
        // the interrupt.  it also means that if we get here on a
        // worker thread, this is a real stack overrun!
        return false;
    }
}

bool
ion::ParCheckInterrupt(ForkJoinSlice *slice)
{
    JS_ASSERT(ForkJoinSlice::current() == slice);
    bool result = slice->check();
    if (!result) {
        return false;
    }
    return true;
}

bool
ion::ParExtendArray(ParExtendArrayArgs *args)
{
    // It is awkward to have the MIR pass the current slice in, so
    // just fetch it from TLS.  Extending the array is kind of the
    // slow path anyhow as it reallocates the elements vector.
    ForkJoinSlice *slice = js::ForkJoinSlice::current();
    return (args->object->parExtendDenseArray(slice->allocator,
                                              &args->value, 1) == JSObject::ED_OK);
}

void
ion::ParallelAbort(JSScript *script)
{
    JS_ASSERT(InParallelSection());

    IonSpew(IonSpew_Bailouts, "Took parallel abort from %s:%d", script->filename, script->lineno);

    PerThreadData *pt = ForkJoinSlice::current()->perThreadData;
    if (!pt->parallelAbortedScript)
        pt->parallelAbortedScript = script;
}

void
ion::ParCallToUncompiledScript(JSFunction *func)
{
    JS_ASSERT(InParallelSection());

    IonSpew(IonSpew_Bailouts, "Call to uncompiled script: %p\n", func);
}
