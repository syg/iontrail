/* -*- Mode: C++; tab-width: 4; indent-tabs-mode: nil; c-basic-offset: 4 -*-
 * vim: set ts=8 sw=4 et tw=99 ft=cpp:
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef ParallelArray_inl_h__
#define ParallelArray_inl_h__

#include "builtin/ParallelArray.h"

#include "jsobjinlines.h"

namespace js {

inline bool
ParallelArrayObject::IndexInfo::inBounds() const
{
    JS_ASSERT(isInitialized());
    JS_ASSERT(indices.length() <= dimensions.length());

    for (uint32_t d = 0; d < indices.length(); d++) {
        if (indices[d] >= dimensions[d])
            return false;
    }

    return true;
}

inline bool
ParallelArrayObject::IndexInfo::bump()
{
    JS_ASSERT(isInitialized());
    JS_ASSERT(indices.length() > 0);

    uint32_t d = indices.length() - 1;
    while (++indices[d] == dimensions[d]) {
        if (d == 0)
            return false;
        indices[d--] = 0;
    }

    return true;
}

inline uint32_t
ParallelArrayObject::IndexInfo::packedDimensions()
{
    JS_ASSERT(isInitialized());
    return partialProducts.length();
}

inline uint32_t
ParallelArrayObject::IndexInfo::scalarLengthOfPackedDimensions()
{
    JS_ASSERT(isInitialized());
    return dimensions[0] * partialProducts[0];
}

inline uint32_t
ParallelArrayObject::IndexInfo::toScalar()
{
    JS_ASSERT(isInitialized());
    JS_ASSERT(partialProducts.length() <= dimensions.length());

    uint32_t d = Min(partialProducts.length(), indices.length());

    if (d == 0)
        return 0;
    if (partialProducts.length() == 1)
        return indices[0];

    uint32_t index = indices[0] * partialProducts[0];
    for (uint32_t i = 1; i < d; i++)
        index += indices[i] * partialProducts[i];

    return index;
}

inline bool
ParallelArrayObject::IndexInfo::fromScalar(uint32_t index)
{
    JS_ASSERT(isInitialized());

    uint32_t d = partialProducts.length();

    if (!indices.resize(d))
        return false;

    if (d == 1) {
        indices[0] = index;
        return true;
    }

    uint32_t prev = index;
    for (uint32_t i = 0; i < d - 1; i++) {
        indices[i] = prev / partialProducts[i];
        prev = prev % partialProducts[i];
    }
    indices[d - 1] = prev;

    return true;
}

inline bool
ParallelArrayObject::IndexInfo::split(IndexInfo &siv, uint32_t sd)
{
    JS_ASSERT(isInitialized());
    JS_ASSERT(!siv.isInitialized());
    JS_ASSERT(partialProducts.length() < indices.length());

    uint32_t d = partialProducts.length();

    // Copy the dimensions and initialize siv first to compute the partial
    // products.
    if (!siv.dimensions.append(dimensions.begin() + d, dimensions.end()) ||
        !siv.initialize(0, sd))
    {
        return false;
    }

    // Copy over the indices.
    siv.indices.infallibleAppend(indices.begin() + d, indices.end());

    // Truncate ourself.
    indices.shrinkBy(indices.length() - d);
    dimensions.shrinkBy(dimensions.length() - d);

    return true;
}

inline bool
ParallelArrayObject::IndexInfo::initialize(uint32_t space, uint32_t d)
{
    // Initialize using a manually set dimension vector.
    JS_ASSERT(dimensions.length() > 0);
    JS_ASSERT(space <= dimensions.length());
    JS_ASSERT(d <= dimensions.length());

    // Compute the partial products of the packed dimensions.
    //
    // NB: partialProducts[i] is the scalar length of packed dimension
    // i+1. The scalar length of the entire packed space is thus dimensions[0]
    // * partialProducts[0].
    if (!partialProducts.resize(d))
        return false;
    partialProducts[d - 1] = 1;
    for (uint32_t i = d - 1; i > 0; i--)
        partialProducts[i - 1] = dimensions[i] * partialProducts[i];

    // Reserve indices.
    return indices.reserve(dimensions.length()) && indices.resize(space);
}

inline bool
ParallelArrayObject::IndexInfo::initialize(JSContext *cx, HandleParallelArrayObject source,
                                           uint32_t space)
{
    // Initialize using a dimension vector gotten from a parallel array
    // source.
    if (!source->getDimensions(cx, dimensions))
        return false;

    return initialize(space, source->packedDimensions());
}

inline bool
ParallelArrayObject::DenseArrayToIndexVector(JSContext *cx, HandleObject obj,
                                             IndexVector &indices)
{
    uint32_t length = obj->getDenseArrayInitializedLength();
    if (!indices.resize(length))
        return false;

    // Read the index vector out of the dense array into an actual Vector for
    // ease of access. We're guaranteed that the elements of the dense array
    // are uint32s, so just cast.
    const Value *src = obj->getDenseArrayElements();
    const Value *end = src + length;
    for (uint32_t *dst = indices.begin(); src < end; dst++, src++)
        *dst = static_cast<uint32_t>(src->toInt32());

    return true;
}

inline bool
ParallelArrayObject::is(const Value &v)
{
    return v.isObject() && is(&v.toObject());
}

inline bool
ParallelArrayObject::is(JSObject *obj)
{
    JS_ASSERT(obj);
    return obj->hasClass(&class_);
}

inline ParallelArrayObject *
ParallelArrayObject::as(JSObject *obj)
{
    JS_ASSERT(is(obj));
    return static_cast<ParallelArrayObject *>(obj);
}

inline JSObject *
ParallelArrayObject::dimensionArray()
{
    JSObject &dimObj = getFixedSlot(SLOT_DIMENSIONS).toObject();
    JS_ASSERT(dimObj.isDenseArray());
    return &dimObj;
}

inline JSObject *
ParallelArrayObject::buffer()
{
    JSObject &buf = getFixedSlot(SLOT_BUFFER).toObject();
    JS_ASSERT(buf.isDenseArray());
    return &buf;
}

inline uint32_t
ParallelArrayObject::bufferOffset()
{
    return static_cast<uint32_t>(getFixedSlot(SLOT_BUFFER_OFFSET).toInt32());
}

inline uint32_t
ParallelArrayObject::outermostDimension()
{
    return static_cast<uint32_t>(dimensionArray()->getDenseArrayElement(0).toInt32());
}

inline uint32_t
ParallelArrayObject::packedDimensions()
{
    uint32_t p = packedDimensionsUnsafe();
    JS_ASSERT(p <= dimensionArray()->getDenseArrayInitializedLength());
    return p;
}

inline uint32_t
ParallelArrayObject::packedDimensionsUnsafe()
{
    return static_cast<uint32_t>(getFixedSlot(SLOT_PACKED_PREFIX_LENGTH).toInt32());
}

inline bool
ParallelArrayObject::isOneDimensional()
{
    // Check if we're logically one dimensional.
    return dimensionArray()->getDenseArrayInitializedLength() == 1;
}

inline bool
ParallelArrayObject::isPackedOneDimensional()
{
    // Check if we only packed one dimension.
    return packedDimensions() == 1;
}

inline bool
ParallelArrayObject::getDimensions(JSContext *cx, IndexVector &dims)
{
    RootedObject obj(cx, dimensionArray());
    if (!obj)
        return false;
    return DenseArrayToIndexVector(cx, obj, dims);
}

inline bool
ParallelArrayObject::getLeaf(JSContext *cx, uint32_t index, MutableHandleValue vp)
{
    RootedValue leaf(cx, buffer()->getDenseArrayElement(index));

    // Rewrap all ParallelArray leaves.
    if (is(leaf)) {
        RootedObject leafClone(cx, as(&leaf.toObject())->clone(cx));
        if (!leafClone)
            return false;
        vp.setObject(*leafClone);
        return true;
    }

    vp.set(leaf);
    return true;
}

} // namespace js

#endif // ParallelArray_inl_h__
