/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 4 -*-
 * vim: set ts=8 sw=4 et tw=78:
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef ForkJoin_inl_h__
#define ForkJoin_inl_h__

namespace js {

/* static */ inline ForkJoinSlice *
ForkJoinSlice::Current()
{
#ifdef JS_THREADSAFE
    return (ForkJoinSlice*) PR_GetThreadPrivate(ThreadPrivateIndex);
#else
    return NULL;
#endif
}

/* static */ inline bool
ForkJoinSlice::InParallelSection()
{
    return Current() != NULL;
}

} // namespace js

#endif // ForkJoin_inl_h__
