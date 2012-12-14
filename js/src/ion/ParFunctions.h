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

// We pass the arguments in a structure because, in code gen, it is
// convenient to store them on the stack to avoid constraining the reg
// alloc for the slow path.
struct ParPushArgs {
    JSObject *object;
    Value value;
};
bool ParPush(ParPushArgs *args);
JSObject *ParExtendArray(ForkJoinSlice *slice, JSObject *array, uint32_t length);

void ParallelAbort(JSScript *script);

// XXX wrong file, not specific to par
void Trace(uint32_t bblock, uint32_t lir,
           uint32_t execMode, const char *opcode);

void ParCallToUncompiledScript(JSFunction *func);

}
}

#endif
