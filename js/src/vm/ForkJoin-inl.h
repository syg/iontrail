/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 4 -*-
 * vim: set ts=8 sw=4 et tw=78:
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef ForkJoin_inl_h__
#define ForkJoin_inl_h__

namespace js {

#ifdef JS_THREADSAFE

/* static */ inline ForkJoinSlice *
ForkJoinSlice::Current()
{
    ForkJoinSlice *slice = (ForkJoinSlice*) PR_GetThreadPrivate(ThreadPrivateIndex);
    JS_ASSERT_IF(slice, InParallelSection());
    return slice;
}

/* static */ inline bool
ForkJoinSlice::Executing()
{
    return Current() != NULL;
}

/* static */ inline bool
ForkJoinSlice::InParallelSection()
{
    // See note in ForkJoin.h for why this is decoupled from being in
    // multithreaded code.
    JS_ASSERT_IF(!InParallelSection_, !Executing());
    return InParallelSection_;
}

/* static */ inline bool
ForkJoinSlice::EnterParallelSection()
{
    if (InParallelSection())
        return false;
    InParallelSection_ = true;
    return true;
}

/* static */ inline void
ForkJoinSlice::LeaveParallelSection()
{
    JS_ASSERT(InParallelSection());
    InParallelSection_ = false;
}

#else

/* static */ inline ForkJoinSlice *ForkJoinSlice::Current() { return NULL; }
/* static */ inline bool *ForkJoinSlice::Executing() { return false; }
/* static */ inline bool *ForkJoinSlice::InParallelSection() { return false; }
/* static */ inline bool *ForkJoinSlice::EnterParallelSection() { return false; }
/* static */ inline void *ForkJoinSlice::LeaveParallelSection() { }
>>>>>>> Stashed changes

#endif // JS_THREADSAFE

} // namespace js

#endif // ForkJoin_inl_h__
