/* -*- Mode: C++; tab-width: 4; indent-tabs-mode: nil; c-basic-offset: 4 -*-
 * vim: set ts=8 sw=4 et tw=99 ft=cpp:
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef ParallelArray_h__
#define ParallelArray_h__

#include "jsapi.h"
#include "jscntxt.h"
#include "jsobj.h"
#include "jsinfer.h"
#include "vm/threadpool.h"
#include "vm/forkjoin.h"

namespace js {

class ParallelArrayObject;
typedef Rooted<ParallelArrayObject *> RootedParallelArrayObject;
typedef Handle<ParallelArrayObject *> HandleParallelArrayObject;

//
// ParallelArray Overview
//
// Parallel arrays are immutable, possibly multi-dimensional arrays which
// enable parallel computation based on a few base operations. The execution
// model is one of fallback: try to execute operations in parallel, falling
// back to sequential implementation if (for whatever reason) the operation
// could not be executed in paralle. The API allows leeway to implementers to
// decide both representation and what is considered parallelizable.
//
// Currently ParallelArray objects are backed by dense arrays for ease of
// GC. For the higher-dimensional case, data is stored in a packed, row-major
// order representation in the backing dense array. See notes below about
// IndexInfo in how to convert between scalar offsets into the backing array
// and a vector of indices.
//
// ParallelArray objects are always dense. That is, all holes are eagerly
// filled in with undefined instead of being JS_ARRAY_HOLE. This results in a
// break from the behavior of normal JavaScript arrays: if a ParallelArray p
// is missing an indexed property i, p[i] is _always_ undefined and will never
// walk up the prototype chain in search of i.
//
// Except for the comprehension form, all operations (e.g. map, filter,
// reduce) operate on the outermost dimension only. That is, those operations
// only operate on the "rows" of the array. "Element" is used in context of
// ParallelArray objects to mean any indexable value of a ParallelArray
// object. For a one dimensional array, elements are always scalar values. For
// a higher dimensional array, elements could either be scalar values
// (i.e. leaves) or ParallelArray objects of lesser dimensions
// (i.e. subarrays).
//

class ParallelArrayObject : public JSObject {
  public:
    typedef Vector<uint32_t, 4> IndexVector;

    //
    // Helper structure to help index higher-dimensional arrays to convert
    // between a vector of indices and scalar offsets for use in the flat
    // backing dense array.
    //
    // IndexInfo instances _must_ be initialized using one of the initialize
    // methods before use.
    //
    // Typical usage is stack allocating an IndexInfo, initializing it with a
    // particular source ParallelArray object's dimensionality, and mutating
    // the indices member vector. For instance, to iterate through everything
    // in the first 2 dimensions of an array of > 2 dimensions:
    //
    //   IndexInfo iv(cx);
    //   if (!iv.initialize(cx, source, 2))
    //       return false;
    //   for (uint32_t i = 0; i < iv.dimensions[0]; i++) {
    //       for (uint32_t j = 0; j < iv.dimensions[1]; j++) {
    //           iv.indices[0] = i;
    //           iv.indices[1] = j;
    //           if (source->getParallelArrayElement(cx, iv, &elem))
    //               ...
    //       }
    //   }
    //
    // Note from the above that it is not required to fill out the indices
    // vector up to the full dimensionality. For an N-dimensional array,
    // having an indices vector of length D < N indexes a subarray.
    //

    struct IndexInfo {
        // Vector of indices. Should be large enough to hold up to
        // dimensions.length() indices.
        IndexVector indices;

        // Vector of dimensions of the ParallelArray object that the indices
        // are meant to index into.
        IndexVector dimensions;

        // Cached partial products of the dimensions up to the packed
        // dimension, d, defined by the following recurrence:
        //
        //   partialProducts[n] =
        //     1                                      if n == d
        //     dimensions[n+1] * partialProducts[n+1] otherwise
        //
        // These are used for computing scalar offsets.
        //
        // This vector may be shorter than the dimensions vector, as these are
        // only computed for the dimensions that we represent packed. That is,
        // the first partialProducts.length() dimensions are packed.
        IndexVector partialProducts;

        IndexInfo(JSContext *cx)
            : indices(cx), dimensions(cx), partialProducts(cx)
        {}

        // Prepares indices and computes partial products. The space argument
        // is the index space. The indices vector is resized to be of length
        // space. The d argument is how many dimensions are packed. Partial
        // products are only computed for packed dimensions.
        //
        // The dimensions vector must be filled already, and space and d must
        // be <= dimensions.length().
        inline bool initialize(uint32_t space, uint32_t d);

        // Load dimensions and the number of packed dimensions from a source,
        // then initialize as above.
        inline bool initialize(JSContext *cx, HandleParallelArrayObject source,
                               uint32_t space);

        // Bump the index by 1, wrapping over if necessary. Returns false when
        // the increment would go out of bounds.
        inline bool bump();

        // Return how many packed dimensions there are, i.e. the length of the
        // partialProducts vector.
        inline uint32_t packedDimensions();

        // Get the scalar length according to the partial products vector,
        // i.e. the product of the dimensions vector for the packed
        // dimensions.
        inline uint32_t scalarLengthOfPackedDimensions();

        // Compute the scalar index from the current index vector up to the
        // packed dimension.
        inline uint32_t toScalar();

        // Set the index vector up to the packed dimension according to a
        // scalar index.
        inline bool fromScalar(uint32_t index);

        // Split on partialProducts.length(), storing the tail of the split
        // into siv and initializing it with sd dimensions packed.
        //
        // This is used when addressing an element that crosses packed
        // dimension boundaries. That is, suppose a ParallelArray has D
        // logical dimensions and P packed dimensions, addressing an element
        // using N indices where P < N < D crosses the packed dimensions
        // boundary. In this case we first need to get the leaf value
        // according to the packed dimensions in the current array, then recur
        // on the sub-array.
        inline bool split(IndexInfo &siv, uint32_t sd);

        inline bool inBounds() const;
        bool isInitialized() const;
    };

    static JSObject *initClass(JSContext *cx, JSObject *obj);
    static Class class_;

    static inline bool is(const Value &v);
    static inline bool is(JSObject *obj);
    static inline ParallelArrayObject *as(JSObject *obj);

    inline JSObject *dimensionArray();
    inline uint32_t packedDimensions();
    inline JSObject *buffer();
    inline uint32_t bufferOffset();
    inline uint32_t outermostDimension();
    inline bool isOneDimensional();
    inline bool isPackedOneDimensional();
    inline bool getDimensions(JSContext *cx, IndexVector &dims);

    // The general case; requires an initialized IndexInfo.
    static bool getParallelArrayElement(JSContext *cx, HandleParallelArrayObject pa,
                                        IndexInfo &iv, MutableHandleValue vp);

    // Get the element at index in the outermost dimension. This is a
    // convenience function designed to require an IndexInfo only if it is
    // actually needed.
    //
    // If the parallel array is multidimensional, then the caller must provide
    // an IndexInfo initialized to length 1, which is used to access the
    // array. This argument is modified. If the parallel array is
    // one-dimensional, then maybeIV may be null.
    static bool getParallelArrayElement(JSContext *cx, HandleParallelArrayObject pa,
                                        uint32_t index, IndexInfo *maybeIV,
                                        MutableHandleValue vp);

    // Get the element at index in the outermost dimension. This is a
    // convenience function that initializes a temporary
    // IndexInfo if the parallel array is multidimensional.
    static bool getParallelArrayElement(JSContext *cx, HandleParallelArrayObject pa,
                                        uint32_t index, MutableHandleValue vp);

    static bool toStringBuffer(JSContext *cx, HandleParallelArrayObject pa,
                               bool useLocale, StringBuffer &sb);

    // Note that this is not an object op but called directly from the
    // iteration code, as we enumerate prototypes ourselves.
    static bool enumerate(JSContext *cx, HandleObject obj, unsigned flags,
                          AutoIdVector *props);

  private:
    enum {
        // The ParallelArray API refers to dimensions as "shape", but to avoid
        // confusion with the internal engine notion of a shape we call it
        // "dimensions" here.
        SLOT_DIMENSIONS = 0,

        // The physical dimensions of this array is the part that we represent
        // in a packed fashion instead of as rows of pointers to other
        // parallel arrays. This is always a prefix of the logical dimensions
        // (the slot above), so we only store the number of dimensions we
        // represent packed here.
        SLOT_PACKED_PREFIX_LENGTH,

        // Underlying dense array.
        //
        // Note that we do not attach TI information to the underlying
        // array. All type information is on the ParallelArray object.
        SLOT_BUFFER,

        // First index of the underlying buffer to be considered in bounds.
        SLOT_BUFFER_OFFSET,

        RESERVED_SLOTS
    };

    enum ExecutionStatus {
        // For some reason not eligible for parallel exec, use seq fallback
        ExecutionDisqualified = 0,

        // Parallel execution went off the safe path, use seq fallback
        ExecutionBailout,

        // Parallel or seq execution terminated in a fatal way, operation failed
        ExecutionFatal,

        // Parallel or seq op was successful
        ExecutionSucceeded
    };

    // Execution modes are kept as static instances of structs that implement
    // a signature that comprises of build, map, fold, scatter, and filter,
    // whose argument lists are defined in the macros below.
    //
    // Even though the base class |ExecutionMode| is purely abstract, we only
    // use dynamic dispatch when using the debug options. Almost always we
    // directly call the member function on one of the statics.
    //
    // XXX: The macros underneath are clunky, largely because warnings
    // regarding empty macro arguments are still enabled. We should like to
    // write:
    //
    //   #define DECLARE_ALL_OPS(QUALIFIERS, EXTRAS...)
    //       QUALIFIERS ExecutionStatus op(args, ##EXTRAS);
    //
    // And call it either like DECLARE_ALL_OPS() or DECLARE_ALL_OPS(virtual)
    // or DECLARE_ALL_OPS(virtual, uint32_t limit). But unused macro arguments
    // makes gcc warn gratuitously.

#define JS_PA_build_ARGS               \
    JSContext *cx,                     \
    HandleParallelArrayObject result,  \
    IndexInfo &iv,                     \
    HandleObject elementalFun

#define JS_PA_map_ARGS                 \
    JSContext *cx,                     \
    HandleParallelArrayObject source,  \
    HandleParallelArrayObject result,  \
    HandleObject elementalFun

#define JS_PA_reduce_ARGS              \
    JSContext *cx,                     \
    HandleParallelArrayObject source,  \
    HandleParallelArrayObject result,  \
    HandleObject elementalFun,         \
    MutableHandleValue vp

#define JS_PA_scatter_ARGS             \
    JSContext *cx,                     \
    HandleParallelArrayObject source,  \
    HandleParallelArrayObject result,  \
    HandleObject targets,              \
    HandleValue defaultValue,          \
    HandleObject conflictFun

#define JS_PA_filter_ARGS              \
    JSContext *cx,                     \
    HandleParallelArrayObject source,  \
    HandleParallelArrayObject result,  \
    HandleObject filters

#define JS_PA_DECLARE_OP(NAME) \
    ExecutionStatus NAME(JS_PA_ ## NAME ## _ARGS)

#define JS_PA_DECLARE_CUSTOM_OP1(NAME, BASE, EXTRA1) \
    ExecutionStatus NAME(JS_PA_ ## BASE ## _ARGS, EXTRA1)

#define JS_PA_DECLARE_ALL_OPS          \
    JS_PA_DECLARE_OP(build);           \
    JS_PA_DECLARE_OP(map);             \
    JS_PA_DECLARE_OP(reduce);          \
    JS_PA_DECLARE_OP(scatter);         \
    JS_PA_DECLARE_OP(filter);

#define JS_PA_DECLARE_ALL_CUSTOM_OPS1(SUFFIX, EXTRA1)             \
    JS_PA_DECLARE_CUSTOM_OP1(build ## SUFFIX, build, EXTRA1);     \
    JS_PA_DECLARE_CUSTOM_OP1(map ## SUFFIX, map, EXTRA1);         \
    JS_PA_DECLARE_CUSTOM_OP1(reduce ## SUFFIX, reduce, EXTRA1);   \
    JS_PA_DECLARE_CUSTOM_OP1(scatter ## SUFFIX, scatter, EXTRA1); \
    JS_PA_DECLARE_CUSTOM_OP1(filter ## SUFFIX, filter, EXTRA1);

    class ExecutionMode {
      public:
        // The comprehension form. Builds a higher-dimensional array using a
        // kernel function.
        virtual JS_PA_DECLARE_OP(build) = 0;

        // Maps a kernel function over the outermost dimension of the array.
        virtual JS_PA_DECLARE_OP(map) = 0;

        // Reduce to a value using a kernel function. Scan is like reduce, but
        // keeps the intermediate results in an array.
        virtual JS_PA_DECLARE_OP(reduce) = 0;

        // Scatter elements according to an index map.
        virtual JS_PA_DECLARE_OP(scatter) = 0;

        // Filter elements according to a truthy array.
        virtual JS_PA_DECLARE_OP(filter) = 0;

        virtual const char *toString() const = 0;
    };

    // Fallback means try parallel first, and if unable to execute in
    // parallel, execute sequentially.
    class FallbackMode : public ExecutionMode {
      public:
        JS_PA_DECLARE_ALL_OPS
        const char *toString() const { return "fallback"; }
        bool shouldTrySequential(ExecutionStatus parStatus);
    };

    class ParallelMode : public ExecutionMode {
      public:
        JS_PA_DECLARE_ALL_OPS
        const char *toString() const { return "parallel"; }
    };

    // Implementation shared by both sequential and warmup modes. If
    // writeElems is false, the operations are a "dry run" and do not write
    // anything to the buffer. These operations perform up to a limit, thus
    // the UpTo suffix. SequentialMode calls these up to the entire size of
    // the array, but warmup usually calls these up to a much smaller number
    // of iterations.
    class BaseSequentialMode : public ExecutionMode {
      protected:
        JS_PA_DECLARE_ALL_CUSTOM_OPS1(UpTo, uint32_t limit)
    };

    class SequentialMode : public BaseSequentialMode {
      public:
        JS_PA_DECLARE_ALL_OPS
        const char *toString() const { return "sequential"; }
    };

    class WarmupMode : public BaseSequentialMode {
      public:
        JS_PA_DECLARE_ALL_OPS
        const char *toString() const { return "warmup"; }
    };

    static SequentialMode sequential;
    static ParallelMode parallel;
    static FallbackMode fallback;
    static WarmupMode warmup;

#undef JS_PA_build_ARGS
#undef JS_PA_map_ARGS
#undef JS_PA_reduce_ARGS
#undef JS_PA_scatter_ARGS
#undef JS_PA_filter_ARGS
#undef JS_PA_DECLARE_OP
#undef JS_PA_DECLARE_CUSTOM_OP1
#undef JS_PA_DECLARE_ALL_OPS

    enum SpewChannel {
        SpewOps,
        SpewTypes,
        NumSpewChannels
    };

#ifdef DEBUG
    // Assert options can be passed in as an extra argument to the
    // operations. The grammar is:
    //
    //   options ::= { mode: "par" | "seq",
    //                 expect: "fatal" | "disqualified" | "bail" | "success" }
    struct AssertOptions {
        ExecutionMode *mode;
        ExecutionStatus expect;
        bool init(JSContext *cx, HandleValue v);
        bool check(JSContext *cx, ExecutionStatus actual);
    };

    static const char *ExecutionStatusToString(ExecutionStatus ss);

    static bool IsSpewActive(SpewChannel channel);
    static void Spew(JSContext *cx, SpewChannel channel, const char *fmt, ...);
    static void SpewWarmup(JSContext *cx, const char *op, uint32_t limit);
    static void SpewExecution(JSContext *cx, const char *op, const ExecutionMode &mode,
                              ExecutionStatus status);
#else
    static bool IsSpewActive(SpewChannel channel) { return false; }
    static void Spew(JSContext *cx, SpewChannel channel, const char *fmt, ...) {}
    static void SpewWarmup(JSContext *cx, const char *op, uint32_t limit) {}
    static void SpewExecution(JSContext *cx, const char *op, const ExecutionMode &mode,
                              ExecutionStatus status) {}
#endif

    static JSFunctionSpec methods[];
    static Class protoClass;

    static inline bool DenseArrayToIndexVector(JSContext *cx, HandleObject obj,
                                               IndexVector &indices);

    // Return the PACKED_PREFIX_LENGTH slot without asserts. This is used for
    // while the object is in an interim state, that is, before finish is
    // called and it escapes to script.
    inline uint32_t packedDimensionsUnsafe();

    // Get the type of an interior view on a packed multidimensional
    // array. The d argument specifies the d-th interior dimension, and must
    // be <= the number of packed dimensions. For example, the 0th interior
    // dimension is the outer ParallelArray object itself.
    types::TypeObject *maybeGetRowType(JSContext *cx, uint32_t d);

    // Get a leaf value according to a scalar index. Rewraps ParallelArray
    // leaves as necessary.
    inline bool getLeaf(JSContext *cx, uint32_t index, MutableHandleValue vp);

    // Create a ParallelArray object. Note that finish must be called before
    // the instance escapes to script.
    static ParallelArrayObject *create(JSContext *cx, uint32_t length, uint32_t npacked = 1);
    static ParallelArrayObject *create(JSContext *cx, HandleObject buffer, uint32_t offset,
                                       uint32_t npacked, bool newType = true);

    // Finish creation: determine logical dimensions and set the dimensions
    // and packed prefix slots. This needs to be called before the object
    // escapes to script.
    //
    // If not given an IndexInfo, a temporary 1-dimensional IndexInfo base on
    // the current length of the buffer is used.
    static bool finish(JSContext *cx, HandleParallelArrayObject pa, MutableHandleValue vp);
    static bool finish(JSContext *cx, HandleParallelArrayObject pa, IndexInfo &iv,
                       MutableHandleValue vp);
    static bool setDimensionsSlot(JSContext *cx, HandleParallelArrayObject pa,
                                  const IndexVector &dims);

    // Ensure that the TypeConstruction information is correct: if we have a
    // construct and it is uninitialized, initialize it with our
    // dimensions. If we already have a construct, ensure that it has the same
    // dimensions, and if not, clear it.
    bool initializeOrClearTypeConstruction(JSContext *cx, const IndexVector &dims);

    // Clone an object, sharing the same backing store and shape array.
    ParallelArrayObject *clone(JSContext *cx);

    static JSBool construct(JSContext *cx, unsigned argc, Value *vp);

    static bool map(JSContext *cx, CallArgs args);
    static bool reduce(JSContext *cx, CallArgs args);
    static bool scan(JSContext *cx, CallArgs args);
    static bool scatter(JSContext *cx, CallArgs args);
    static bool filter(JSContext *cx, CallArgs args);
    static bool flatten(JSContext *cx, CallArgs args);
    static bool partition(JSContext *cx, CallArgs args);
    static bool get(JSContext *cx, CallArgs args);
    static bool dimensionsGetter(JSContext *cx, CallArgs args);
    static bool lengthGetter(JSContext *cx, CallArgs args);
    static bool toString(JSContext *cx, CallArgs args);
    static bool toLocaleString(JSContext *cx, CallArgs args);
    static bool toSource(JSContext *cx, CallArgs args);

    static void mark(JSTracer *trc, RawObject obj);
    static JSBool lookupGeneric(JSContext *cx, HandleObject obj, HandleId id,
                                MutableHandleObject objp, MutableHandleShape propp);
    static JSBool lookupProperty(JSContext *cx, HandleObject obj, HandlePropertyName name,
                                 MutableHandleObject objp, MutableHandleShape propp);
    static JSBool lookupElement(JSContext *cx, HandleObject obj, uint32_t index,
                                MutableHandleObject objp, MutableHandleShape propp);
    static JSBool lookupSpecial(JSContext *cx, HandleObject obj, HandleSpecialId sid,
                                MutableHandleObject objp, MutableHandleShape propp);
    static JSBool defineGeneric(JSContext *cx, HandleObject obj, HandleId id, HandleValue value,
                                JSPropertyOp getter, StrictPropertyOp setter, unsigned attrs);
    static JSBool defineProperty(JSContext *cx, HandleObject obj,
                                 HandlePropertyName name, HandleValue value,
                                 JSPropertyOp getter, StrictPropertyOp setter, unsigned attrs);
    static JSBool defineElement(JSContext *cx, HandleObject obj,
                                uint32_t index, HandleValue value,
                                PropertyOp getter, StrictPropertyOp setter, unsigned attrs);
    static JSBool defineSpecial(JSContext *cx, HandleObject obj,
                                HandleSpecialId sid, HandleValue value,
                                PropertyOp getter, StrictPropertyOp setter, unsigned attrs);
    static JSBool getGeneric(JSContext *cx, HandleObject obj, HandleObject receiver,
                             HandleId id, MutableHandleValue vp);
    static JSBool getProperty(JSContext *cx, HandleObject obj, HandleObject receiver,
                              HandlePropertyName name, MutableHandleValue vp);
    static JSBool getElement(JSContext *cx, HandleObject obj, HandleObject receiver,
                             uint32_t index, MutableHandleValue vp);
    static JSBool getElementIfPresent(JSContext *cx, HandleObject obj, HandleObject receiver,
                                      uint32_t index, MutableHandleValue vp, bool *present);
    static JSBool getSpecial(JSContext *cx, HandleObject obj, HandleObject receiver,
                             HandleSpecialId sid, MutableHandleValue vp);
    static JSBool setGeneric(JSContext *cx, HandleObject obj, HandleId id,
                             MutableHandleValue vp, JSBool strict);
    static JSBool setProperty(JSContext *cx, HandleObject obj, HandlePropertyName name,
                              MutableHandleValue vp, JSBool strict);
    static JSBool setElement(JSContext *cx, HandleObject obj, uint32_t index,
                             MutableHandleValue vp, JSBool strict);
    static JSBool setSpecial(JSContext *cx, HandleObject obj, HandleSpecialId sid,
                             MutableHandleValue vp, JSBool strict);
    static JSBool getGenericAttributes(JSContext *cx, HandleObject obj, HandleId id,
                                       unsigned *attrsp);
    static JSBool getPropertyAttributes(JSContext *cx, HandleObject obj, HandlePropertyName name,
                                        unsigned *attrsp);
    static JSBool getElementAttributes(JSContext *cx, HandleObject obj, uint32_t index,
                                       unsigned *attrsp);
    static JSBool getSpecialAttributes(JSContext *cx, HandleObject obj, HandleSpecialId sid,
                                       unsigned *attrsp);
    static JSBool setGenericAttributes(JSContext *cx, HandleObject obj, HandleId id,
                                       unsigned *attrsp);
    static JSBool setPropertyAttributes(JSContext *cx, HandleObject obj, HandlePropertyName name,
                                        unsigned *attrsp);
    static JSBool setElementAttributes(JSContext *cx, HandleObject obj, uint32_t index,
                                       unsigned *attrsp);
    static JSBool setSpecialAttributes(JSContext *cx, HandleObject obj, HandleSpecialId sid,
                                       unsigned *attrsp);
    static JSBool deleteGeneric(JSContext *cx, HandleObject obj, HandleId id,
                                MutableHandleValue rval, JSBool strict);
    static JSBool deleteProperty(JSContext *cx, HandleObject obj, HandlePropertyName name,
                                 MutableHandleValue rval, JSBool strict);
    static JSBool deleteElement(JSContext *cx, HandleObject obj, uint32_t index,
                                MutableHandleValue rval, JSBool strict);
    static JSBool deleteSpecial(JSContext *cx, HandleObject obj, HandleSpecialId sid,
                                MutableHandleValue rval, JSBool strict);

    //////////////////////////////////////////////////////////////////////////
    // Parallel execution
    //////////////////////////////////////////////////////////////////////////
    //
    // The ParallelArrayTaskSet embodies the main logic for running
    // any parallel operation.  It is parameterized by a types
    // BodyDefn.  BodyDefn corresponds to the state for a particular
    // operation that is shared between all threads.  The BodyDefn
    // instance is created by the method (e.g., ParallelMode::map())
    // for the operation.  Each BodyDefn must define an associated
    // type BodyDefn::Instance that corresponds the per-thread state.
    // It will be instantiated and initialized once per worker.  The
    // final template parameter, MaxArgc, indicates how large of an
    // argc vector we should statically allocate.  The operation does
    // not have to use the entire thing; the BodyDefn returns a value
    // (argc) for the actual number of arguments (sometimes it varies
    // depending on the number of dimensions and so forth).  The
    // ParallelArrayTaskSet will check that argc is less than MaxArgc
    // and abort otherwise.

    static ExecutionStatus ToExecutionStatus(JSContext *cx,
                                             const char *opName,
                                             ParallelResult pr);

    template<typename BodyDefn, uint32_t MaxArgc>
    class ParallelArrayOp : public ForkJoinOp {
    private:
        JSContext *cx_;
        BodyDefn &bodyDefn_;
        HandleObject elementalFun_;
        HandleParallelArrayObject result_;

    public:
        ParallelArrayOp(JSContext *cx,
                       BodyDefn &bodyDefn,
                       HandleObject elementalFun,
                       HandleParallelArrayObject result)
            : cx_(cx)
            , bodyDefn_(bodyDefn)
            , elementalFun_(elementalFun)
            , result_(result)
        {}

        ~ParallelArrayOp();

        ExecutionStatus apply();

        bool compileForParallelExecution();

        virtual bool pre(size_t numSlices);
        virtual bool parallel(ForkJoinSlice &slice);
        virtual bool post(size_t numSlices);
    };

    struct ExecuteArgs;

    // A base class for |BodyDefns| that will apply to each member of the result
    // vector.  The associated |Op| type should be |ApplyToEach<BodyDefn,
    // PerElemOp>|.  Here |BodyDefn| is a subtype of |ApplyToEachBodyDefn| and
    // |PerElemOp| defines the code to apply per-element. An example would be
    // |MapBodyDefn| and |MapOp|.
    //
    // The type supplied |PerElemOp| must |init()| (which is called once) and
    // |initializeArgv()| (which is called per element).
    class ApplyToEachBodyDefn {
    public:
        JSContext *cx;
        HandleParallelArrayObject result;

        // the type set of the buffer
        types::TypeSet *typeSet;

        ApplyToEachBodyDefn(JSContext *cx,
                          HandleParallelArrayObject result)
            : cx(cx), result(result)
        {}

        bool pre(size_t numSlices, RootedTypeObject &resultType);
    };

    template<typename BodyDefn>
    class ApplyToEachBodyInstance {
    protected:
        BodyDefn &bodyDefn_;

    public:
        ApplyToEachBodyInstance(BodyDefn &bodyDefn);
        bool execute(ExecuteArgs &args);
    };

    class MapBodyInstance;
    class MapBodyDefn : public ApplyToEachBodyDefn {
    public:
        typedef MapBodyInstance Instance;

        HandleParallelArrayObject source;
        HandleObject elementalFun;

        MapBodyDefn(JSContext *cx,
                  HandleParallelArrayObject source,
                  HandleParallelArrayObject result,
                  HandleObject elementalFun)
            : ApplyToEachBodyDefn(cx, result),
              source(source),
              elementalFun(elementalFun)
        {}

        unsigned length() {
            // Number of elements to process.
            return source->outermostDimension();
        }

        uint32_t argc() {
            // Number of arguments, *including* this.
            return 4;
        }

        const char *toString() {
            // For debugging.
            return "map";
        }

        bool doWarmup();
        bool pre(size_t numSlices);
    };

    class MapBodyInstance : public ApplyToEachBodyInstance<MapBodyDefn> {
    private:
        HandleParallelArrayObject source;
        RootedValue elem;

    public:
        MapBodyInstance(MapBodyDefn &bodyDefn, size_t sliceId, size_t numSlices);
        bool init();
        bool initializeArgv(Value *argv, unsigned i);
    };

    class BuildBodyInstance;
    class BuildBodyDefn : public ApplyToEachBodyDefn {
    public:
        typedef BuildBodyInstance Instance;

        IndexInfo &iv;
        HandleObject elementalFun;

        BuildBodyDefn(JSContext *cx,
                    HandleParallelArrayObject result,
                    IndexInfo &iv,
                    HandleObject elementalFun)
            : ApplyToEachBodyDefn(cx, result),
              iv(iv),
              elementalFun(elementalFun)
        {}

        unsigned length() {
            // See MapBodyDefn::length()
            return iv.scalarLengthOfPackedDimensions();
        }

        uint32_t argc() {
            // See MapBodyDefn::argc()
            return iv.packedDimensions() + 1;
        }

        const char *toString() {
            // See MapBodyDefn::toString()
            return "build";
        }

        bool doWarmup();
        bool pre(size_t numSlices);
    };

    class BuildBodyInstance : public ApplyToEachBodyInstance<BuildBodyDefn> {
    private:
        IndexInfo iv;

    public:
        BuildBodyInstance(BuildBodyDefn &bodyDefn, size_t sliceId,
                          size_t numSlices);
        bool init();
        bool initializeArgv(Value *argv, unsigned i);
    };

    class ReduceBodyInstance;
    class ReduceBodyDefn {
    public:
        typedef ReduceBodyInstance Instance;

        JSContext *cx;
        HandleParallelArrayObject source;
        HandleObject elementalFun;
        AutoValueVector results;

        ReduceBodyDefn(JSContext *cx,
                     HandleParallelArrayObject source,
                     HandleObject elementalFun)
            : cx(cx),
              source(source),
              elementalFun(elementalFun),
              results(cx)
        {}

        unsigned length() {
            // Number of elements to process.
            return source->outermostDimension();
        }

        uint32_t argc() {
            // Number of arguments, *including* this.
            return 3;
        }

        const char *toString() {
            // For debugging.
            return "reduce";
        }

        bool doWarmup();
        bool pre(size_t numSlices);
        ExecutionStatus post(MutableHandleValue vp);
    };

    class ReduceBodyInstance {
    private:
        ReduceBodyDefn &bodyDefn_;
    public:
        ReduceBodyInstance(ReduceBodyDefn &bodyDefn, size_t sliceId,
                           size_t numSlices);
        bool init();
        bool execute(ExecuteArgs &args);
    };

    typedef Vector<uint32_t, 32> CountVector;

    class FilterCountOp : public ForkJoinOp {
    private:
        JSContext *cx_;
        HandleParallelArrayObject source_;
        HandleObject filter_; // always a dense array of len >= source
        uint32_t filterBase_;
        CountVector counts_;

    public:
        FilterCountOp(JSContext *cx,
                           HandleParallelArrayObject source,
                           HandleObject filter,
                           uint32_t filterBase)
            : cx_(cx)
            , source_(source)
            , filter_(filter)
            , filterBase_(filterBase)
            , counts_(cx)
        {}

        ~FilterCountOp() {}

        const CountVector &counts() { return counts_; }

        virtual bool pre(size_t numSlices);
        virtual bool parallel(ForkJoinSlice &slice);
        virtual bool post(size_t numSlices);
    };

    class FilterCopyOp : public ForkJoinOp {
    private:
        JSContext *cx_;
        HandleParallelArrayObject source_;
        HandleObject filter_; // always a dense array of len >= source
        uint32_t filterBase_;
        const CountVector &counts_;
        HandleObject resultBuffer_;

    public:
        FilterCopyOp(JSContext *cx,
                          HandleParallelArrayObject source,
                          HandleObject filter,
                          uint32_t filterBase,
                          const CountVector &counts,
                          HandleObject resultBuffer)
            : cx_(cx)
            , source_(source)
            , filter_(filter)
            , filterBase_(filterBase)
            , counts_(counts)
            , resultBuffer_(resultBuffer)
        {}

        ~FilterCopyOp() {}

        virtual bool pre(size_t numSlices);
        virtual bool parallel(ForkJoinSlice &slice);
        virtual bool post(size_t numSlices);
    };
};

} // namespace js

extern JSObject *
js_InitParallelArrayClass(JSContext *cx, js::HandleObject obj);

#endif // ParallelArray_h__
