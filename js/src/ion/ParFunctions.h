/* -*- Mode: C++; tab-width: 4; indent-tabs-mode: nil; c-basic-offset: 4 -*-
 * vim: set ts=4 sw=4 et tw=99:
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef jsion_par_functions_h__
#define jsion_par_functions_h__

#include "vm/threadpool.h"
#include "vm/forkjoin.h"
#include "gc/Heap.h"

namespace js {
namespace ion {

ForkJoinSlice *ParForkJoinSlice();
JSObject *ParNewGCThing(ForkJoinSlice *threadContext, gc::AllocKind allocKind, uint32_t thingSize);
bool ParWriteGuard(ForkJoinSlice *context, JSObject *object);
void ParBailout(uint32_t id);
bool ParCheckInterrupt(ForkJoinSlice *context);

// We pass the arguments in a structure because, in code gen, it is
// convenient to store them on the stack to avoid constraining the reg
// alloc for the slow path.
struct ParExtendArrayArgs {
    JSObject *object;
    Value value;
};
bool ParExtendArray(ParExtendArrayArgs *args);

// XXX wrong file, not specific to par
void Trace(uint32_t bblock, uint32_t lir, const char *opcode);

}
}

#endif
