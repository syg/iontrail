/* -*- Mode: C++; tab-width: 4; indent-tabs-mode: nil; c-basic-offset: 4 -*-
 * vim: set ts=8 sw=4 et tw=99 ft=cpp:
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "builtin/ParallelArray.h"
#include "builtin/ParallelArray-inl.h"

#include "jsapi.h"
#include "jsobj.h"
#include "jsarray.h"
#include "jsprf.h"
#include "jsgc.h"

#include "gc/Marking.h"
#include "vm/GlobalObject.h"
#include "vm/Stack.h"
#include "vm/StringBuffer.h"

#include "vm/threadpool.h"

#include "jsinterpinlines.h"
#include "jsobjinlines.h"
#include "jsarrayinlines.h"
#include "vm/forkjoininlines.h"

#include "ion/Ion.h"
#include "ion/IonCompartment.h"
#include "ion/ParallelArrayAnalysis.h"

using namespace js;
using namespace js::types;
using namespace js::ion;

//
// Utilities
//

typedef ParallelArrayObject::IndexVector IndexVector;
typedef ParallelArrayObject::IndexInfo IndexInfo;

static bool
ReportMoreArgsNeeded(JSContext *cx, const char *name, const char *num, const char *p)
{
    JS_ReportErrorNumber(cx, js_GetErrorMessage, NULL, JSMSG_MORE_ARGS_NEEDED, name, num, p);
    return false;
}

static bool
ReportBadArg(JSContext *cx, const char *s = "")
{
    JS_ReportErrorNumber(cx, js_GetErrorMessage, NULL, JSMSG_PAR_ARRAY_BAD_ARG, s);
    return false;
}

static bool
ReportBadLength(JSContext *cx)
{
    JS_ReportErrorNumber(cx, js_GetErrorMessage, NULL, JSMSG_BAD_ARRAY_LENGTH);
    return false;
}

static bool
ReportBadLengthOrArg(JSContext *cx, HandleValue v, const char *s = "")
{
    return v.isNumber() ? ReportBadLength(cx) : ReportBadArg(cx, s);
}

static bool
ReportBadPartition(JSContext *cx)
{
    JS_ReportErrorNumber(cx, js_GetErrorMessage, NULL, JSMSG_PAR_ARRAY_BAD_PARTITION);
    return false;
}

bool
ParallelArrayObject::IndexInfo::isInitialized() const
{
    return (dimensions.length() > 0 &&
            indices.capacity() >= dimensions.length() &&
            partialProducts.length() <= dimensions.length());
}

static inline bool
OpenDelimiters(const IndexInfo &iv, StringBuffer &sb)
{
    JS_ASSERT(iv.isInitialized());

    uint32_t d = iv.dimensions.length() - 1;
    do {
        if (iv.indices[d] != 0)
            break;
        if (!sb.append('<'))
            return false;
    } while (d-- > 0);

    return true;
}

static inline bool
CloseDelimiters(const IndexInfo &iv, StringBuffer &sb)
{
    JS_ASSERT(iv.isInitialized());

    uint32_t d = iv.dimensions.length() - 1;
    do {
        if (iv.indices[d] != iv.dimensions[d] - 1) {
            if (!sb.append(','))
                return false;
            break;
        }

        if (!sb.append('>'))
            return false;
    } while (d-- > 0);

    return true;
}

// A version of ToUint32 that reports if the input value is malformed: either
// it is given to us as a negative integer or it overflows.
static bool
ToUint32(JSContext *cx, const Value &v, uint32_t *out, bool *malformed)
{
    AssertArgumentsAreSane(cx, v);
    {
        js::SkipRoot skip(cx, &v);
        js::MaybeCheckStackRoots(cx);
    }

    *malformed = false;

    if (v.isInt32()) {
        int32_t i = v.toInt32();
        if (i < 0) {
            *malformed = true;
            return true;
        }
        *out = static_cast<uint32_t>(i);
        return true;
    }

    double d;
    if (v.isDouble()) {
        d = v.toDouble();
    } else {
        if (!ToNumberSlow(cx, v, &d))
            return false;
    }

    *out = ToUint32(d);

    if (!MOZ_DOUBLE_IS_FINITE(d) || d != static_cast<double>(*out)) {
        *malformed = true;
        return true;
    }

    return true;
}

static bool
GetLength(JSContext *cx, HandleObject obj, uint32_t *length)
{
    // If obj's length cannot overflow, just use GetLengthProperty.
    if (obj->isArray() || obj->isArguments())
        return GetLengthProperty(cx, obj, length);

    // Otherwise check that we don't overflow uint32_t.
    RootedValue value(cx);
    if (!JSObject::getProperty(cx, obj, obj, cx->names().length, &value))
        return false;

    bool malformed;
    if (!ToUint32(cx, value, length, &malformed))
        return false;
    if (malformed)
        return ReportBadLengthOrArg(cx, value);

    return true;
}

// Check if obj is a parallel array, and if so, cast to pa and initialize
// the IndexInfo accordingly.
//
// This function is designed to be used in conjunction with
// GetElementFromArrayLikeObject; see below.
static bool
MaybeGetParallelArrayObjectAndLength(JSContext *cx, HandleObject obj,
                                     MutableHandle<ParallelArrayObject *> pa,
                                     IndexInfo *iv, uint32_t *length)
{
    if (ParallelArrayObject::is(obj)) {
        pa.set(ParallelArrayObject::as(obj));
        if (!pa->isPackedOneDimensional() && !iv->initialize(cx, pa, 1))
            return false;
        *length = pa->outermostDimension();
    } else if (!GetLength(cx, obj, length)) {
        return false;
    }

    return true;
}

// Store the i-th element of the array-like object obj into vp.
//
// If pa is not null, then pa is obj casted to a ParallelArrayObject
// and iv is initialized according to the dimensions of pa. In this case,
// we get the element using the ParallelArrayObject.
//
// Otherwise we do what is done in GetElement in jsarray.cpp.
static bool
GetElementFromArrayLikeObject(JSContext *cx, HandleObject obj, HandleParallelArrayObject pa,
                              IndexInfo &iv, uint32_t i, MutableHandleValue vp)
{
    // Fast path getting an element from parallel and dense arrays. For dense
    // arrays, we only do this if the prototype doesn't have indexed
    // properties. In this case holes = undefined.
    if (pa && ParallelArrayObject::getParallelArrayElement(cx, pa, i, &iv, vp))
        return true;

    if (obj->isDenseArray() && i < obj->getDenseArrayInitializedLength() &&
        !js_PrototypeHasIndexedProperties(obj))
    {
        vp.set(obj->getDenseArrayElement(i));
        if (vp.isMagic(JS_ARRAY_HOLE))
            vp.setUndefined();
        return true;
    }

    if (obj->isArguments()) {
        if (obj->asArguments().maybeGetElement(static_cast<uint32_t>(i), vp))
            return true;
    }

    // Slow path everything else: objects with indexed properties on the
    // prototype, non-parallel and dense arrays.
    return JSObject::getElement(cx, obj, obj, i, vp);
}

static inline bool
SetArrayNewType(JSContext *cx, HandleObject obj)
{
    RootedTypeObject newtype(cx, GetTypeCallerInitObject(cx, JSProto_Array));
    if (!newtype)
        return false;
    obj->setType(newtype);
    return true;
}

// Copy source into a dense array, eagerly converting holes to undefined.
static JSObject *
NewFilledCopiedArray(JSContext *cx, uint32_t length, HandleObject source)
{
    JS_ASSERT(source);

    RootedObject buffer(cx, NewDenseAllocatedArray(cx, length));
    if (!buffer)
        return NULL;
    JS_ASSERT(buffer->getDenseArrayCapacity() >= length);
    buffer->setDenseArrayInitializedLength(length);

    uint32_t srclen;
    uint32_t copyUpTo;

    if (source->isDenseArray() && !js_PrototypeHasIndexedProperties(source)) {
        // Optimize for the common case: if we have a dense array source, copy
        // whatever we can, truncating to length. This path doesn't trigger
        // GC, so we don't need to initialize all the array's slots before
        // copying.
        const Value *srcvp = source->getDenseArrayElements();

        srclen = source->getDenseArrayInitializedLength();
        copyUpTo = Min(length, srclen);

        // Convert any existing holes into undefined.
        Value elem;
        for (uint32_t i = 0; i < copyUpTo; i++) {
            elem = srcvp[i].isMagic(JS_ARRAY_HOLE) ? UndefinedValue() : srcvp[i];
            buffer->initDenseArrayElement(i, elem);
        }

        // Fill the rest with undefineds.
        for (uint32_t i = copyUpTo; i < length; i++)
            buffer->initDenseArrayElement(i, UndefinedValue());
    } else {
        // This path might GC. The GC expects an object's slots to be
        // initialized, so we have to make sure all the array's slots are
        // initialized.
        for (uint32_t i = 0; i < length; i++)
            buffer->initDenseArrayElement(i, UndefinedValue());

        IndexInfo siv(cx);
        RootedParallelArrayObject sourcePA(cx);

        if (!MaybeGetParallelArrayObjectAndLength(cx, source, &sourcePA, &siv, &srclen))
            return NULL;
        copyUpTo = Min(length, srclen);

        // Copy elements pointwise.
        RootedValue elem(cx);
        for (uint32_t i = 0; i < copyUpTo; i++) {
            if (!GetElementFromArrayLikeObject(cx, source, sourcePA, siv, i, &elem))
                return NULL;
            buffer->setDenseArrayElement(i, elem);
        }
    }

    return *buffer.address();
}

// Allocate a new dense array and ensure the initialized length, setting all
// values to JS_ARRAY_HOLE.
static inline JSObject *
NewDenseEnsuredArray(JSContext *cx, uint32_t length)
{
    RootedObject buffer(cx, NewDenseAllocatedArray(cx, length));
    if (!buffer)
        return NULL;

    buffer->ensureDenseArrayInitializedLength(cx, length, 0);

    return *buffer.address();
}

// Copy an array like object obj into an IndexVector, indices, using
// ToUint32.
static inline bool
ArrayLikeToIndexVector(JSContext *cx, HandleObject obj, IndexVector &indices,
                       bool *malformed)
{
    IndexInfo iv(cx);
    RootedParallelArrayObject pa(cx);
    uint32_t length;

    *malformed = false;

    if (!MaybeGetParallelArrayObjectAndLength(cx, obj, &pa, &iv, &length))
        return false;

    if (!indices.resize(length))
        return false;

    RootedValue elem(cx);
    bool malformed_;
    for (uint32_t i = 0; i < length; i++) {
        if (!GetElementFromArrayLikeObject(cx, obj, pa, iv, i, &elem) ||
            !ToUint32(cx, elem, &indices[i], &malformed_))
        {
            return false;
        }

        if (malformed_)
            *malformed = true;
    }

    return true;
}

// Given two index vectors, truncate the first vector such that the first
// vector is a prefix of the second.
static uint32_t
TruncateMismatchingSuffix(IndexVector &v, IndexVector &mask)
{
    // If the mask is shorter than the v, then we know v can at most be
    // mask.length() long.
    if (mask.length() < v.length())
        v.shrinkBy(mask.length() - v.length());

    for (uint32_t i = 0; i < v.length(); i++) {
        if (v[i] != mask[i]) {
            v.shrinkBy(v.length() - i);
            break;
        }
    }

    return v.length();
}

static inline bool
IdIsLengthOrShapeAtom(JSContext *cx, HandleId id)
{
    return (JSID_IS_ATOM(id, cx->names().length) ||
            JSID_IS_ATOM(id, cx->names().shape));
}

static inline bool
IdIsInBoundsIndex(JSContext *cx, HandleObject obj, HandleId id)
{
    uint32_t i;
    return js_IdIsIndex(id, &i) && i < ParallelArrayObject::as(obj)->outermostDimension();
}

template <bool impl(JSContext *, CallArgs)>
static inline JSBool
NonGenericMethod(JSContext *cx, unsigned argc, Value *vp)
{
    CallArgs args = CallArgsFromVp(argc, vp);
    return CallNonGenericMethod<ParallelArrayObject::is, impl>(cx, args);
}

static inline TypeConstruction *
NewTypeConstructionParallelArray(JSContext *cx, uint32_t npacked)
{
    JS_ASSERT(npacked > 0);

    TypeConstruction *construct = reinterpret_cast<TypeConstruction *>(
        cx->calloc_(sizeof(TypeConstruction) + npacked * sizeof(uint32_t)));
    if (!construct)
        return NULL;

    construct->kind = TypeConstruction::PARALLEL_ARRAY;
    construct->dimensions = reinterpret_cast<uint32_t *>(
        reinterpret_cast<char *>(construct) + sizeof(TypeConstruction));

    return construct;
}

static inline void
SetDimensions(TypeConstruction *construct, const uint32_t *dims, uint32_t length)
{
    // Don't use construct->isParallelArray() because we need to assert that
    // construct is an uninitialized parallel array TypeConstruction.
    JS_ASSERT(construct);
    JS_ASSERT(construct->isParallelArray());
    JS_ASSERT(construct->numDimensions == 0);

    construct->numDimensions = length;
    PodCopy(construct->dimensions, dims, length);
}

enum MatchDimensionsResult {
    SameExactDimensions,
    SameNumberOfDimensions,
    DifferentDimensions
};

static inline MatchDimensionsResult
MatchDimensions(TypeConstruction *construct, const uint32_t *dims, uint32_t length)
{
    JS_ASSERT(construct->isParallelArray());
    JS_ASSERT(construct->numDimensions > 0);
    JS_ASSERT(length > 0);

    uint32_t oldLength = construct->numDimensions;

    if (oldLength != length)
        return DifferentDimensions;

    if (!construct->dimensions)
        return SameNumberOfDimensions;

    // Cast to const pointer so types match with dims.
    const uint32_t *oldDims = construct->dimensions;
    if (PodEqual(oldDims, dims, length))
        return SameExactDimensions;

    return DifferentDimensions;
}

static inline void
SetLeafValueWithType(JSContext *cx, HandleObject buffer, HandleTypeObject type,
                     uint32_t index, HandleValue value)
{
    JS_ASSERT(buffer->isDenseArray());

    if (cx->typeInferenceEnabled() && type)
        type->addPropertyType(cx, JSID_VOID, value);
    buffer->setDenseArrayElement(index, value);
}

static inline bool
SetNonExtensible(JSContext *cx, Class *clasp, HandleObject result)
{
    Shape *empty = EmptyShape::getInitialShape(cx, clasp,
                                               result->getProto(), result->getParent(),
                                               result->getAllocKind(),
                                               BaseShape::NOT_EXTENSIBLE);
    if (!empty)
        return false;
    result->setLastPropertyInfallible(empty);
    return true;
}

#ifdef DEBUG

static bool
IndexInfoIsSane(JSContext *cx, HandleParallelArrayObject obj, IndexInfo &iv)
{
    JS_ASSERT(iv.isInitialized());

    // Check that iv's dimensions are the same as obj's. Used for checking
    // sanity of split dimensions.
    IndexVector dims(cx);
    if (!obj->getDimensions(cx, dims))
        return false;

    if (iv.dimensions.length() != dims.length())
        return false;

    for (uint32_t i = 0; i < dims.length(); i++) {
        if (dims[i] != iv.dimensions[i])
            return false;
    }

    return true;
}

#endif // DEBUG

//
// Parallel Utilities
//

static void
ComputeTileBounds(ForkJoinSlice &slice, unsigned length, unsigned *start, unsigned *end)
{
    size_t sliceId = slice.sliceId;
    size_t numSlices = slice.numSlices;

    // compute our tile bounds
    double perTask = ((double)length) / numSlices;
    *start = perTask * sliceId;
    if (sliceId == (numSlices - 1))
        *end = length;
    else
        *end = perTask * (sliceId + 1);
}

struct ParallelArrayObject::ExecuteArgs
{
    ForkJoinSlice &slice;
    EnterIonCode enter;
    void *jitcode;
    void *calleeToken;
    uint32_t argc;
    Value *argv;

    ExecuteArgs(ForkJoinSlice &slice, EnterIonCode enter,
                void *jitcode, void *calleeToken, uint32_t argc, Value *argv)
        : slice(slice), enter(enter), jitcode(jitcode),
          calleeToken(calleeToken), argc(argc), argv(argv)
    {}
};

template<typename BodyDefn, uint32_t MaxArgc>
ParallelArrayObject::ParallelArrayOp<BodyDefn, MaxArgc>::~ParallelArrayOp() {
}

template<typename BodyDefn, uint32_t MaxArgc>
ParallelArrayObject::ExecutionStatus
ParallelArrayObject::ParallelArrayOp<BodyDefn, MaxArgc>::apply()
{
    if (bodyDefn_.argc() > MaxArgc)
        return ExecutionDisqualified;

    Spew(cx_, SpewOps, "%s: attempting parallel compilation",
         bodyDefn_.toString());
    if (!compileForParallelExecution())
        return ExecutionDisqualified;

    Spew(cx_, SpewOps, "%s: entering parallel section",
         bodyDefn_.toString());
    ParallelResult pr = js::ExecuteForkJoinOp(cx_, *this);
    return ToExecutionStatus(cx_, bodyDefn_.toString(), pr);
}

template<typename BodyDefn, uint32_t MaxArgc>
bool
ParallelArrayObject::ParallelArrayOp<BodyDefn, MaxArgc>::compileForParallelExecution()
{
    if (!ion::IsEnabled(cx_))
        return false;

    // The kernel should be a function.
    if (!elementalFun_->isFunction())
        return ExecutionDisqualified;

    RootedFunction callee(cx_, elementalFun_->toFunction());

    if (!callee->isInterpreted())
        return false;

    // Ensure that the function is analyzed by TI.
    bool hasIonScript;
    {
        AutoAssertNoGC nogc;
        hasIonScript = callee->script()->hasParallelIonScript();
    }
    if (!hasIonScript) {
        // If the script has not been compiled in parallel, then type
        // inference will have no particular information.  In that
        // case, we need to do a few "warm-up" iterations to give type
        // inference some data to work with.
        if (!bodyDefn_.doWarmup())
            return false;

        // Try to compile the kernel.
        ion::ParallelCompilationContext compileContext(cx_);
        if (!compileContext.compileFunctionAndInvokedFunctions(callee))
            return false;
    }

    return true;
}

template<typename BodyDefn, uint32_t MaxArgc>
bool
ParallelArrayObject::ParallelArrayOp<BodyDefn, MaxArgc>::pre(
    size_t numSlices)
{
    return bodyDefn_.pre(numSlices);
}

template<typename BodyDefn, uint32_t MaxArgc>
bool
ParallelArrayObject::ParallelArrayOp<BodyDefn, MaxArgc>::parallel(
    ForkJoinSlice &slice)
{
    js::PerThreadData *pt = slice.perThreadData;

    // compute number of arguments
    const uint32_t argc = bodyDefn_.argc();
    JS_ASSERT(argc <= MaxArgc); // other compilation should have failed
    Value actualArgv[MaxArgc + 1], *argv = actualArgv + 1;

    // Set 'callee' and 'this'.
    RootedFunction callee(pt, elementalFun_->toFunction());
    argv[-1] = ObjectValue(*callee);
    argv[0] = UndefinedValue();

    // find jitcode ptr
    IonScript *ion = callee->script()->parallelIonScript();
    IonCode *code = ion->method();
    void *jitcode = code->raw();
    EnterIonCode enter = cx_->compartment->ionCompartment()->enterJIT();
    void *calleeToken = CalleeToToken(callee);

    // Prepare and execute the per-thread state for the operation:
    typename BodyDefn::Instance op(bodyDefn_, slice.sliceId, slice.numSlices);
    if (!op.init())
        return false;
    ExecuteArgs args(slice, enter, jitcode, calleeToken, argc, argv);
    return op.execute(args);
}

template<typename BodyDefn, uint32_t MaxArgc>
bool
ParallelArrayObject::ParallelArrayOp<BodyDefn, MaxArgc>::post(
    size_t numSlices)
{
    return true;
}

//
// Debug spew
//

#ifdef DEBUG

static bool
SpewColorable()
{
    // Only spew colors on xterm-color to not screw up emacs.
    static bool colorable = false;
    static bool checked = false;
    if (!checked) {
        checked = true;
        const char *env = getenv("TERM");
        if (!env)
            return false;
        if (strcmp(env, "xterm-color") == 0 || strcmp(env, "xterm-256color") == 0)
            colorable = true;
    }
    return colorable;
}

static inline const char *
SpewColorReset()
{
    if (!SpewColorable())
        return "";
    return "\x1b[0m";
}

static inline const char *
SpewColorRaw(const char *colorCode) {
    if (!SpewColorable())
        return "";
    return colorCode;
}

static inline const char *
SpewColorRed() { return SpewColorRaw("\x1b[31m"); }
static inline const char *
SpewColorGreen() { return SpewColorRaw("\x1b[32m"); }
static inline const char *
SpewColorYellow() { return SpewColorRaw("\x1b[33m"); }

bool
ParallelArrayObject::IsSpewActive(SpewChannel channel)
{
    // Note: in addition, setting PAFLAGS=trace will cause
    // special instructions to be written into the generated
    // code that look like:
    //
    // mov 0xDEADBEEF, eax
    // mov N, eax

    static bool active[NumSpewChannels];
    static bool checked = false;
    if (!checked) {
        checked = true;
        PodArrayZero(active);
        const char *env = getenv("PAFLAGS");
        if (!env)
            return false;
        if (strstr(env, "ops"))
            active[SpewOps] = true;
        if (strstr(env, "types"))
            active[SpewTypes] = true;
        if (strstr(env, "full")) {
            for (uint32_t i = 0; i < NumSpewChannels; i++)
                active[i] = true;
        }
    }
    return active[channel];
}

void
ParallelArrayObject::Spew(JSContext *cx, SpewChannel channel, const char *fmt, ...)
{
    if (!IsSpewActive(channel))
        return;

    jsbytecode *pc;
    JSScript *script = cx->stack.currentScript(&pc);
    if (!script || !pc)
        return;

    va_list ap;
    va_start(ap, fmt);
    fprintf(stdout, "[pa] %s:%u: ", script->filename, PCToLineNumber(script, pc));
    vfprintf(stdout, fmt, ap);
    fprintf(stdout, "\n");
    va_end(ap);
}

void
ParallelArrayObject::SpewWarmup(JSContext *cx, const char *op, uint32_t limit)
{
    Spew(cx, SpewOps, "warming up %s with %d iters", op, limit);
}

void
ParallelArrayObject::SpewExecution(JSContext *cx, const char *op, const ExecutionMode &mode,
                                   ExecutionStatus status)
{
    const char *statusColor;
    switch (status) {
      case ExecutionDisqualified:
      case ExecutionFatal:
        statusColor = SpewColorRed();
        break;
      case ExecutionBailout:
        statusColor = SpewColorYellow();
        break;
      case ExecutionSucceeded:
        statusColor = SpewColorGreen();
        break;
    }
    Spew(cx, SpewOps, "%s %s: %s%s%s", mode.toString(), op, statusColor,
         ExecutionStatusToString(status), SpewColorReset());
}

#endif

//
// Operations Overview
//
// The different execution modes implement different versions of a set of
// operations with the same signatures, detailed below.
//
// build
// -----
// The comprehension form. Build a parallel array from a dimension vector and
// using elementalFun, writing the results into result's buffer. The dimension
// vector and its partial products are kept in iv. The function elementalFun
// is passed indices as multiple arguments.
//
// bool build(JSContext *cx,
//            HandleParallelArrayObject result,
//            IndexInfo &iv,
//            HandleObject elementalFun)
//
// map
// ---
// Map elementalFun over the elements of the outermost dimension of source,
// writing the results into result's buffer. The buffer must be as long as the
// outermost dimension of the source. The elementalFun is passed (element,
// index, collection) as arguments, in that order.
//
// bool map(JSContext *cx,
//          HandleParallelArrayObject source,
//          HandleParallelArrayObject result,
//          HandleObject elementalFun)
//
// reduce
// ------
// Reduce source in the outermost dimension using elementalFun. If vp is not
// null, then the final value of the reduction is stored into vp. If result is
// not null, then result's buffer[i] is the final value of calling reduce on
// the subarray from [0,i]. The elementalFun is passed 2 values to be
// reduced. There is no specified order in which the elements of the array are
// reduced. If elementalFun is not commutative and associative, there is no
// guarantee that the final value is deterministic.
//
// bool reduce(JSContext *cx,
//             HandleParallelArrayObject source,
//             HandleParallelArrayObject result,
//             HandleObject elementalFun,
//             MutableHandleValue vp)
//
// scatter
// -------
// Reassign elements in source in the outermost dimension according to a
// scatter vector, targets, writing results into result's buffer. The targets
// object should be array-like. The element source[i] is reassigned to the
// index targets[i]. If multiple elements map to the same target index, the
// conflictFun is used to resolve the resolution. If nothing maps to i for
// some i, defaultValue is used for that index. Note that result's buffer can
// be longer than the source, in which case all the remaining holes are filled
// with defaultValue.
//
// bool scatter(JSContext *cx,
//              HandleParallelArrayObject source,
//              HandleParallelArrayObject result,
//              HandleObject targets,
//              HandleValue defaultValue,
//              HandleObject conflictFun)
//
// filter
// ------
// Filter the source in the outermost dimension using an array of truthy
// values, filters, writing the results into result's buffer. All elements
// with index i in outermost dimension such that filters[i] is not truthy are
// removed.
//
// bool filter(JSContext *cx,
//             HandleParallelArrayObject source,
//             HandleParallelArrayObject result,
//             HandleObject filters)
//

ParallelArrayObject::SequentialMode ParallelArrayObject::sequential;
ParallelArrayObject::ParallelMode ParallelArrayObject::parallel;
ParallelArrayObject::FallbackMode ParallelArrayObject::fallback;
ParallelArrayObject::WarmupMode ParallelArrayObject::warmup;

ParallelArrayObject::ExecutionStatus
ParallelArrayObject::BaseSequentialMode::buildUpTo(JSContext *cx,
                                                   HandleParallelArrayObject result,
                                                   IndexInfo &iv,
                                                   HandleObject elementalFun,
                                                   uint32_t limit)
{
    JS_ASSERT(iv.isInitialized());
    JS_ASSERT(limit <= iv.scalarLengthOfPackedDimensions());

    FastInvokeGuard fig(cx, ObjectValue(*elementalFun));
    InvokeArgsGuard &args = fig.args();
    if (!cx->stack.pushInvokeArgs(cx, iv.dimensions.length(), &args))
        return ExecutionFatal;

    RootedObject buffer(cx, result->buffer());
    RootedTypeObject type(cx, result->maybeGetRowType(cx, iv.packedDimensions() - 1));

    for (uint32_t i = 0; i < limit; i++, iv.bump()) {
        if (!JS_CHECK_OPERATION_LIMIT(cx))
            return ExecutionFatal;

        args.setCallee(ObjectValue(*elementalFun));
        args.setThis(UndefinedValue());

        for (size_t j = 0; j < iv.indices.length(); j++)
            args[j].setNumber(iv.indices[j]);

        if (!fig.invoke(cx))
            return ExecutionFatal;

        SetLeafValueWithType(cx, buffer, type, i, args.rval());
    }

    return ExecutionSucceeded;
}

ParallelArrayObject::ExecutionStatus
ParallelArrayObject::BaseSequentialMode::mapUpTo(JSContext *cx,
                                                 HandleParallelArrayObject source,
                                                 HandleParallelArrayObject result,
                                                 HandleObject elementalFun,
                                                 uint32_t limit)
{
    JS_ASSERT(is(source));
    JS_ASSERT(is(result));
    JS_ASSERT(limit <= source->outermostDimension());
    JS_ASSERT(source->outermostDimension() == result->buffer()->getDenseArrayInitializedLength());

    RootedObject buffer(cx, result->buffer());
    RootedTypeObject type(cx, result->type());

    IndexInfo iv(cx);
    if (!source->isPackedOneDimensional() && !iv.initialize(cx, source, 1))
        return ExecutionFatal;

    FastInvokeGuard fig(cx, ObjectValue(*elementalFun));
    InvokeArgsGuard &args = fig.args();
    if (!cx->stack.pushInvokeArgs(cx, 3, &args))
        return ExecutionFatal;

    RootedValue elem(cx);
    for (uint32_t i = 0; i < limit; i++) {
        if (!JS_CHECK_OPERATION_LIMIT(cx))
            return ExecutionFatal;

        args.setCallee(ObjectValue(*elementalFun));
        args.setThis(UndefinedValue());

        if (!getParallelArrayElement(cx, source, i, &iv, &elem))
            return ExecutionFatal;

        // The arguments are in eic(h) order.
        args[0] = elem;
        args[1].setNumber(i);
        args[2].setObject(*source);

        if (!fig.invoke(cx))
            return ExecutionFatal;

        SetLeafValueWithType(cx, buffer, type, i, args.rval());
    }

    return ExecutionSucceeded;
}

template<typename Source, typename Result>
static bool ReduceUpToGeneric(JSContext *cx, Source &source, Result &result,
                              HandleObject elementalFun, MutableHandleValue vp,
                              uint32_t limit)
{
    // The accumulator: the objet petit a.
    //
    // "A VM's accumulator register is Objet petit a: the unattainable object
    // of desire that sets in motion the symbolic movement of interpretation."
    //     -- PLT Žižek
    RootedValue acc(cx);

    if (!source.getElement(cx, 0, &acc))
        return false;

    result.update(cx, 0, acc);

    FastInvokeGuard fig(cx, ObjectValue(*elementalFun));
    InvokeArgsGuard &args = fig.args();
    if (!cx->stack.pushInvokeArgs(cx, 2, &args))
        return false;

    RootedValue elem(cx);
    for (uint32_t i = 1; i < limit; i++) {
        args.setCallee(ObjectValue(*elementalFun));
        args.setThis(UndefinedValue());

        if (!source.getElement(cx, i, &elem))
            return false;

        // Set the two arguments to the elemental function.
        args[0] = acc;
        args[1] = elem;

        if (!fig.invoke(cx))
            return false;

        // Update the accumulator.
        acc = args.rval();
        result.update(cx, i, args.rval());
    }

    vp.set(acc);
    return true;
}

class ParallelArrayObjectSource
{
private:
    HandleParallelArrayObject source;
    IndexInfo iv;

public:
    ParallelArrayObjectSource(JSContext *cx,
                              HandleParallelArrayObject source)
        : source(source),
          iv(cx)
    {}

    bool init(JSContext *cx) {
        return source->isPackedOneDimensional() ||
          iv.initialize(cx, source, 1);
    }

    bool getElement(JSContext *cx, uint32_t index, MutableHandleValue elem) {
        return ParallelArrayObject::getParallelArrayElement(
            cx, source, index, &iv, elem);
    }
};

class ParallelArrayObjectResult
{
private:
    RootedObject buffer;
    RootedTypeObject type;

public:
    ParallelArrayObjectResult(JSContext *cx,
                              HandleParallelArrayObject result)
        : buffer(cx), type(cx)
    {
        if (result) {
            buffer = result->buffer();
            type = result->type();
        }
    }

    void update(JSContext *cx, uint32_t index, HandleValue value) {
        if (buffer) {
            SetLeafValueWithType(cx, buffer, type, index, value);
        }
    }
};

ParallelArrayObject::ExecutionStatus
ParallelArrayObject::BaseSequentialMode::reduceUpTo(JSContext *cx,
                                                    HandleParallelArrayObject source,
                                                    HandleParallelArrayObject result,
                                                    HandleObject elementalFun,
                                                    MutableHandleValue vp,
                                                    uint32_t limit)
{
    JS_ASSERT(is(source));
    JS_ASSERT(limit <= source->outermostDimension());
    JS_ASSERT_IF(result, is(result));
    JS_ASSERT_IF(result, result->buffer()->getDenseArrayInitializedLength() >= 1);

    ParallelArrayObjectSource pasource(cx, source);
    if (!pasource.init(cx))
        return ExecutionFatal;

    ParallelArrayObjectResult paresult(cx, result);

    if (!ReduceUpToGeneric(cx, pasource, paresult, elementalFun, vp, limit))
        return ExecutionFatal;
    return ExecutionSucceeded;
}

ParallelArrayObject::ExecutionStatus
ParallelArrayObject::BaseSequentialMode::scatterUpTo(JSContext *cx,
                                                     HandleParallelArrayObject source,
                                                     HandleParallelArrayObject result,
                                                     HandleObject targets,
                                                     HandleValue defaultValue,
                                                     HandleObject conflictFun,
                                                     uint32_t limit)
{
    JS_ASSERT(is(source));
    JS_ASSERT(is(result));

    RootedObject buffer(cx, result->buffer());
    RootedTypeObject type(cx, result->type());
    uint32_t length = buffer->getDenseArrayInitializedLength();

    IndexInfo iv(cx);
    if (!source->isPackedOneDimensional() && !iv.initialize(cx, source, 1))
        return ExecutionFatal;

    // Index vector and parallel array pointer for targets, in case targets is
    // a ParallelArray object. If not, these are uninitialized.
    IndexInfo tiv(cx);
    RootedParallelArrayObject targetsPA(cx);

    // The length of the scatter vector.
    uint32_t targetsLength;
    if (!MaybeGetParallelArrayObjectAndLength(cx, targets, &targetsPA, &tiv, &targetsLength))
        return ExecutionFatal;

    JS_ASSERT(limit <= Min(targetsLength, source->outermostDimension()));

    // If we don't have a conflict fun, pass in a sentinel undefined as the
    // function; we'll never invoke anyways but it is nice to lift the guard
    // out of the loop.
    FastInvokeGuard fig(cx, conflictFun ? ObjectValue(*conflictFun) : UndefinedValue());
    InvokeArgsGuard &args = fig.args();

    // Iterate over the scatter vector, but not more than the length of the
    // source array.
    RootedValue elem(cx);
    RootedValue telem(cx);
    RootedValue targetElem(cx);
    for (uint32_t i = 0; i < limit; i++) {
        if (!JS_CHECK_OPERATION_LIMIT(cx))
            return ExecutionFatal;

        uint32_t targetIndex;
        bool malformed;

        if (!GetElementFromArrayLikeObject(cx, targets, targetsPA, tiv, i, &telem) ||
            !ToUint32(cx, telem, &targetIndex, &malformed))
        {
            return ExecutionFatal;
        }

        if (malformed) {
            ReportBadArg(cx, ".prototype.scatter");
            return ExecutionFatal;
        }

        if (targetIndex >= length) {
            JS_ReportErrorNumber(cx, js_GetErrorMessage, NULL, JSMSG_PAR_ARRAY_SCATTER_BOUNDS);
            return ExecutionFatal;
        }

        if (!getParallelArrayElement(cx, source, i, &iv, &elem))
            return ExecutionFatal;

        targetElem = buffer->getDenseArrayElement(targetIndex);

        // We initialized the dense buffer with holes. If the target element
        // in the source array is not a hole, that means we have set it
        // already and we have a conflict.
        if (!targetElem.isMagic(JS_ARRAY_HOLE)) {
            if (conflictFun) {
                if (!args.pushed() && !cx->stack.pushInvokeArgs(cx, 2, &args))
                    return ExecutionFatal;

                args.setCallee(ObjectValue(*conflictFun));
                args.setThis(UndefinedValue());
                args[0] = elem;
                args[1] = targetElem;

                if (!fig.invoke(cx))
                    return ExecutionFatal;

                elem = args.rval();
            } else {
                JS_ReportErrorNumber(cx, js_GetErrorMessage, NULL,
                                     JSMSG_PAR_ARRAY_SCATTER_CONFLICT);
                return ExecutionFatal;
            }
        }

        SetLeafValueWithType(cx, buffer, type, targetIndex, elem);
    }

    return ExecutionSucceeded;
}

ParallelArrayObject::ExecutionStatus
ParallelArrayObject::SequentialMode::build(JSContext *cx, HandleParallelArrayObject result,
                                           IndexInfo &iv, HandleObject elementalFun)
{
    return buildUpTo(cx, result, iv, elementalFun, iv.scalarLengthOfPackedDimensions());
}

ParallelArrayObject::ExecutionStatus
ParallelArrayObject::SequentialMode::map(JSContext *cx, HandleParallelArrayObject source,
                                         HandleParallelArrayObject result, HandleObject elementalFun)
{
    return mapUpTo(cx, source, result, elementalFun, source->outermostDimension());
}

ParallelArrayObject::ExecutionStatus
ParallelArrayObject::SequentialMode::reduce(JSContext *cx, HandleParallelArrayObject source,
                                            HandleParallelArrayObject result,
                                            HandleObject elementalFun, MutableHandleValue vp)
{
    ExecutionStatus es = reduceUpTo(cx, source, result, elementalFun, vp,
                                    source->outermostDimension());
    SpewExecution(cx, "reduce", ParallelArrayObject::sequential, es);
    return es;
}

ParallelArrayObject::ExecutionStatus
ParallelArrayObject::SequentialMode::scatter(JSContext *cx, HandleParallelArrayObject source,
                                             HandleParallelArrayObject result,
                                             HandleObject targetsObj, HandleValue defaultValue,
                                             HandleObject conflictFun)
{
    uint32_t targetsLength;
    if (is(targetsObj))
        targetsLength = as(targetsObj)->outermostDimension();
    else if (!GetLengthProperty(cx, targetsObj, &targetsLength))
        return ExecutionFatal;

    ExecutionStatus status =
        scatterUpTo(cx, source, result, targetsObj, defaultValue, conflictFun,
                    Min(targetsLength, source->outermostDimension()));
    if (status != ExecutionSucceeded)
        return status;

    // Fill holes with the default value.
    RootedObject buffer(cx, result->buffer());
    RootedTypeObject type(cx, result->type());
    uint32_t length = buffer->getDenseArrayInitializedLength();
    for (uint32_t i = 0; i < length; i++) {
        if (buffer->getDenseArrayElement(i).isMagic(JS_ARRAY_HOLE))
            SetLeafValueWithType(cx, buffer, type, i, defaultValue);
    }

    return ExecutionSucceeded;
}

ParallelArrayObject::ExecutionStatus
ParallelArrayObject::SequentialMode::filter(JSContext *cx, HandleParallelArrayObject source,
                                            HandleParallelArrayObject result, HandleObject filters)
{
    JS_ASSERT(is(source));
    JS_ASSERT(is(result));

    IndexInfo iv(cx);
    if (!source->isPackedOneDimensional() && !iv.initialize(cx, source, 1))
        return ExecutionFatal;

    // Index vector and parallel array pointer for filters, in case filters is
    // a ParallelArray object. If not, these are uninitialized.
    IndexInfo fiv(cx);
    RootedParallelArrayObject filtersPA(cx);

    // The length of the filter array.
    uint32_t filtersLength;

    if (!MaybeGetParallelArrayObjectAndLength(cx, filters, &filtersPA, &fiv, &filtersLength))
        return ExecutionFatal;

    RootedObject buffer(cx, result->buffer());
    RootedTypeObject type(cx, result->type());
    RootedValue elem(cx);
    RootedValue felem(cx);
    for (uint32_t i = 0, pos = 0; i < filtersLength; i++) {
        if (!JS_CHECK_OPERATION_LIMIT(cx))
            return ExecutionFatal;

        if (!GetElementFromArrayLikeObject(cx, filters, filtersPA, fiv, i, &felem))
            return ExecutionFatal;

        // Skip if the filter element isn't truthy.
        if (!ToBoolean(felem))
            continue;

        if (!getParallelArrayElement(cx, source, i, &iv, &elem))
            return ExecutionFatal;

        // Set the element on the buffer. If we couldn't stay dense, fail.
        JSObject::EnsureDenseResult edr = JSObject::ED_SPARSE;
        edr = buffer->ensureDenseArrayElements(cx, pos, 1);
        if (edr != JSObject::ED_OK)
            return ExecutionFatal;
        if (i >= buffer->getArrayLength())
            buffer->setDenseArrayLength(pos + 1);
        SetLeafValueWithType(cx, buffer, type, pos, elem);

        // We didn't filter this element out, so bump the position.
        pos++;
    }

    return ExecutionSucceeded;
}

ParallelArrayObject::ExecutionStatus
ParallelArrayObject::WarmupMode::build(JSContext *cx, HandleParallelArrayObject result,
                                       IndexInfo &iv, HandleObject elementalFun)
{
    uint32_t limit = Min(iv.scalarLengthOfPackedDimensions(), 3U);
    SpewWarmup(cx, "build", limit);
    return buildUpTo(cx, result, iv, elementalFun, limit);
}

ParallelArrayObject::ExecutionStatus
ParallelArrayObject::WarmupMode::map(JSContext *cx, HandleParallelArrayObject source,
                                     HandleParallelArrayObject result, HandleObject elementalFun)
{
    uint32_t limit = Min(source->outermostDimension(), 3U);
    SpewWarmup(cx, "map", limit);
    return mapUpTo(cx, source, result, elementalFun, limit);
}

ParallelArrayObject::ExecutionStatus
ParallelArrayObject::WarmupMode::reduce(JSContext *cx, HandleParallelArrayObject source,
                                        HandleParallelArrayObject result,
                                        HandleObject elementalFun, MutableHandleValue vp)
{
    uint32_t limit = Min(source->outermostDimension(), 3U);
    SpewWarmup(cx, "reduce", limit);
    return reduceUpTo(cx, source, result, elementalFun, vp, limit);
}

ParallelArrayObject::ExecutionStatus
ParallelArrayObject::WarmupMode::scatter(JSContext *cx, HandleParallelArrayObject source,
                                         HandleParallelArrayObject result, HandleObject targetsObj,
                                         HandleValue defaultValue, HandleObject conflictFun)
{
    uint32_t targetsLength;
    if (is(targetsObj))
        targetsLength = as(targetsObj)->outermostDimension();
    else if (!GetLengthProperty(cx, targetsObj, &targetsLength))
        return ExecutionFatal;
    uint32_t limit = Min(source->outermostDimension(), Min(targetsLength, 3U));
    SpewWarmup(cx, "scatter", limit);
    return scatterUpTo(cx, source, result, targetsObj, defaultValue, conflictFun, limit);
}

ParallelArrayObject::ExecutionStatus
ParallelArrayObject::WarmupMode::filter(JSContext *cx, HandleParallelArrayObject source,
                                        HandleParallelArrayObject result, HandleObject filters)
{
    return ExecutionSucceeded;
}

ParallelArrayObject::ExecutionStatus
ParallelArrayObject::ParallelMode::build(JSContext *cx, HandleParallelArrayObject buffer,
                                         IndexInfo &iv, HandleObject elementalFun)
{
    BuildBodyDefn bodyDefn(cx, buffer, iv, elementalFun);
    ParallelArrayOp<BuildBodyDefn, 31> taskSet(cx, bodyDefn, elementalFun, buffer);
    return taskSet.apply();
}

bool
ParallelArrayObject::ApplyToEachBodyDefn::pre(size_t numSlices,
                                              RootedTypeObject &resultType)
{
    // Find the TI object for the results we will be writing.  This
    // may be NULL if we are doing a multidim op and there is no
    // appropriate row type.  In that case, we bail to sequential.
    if (!resultType.get())
        return false;

    {
        types::AutoEnterTypeInference enter(cx);
        typeSet = resultType->getProperty(cx, JSID_VOID, true);
        if (!typeSet)
            return false;
    }

    return true;
}

/* static */ ParallelArrayObject::ExecutionStatus
ParallelArrayObject::ToExecutionStatus(JSContext *cx,
                                       const char *opName,
                                       ParallelResult pr) {
    switch (pr) {
      case TP_SUCCESS:
        SpewExecution(cx, opName, ParallelArrayObject::parallel, ExecutionSucceeded);
        return ExecutionSucceeded;

      case TP_RETRY_SEQUENTIALLY:
        SpewExecution(cx, opName, ParallelArrayObject::parallel, ExecutionBailout);
        return ExecutionBailout;

      case TP_FATAL:
        SpewExecution(cx, opName, ParallelArrayObject::parallel, ExecutionFatal);
        return ExecutionFatal;
    }

    JS_ASSERT(false);
    return ExecutionFatal;
}

template<typename BodyDefn>
ParallelArrayObject::ApplyToEachBodyInstance<BodyDefn>::ApplyToEachBodyInstance(BodyDefn &bodyDefn)
    : bodyDefn_(bodyDefn)
{}

template<typename BodyDefn>
bool
ParallelArrayObject::ApplyToEachBodyInstance<BodyDefn>::execute(ExecuteArgs &args)
{
    unsigned start, end;
    ComputeTileBounds(args.slice, bodyDefn_.length(), &start, &end);

    typename BodyDefn::Instance *self = static_cast<typename BodyDefn::Instance*>(this);

    RootedObject buffer(bodyDefn_.cx, bodyDefn_.result->buffer());
    for (unsigned i = start; i < end; i++) {
        if (!args.slice.check())
            return false;

        if (!self->initializeArgv(args.argv, i))
            return false;

        Value result;
        args.enter(args.jitcode, args.argc, args.argv, NULL,
                   args.calleeToken, &result);

        if (result.isMagic()) {
            return false;
        } else {
            // check that the type of this value is already present
            // in the typeset: this must be true because we cannot
            // safely update type sets in parallel.
            Type resultType = GetValueType(bodyDefn_.cx, result);
            if (!bodyDefn_.typeSet->hasType(resultType))
                return false;

            // because we know that the type is already present, we
            // can bypass type inference here.
            buffer->setDenseArrayElement(i, result);
        }
    }

    return true;
}

bool
ParallelArrayObject::BuildBodyDefn::doWarmup() {
    ExecutionStatus warmupStatus = warmup.build(cx, result, iv, elementalFun);

    // reset the various indices after the warmup, since it modifies iv in place
    for (unsigned i = 0; i < iv.partialProducts.length(); i++) {
        iv.indices[i] = 0;
    }

    return (warmupStatus == ExecutionSucceeded);
}

bool
ParallelArrayObject::BuildBodyDefn::pre(size_t numSlices) {
    RootedTypeObject resultType(cx, result->maybeGetRowType(cx, iv.packedDimensions() - 1));
    return ApplyToEachBodyDefn::pre(numSlices, resultType);
}

ParallelArrayObject::BuildBodyInstance::BuildBodyInstance(BuildBodyDefn &bodyDefn, size_t sliceId,
                                      size_t numSlices)
    : ApplyToEachBodyInstance(bodyDefn),
      iv(bodyDefn.cx) // FIXME--not thread safe!!
{}

bool
ParallelArrayObject::BuildBodyInstance::init()
{
    if (!iv.dimensions.append(bodyDefn_.iv.dimensions.begin(),
                              bodyDefn_.iv.dimensions.end()))
        return false;

    if (!iv.initialize(bodyDefn_.iv.dimensions.length(),
                       bodyDefn_.iv.dimensions.length()))
        return false;

    return true;
}

bool
ParallelArrayObject::BuildBodyInstance::initializeArgv(Value *argv, unsigned idx) {
    if (!iv.fromScalar(idx))
        return false;

    for (uint32_t i = 0; i < iv.dimensions.length(); i++)
        argv[i+1].setNumber(iv.indices[i]);

    return true;
}

ParallelArrayObject::ExecutionStatus
ParallelArrayObject::ParallelMode::map(JSContext *cx, HandleParallelArrayObject source,
                                       HandleParallelArrayObject result, HandleObject elementalFun)
{
    JS_ASSERT(is(source));
    JS_ASSERT(is(result));

    Spew(cx, SpewOps, "%s map: attempting execution", toString());

    // TODO: Deal with multidimensional arrays.
    if (!source->isPackedOneDimensional())
        return ExecutionDisqualified;

    MapBodyDefn bodyDefn(cx, source, result, elementalFun);
    ParallelArrayOp<MapBodyDefn, 4> taskSet(cx, bodyDefn, elementalFun, result);
    return taskSet.apply();
}

bool
ParallelArrayObject::MapBodyDefn::doWarmup() {
    ExecutionStatus warmupStatus = warmup.map(cx, source, result, elementalFun);
    return (warmupStatus == ExecutionSucceeded);
}

bool
ParallelArrayObject::MapBodyDefn::pre(size_t numSlices) {
    RootedTypeObject resultType(cx, result->type());
    return ApplyToEachBodyDefn::pre(numSlices, resultType);
}

ParallelArrayObject::MapBodyInstance::MapBodyInstance(
    MapBodyDefn &bodyDefn, size_t sliceId,
    size_t numSlices)
    : ApplyToEachBodyInstance(bodyDefn),
      source(bodyDefn.source),
      elem(bodyDefn.cx)
{}

bool
ParallelArrayObject::MapBodyInstance::init() {
    return true;
}

bool
ParallelArrayObject::MapBodyInstance::initializeArgv(Value *argv, unsigned i) {
    // XXX should not use cx
    if (!getParallelArrayElement(bodyDefn_.cx, source, i, &elem))
        return false;

    // Arguments are in eic(h) order.
    argv[1] = elem;
    argv[2] = Int32Value(i);
    argv[3] = ObjectValue(*source);
    return true;
}

ParallelArrayObject::ExecutionStatus
ParallelArrayObject::ParallelMode::reduce(JSContext *cx, HandleParallelArrayObject source,
                                          HandleParallelArrayObject result,
                                          HandleObject elementalFun, MutableHandleValue vp)
{
    JS_ASSERT(is(source));
    JS_ASSERT_IF(result, is(result));

    Spew(cx, SpewOps, "%s map: attempting execution", toString());

    // TODO: Deal with multidimensional arrays.
    if (!source->isPackedOneDimensional())
        return ExecutionDisqualified;

    if (result) // we don't support scan in parallel right now
        return ExecutionDisqualified;

    ReduceBodyDefn bodyDefn(cx, source, elementalFun);
    ParallelArrayOp<ReduceBodyDefn, 4> taskSet(cx, bodyDefn, elementalFun, result);
    ExecutionStatus es;
    if ((es = taskSet.apply()) != ExecutionSucceeded)
        return es;
    return bodyDefn.post(vp);
}

bool
ParallelArrayObject::ReduceBodyDefn::doWarmup()
{
    RootedValue temp(cx);
    ExecutionStatus warmupStatus = warmup.reduce(cx, source, NullPtr(),
                                                 elementalFun, &temp);
    return (warmupStatus == ExecutionSucceeded);
}

bool
ParallelArrayObject::ReduceBodyDefn::pre(size_t numSlices)
{
    // to simplify parallel execution, we require at least ONE item
    // per parallel thread, so that none of them have to reduce an
    // empty vector.  In truth the threshold should probably be
    // significantly higher, for perf reasons, but that ought to be
    // enforced in the fallback code (i.e., before we even get here)
    if (length() < numSlices) {
        return false;
    }

    for (size_t i = 0; i < numSlices; i++) {
        if (!results.append(JSVAL_NULL))
            return false;
    }

    return true;
}

class AutoValueVectorSource {
private:
    AutoValueVector &results_;

public:
    AutoValueVectorSource(AutoValueVector &results)
        : results_(results)
    {}

    bool getElement(JSContext *cx, uint32_t index, MutableHandleValue elem) {
        elem.set(results_[index]);
        return true;
    }
};

class DummyResult {
public:
    void update(JSContext *cx, uint32_t index, HandleValue value) {}
};

ParallelArrayObject::ExecutionStatus
ParallelArrayObject::ReduceBodyDefn::post(MutableHandleValue vp)
{
    // Reduce the results from each thread into a single result:
    AutoValueVectorSource avsource(results);
    DummyResult result;
    if (!ReduceUpToGeneric(cx, avsource, result, elementalFun, vp, results.length()))
        return ExecutionFatal;
    return ExecutionSucceeded;
}

ParallelArrayObject::ReduceBodyInstance::ReduceBodyInstance(ReduceBodyDefn &bodyDefn, size_t sliceId,
                                        size_t numSlices)
    : bodyDefn_(bodyDefn)
{}

bool
ParallelArrayObject::ReduceBodyInstance::init()
{
    return true;
}

bool
ParallelArrayObject::ReduceBodyInstance::execute(ExecuteArgs &args)
{
    unsigned start, end;
    ComputeTileBounds(args.slice, bodyDefn_.length(), &start, &end);

    // ReduceBodyDefn::pre() ensures that there is at least one element
    // for each thread to process:
    JS_ASSERT(end - start > 1);

    // NB---I am not 100% comfortable with using RootedValue here, but
    // there doesn't seem to be an easy alternative.

    PerThreadData *pt = args.slice.perThreadData;
    HandleParallelArrayObject source = bodyDefn_.source;
    RootedValue acc(pt);
    if (!getParallelArrayElement(bodyDefn_.cx, source, start, &acc))
        return false;

    RootedValue elem(pt);
    for (unsigned i = start + 1; i < end; i++) {
        if (!args.slice.check())
            return false;

        if (!getParallelArrayElement(bodyDefn_.cx, source, i, &elem))
            return false;

        args.argv[1] = acc;
        args.argv[2] = elem;

        Value result;
        args.enter(args.jitcode, args.argc, args.argv, NULL,
                   args.calleeToken, &result);

        if (result.isMagic()) {
            return false;
        } else {
            acc = result;
        }
    }

    bodyDefn_.results[args.slice.sliceId] = acc;
    return true;
}

ParallelArrayObject::ExecutionStatus
ParallelArrayObject::ParallelMode::scatter(JSContext *cx, HandleParallelArrayObject source,
                                           HandleParallelArrayObject result,
                                           HandleObject targetsObj, HandleValue defaultValue,
                                           HandleObject conflictFun)
{
    return ExecutionDisqualified;
}

ParallelArrayObject::ExecutionStatus
ParallelArrayObject::ParallelMode::filter(JSContext *cx, HandleParallelArrayObject source,
                                          HandleParallelArrayObject result,
                                          HandleObject filter)
{
    // TODO: Deal with multidimensional arrays.
    if (!source->isPackedOneDimensional())
        return ExecutionDisqualified;

    uint32_t sourceLen = source->outermostDimension();

    // We must be able to treat the filter array as a dense array.  If this is
    // a PA, extract the underlying dense array.
    uint32_t filterLen;
    IndexInfo filterInfo(cx);
    RootedParallelArrayObject filterPA(cx);
    RootedObject filterArray(cx);
    uint32_t filterBase;
    if (!MaybeGetParallelArrayObjectAndLength(cx, filter, &filterPA, &filterInfo, &filterLen))
        return ExecutionFatal;
    if (filterPA) {
        if (!filterPA->isPackedOneDimensional())
            return ExecutionDisqualified;
        filterBase = filterPA->bufferOffset();
        if (filterPA->outermostDimension() < sourceLen)
            return ExecutionDisqualified;
        filterArray = filterPA->buffer();
    } else if (filter->isDenseArray()) {
        if (filter->getDenseArrayInitializedLength() < sourceLen)
            return ExecutionDisqualified;
        if (js_PrototypeHasIndexedProperties(filter))
            return ExecutionDisqualified;
        filterBase = 0;
        filterArray = filter;
    } else {
        return ExecutionDisqualified;
    }

    // For each worker thread, count how many non-zeros are present in
    // its slice.
    JS_ASSERT(filterArray->isDenseArray());
    FilterCountOp count(cx, source, filterArray, filterBase);
    ParallelResult pr = js::ExecuteForkJoinOp(cx, count);
    if (pr != TP_SUCCESS)
        return ToExecutionStatus(cx, "filter", pr);

    const CountVector &counts = count.counts();
    size_t countAll = 0;
    for (int i = 0; i < counts.length(); i++)
        countAll += counts[i];

    // Ideally, we would not initialize here.
    RootedObject resultBuffer(cx, result->buffer());
    resultBuffer->ensureDenseArrayInitializedLength(cx, countAll, 0);
    resultBuffer->setDenseArrayLength(countAll);

    // Now, do a second pass to copy the data.
    JS_ASSERT(filterArray->isDenseArray());
    FilterCopyOp copy(cx, source, filterArray, filterBase, count.counts(),
                           resultBuffer);
    pr = js::ExecuteForkJoinOp(cx, copy);
    return ToExecutionStatus(cx, "filter", pr);
}

bool
ParallelArrayObject::FilterCountOp::pre(size_t numSlices) {
    for (size_t i = 0; i < numSlices; i++)
        if (!counts_.append(0))
            return false;
    return true;
}

bool
ParallelArrayObject::FilterCountOp::parallel(ForkJoinSlice &slice) {
    size_t count = 0;
    unsigned length, start, end;
    length = source_->outermostDimension();
    ComputeTileBounds(slice, length, &start, &end);
    for (unsigned i = start; i < end; i++) {
        Value felem = filter_->getDenseArrayElement(i + filterBase_);
        count += (int)(ToBoolean(felem));
    }
    counts_[slice.sliceId] = count;
    return true;
}

bool
ParallelArrayObject::FilterCountOp::post(size_t numSlices) {
    return true;
}

bool
ParallelArrayObject::FilterCopyOp::pre(size_t numSlices) {
    JS_ASSERT(counts_.length() == numSlices);
    return true;
}

bool
ParallelArrayObject::FilterCopyOp::parallel(ForkJoinSlice &slice) {
    size_t count = 0;

    for (int i = 0; i < slice.sliceId; i++)
        count += counts_[i];

    unsigned length, start, end;
    length = source_->outermostDimension();
    ComputeTileBounds(slice, length, &start, &end);
    for (unsigned i = start; i < end; i++) {
        Value felem = filter_->getDenseArrayElement(i + filterBase_);
        if (ToBoolean(felem)) {
            RootedValue elem(slice.perThreadData);
            if (!getParallelArrayElement(cx_, source_, i, &elem))
                return false;
            resultBuffer_->setDenseArrayElement(count++, elem);
        }
    }
    return true;
}

bool
ParallelArrayObject::FilterCopyOp::post(size_t numSlices) {
    return true;
}

bool
ParallelArrayObject::FallbackMode::shouldTrySequential(ExecutionStatus parStatus)
{
    switch (parStatus) {
      case ExecutionFatal:
      case ExecutionSucceeded:
        return false;

      case ExecutionDisqualified:
      case ExecutionBailout:
        break;
    }

    // put the return true out here in case parStatus is corrupted.
    // do not add default: so we get warnings if enum is incomplete.
    return true;
}

#define FALLBACK_OP_BODY(NM,OP) do {                    \
        ExecutionStatus status;                         \
        status = parallel.OP;                           \
        SpewExecution(cx, NM, parallel, status);        \
                                                        \
        if (shouldTrySequential(status)) {              \
            status = sequential.OP;                     \
            SpewExecution(cx, NM, sequential, status);  \
        }                                               \
                                                        \
        return status;                                  \
    } while (false)

ParallelArrayObject::ExecutionStatus
ParallelArrayObject::FallbackMode::build(JSContext *cx, HandleParallelArrayObject result,
                                         IndexInfo &iv, HandleObject elementalFun)
{
    FALLBACK_OP_BODY("build", build(cx, result, iv, elementalFun));
}

ParallelArrayObject::ExecutionStatus
ParallelArrayObject::FallbackMode::map(JSContext *cx, HandleParallelArrayObject source,
                                       HandleParallelArrayObject result, HandleObject elementalFun)
{
    FALLBACK_OP_BODY("map", map(cx, source, result, elementalFun));
}

ParallelArrayObject::ExecutionStatus
ParallelArrayObject::FallbackMode::reduce(JSContext *cx, HandleParallelArrayObject source,
                                          HandleParallelArrayObject result,
                                          HandleObject elementalFun, MutableHandleValue vp)
{
    FALLBACK_OP_BODY("reduce", reduce(cx, source, result, elementalFun, vp));
}

ParallelArrayObject::ExecutionStatus
ParallelArrayObject::FallbackMode::scatter(JSContext *cx, HandleParallelArrayObject source,
                                           HandleParallelArrayObject result,
                                           HandleObject targetsObj, HandleValue defaultValue,
                                           HandleObject conflictFun)
{
    FALLBACK_OP_BODY("scatter", scatter(cx, source, result, targetsObj,
                                        defaultValue, conflictFun));
}

ParallelArrayObject::ExecutionStatus
ParallelArrayObject::FallbackMode::filter(JSContext *cx, HandleParallelArrayObject source,
                                          HandleParallelArrayObject result, HandleObject filters)
{
    FALLBACK_OP_BODY("filter", filter(cx, source, result, filters));
}

#ifdef DEBUG

const char *
ParallelArrayObject::ExecutionStatusToString(ExecutionStatus ss)
{
    switch (ss) {
      case ExecutionDisqualified:
        return "disqualified";
      case ExecutionBailout:
        return "bailout";
      case ExecutionFatal:
        return "fatal";
      case ExecutionSucceeded:
        return "success";
    }
    return "(unknown status)";
}

bool
ParallelArrayObject::AssertOptions::init(JSContext *cx, HandleValue v)
{
    RootedObject obj(cx, NonNullObject(cx, v));
    if (!obj)
        return false;

    RootedId id(cx);
    RootedValue propv(cx);
    JSString *propStr;
    JSBool match = false;
    bool ok;

    id = AtomToId(Atomize(cx, "mode", strlen("mode")));
    if (!JSObject::getGeneric(cx, obj, obj, id, &propv))
        return false;

    propStr = ToString(cx, propv);
    if (!propStr)
        return false;

    if ((ok = JS_StringEqualsAscii(cx, propStr, "par", &match)) && match)
        mode = &parallel;
    else if (ok && (ok = JS_StringEqualsAscii(cx, propStr, "seq", &match)) && match)
        mode = &sequential;
    else if (ok)
        return ReportBadArg(cx);
    else
        return false;

    id = AtomToId(Atomize(cx, "expect", strlen("expect")));
    if (!JSObject::getGeneric(cx, obj, obj, id, &propv))
        return false;

    propStr = ToString(cx, propv);
    if (!propStr)
        return false;

    if ((ok = JS_StringEqualsAscii(cx, propStr, "fatal", &match)) && match)
        expect = ExecutionFatal;
    else if (ok && (ok = JS_StringEqualsAscii(cx, propStr, "disqualified", &match)) && match)
        expect = ExecutionDisqualified;
    else if (ok && (ok = JS_StringEqualsAscii(cx, propStr, "bail", &match)) && match)
        expect = ExecutionBailout;
    else if (ok && (ok = JS_StringEqualsAscii(cx, propStr, "success", &match)) && match)
        expect = ExecutionSucceeded;
    else if (ok)
        return ReportBadArg(cx);
    else
        return false;

    return true;
}

bool
ParallelArrayObject::AssertOptions::check(JSContext *cx, ExecutionStatus actual)
{
    if (expect != actual) {
        JS_ReportError(cx, "expected %s for %s execution, got %s",
                       ExecutionStatusToString(expect),
                       mode->toString(),
                       ExecutionStatusToString(actual));
        return false;
    }

    return true;
}

#endif // DEBUG

//
// ParallelArrayObject
//

JSFunctionSpec ParallelArrayObject::methods[] = {
    JS_FN("map",                 NonGenericMethod<map>,            1, 0),
    JS_FN("reduce",              NonGenericMethod<reduce>,         1, 0),
    JS_FN("scan",                NonGenericMethod<scan>,           1, 0),
    JS_FN("scatter",             NonGenericMethod<scatter>,        1, 0),
    JS_FN("filter",              NonGenericMethod<filter>,         1, 0),
    JS_FN("flatten",             NonGenericMethod<flatten>,        0, 0),
    JS_FN("partition",           NonGenericMethod<partition>,      1, 0),
    JS_FN("get",                 NonGenericMethod<get>,            1, 0),
    JS_FN(js_toString_str,       NonGenericMethod<toString>,       0, 0),
    JS_FN(js_toLocaleString_str, NonGenericMethod<toLocaleString>, 0, 0),
    JS_FS_END
};

Class ParallelArrayObject::protoClass = {
    "ParallelArray",
    JSCLASS_HAS_CACHED_PROTO(JSProto_ParallelArray),
    JS_PropertyStub,         // addProperty
    JS_PropertyStub,         // delProperty
    JS_PropertyStub,         // getProperty
    JS_StrictPropertyStub,   // setProperty
    JS_EnumerateStub,
    JS_ResolveStub,
    JS_ConvertStub
};

Class ParallelArrayObject::class_ = {
    "ParallelArray",
    Class::NON_NATIVE |
    JSCLASS_HAS_RESERVED_SLOTS(RESERVED_SLOTS) |
    JSCLASS_HAS_CACHED_PROTO(JSProto_ParallelArray),
    JS_PropertyStub,         // addProperty
    JS_PropertyStub,         // delProperty
    JS_PropertyStub,         // getProperty
    JS_StrictPropertyStub,   // setProperty
    JS_EnumerateStub,
    JS_ResolveStub,
    JS_ConvertStub,
    NULL,                    // finalize
    NULL,                    // checkAccess
    NULL,                    // call
    NULL,                    // construct
    NULL,                    // hasInstance
    mark,                    // trace
    JS_NULL_CLASS_EXT,
    {
        lookupGeneric,
        lookupProperty,
        lookupElement,
        lookupSpecial,
        defineGeneric,
        defineProperty,
        defineElement,
        defineSpecial,
        getGeneric,
        getProperty,
        getElement,
        getElementIfPresent,
        getSpecial,
        setGeneric,
        setProperty,
        setElement,
        setSpecial,
        getGenericAttributes,
        getPropertyAttributes,
        getElementAttributes,
        getSpecialAttributes,
        setGenericAttributes,
        setPropertyAttributes,
        setElementAttributes,
        setSpecialAttributes,
        deleteProperty,
        deleteElement,
        deleteSpecial,
        NULL,                // enumerate
        NULL,                // typeof
        NULL,                // thisObject
    }
};

JSObject *
ParallelArrayObject::initClass(JSContext *cx, JSObject *obj)
{
    JS_ASSERT(obj->isNative());

    Rooted<GlobalObject *> global(cx, &obj->asGlobal());

    RootedObject proto(cx, global->createBlankPrototype(cx, &protoClass));
    if (!proto)
        return NULL;

    JSProtoKey key = JSProto_ParallelArray;
    RootedFunction ctor(cx);
    ctor = global->createConstructor(cx, construct, cx->names().ParallelArray, 0);
    if (!ctor ||
        !LinkConstructorAndPrototype(cx, ctor, proto) ||
        !DefinePropertiesAndBrand(cx, proto, NULL, methods) ||
        !DefineConstructorAndPrototype(cx, global, key, ctor, proto))
    {
        return NULL;
    }

    // Define the length and shape properties.
    RootedId lengthId(cx, AtomToId(cx->names().length));
    RootedId shapeId(cx, AtomToId(cx->names().shape));
    unsigned flags = JSPROP_PERMANENT | JSPROP_SHARED | JSPROP_GETTER;

    RootedObject scriptedLength(cx, js_NewFunction(cx, NullPtr(), NonGenericMethod<lengthGetter>,
                                                   0, JSFunction::NATIVE_FUN, global, NullPtr()));
    RootedObject scriptedShape(cx, js_NewFunction(cx, NullPtr(), NonGenericMethod<dimensionsGetter>,
                                                  0, JSFunction::NATIVE_FUN, global, NullPtr()));

    RootedValue value(cx, UndefinedValue());
    if (!scriptedLength || !scriptedShape ||
        !DefineNativeProperty(cx, proto, lengthId, value,
                              JS_DATA_TO_FUNC_PTR(PropertyOp, scriptedLength.get()), NULL,
                              flags, 0, 0) ||
        !DefineNativeProperty(cx, proto, shapeId, value,
                              JS_DATA_TO_FUNC_PTR(PropertyOp, scriptedShape.get()), NULL,
                              flags, 0, 0))
    {
        return NULL;
    }

    return proto;
}

bool
ParallelArrayObject::getParallelArrayElement(JSContext *cx, HandleParallelArrayObject pa,
                                             IndexInfo &iv, MutableHandleValue vp)
{
    JS_CHECK_RECURSION(cx, return false);

    JS_ASSERT(iv.isInitialized());

    // How many indices we have determine what dimension we are indexing. For
    // example, if we have 2 indices [n,m], we are indexing something on the
    // 2nd dimension.
    uint32_t d = iv.indices.length();
    uint32_t ndims = iv.dimensions.length();
    JS_ASSERT(d <= ndims);

    uint32_t npacked = pa->packedDimensions();
    uint32_t base = pa->bufferOffset();
    uint32_t end = base + iv.scalarLengthOfPackedDimensions();

    // If we are provided an index vector with every dimension up to the
    // packed dimensions specified, we are indexing a leaf. Leaves are always
    // values, but we do need to rewrap ParallelArray leaves. Note that the
    // scalar indices have to be scaled differently.
    if (d == npacked) {
        uint32_t index = base + iv.toScalar();
        if (index < end)
            return pa->getLeaf(cx, index, vp);

        vp.setUndefined();
        return true;
    }

    RootedObject buffer(cx, pa->buffer());

    // If we are provided with an index vector with more indices than we have
    // packed dimensions, then we need to get the leaf value, make a new
    // IndexInfo with the packed dimensions removed, and recur.
    if (d > npacked) {
        uint32_t index = base + iv.toScalar();
        if (index < end) {
            RootedParallelArrayObject sub(cx, as(&buffer->getDenseArrayElement(index).toObject()));
            IndexInfo siv(cx);
            if (!iv.split(siv, sub->packedDimensions()))
                return false;
            JS_ASSERT(IndexInfoIsSane(cx, sub, siv));
            return getParallelArrayElement(cx, sub, siv, vp);
        }

        vp.setUndefined();
        return true;
    }

    // Otherwise, if we don't need to recur and we aren't indexing a leaf
    // value, we should return a new ParallelArray of lesser
    // dimensionality. Here we create a new 'view' on the underlying buffer,
    // though whether a ParallelArray is a view or a copy is not observable by
    // the user.
    //
    // It is not enough to compute the scalar index and check bounds that way,
    // since the row length can be 0.
    if (iv.inBounds()) {
        IndexVector dims(cx);
        if (!dims.append(iv.dimensions.begin() + d, iv.dimensions.end()))
            return false;

        // Don't specialize the type when creating a row.
        RootedParallelArrayObject row(cx, create(cx, buffer, base + iv.toScalar(),
                                                 npacked - d, false));
        if (!row || !setDimensionsSlot(cx, row, dims))
            return false;

        RootedTypeObject rowType(cx, pa->maybeGetRowType(cx, d));
        if (rowType)
            row->setType(rowType);

        Spew(cx, SpewTypes, "rowtype: %s", TypeObjectString(row->type()));

        vp.setObject(*row);
        return true;
    }

    vp.setUndefined();
    return true;
}

bool
ParallelArrayObject::getParallelArrayElement(JSContext *cx, HandleParallelArrayObject pa,
                                             uint32_t index, IndexInfo *maybeIV,
                                             MutableHandleValue vp)
{
    // If we're higher dimensional, an initialized IndexInfo must be provided.
    JS_ASSERT_IF(maybeIV == NULL, pa->isPackedOneDimensional());
    JS_ASSERT_IF(!pa->isPackedOneDimensional(), maybeIV->isInitialized());
    JS_ASSERT_IF(!pa->isPackedOneDimensional(), maybeIV->indices.length() == 1);

    // If we only packed one dimension, we don't need to use IndexInfo.
    if (maybeIV == NULL || pa->isPackedOneDimensional()) {
        uint32_t base = pa->bufferOffset();
        uint32_t end = base + pa->outermostDimension();

        if (base + index < end)
            return pa->getLeaf(cx, base + index, vp);

        vp.setUndefined();
        return true;
    }

    maybeIV->indices[0] = index;
    return getParallelArrayElement(cx, pa, *maybeIV, vp);
}

bool
ParallelArrayObject::getParallelArrayElement(JSContext *cx, HandleParallelArrayObject pa,
                                             uint32_t index, MutableHandleValue vp)
{
    if (pa->isPackedOneDimensional())
        return getParallelArrayElement(cx, pa, index, NULL, vp);

    // Manually initialize to avoid re-rooting 'this', as this code could be
    // called from inside a loop, though you really should hoist out the
    // IndexInfo if that's the case.
    IndexInfo iv(cx);
    if (!pa->getDimensions(cx, iv.dimensions) || !iv.initialize(1, pa->packedDimensions()))
        return false;
    iv.indices[0] = index;
    return getParallelArrayElement(cx, pa, iv, vp);
}

TypeObject *
ParallelArrayObject::maybeGetRowType(JSContext *cx, uint32_t d)
{
    JS_ASSERT(d < packedDimensionsUnsafe());
    JS_ASSERT_IF(this->type()->construct, this->type()->construct->isParallelArray());

    if (!cx->typeInferenceEnabled())
        return NULL;

    RootedTypeObject type(cx, this->type());
    while (d--) {
        // This might happen if we have cleared construct and are not tracking
        // more specific row types.
        if (!type)
            return NULL;
        type = type->maybeGetRowType();
    }

    return *type.address();
}

ParallelArrayObject *
ParallelArrayObject::create(JSContext *cx, uint32_t length, uint32_t npacked)
{
    RootedObject buffer(cx, NewDenseEnsuredArray(cx, length));
    if (!buffer)
        return NULL;

    return create(cx, buffer, 0, npacked);
}

ParallelArrayObject *
ParallelArrayObject::create(JSContext *cx, HandleObject buffer, uint32_t offset,
                            uint32_t npacked, bool newType)
{
    RootedParallelArrayObject result(cx, as(NewBuiltinClassInstance(cx, &class_)));
    if (!result)
        return NULL;

    // We do not know SLOT_DIMENSIONS ahead of time, so we can't set it until
    // it's computed in finish, below.
    result->setSlot(SLOT_BUFFER, ObjectValue(*buffer));
    result->setSlot(SLOT_BUFFER_OFFSET, Int32Value(static_cast<int32_t>(offset)));
    result->setSlot(SLOT_PACKED_PREFIX_LENGTH, Int32Value(static_cast<int32_t>(npacked)));

    if (cx->typeInferenceEnabled() && newType) {
        // Try to specialize the type of the array to the call site, and make
        // it track the dimension, which is crucial for JIT optimizations.
        RootedTypeObject type(cx, GetTypeCallerInitObject(cx, JSProto_ParallelArray));
        if (!type)
            return NULL;
        result->setType(type);

        if (!(type->flags & OBJECT_FLAG_CONSTRUCT_CLEARED) && !type->construct) {
            Spew(cx, SpewTypes, "newtype: %s", TypeObjectString(type));

            // See jsinfer.h for note on TypeConstruction.
            type->construct = NewTypeConstructionParallelArray(cx, npacked);
            if (!type->construct)
                return NULL;

            // Allocate npacked - 1 row types. The dimensions of the row
            // types aren't filled in until finish is called.
            RootedTypeObject rowType(cx);
            Rooted<TaggedProto> tagged(cx);
            while (--npacked) {
                tagged = result->getProto();
                rowType = cx->compartment->types.newTypeObject(cx, JSProto_ParallelArray, tagged);
                if (!rowType)
                    return NULL;

                Spew(cx, SpewTypes, "new rowtype[%d]: %s", npacked,
                     TypeObjectString(rowType));

                rowType->construct = NewTypeConstructionParallelArray(cx, npacked);
                if (!rowType->construct)
                    return NULL;

                type->addPropertyType(cx, JSID_VOID, Type::ObjectType(rowType));
                type->construct->rowType = rowType;
                type = rowType;
            }
        }
    }

    // ParallelArray objects are frozen, so set it as non-extensible.
    if (!SetNonExtensible(cx, &class_, result))
        return NULL;

    return *result.address();
}

bool
ParallelArrayObject::finish(JSContext *cx, HandleParallelArrayObject pa, MutableHandleValue vp)
{
    IndexInfo dimsIV(cx);
    if (!dimsIV.dimensions.append(pa->buffer()->getArrayLength()) || !dimsIV.initialize(0, 1))
        return false;
    return finish(cx, pa, dimsIV, vp);
}

bool
ParallelArrayObject::finish(JSContext *cx, HandleParallelArrayObject pa,
                            IndexInfo &iv, MutableHandleValue vp)
{
    // Don't use packedDimensions() here as the dimensions slot isn't set
    // yet. Get the slot without asserts.
    JS_ASSERT(pa->packedDimensionsUnsafe() == iv.dimensions.length());

    // Step 1: Track packed dimensions in the type.
    if (!pa->initializeOrClearTypeConstruction(cx, iv.dimensions))
        return false;

    // Step 2: Compute the logical dimensions.
    //
    // Compute the maximum regular dimensions among all leaf ParallelArray
    // objects. The iv parameter has the dimensions that are represented
    // _packed_, but the actual user-visible dimensions could be bigger.
    //
    // Note that the iv must be initialized as we need the scalar length of
    // the dimensions, but its actual index values are ignored.
    IndexVector suffixInfimum(cx);
    IndexVector suffix(cx);

    const Value *start = pa->buffer()->getDenseArrayElements();
    const Value *end = start + iv.scalarLengthOfPackedDimensions();
    const Value *elem;

    if (is(*start)) {
        // The first time we see a ParallelArray leaf, record its shape as
        // the longest suffix we can append.
        if (!as(&start->toObject())->getDimensions(cx, suffixInfimum))
            return false;

        for (elem = start + 1; elem < end; elem++) {
            // If we get any non-ParallelArray leaf values, then we know the
            // dimensions cannot be larger and still be regular.
            if (!is(*elem)) {
                if (!suffixInfimum.resize(0))
                    return false;
                break;
            }

            // Otherwise truncate the longest suffix such that it is a prefix
            // of the current elem's dimensions.
            if (!as(&elem->toObject())->getDimensions(cx, suffix))
                return false;
            TruncateMismatchingSuffix(suffixInfimum, suffix);

            // If the longest suffix gets entirely truncated, then we are already
            // not regular, so just break.
            if (suffixInfimum.length() == 0)
                break;
        }
    }

    if (!iv.dimensions.append(suffixInfimum) || !setDimensionsSlot(cx, pa, iv.dimensions))
        return false;

    vp.setObject(*pa);
    return true;
}

bool
ParallelArrayObject::setDimensionsSlot(JSContext *cx, HandleParallelArrayObject pa,
                                       const IndexVector &dims)
{
    JS_ASSERT(pa->packedDimensionsUnsafe() <= dims.length());

    // Store the dimension vector into a dense array for better GC / layout.
    RootedObject dimArray(cx, NewDenseEnsuredArray(cx, dims.length()));
    if (!dimArray || !SetArrayNewType(cx, dimArray))
        return false;
    for (uint32_t i = 0; i < dims.length(); i++)
        JSObject::setDenseArrayElementWithType(cx, dimArray, i,
                                               Int32Value(static_cast<int32_t>(dims[i])));

    pa->setSlot(SLOT_DIMENSIONS, ObjectValue(*dimArray));

    return true;
}

bool
ParallelArrayObject::initializeOrClearTypeConstruction(JSContext *cx, const IndexVector &dims)
{
    JS_ASSERT(packedDimensionsUnsafe() <= dims.length());
    JS_ASSERT_IF(this->type()->construct, this->type()->construct->isParallelArray());

    RootedTypeObject type(cx, this->type());
    if (!type->construct)
        return true;

    uint32_t npacked = packedDimensionsUnsafe();

    // If we aren't initialized yet, initialize now.
    if (type->construct->numDimensions == 0) {
        for (uint32_t d = 0; d < npacked; d++, type = type->construct->rowType) {
            JS_ASSERT(type && type->construct);
            SetDimensions(type->construct, dims.begin() + d, npacked - d);
        }

        return true;
    }

    // If we have an existing construct, then we have to make sure that
    // the new array has the same dimensions.
    switch (MatchDimensions(type->construct, dims.begin(), npacked)) {
      case SameExactDimensions:
        // Don't need to invalidate any information.
        break;

      case SameNumberOfDimensions:
        if (type->construct->dimensions) {
            Spew(cx, SpewTypes, "clearing newtype dimensions: %s", TypeObjectString(type));

            // We don't need to clear the entire TypeConstruction as we can still
            // hold on to the specialized row types, but we need to NULL out the
            // specific dimensions.
            do {
                JS_ASSERT(type->construct);
                type->construct->dimensions = NULL;
            } while ((type = type->maybeGetRowType()));
        }

        break;

      case DifferentDimensions:
        Spew(cx, SpewTypes, "clearing newtype: %s", TypeObjectString(type));

        // Clearing the TypeConstruction so that in the future, dimensions are
        // not tracked and the row types are not used.
        type->clearConstruct(cx);

        // Dilute the typeset of JSID_VOID with unknown.
        type->addPropertyType(cx, JSID_VOID, Type::UnknownType());

        break;
    }

    return true;
}

ParallelArrayObject *
ParallelArrayObject::clone(JSContext *cx)
{
    RootedParallelArrayObject result(cx, as(NewBuiltinClassInstance(cx, &class_)));
    if (!result)
        return NULL;

    result->setType(this->type());
    result->setSlot(SLOT_DIMENSIONS, getFixedSlot(SLOT_DIMENSIONS));
    result->setSlot(SLOT_PACKED_PREFIX_LENGTH, getFixedSlot(SLOT_PACKED_PREFIX_LENGTH));
    result->setSlot(SLOT_BUFFER, getFixedSlot(SLOT_BUFFER));
    result->setSlot(SLOT_BUFFER_OFFSET, getFixedSlot(SLOT_BUFFER_OFFSET));

    if (!SetNonExtensible(cx, &class_, result))
        return NULL;

    return *result.address();
}

JSBool
ParallelArrayObject::construct(JSContext *cx, unsigned argc, Value *vp)
{
    CallArgs args = CallArgsFromVp(argc, vp);

    // Trivial case: create an empty ParallelArray object.
    if (args.length() < 1) {
        RootedParallelArrayObject result(cx, create(cx, 0));
        return result && finish(cx, result, args.rval());
    }

    // Case 1: initialize using an array value.
    if (args.length() == 1) {
        RootedObject source(cx, NonNullObject(cx, args[0]));
        if (!source)
            return false;

        uint32_t length;
        if (!GetLength(cx, source, &length))
            return false;

        RootedObject buffer(cx, NewFilledCopiedArray(cx, length, source));
        if (!buffer)
            return false;

        RootedParallelArrayObject result(cx, create(cx, buffer, 0, 1));

        // Transfer element types since we are copying. Usually the operations
        // themselves set the JSID_VOID type.
        if (cx->typeInferenceEnabled()) {
            AutoEnterTypeInference enter(cx);
            TypeObject *sourceType = source->getType(cx);
            TypeObject *resultType = result->type();
            if (!sourceType->unknownProperties() && !resultType->unknownProperties()) {
                HeapTypeSet *sourceIndexTypes = sourceType->getProperty(cx, JSID_VOID, false);
                HeapTypeSet *resultIndexTypes = resultType->getProperty(cx, JSID_VOID, true);
                sourceIndexTypes->addSubset(cx, resultIndexTypes);
            }
        }

        return finish(cx, result, args.rval());
    }

    // Case 2: initialize using a length/dimensions vector and kernel.
    //
    // If the length is an integer, we build a 1-dimensional parallel
    // array using the kernel.
    //
    // If the length is an array-like object of sizes, the i-th value in the
    // dimension array is the size of the i-th dimension.
    IndexInfo iv(cx);
    bool malformed;
    if (args[0].isObject()) {
        RootedObject dimObj(cx, &(args[0].toObject()));
        if (!ArrayLikeToIndexVector(cx, dimObj, iv.dimensions, &malformed))
            return false;
        if (malformed)
            return ReportBadLength(cx);
    } else {
        if (!iv.dimensions.resize(1))
            return false;

        if (!ToUint32(cx, args[0], &iv.dimensions[0], &malformed))
            return false;
        if (malformed) {
            RootedValue arg0(cx, args[0]);
            return ReportBadLengthOrArg(cx, arg0);
        }
    }

    // If the first argument wasn't a array-like or had no length, assume
    // empty parallel array, i.e. with shape being [0].
    if (iv.dimensions.length() == 0 && !iv.dimensions.append(0))
        return false;

    // Initialize with every dimension packed.
    if (!iv.initialize(iv.dimensions.length(), iv.dimensions.length()))
        return false;

    // We checked that each individual dimension does not overflow; now check
    // that the scalar length does not overflow.
    uint32_t length = iv.scalarLengthOfPackedDimensions();
    double d = iv.dimensions[0];
    for (uint32_t i = 1; i < iv.dimensions.length(); i++)
        d *= iv.dimensions[i];
    if (d != static_cast<double>(length))
        return ReportBadLength(cx);

    // Extract second argument, the elemental function.
    RootedObject elementalFun(cx, ValueToCallable(cx, &args[1]));
    if (!elementalFun)
        return false;

    RootedParallelArrayObject result(cx, create(cx, length, iv.dimensions.length()));
    if (!result)
        return false;

#ifdef DEBUG
    if (args.length() > 2) {
        AssertOptions options;
        RootedValue arg(cx, args[2]);
        if (!options.init(cx, arg) ||
            !options.check(cx, options.mode->build(cx, result, iv, elementalFun)))
        {
            return false;
        }

        return finish(cx, result, iv, args.rval());
    }
#endif

    if (fallback.build(cx, result, iv, elementalFun) != ExecutionSucceeded)
        return false;

    return finish(cx, result, iv, args.rval());
}

bool
ParallelArrayObject::map(JSContext *cx, CallArgs args)
{
    if (args.length() < 1)
        return ReportMoreArgsNeeded(cx, "ParallelArray.prototype.map", "0", "s");

    RootedParallelArrayObject source(cx, as(&args.thisv().toObject()));
    RootedParallelArrayObject result(cx, create(cx, source->outermostDimension()));
    if (!result)
        return false;

    RootedObject elementalFun(cx, ValueToCallable(cx, &args[0]));
    if (!elementalFun)
        return false;

#ifdef DEBUG
    if (args.length() > 1) {
        AssertOptions options;
        RootedValue arg(cx, args[1]);
        if (!options.init(cx, arg) ||
            !options.check(cx, options.mode->map(cx, source, result, elementalFun)))
        {
                return false;
        }

        return finish(cx, result, args.rval());
    }
#endif

    if (fallback.map(cx, source, result, elementalFun) != ExecutionSucceeded)
        return false;

    return finish(cx, result, args.rval());
}

bool
ParallelArrayObject::reduce(JSContext *cx, CallArgs args)
{
    if (args.length() < 1)
        return ReportMoreArgsNeeded(cx, "ParallelArray.prototype.reduce", "0", "s");

    RootedParallelArrayObject source(cx, as(&args.thisv().toObject()));
    uint32_t outer = source->outermostDimension();

    // Throw if the array is empty.
    if (outer == 0) {
        JS_ReportErrorNumber(cx, js_GetErrorMessage, NULL, JSMSG_PAR_ARRAY_REDUCE_EMPTY);
        return false;
    }

    RootedObject elementalFun(cx, ValueToCallable(cx, &args[0]));
    if (!elementalFun)
        return false;

#ifdef DEBUG
    if (args.length() > 1) {
        AssertOptions options;
        RootedValue arg(cx, args[1]);
        if (!options.init(cx, arg))
            return false;

        return options.check(cx, options.mode->reduce(cx, source, NullPtr(), elementalFun,
                                                      args.rval()));
    }
#endif

    // Call reduce with a null destination buffer to not store intermediates.
    return fallback.reduce(cx, source, NullPtr(), elementalFun, args.rval()) == ExecutionSucceeded;
}

bool
ParallelArrayObject::scan(JSContext *cx, CallArgs args)
{
    if (args.length() < 1)
        return ReportMoreArgsNeeded(cx, "ParallelArray.prototype.scan", "0", "s");

    RootedParallelArrayObject source(cx, as(&args.thisv().toObject()));
    uint32_t outer = source->outermostDimension();

    // Throw if the array is empty.
    if (outer == 0) {
        JS_ReportErrorNumber(cx, js_GetErrorMessage, NULL, JSMSG_PAR_ARRAY_REDUCE_EMPTY);
        return false;
    }

    RootedParallelArrayObject result(cx, create(cx, outer));
    if (!result)
        return false;

    RootedObject elementalFun(cx, ValueToCallable(cx, &args[0]));
    if (!elementalFun)
        return false;

    // Call reduce with a dummy out value to be discarded and a buffer to
    // store intermediates.
    RootedValue dummy(cx);

#ifdef DEBUG
    if (args.length() > 1) {
        AssertOptions options;
        RootedValue arg(cx, args[1]);
        if (!options.init(cx, arg) ||
            !options.check(cx, options.mode->reduce(cx, source, result, elementalFun, &dummy)))
        {
            return false;
        }
        return finish(cx, result, args.rval());
    }
#endif

    if (fallback.reduce(cx, source, result, elementalFun, &dummy) != ExecutionSucceeded)
        return false;

    return finish(cx, result, args.rval());
}

bool
ParallelArrayObject::scatter(JSContext *cx, CallArgs args)
{
    if (args.length() < 1)
        return ReportMoreArgsNeeded(cx, "ParallelArray.prototype.scatter", "0", "s");

    RootedParallelArrayObject source(cx, as(&args.thisv().toObject()));

    // Get the scatter vector.
    RootedObject targets(cx, NonNullObject(cx, args[0]));
    if (!targets)
        return false;

    // The default value is optional and defaults to undefined.
    RootedValue defaultValue(cx);
    if (args.length() >= 2)
        defaultValue = args[1];
    else
        defaultValue.setUndefined();

    // The conflict function is optional.
    RootedObject conflictFun(cx);
    if (args.length() >= 3 && !args[2].isUndefined()) {
        conflictFun = ValueToCallable(cx, &args[2]);
        if (!conflictFun)
            return false;
    }

    // The length of the result array is optional and defaults to the length
    // of the source array.
    uint32_t resultLength;
    if (args.length() >= 4) {
        bool malformed;
        if (!ToUint32(cx, args[3], &resultLength, &malformed))
            return false;
        if (malformed) {
            RootedValue arg3(cx, args[3]);
            return ReportBadLengthOrArg(cx, arg3, ".prototype.scatter");
        }
    } else {
        resultLength = source->outermostDimension();
    }

    RootedParallelArrayObject result(cx, create(cx, resultLength));
    if (!result)
        return false;

#ifdef DEBUG
    if (args.length() > 4) {
        AssertOptions options;
        RootedValue arg(cx, args[4]);
        if (!options.init(cx, arg) ||
            !options.check(cx, options.mode->scatter(cx, source, result, targets,
                                                     defaultValue, conflictFun)))
        {
            return false;
        }

        return finish(cx, result, args.rval());
    }
#endif

    if (fallback.scatter(cx, source, result, targets,
                         defaultValue, conflictFun) != ExecutionSucceeded)
    {
        return false;
    }

    return finish(cx, result, args.rval());
}

bool
ParallelArrayObject::filter(JSContext *cx, CallArgs args)
{
    if (args.length() < 1)
        return ReportMoreArgsNeeded(cx, "ParallelArray.prototype.filter", "0", "s");

    RootedParallelArrayObject source(cx, as(&args.thisv().toObject()));
    RootedParallelArrayObject result(cx, create(cx, 0));
    if (!result)
        return false;

    // Get the filter vector.
    RootedObject filters(cx, NonNullObject(cx, args[0]));
    if (!filters)
        return false;

#ifdef DEBUG
    if (args.length() > 1) {
        AssertOptions options;
        RootedValue arg(cx, args[1]);
        if (!options.init(cx, arg) ||
            !options.check(cx, options.mode->filter(cx, source, result, filters)))
        {
            return false;
        }

        return finish(cx, result, args.rval());
    }
#endif

    if (fallback.filter(cx, source, result, filters) != ExecutionSucceeded)
        return false;

    return finish(cx, result, args.rval());
}

bool
ParallelArrayObject::flatten(JSContext *cx, CallArgs args)
{
    RootedParallelArrayObject source(cx, as(&args.thisv().toObject()));

    IndexInfo iv(cx);
    if (!iv.initialize(cx, source, 0))
        return false;

    // Throw if already flat.
    if (iv.dimensions.length() == 1) {
        JS_ReportErrorNumber(cx, js_GetErrorMessage, NULL, JSMSG_PAR_ARRAY_ALREADY_FLAT);
        return false;
    }

    RootedObject buffer(cx, source->buffer());
    uint32_t npacked = source->packedDimensions();

    // The easy case: we have at least 2 dimensions packed, so we can just
    // create a view and decrement the packed dimensions.
    if (npacked >= 2) {
        // Flatten the two outermost dimensions.
        iv.dimensions[1] *= iv.dimensions[0];
        iv.dimensions.erase(iv.dimensions.begin());

        RootedParallelArrayObject result(cx, create(cx, buffer, source->bufferOffset(),
                                                    npacked - 1));
        if (!result || !setDimensionsSlot(cx, result, iv.dimensions))
            return false;

        args.rval().setObject(*result);
        return true;
    }

    // Otherwise we only have one packed dimension. We make a new packed
    // backing and copy elements point-wise. We make the new packed backing as
    // big as possible without creating unnecessary wrappers.
    //
    // Note that we get values directly from the backing buffer without going
    // through getParallelArrayElement, as we don't want to rewrap leaves
    // here.
    JS_ASSERT(source->isPackedOneDimensional());
    JS_ASSERT(source->bufferOffset() + iv.dimensions[0] <=
              buffer->getDenseArrayInitializedLength());

    uint32_t dim0 = iv.dimensions[0];

    const Value *start = buffer->getDenseArrayElements() + source->bufferOffset();
    const Value *end = start + dim0;
    const Value *elem;

    // Find the least number of dimensions packed across all leaf arrays. This
    // is at least 1.
    uint32_t leafPacked = as(&start->toObject())->packedDimensions();
    for (elem = start + 1; elem < end; elem++) {
        leafPacked = Min(as(&elem->toObject())->packedDimensions(), leafPacked);
        if (leafPacked == 1)
            break;
    }

    // The length is at least dimensions[0] * dimensions[1]. We're guaranteed
    // to be in bounds here, as leafPacked is at most iv.dimensions.length() -
    // 1.
    uint32_t length = dim0;
    for (uint32_t i = 0; i < leafPacked; i++)
        length *= iv.dimensions[i + 1];

    RootedParallelArrayObject result(cx, create(cx, length, npacked - 1 + leafPacked));
    if (!result)
        return false;
    RootedObject resultBuffer(cx, result->buffer());
    RootedTypeObject resultType(cx, result->type());

    // Pointers for the fast path.
    const Value *leafStart;
    const Value *leafEnd;
    const Value *leafElem;

    // A place to store the element to be copied for the slow path.
    RootedValue copyElem(cx);

    RootedParallelArrayObject leaf(cx);
    uint32_t i = 0;

    // Note that the body of the slow path assumes the leafs to not be empty
    // arrays, so we check that length > 0 here.
    for (elem = start; length > 0 && elem < end; elem++) {
        if (!JS_CHECK_OPERATION_LIMIT(cx))
            return ExecutionFatal;

        leaf = as(&elem->toObject());

        if (leaf->packedDimensions() == leafPacked) {
            leafStart = leaf->buffer()->getDenseArrayElements() + leaf->bufferOffset();
            leafEnd = leafStart + length / dim0;

            for (leafElem = leafStart; leafElem < leafEnd; leafElem++) {
                copyElem = *leafElem;
                SetLeafValueWithType(cx, resultBuffer, resultType, i++, copyElem);
            }
        } else {
            IndexInfo liv(cx);
            if (!liv.initialize(cx, leaf, leafPacked))
                return false;

            // We can use bump() here and be guaranteed that it iterates
            // exactly length / dim0 times, as the logical shape invariant
            // guarantee that the first leafPacked dimensions of the leaf
            // array are the same as the dimensions of the source array we
            // multiplied to obtain length.
            do {
                if (!getParallelArrayElement(cx, leaf, liv, &copyElem))
                    return false;
                SetLeafValueWithType(cx, resultBuffer, resultType, i++, copyElem);
            } while (liv.bump());
        }
    }

    // Flatten the two outermost dimensions.
    iv.dimensions[1] *= dim0;
    iv.dimensions.erase(iv.dimensions.begin());

    if (!result->initializeOrClearTypeConstruction(cx, iv.dimensions))
        return false;

    if (!setDimensionsSlot(cx, result, iv.dimensions))
        return false;

    args.rval().setObject(*result);
    return true;
}

bool
ParallelArrayObject::partition(JSContext *cx, CallArgs args)
{
    if (args.length() < 1)
        return ReportMoreArgsNeeded(cx, "ParallelArray.prototype.partition", "0", "s");

    uint32_t newDimension;
    bool malformed;
    if (!ToUint32(cx, args[0], &newDimension, &malformed))
        return false;
    if (malformed)
        return ReportBadPartition(cx);

    RootedParallelArrayObject source(cx, as(&args.thisv().toObject()));

    IndexVector dims(cx);
    if (!source->getDimensions(cx, dims))
        return false;

    // Throw if the outer dimension is not divisible by the new dimension.
    uint32_t outer = dims[0];
    if (newDimension == 0 || outer % newDimension)
        return ReportBadPartition(cx);

    // Set the new outermost dimension to be the quotient of the old outermost
    // dimension and the new dimension.
    if (!dims.insert(dims.begin(), outer / newDimension))
        return false;

    // Set the old outermost dimension to be the new dimension.
    dims[1] = newDimension;

    uint32_t npacked = source->packedDimensions() + 1;
    RootedObject buffer(cx, source->buffer());
    RootedParallelArrayObject result(cx, create(cx, buffer, source->bufferOffset(), npacked));

    if (!result->initializeOrClearTypeConstruction(cx, dims))
        return false;

    if (!result || !setDimensionsSlot(cx, result, dims))
        return false;

    args.rval().setObject(*result);
    return true;
}

bool
ParallelArrayObject::get(JSContext *cx, CallArgs args)
{
    if (args.length() < 1)
        return ReportMoreArgsNeeded(cx, "ParallelArray.prototype.get", "0", "s");

    RootedParallelArrayObject source(cx, as(&args.thisv().toObject()));
    RootedObject indicesObj(cx, NonNullObject(cx, args[0]));
    if (!indicesObj)
        return false;

    IndexInfo iv(cx);
    if (!iv.initialize(cx, source, 0))
        return false;

    bool malformed;
    if (!ArrayLikeToIndexVector(cx, indicesObj, iv.indices, &malformed))
        return false;

    // Throw if the shape of the index vector is wrong.
    if (iv.indices.length() == 0 || iv.indices.length() > iv.dimensions.length())
        return ReportBadArg(cx, ".prototype.get");

    // Don't throw on overflow, just return undefined.
    if (malformed) {
        args.rval().setUndefined();
        return true;
    }

    return getParallelArrayElement(cx, source, iv, args.rval());
}

bool
ParallelArrayObject::dimensionsGetter(JSContext *cx, CallArgs args)
{
    RootedObject dimArray(cx, as(&args.thisv().toObject())->dimensionArray());
    RootedObject copy(cx, NewDenseCopiedArray(cx, dimArray->getDenseArrayInitializedLength(),
                                              dimArray->getDenseArrayElements()));
    if (!copy)
        return false;
    // Reuse the existing dimension array's type.
    copy->setType(dimArray->type());
    args.rval().setObject(*copy);
    return true;
}

bool
ParallelArrayObject::lengthGetter(JSContext *cx, CallArgs args)
{
    args.rval().setNumber(as(&args.thisv().toObject())->outermostDimension());
    return true;
}

bool
ParallelArrayObject::toStringBuffer(JSContext *cx, HandleParallelArrayObject pa,
                                    bool useLocale, StringBuffer &sb)
{
    JS_CHECK_RECURSION(cx, return false);

    IndexInfo iv(cx);

    uint32_t npacked = pa->packedDimensions();
    if (!iv.initialize(cx, pa, npacked))
        return false;

    // Truncate up to the packed dimensions for use with the
    // {Open,Close}Delimiter helpers below.
    iv.dimensions.shrinkBy(iv.dimensions.length() - npacked);
    uint32_t length = iv.scalarLengthOfPackedDimensions();

    RootedValue tmp(cx);
    RootedValue localeElem(cx);
    RootedId id(cx);

    const Value *start = pa->buffer()->getDenseArrayElements() + pa->bufferOffset();
    const Value *end = start + length;
    const Value *elem;

    for (elem = start; elem < end; elem++, iv.bump()) {
        if (!JS_CHECK_OPERATION_LIMIT(cx))
            return false;

        // All holes in parallel arrays are eagerly filled with undefined.
        JS_ASSERT(!elem->isMagic(JS_ARRAY_HOLE));

        if (!OpenDelimiters(iv, sb))
            return false;

        if (!elem->isNullOrUndefined()) {
            if (useLocale) {
                tmp = *elem;
                RootedObject robj(cx, ToObject(cx, tmp));
                if (!robj)
                    return false;

                id = NameToId(cx->names().toLocaleString);
                if (!robj->callMethod(cx, id, 0, NULL, &localeElem) ||
                    !ValueToStringBuffer(cx, localeElem, sb))
                {
                    return false;
                }
            } else {
                if (!ValueToStringBuffer(cx, *elem, sb))
                    return false;
            }
        }

        if (!CloseDelimiters(iv, sb))
            return false;
    }

    return true;
}

bool
ParallelArrayObject::toString(JSContext *cx, CallArgs args)
{
    StringBuffer sb(cx);
    RootedParallelArrayObject pa(cx, as(&args.thisv().toObject()));
    if (!toStringBuffer(cx, pa, false, sb))
        return false;

    if (JSString *str = sb.finishString()) {
        args.rval().setString(str);
        return true;
    }

    return false;
}

bool
ParallelArrayObject::toLocaleString(JSContext *cx, CallArgs args)
{
    StringBuffer sb(cx);
    RootedParallelArrayObject pa(cx, as(&args.thisv().toObject()));
    if (!toStringBuffer(cx, pa, true, sb))
        return false;

    if (JSString *str = sb.finishString()) {
        args.rval().setString(str);
        return true;
    }

    return false;
}

void
ParallelArrayObject::mark(JSTracer *trc, RawObject obj)
{
    gc::MarkSlot(trc, &obj->getReservedSlotRef(SLOT_DIMENSIONS), "parallelarray.shape");
    gc::MarkSlot(trc, &obj->getReservedSlotRef(SLOT_PACKED_PREFIX_LENGTH), "parallelarray.packed-prefix");
    gc::MarkSlot(trc, &obj->getReservedSlotRef(SLOT_BUFFER), "parallelarray.buffer");
    gc::MarkSlot(trc, &obj->getReservedSlotRef(SLOT_BUFFER_OFFSET), "parallelarray.buffer-offset");
}

JSBool
ParallelArrayObject::lookupGeneric(JSContext *cx, HandleObject obj, HandleId id,
                                   MutableHandleObject objp, MutableHandleShape propp)
{
    uint32_t i;
    if (js_IdIsIndex(id, &i))
        return lookupElement(cx, obj, i, objp, propp);

    RootedObject proto(cx, obj->getProto());
    if (proto)
        return JSObject::lookupGeneric(cx, proto, id, objp, propp);

    objp.set(NULL);
    propp.set(NULL);
    return true;
}

JSBool
ParallelArrayObject::lookupProperty(JSContext *cx, HandleObject obj, HandlePropertyName name,
                                    MutableHandleObject objp, MutableHandleShape propp)
{
    RootedId id(cx, NameToId(name));
    return lookupGeneric(cx, obj, id, objp, propp);
}

JSBool
ParallelArrayObject::lookupElement(JSContext *cx, HandleObject obj, uint32_t index,
                                   MutableHandleObject objp, MutableHandleShape propp)
{
    // No prototype walking for elements.
    if (index < as(obj)->outermostDimension()) {
        MarkNonNativePropertyFound(obj, propp);
        objp.set(obj);
        return true;
    }

    objp.set(NULL);
    propp.set(NULL);
    return true;
}

JSBool
ParallelArrayObject::lookupSpecial(JSContext *cx, HandleObject obj, HandleSpecialId sid,
                                   MutableHandleObject objp, MutableHandleShape propp)
{
    RootedId id(cx, SPECIALID_TO_JSID(sid));
    return lookupGeneric(cx, obj, id, objp, propp);
}

JSBool
ParallelArrayObject::defineGeneric(JSContext *cx, HandleObject obj, HandleId id, HandleValue value,
                                   JSPropertyOp getter, StrictPropertyOp setter, unsigned attrs)
{
    uint32_t i;
    RootedParallelArrayObject pa(cx, as(obj));
    if (js_IdIsIndex(id, &i) && i < pa->outermostDimension()) {
        RootedValue existingValue(cx);
        if (!getParallelArrayElement(cx, pa, i, &existingValue))
            return false;

        bool same;
        if (!SameValue(cx, value, existingValue, &same))
            return false;
        if (!same)
            return Throw(cx, id, JSMSG_CANT_REDEFINE_PROP);
    } else {
        RootedValue tmp(cx, value);
        if (!setGeneric(cx, obj, id, &tmp, true))
            return false;
    }

    return setGenericAttributes(cx, obj, id, &attrs);
}

JSBool
ParallelArrayObject::defineProperty(JSContext *cx, HandleObject obj,
                                    HandlePropertyName name, HandleValue value,
                                    JSPropertyOp getter, StrictPropertyOp setter, unsigned attrs)
{
    RootedId id(cx, NameToId(name));
    return defineGeneric(cx, obj, id, value, getter, setter, attrs);
}

JSBool
ParallelArrayObject::defineElement(JSContext *cx, HandleObject obj,
                                   uint32_t index, HandleValue value,
                                   PropertyOp getter, StrictPropertyOp setter, unsigned attrs)
{
    RootedId id(cx);
    if (!IndexToId(cx, index, id.address()))
        return false;
    return defineGeneric(cx, obj, id, value, getter, setter, attrs);
}

JSBool
ParallelArrayObject::defineSpecial(JSContext *cx, HandleObject obj,
                                   HandleSpecialId sid, HandleValue value,
                                   PropertyOp getter, StrictPropertyOp setter, unsigned attrs)
{
    RootedId id(cx, SPECIALID_TO_JSID(sid));
    return defineGeneric(cx, obj, id, value, getter, setter, attrs);
}

JSBool
ParallelArrayObject::getGeneric(JSContext *cx, HandleObject obj, HandleObject receiver,
                                HandleId id, MutableHandleValue vp)
{
    RootedValue idval(cx, IdToValue(id));

    uint32_t index;
    if (IsDefinitelyIndex(idval, &index))
        return getElement(cx, obj, receiver, index, vp);

    Rooted<SpecialId> sid(cx);
    if (ValueIsSpecial(obj, &idval, sid.address(), cx))
        return getSpecial(cx, obj, receiver, sid, vp);

    JSAtom *atom = ToAtom(cx, idval);
    if (!atom)
        return false;

    if (atom->isIndex(&index))
        return getElement(cx, obj, receiver, index, vp);

    Rooted<PropertyName*> name(cx, atom->asPropertyName());
    return getProperty(cx, obj, receiver, name, vp);
}

JSBool
ParallelArrayObject::getProperty(JSContext *cx, HandleObject obj, HandleObject receiver,
                                 HandlePropertyName name, MutableHandleValue vp)
{
    RootedObject proto(cx, obj->getProto());
    if (proto)
        return JSObject::getProperty(cx, proto, receiver, name, vp);

    vp.setUndefined();
    return true;
}

JSBool
ParallelArrayObject::getElement(JSContext *cx, HandleObject obj, HandleObject receiver,
                                uint32_t index, MutableHandleValue vp)
{
    // Unlike normal arrays, [] for ParallelArray does not walk the prototype
    // chain and just returns undefined.
    RootedParallelArrayObject pa(cx, as(obj));
    return getParallelArrayElement(cx, pa, index, vp);
}

JSBool
ParallelArrayObject::getElementIfPresent(JSContext *cx, HandleObject obj, HandleObject receiver,
                                         uint32_t index, MutableHandleValue vp, bool *present)
{
    RootedParallelArrayObject source(cx, as(obj));
    if (index < source->outermostDimension()) {
        if (!getParallelArrayElement(cx, source, index, vp))
            return false;
        *present = true;
        return true;
    }

    *present = false;
    vp.setUndefined();
    return true;
}

JSBool
ParallelArrayObject::getSpecial(JSContext *cx, HandleObject obj, HandleObject receiver,
                                HandleSpecialId sid, MutableHandleValue vp)
{
    if (!obj->getProto()) {
        vp.setUndefined();
        return true;
    }

    RootedId id(cx, SPECIALID_TO_JSID(sid));
    return baseops::GetProperty(cx, obj, receiver, id, vp);
}

JSBool
ParallelArrayObject::setGeneric(JSContext *cx, HandleObject obj, HandleId id,
                                MutableHandleValue vp, JSBool strict)
{
    JS_ASSERT(!obj->isExtensible());

    if (IdIsInBoundsIndex(cx, obj, id)) {
        if (strict)
            return JSObject::reportReadOnly(cx, id);
        if (cx->hasStrictOption())
            return JSObject::reportReadOnly(cx, id, JSREPORT_STRICT | JSREPORT_WARNING);
    } else {
        if (strict)
            return obj->reportNotExtensible(cx);
        if (cx->hasStrictOption())
            return obj->reportNotExtensible(cx, JSREPORT_STRICT | JSREPORT_WARNING);
    }

    return true;
}

JSBool
ParallelArrayObject::setProperty(JSContext *cx, HandleObject obj, HandlePropertyName name,
                                 MutableHandleValue vp, JSBool strict)
{
    RootedId id(cx, NameToId(name));
    return setGeneric(cx, obj, id, vp, strict);
}

JSBool
ParallelArrayObject::setElement(JSContext *cx, HandleObject obj, uint32_t index,
                                MutableHandleValue vp, JSBool strict)
{
    RootedId id(cx);
    if (!IndexToId(cx, index, id.address()))
        return false;
    return setGeneric(cx, obj, id, vp, strict);
}

JSBool
ParallelArrayObject::setSpecial(JSContext *cx, HandleObject obj, HandleSpecialId sid,
                                MutableHandleValue vp, JSBool strict)
{
    RootedId id(cx, SPECIALID_TO_JSID(sid));
    return setGeneric(cx, obj, id, vp, strict);
}

JSBool
ParallelArrayObject::getGenericAttributes(JSContext *cx, HandleObject obj, HandleId id,
                                          unsigned *attrsp)
{
    *attrsp = JSPROP_PERMANENT | JSPROP_READONLY;

    uint32_t i;
    if (js_IdIsIndex(id, &i))
        *attrsp |= JSPROP_ENUMERATE;
    return true;
}

JSBool
ParallelArrayObject::getPropertyAttributes(JSContext *cx, HandleObject obj, HandlePropertyName name,
                                           unsigned *attrsp)
{
    *attrsp = JSPROP_PERMANENT | JSPROP_READONLY;
    return true;
}

JSBool
ParallelArrayObject::getElementAttributes(JSContext *cx, HandleObject obj, uint32_t index,
                                          unsigned *attrsp)
{
    *attrsp = JSPROP_PERMANENT | JSPROP_READONLY | JSPROP_ENUMERATE;
    return true;
}

JSBool
ParallelArrayObject::getSpecialAttributes(JSContext *cx, HandleObject obj, HandleSpecialId sid,
                                          unsigned *attrsp)
{
    *attrsp = JSPROP_PERMANENT | JSPROP_READONLY | JSPROP_ENUMERATE;
    return true;
}

JSBool
ParallelArrayObject::setGenericAttributes(JSContext *cx, HandleObject obj, HandleId id,
                                          unsigned *attrsp)
{
    if (IdIsInBoundsIndex(cx, obj, id)) {
        unsigned attrs;
        if (!getGenericAttributes(cx, obj, id, &attrs))
            return false;
        if (*attrsp != attrs)
            return Throw(cx, id, JSMSG_CANT_REDEFINE_PROP);
    }

    return obj->reportNotExtensible(cx);
}

JSBool
ParallelArrayObject::setPropertyAttributes(JSContext *cx, HandleObject obj, HandlePropertyName name,
                                           unsigned *attrsp)
{
    RootedId id(cx, NameToId(name));
    return setGenericAttributes(cx, obj, id, attrsp);
}

JSBool
ParallelArrayObject::setElementAttributes(JSContext *cx, HandleObject obj, uint32_t index,
                                          unsigned *attrsp)
{
    RootedId id(cx);
    if (!IndexToId(cx, index, id.address()))
        return false;
    return setGenericAttributes(cx, obj, id, attrsp);
}

JSBool
ParallelArrayObject::setSpecialAttributes(JSContext *cx, HandleObject obj, HandleSpecialId sid,
                                          unsigned *attrsp)
{
    RootedId id(cx, SPECIALID_TO_JSID(sid));
    return setGenericAttributes(cx, obj, id, attrsp);
}

JSBool
ParallelArrayObject::deleteGeneric(JSContext *cx, HandleObject obj, HandleId id,
                                   MutableHandleValue rval, JSBool strict)
{
    if (IdIsInBoundsIndex(cx, obj, id)) {
        if (strict)
            return obj->reportNotConfigurable(cx, id);
        if (cx->hasStrictOption()) {
            if (!obj->reportNotConfigurable(cx, id, JSREPORT_STRICT | JSREPORT_WARNING))
                return false;
        }

        rval.setBoolean(false);
        return true;
    }

    rval.setBoolean(true);
    return true;
}

JSBool
ParallelArrayObject::deleteProperty(JSContext *cx, HandleObject obj, HandlePropertyName name,
                                    MutableHandleValue rval, JSBool strict)
{
    RootedId id(cx, NameToId(name));
    return deleteGeneric(cx, obj, id, rval, strict);
}

JSBool
ParallelArrayObject::deleteElement(JSContext *cx, HandleObject obj, uint32_t index,
                                   MutableHandleValue rval, JSBool strict)
{
    RootedId id(cx);
    if (!IndexToId(cx, index, id.address()))
        return false;
    return deleteGeneric(cx, obj, id, rval, strict);
}

JSBool
ParallelArrayObject::deleteSpecial(JSContext *cx, HandleObject obj, HandleSpecialId sid,
                                   MutableHandleValue rval, JSBool strict)
{
    RootedId id(cx, SPECIALID_TO_JSID(sid));
    return deleteGeneric(cx, obj, id, rval, strict);
}

bool
ParallelArrayObject::enumerate(JSContext *cx, HandleObject obj, unsigned flags,
                               AutoIdVector *props)
{
    RootedParallelArrayObject source(cx, as(obj));

    // ParallelArray objects have no holes.
    if (source->outermostDimension() > 0) {
        for (uint32_t i = 0; i < source->outermostDimension(); i++) {
            if (!props->append(INT_TO_JSID(i)))
                return false;
        }
    }

    if (flags & JSITER_OWNONLY)
        return true;

    RootedObject proto(cx, obj->getProto());
    if (proto) {
        AutoIdVector protoProps(cx);
        if (!GetPropertyNames(cx, proto, flags, &protoProps))
            return false;

        // ParallelArray objects do not inherit any indexed properties on the
        // prototype chain.
        uint32_t dummy;
        for (uint32_t i = 0; i < protoProps.length(); i++) {
            if (!js_IdIsIndex(protoProps[i], &dummy) && !props->append(protoProps[i]))
                return false;
        }
    }

    return true;
}

JSObject *
js_InitParallelArrayClass(JSContext *cx, HandleObject obj)
{
    return ParallelArrayObject::initClass(cx, obj);
}
