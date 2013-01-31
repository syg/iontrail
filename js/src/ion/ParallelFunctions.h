/* -*- Mode: C++; tab-width: 4; indent-tabs-mode: nil; c-basic-offset: 4 -*-
 * vim: set ts=4 sw=4 et tw=99:
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef jsion_par_functions_h__
#define jsion_par_functions_h__

#include "vm/ThreadPool.h"
#include "vm/ForkJoin.h"
#include "gc/Heap.h"

namespace js {
namespace ion {

ForkJoinSlice *ParForkJoinSlice();
JSObject *ParNewGCThing(gc::AllocKind allocKind);
bool ParWriteGuard(ForkJoinSlice *context, JSObject *object);
void ParBailout(uint32_t id);
bool ParCheckOverRecursed(ForkJoinSlice *slice);
bool ParCheckInterrupt(ForkJoinSlice *context);

void ParDumpValue(Value *v);

// We pass the arguments to ParPush in a structure because, in code
// gen, it is convenient to store them on the stack to avoid
// constraining the reg alloc for the slow path.
struct ParPushArgs {
    JSObject *object;
    Value value;
};

// Extends the given object with the given value (like `Array.push`).
// Returns NULL on failure or else `args->object`, which is convenient
// during code generation.
JSObject* ParPush(ParPushArgs *args);

// Extends the given array with `length` new holes.  Returns NULL on
// failure or else `array`, which is convenient during code
// generation.
JSObject *ParExtendArray(ForkJoinSlice *slice, JSObject *array, uint32_t length);

enum ParCompareResult {
    ParCompareNe = false,
    ParCompareEq = true,
    ParCompareUnknown = 2
};
ParCompareResult ParCompareStrings(JSString *str1, JSString *str2);

void ParallelAbort(JSScript *script);

void TraceLIR(uint32_t bblock, uint32_t lir, uint32_t execModeInt,
              const char *lirOpName, const char *mirOpName,
              JSScript *script, jsbytecode *pc);

void ParCallToUncompiledScript(JSFunction *func);

//
// A helper struct to automatically lock the JSContext before calling a
// VMFunction. Use it when defining the parallel version of VMFunctions:
//
//   typedef bool (*FooFn)(JSContext *);
//   static const VMFunction FooInfo =
//       FunctionInfo<FooFn>(Foo);
//
//   typedef bool (*ParFooFn)(ForkJoinSlice *);
//   static const VMFunction ParFooInfo = 
//       FunctionInfo<ParFooFn>(LockedVMFunction<FooFn>::Wrap<Foo>);
//

template <typename T>
struct LockedVMFunction {
};

template <class R>
struct LockedVMFunction<R (*)(JSContext *)> {
    template <R (*pf)(JSContext *)>
    static R Wrap(ForkJoinSlice *slice) {
        LockedJSContext cx(slice);
        return pf(cx);
    }
};

template <class R, class A1>
struct LockedVMFunction<R (*)(JSContext *, A1)> {
    template <R (*pf)(JSContext *, A1)>
    static R Wrap(ForkJoinSlice *slice, A1 a1) {
        LockedJSContext cx(slice);
        return pf(cx, a1);
    }
};

template <class R, class A1, class A2>
struct LockedVMFunction<R (*)(JSContext *, A1, A2)> {
    template <R (*pf)(JSContext *, A1, A2)>
    static R Wrap(ForkJoinSlice *slice, A1 a1, A2 a2) {
        LockedJSContext cx(slice);
        return pf(cx, a1, a2);
    }
};

template <class R, class A1, class A2, class A3>
struct LockedVMFunction<R (*)(JSContext *, A1, A2, A3)> {
    template <R (*pf)(JSContext *, A1, A2, A3)>
    static R Wrap(ForkJoinSlice *slice, A1 a1, A2 a2, A3 a3) {
        LockedJSContext cx(slice);
        return pf(cx, a1, a2, a3);
    }
};

template <class R, class A1, class A2, class A3, class A4>
struct LockedVMFunction<R (*)(JSContext *, A1, A2, A3, A4)> {
    template <R (*pf)(JSContext *, A1, A2, A3, A4)>
    static R Wrap(ForkJoinSlice *slice, A1 a1, A2 a2, A3 a3, A4 a4) {
        LockedJSContext cx(slice);
        return pf(cx, a1, a2, a3, a4);
    }
};

template <class R, class A1, class A2, class A3, class A4, class A5>
struct LockedVMFunction<R (*)(JSContext *, A1, A2, A3, A4, A5)> {
    template <R (*pf)(JSContext *, A1, A2, A3, A4, A5)>
    static R Wrap(ForkJoinSlice *slice, A1 a1, A2 a2, A3 a3, A4 a4, A5 a5) {
        LockedJSContext cx(slice);
        return pf(cx, a1, a2, a3, a4, a5);
    }
};

}
}

#endif
