/* -*- Mode: C++; tab-width: 4; indent-tabs-mode: nil; c-basic-offset: 4 -*-
 * vim: set ts=4 sw=4 et tw=99:
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "jsinterp.h"
#include "ParallelFunctions.h"
#include "IonSpewer.h"

#include "jsinterpinlines.h"
#include "jscompartmentinlines.h"

using namespace js;
using namespace ion;

using parallel::Spew;
using parallel::SpewBailouts;
using parallel::SpewBailoutIR;

// Load the current thread context.
ForkJoinSlice *
ion::ParForkJoinSlice()
{
    return ForkJoinSlice::Current();
}

// ParNewGCThing() is called in place of NewGCThing() when executing
// parallel code.  It uses the ArenaLists for the current thread and
// allocates from there.
JSObject *
ion::ParNewGCThing(gc::AllocKind allocKind)
{
    ForkJoinSlice *slice = ForkJoinSlice::Current();
    uint32_t thingSize = (uint32_t)gc::Arena::thingSize(allocKind);
    void *t = slice->allocator->parallelNewGCThing(allocKind, thingSize);
    return static_cast<JSObject *>(t);
}

// Check that the object was created by the current thread
// (and hence is writable).
bool
ion::ParWriteGuard(ForkJoinSlice *slice, JSObject *object)
{
    JS_ASSERT(ForkJoinSlice::Current() == slice);
    return slice->allocator->arenas.containsArena(slice->runtime(),
                                                  object->arenaHeader());
}

#ifdef DEBUG
static void
printTrace(const char *prefix, struct IonLIRTraceData *cached)
{
    fprintf(stderr, "%s / Block %3u / LIR %3u / Mode %u / LIR %s\n",
            prefix,
            cached->bblock, cached->lir, cached->execModeInt, cached->lirOpName);
}

struct IonLIRTraceData seqTraceData;
#endif

void
ion::TraceLIR(uint32_t bblock, uint32_t lir, uint32_t execModeInt,
              const char *lirOpName, const char *mirOpName,
              JSScript *script, jsbytecode *pc)
{
#ifdef DEBUG
    static enum { NotSet, All, Bailouts } traceMode;

    // If you set IONFLAGS=trace, this function will be invoked before every LIR.
    //
    // You can either modify it to do whatever you like, or use gdb scripting.
    // For example:
    //
    // break ParTrace
    // commands
    // continue
    // exit

    if (traceMode == NotSet) {
        // Racy, but that's ok.
        const char *env = getenv("IONFLAGS");
        if (strstr(env, "trace-all"))
            traceMode = All;
        else
            traceMode = Bailouts;
    }

    IonLIRTraceData *cached;
    if (execModeInt == 0)
        cached = &seqTraceData;
    else
        cached = &ForkJoinSlice::Current()->traceData;

    if (bblock == 0xDEADBEEF) {
        if (execModeInt == 0)
            printTrace("BAILOUT", cached);
        else
            SpewBailoutIR(cached->bblock, cached->lir,
                          cached->lirOpName, cached->mirOpName,
                          cached->script, cached->pc);
    }

    cached->bblock = bblock;
    cached->lir = lir;
    cached->execModeInt = execModeInt;
    cached->lirOpName = lirOpName;
    cached->mirOpName = mirOpName;
    cached->script = script;
    cached->pc = pc;

    if (traceMode == All)
        printTrace("Exec", cached);
#endif
}

bool
ion::ParCheckOverRecursed(ForkJoinSlice *slice)
{
    JS_ASSERT(ForkJoinSlice::Current() == slice);
    int stackDummy_;

    // When an interrupt is triggered, the main thread stack limit is
    // overwritten with a sentinel value that brings us here.
    // Therefore, we must check whether this is really a stack overrun
    // and, if not, check whether an interrupt is needed.
    //
    // When not on the main thread, we don't overwrite the stack
    // limit, but we do still call into this routine if the interrupt
    // flag is set, so we still need to double check.

    uintptr_t realStackLimit;
    if (slice->isMainThread()) {
        realStackLimit = js::GetNativeStackLimit(slice->runtime());
    } else {
        realStackLimit = slice->perThreadData->ionStackLimit;
    }

    if (!JS_CHECK_STACK_SIZE(realStackLimit, &stackDummy_)) {
        slice->bailoutRecord->setCause(ParallelBailoutOverRecursed, NULL, NULL);
        return false;
    }

    return ParCheckInterrupt(slice);
}

bool
ion::ParCheckInterrupt(ForkJoinSlice *slice)
{
    JS_ASSERT(ForkJoinSlice::Current() == slice);
    bool result = slice->check();
    if (!result) {
        // Do not set the cause here.  Either it was set by this
        // thread already by some code that then triggered an abort,
        // or else we are just picking up an abort from some other
        // thread.  Either way we have nothing useful to contribute so
        // we might as well leave our bailout case unset.
        return false;
    }
    return true;
}

void
ion::ParDumpValue(Value *v)
{
#ifdef DEBUG
    js_DumpValue(*v);
#endif
}

JSObject*
ion::ParPush(ParPushArgs *args)
{
    // It is awkward to have the MIR pass the current slice in, so
    // just fetch it from TLS.  Extending the array is kind of the
    // slow path anyhow as it reallocates the elements vector.
    ForkJoinSlice *slice = js::ForkJoinSlice::Current();
    JSObject::EnsureDenseResult res =
        args->object->parExtendDenseElements(slice->allocator,
                                             &args->value, 1);
    if (res != JSObject::ED_OK)
        return NULL;
    return args->object;
}

JSObject *
ion::ParExtendArray(ForkJoinSlice *slice, JSObject *array, uint32_t length)
{
    JSObject::EnsureDenseResult res =
        array->parExtendDenseElements(slice->allocator, NULL, length);
    if (res != JSObject::ED_OK)
        return NULL;
    return array;
}

ParCompareResult
ion::ParCompareStrings(JSString *str1, JSString *str2)
{
    // NYI---the rope case
    if (!str1->isLinear())
        return ParCompareUnknown;
    if (!str2->isLinear())
        return ParCompareUnknown;

    JSLinearString &linearStr1 = str1->asLinear();
    JSLinearString &linearStr2 = str2->asLinear();
    if (EqualStrings(&linearStr1, &linearStr2))
        return ParCompareEq;
    return ParCompareNe;
}

void
ion::ParallelAbort(ParallelBailoutCause cause,
                   JSScript *script,
                   jsbytecode *bytecode)
{
    JS_ASSERT(ParallelJSActive());
    JS_ASSERT(script != NULL);
    ForkJoinSlice *slice = ForkJoinSlice::Current();

    Spew(SpewBailouts, "Parallel abort with cause %d in %p:%s:%d at line %d",
         cause, script, script->filename(), script->lineno);

    JS_ASSERT(slice->bailoutRecord->depth == 0);
    slice->bailoutRecord->setCause(cause, script, bytecode);
}

void
ion::PropagateParallelAbort(JSScript *script)
{
    JS_ASSERT(ParallelJSActive());
    ForkJoinSlice *slice = ForkJoinSlice::Current();
    script->parallelIonScript()->setHasInvalidatedCallTarget();
    slice->bailoutRecord->addTrace(script, NULL);
}

void
ion::ParCallToUncompiledScript(JSFunction *func)
{
    JS_ASSERT(ParallelJSActive());

#ifdef DEBUG
    RawScript script = func->nonLazyScript();
    Spew(SpewBailouts, "Call to uncompiled script: %p:%s:%d", script, script->filename(), script->lineno);
#endif
}
