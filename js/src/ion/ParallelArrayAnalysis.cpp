#include <stdio.h>

#include "Ion.h"
#include "MIR.h"
#include "MIRGraph.h"
#include "ParallelArrayAnalysis.h"
#include "IonSpewer.h"

#include "builtin/ParallelArray.h"

#include "vm/Stack.h"

namespace js {
namespace ion {

using parallel::Spew;
using parallel::SpewMIR;
using parallel::SpewCompile;

typedef uint32_t typeset_t;

static inline typeset_t typeset(MIRType type) {
    return 1 << (uint32_t) type;
}

static inline bool isSubset(typeset_t small, typeset_t big) {
    return ((big & small) == small);
}

static inline typeset_t containsType(typeset_t set, MIRType type) {
    return (set & typeset(type)) != 0;
}

#define SAFE_OP(op)                             \
    virtual bool visit##op(M##op *prop) { return true; }

#define CUSTOM_OP(op)                        \
    virtual bool visit##op(M##op *prop);

#define DROP_OP(op)                             \
    virtual bool visit##op(M##op *ins) {        \
        MBasicBlock *block = ins->block();      \
        block->discard(ins);                    \
        return true;                            \
    }

#define PERMIT(T) (1 << T)

#define PERMIT_NUMERIC (PERMIT(MIRType_Int32) | PERMIT(MIRType_Double))

#define SPECIALIZED_OP(op, flags)                                               \
    virtual bool visit##op(M##op *ins) {                                        \
        return visitSpecializedInstruction(ins, ins->specialization(), flags);  \
    }

#define UNSAFE_OP(op)                                               \
    virtual bool visit##op(M##op *ins) {                            \
        SpewMIR(ins, "Unsafe");                                     \
        return false;                                               \
    }

#define WRITE_GUARDED_OP(op, obj)                   \
    virtual bool visit##op(M##op *prop) {           \
        return insertWriteGuard(prop, prop->obj()); \
    }

#define MAYBE_WRITE_GUARDED_OP(op, obj)                                       \
    virtual bool visit##op(M##op *prop) {                                     \
        if (prop->racy())                                                     \
            return true;                                                      \
        return insertWriteGuard(prop, prop->obj());                           \
    }

class ParallelArrayVisitor : public MInstructionVisitor
{
    JSContext *cx_;
    ParallelCompileContext &compileContext_;
    MIRGraph &graph_;

    bool insertWriteGuard(MInstruction *writeInstruction,
                          MDefinition *valueBeingWritten);

    bool replaceWithParNew(MInstruction *newInstruction,
                           JSObject *templateObject);

    bool replace(MInstruction *oldInstruction,
                 MInstruction *replacementInstruction);

    bool visitSpecializedInstruction(MInstruction *ins, MIRType spec, uint32_t flags);

  public:
    AutoObjectVector callTargets;

    ParallelArrayVisitor(JSContext *cx, ParallelCompileContext &compileContext,
                         MIRGraph &graph)
      : cx_(cx),
        compileContext_(compileContext),
        graph_(graph),
        callTargets(cx)
    { }

    MDefinition *parSlice() { return graph_.parSlice(); }

    // I am taking the policy of blacklisting everything that's not
    // obviously safe for now.  We can loosen as we need.

    SAFE_OP(Constant)
    SAFE_OP(Parameter)
    SAFE_OP(Callee)
    SAFE_OP(TableSwitch)
    SAFE_OP(Goto)
    CUSTOM_OP(Test)
    SPECIALIZED_OP(Compare, PERMIT_NUMERIC | PERMIT(MIRType_String))
    SAFE_OP(Phi)
    SAFE_OP(Beta)
    UNSAFE_OP(OsrValue)
    UNSAFE_OP(OsrScopeChain)
    UNSAFE_OP(ReturnFromCtor)
    CUSTOM_OP(CheckOverRecursed)
    DROP_OP(RecompileCheck)
    UNSAFE_OP(DefVar)
    UNSAFE_OP(CreateThis)
    SAFE_OP(PrepareCall)
    SAFE_OP(PassArg)
    CUSTOM_OP(Call)
    UNSAFE_OP(ApplyArgs)
    SAFE_OP(BitNot)
    UNSAFE_OP(TypeOf)
    SAFE_OP(ToId)
    SAFE_OP(BitAnd)
    SAFE_OP(BitOr)
    SAFE_OP(BitXor)
    SAFE_OP(Lsh)
    SAFE_OP(Rsh)
    SPECIALIZED_OP(Ursh, PERMIT_NUMERIC)
    SPECIALIZED_OP(MinMax, PERMIT_NUMERIC)
    SAFE_OP(Abs)
    SAFE_OP(Sqrt)
    SAFE_OP(MathFunction)
    SPECIALIZED_OP(Add, PERMIT_NUMERIC)
    SPECIALIZED_OP(Sub, PERMIT_NUMERIC)
    SPECIALIZED_OP(Mul, PERMIT_NUMERIC)
    SPECIALIZED_OP(Div, PERMIT_NUMERIC)
    SPECIALIZED_OP(Mod, PERMIT_NUMERIC)
    UNSAFE_OP(Concat)
    UNSAFE_OP(CharCodeAt)
    UNSAFE_OP(FromCharCode)
    SAFE_OP(Return)
    CUSTOM_OP(Throw)
    SAFE_OP(Box)     // Boxing just creates a JSVal, doesn't alloc.
    SAFE_OP(Unbox)
    SAFE_OP(GuardObject)
    SAFE_OP(ToDouble)
    SAFE_OP(ToInt32)
    SAFE_OP(TruncateToInt32)
    UNSAFE_OP(ToString)
    SAFE_OP(NewSlots)
    CUSTOM_OP(NewArray)
    CUSTOM_OP(NewObject)
    CUSTOM_OP(NewCallObject)
    CUSTOM_OP(NewParallelArray)
    UNSAFE_OP(InitProp)
    SAFE_OP(Start)
    UNSAFE_OP(OsrEntry)
    SAFE_OP(Nop)
    UNSAFE_OP(RegExp)
    CUSTOM_OP(Lambda)
    UNSAFE_OP(ImplicitThis)
    SAFE_OP(Slots)
    SAFE_OP(Elements)
    SAFE_OP(ConstantElements)
    SAFE_OP(LoadSlot)
    WRITE_GUARDED_OP(StoreSlot, slots)
    SAFE_OP(FunctionEnvironment) // just a load of func env ptr
    SAFE_OP(TypeBarrier) // causes a bailout if the type is not found: a-ok with us
    SAFE_OP(MonitorTypes) // causes a bailout if the type is not found: a-ok with us
    UNSAFE_OP(GetPropertyCache)
    UNSAFE_OP(GetElementCache)
    UNSAFE_OP(BindNameCache)
    SAFE_OP(GuardShape)
    SAFE_OP(GuardClass)
    SAFE_OP(ArrayLength)
    SAFE_OP(TypedArrayLength)
    SAFE_OP(TypedArrayElements)
    SAFE_OP(InitializedLength)
    WRITE_GUARDED_OP(SetInitializedLength, elements)
    SAFE_OP(Not)
    SAFE_OP(BoundsCheck)
    SAFE_OP(BoundsCheckLower)
    SAFE_OP(LoadElement)
    SAFE_OP(LoadElementHole)
    MAYBE_WRITE_GUARDED_OP(StoreElement, elements)
    WRITE_GUARDED_OP(StoreElementHole, elements)
    UNSAFE_OP(ArrayPopShift)
    UNSAFE_OP(ArrayPush)
    SAFE_OP(LoadTypedArrayElement)
    SAFE_OP(LoadTypedArrayElementHole)
    MAYBE_WRITE_GUARDED_OP(StoreTypedArrayElement, elements)
    UNSAFE_OP(ClampToUint8)
    SAFE_OP(LoadFixedSlot)
    WRITE_GUARDED_OP(StoreFixedSlot, object)
    UNSAFE_OP(CallGetProperty)
    UNSAFE_OP(GetNameCache)
    SAFE_OP(CallGetIntrinsicValue) // Bails in parallel mode
    UNSAFE_OP(CallGetElement)
    UNSAFE_OP(CallSetElement)
    UNSAFE_OP(CallSetProperty)
    UNSAFE_OP(DeleteProperty)
    UNSAFE_OP(SetPropertyCache)
    UNSAFE_OP(IteratorStart)
    UNSAFE_OP(IteratorNext)
    UNSAFE_OP(IteratorMore)
    UNSAFE_OP(IteratorEnd)
    SAFE_OP(StringLength)
    UNSAFE_OP(ArgumentsLength)
    UNSAFE_OP(GetArgument)
    SAFE_OP(Floor)
    SAFE_OP(Round)
    UNSAFE_OP(InstanceOf)
    CUSTOM_OP(InterruptCheck)
    SAFE_OP(ParSlice)
    SAFE_OP(ParNew)
    SAFE_OP(ParNewDenseArray)
    SAFE_OP(ParNewCallObject)
    SAFE_OP(ParLambda)
};

bool
ParallelCompileContext::appendToWorklist(HandleFunction fun)
{
    if (!fun->isInterpreted())
        return true;

    RootedScript script(cx_, fun->nonLazyScript());

    // Skip if we're disabled.
    if (!script->canParallelIonCompile())
        return true;

    // Skip if we're compiling off thread.
    if (script->parallelIon == ION_COMPILING_SCRIPT)
        return true;

    // Skip if the code is expected to result in a bailout.
    if (script->parallelIon && script->parallelIon->bailoutExpected())
        return true;

    // Skip if we haven't warmed up to get some type info. We're betting
    // that the parallel kernel will be non-branchy for the most part, so
    // this threshold is usually very low (1).
    if (script->getUseCount() < js_IonOptions.usesBeforeCompileParallel)
        return true;

    // TODO: Have worklist use an auto hash set or something.
    for (uint32_t i = 0; i < worklist_.length(); i++) {
        if (worklist_[i]->toFunction() == fun)
            return true;
    }

    // Note that we add all possibly compilable functions to the worklist,
    // even if they're already compiled. This is so that we can return
    // Method_Compiled and not Method_Skipped if we have a worklist full of
    // already-compiled functions.
    return worklist_.append(fun);
}

bool
ParallelCompileContext::analyzeAndGrowWorklist(MIRGraph *graph)
{
    // Scan the IR and validate the instructions used in a peephole fashion
    ParallelArrayVisitor visitor(cx_, *this, *graph);
    for (MBasicBlockIterator block(graph->begin()); block != graph->end(); block++) {
        for (MInstructionIterator ins(block->begin()); ins != block->end();) {
            // We may be removing or replacing the current instruction,
            // so advance `ins` now.
            MInstruction *instr = *ins++;

            if (!instr->accept(&visitor))
                return false;
        }
    }

    // Append newly discovered outgoing callgraph edges to the worklist.
    RootedFunction target(cx_);
    for (uint32_t i = 0; i < visitor.callTargets.length(); i++) {
        target = visitor.callTargets[i]->toFunction();
        appendToWorklist(target);
    }

    Spew(SpewCompile, "Safe");

    IonSpewPass("Parallel Array Analysis");

    return true;
}

bool
ParallelArrayVisitor::visitTest(MTest *) {
    return true;
}

/////////////////////////////////////////////////////////////////////////////
// Memory allocation
//
// Simple memory allocation opcodes---those which ultimately compile
// down to a (possibly inlined) invocation of NewGCThing()---are
// replaced with MParNew, which is supplied with the thread context.
// These allocations will take place using per-helper-thread arenas.

bool
ParallelArrayVisitor::visitNewParallelArray(MNewParallelArray *ins) {
    MParNew *parNew = new MParNew(parSlice(), ins->templateObject());
    replace(ins, parNew);
    return true;
}

bool
ParallelArrayVisitor::visitNewCallObject(MNewCallObject *ins) {
    // fast path: replace with ParNewCallObject op
    MParNewCallObject *parNewCallObjectInstruction =
        MParNewCallObject::New(parSlice(), ins);
    replace(ins, parNewCallObjectInstruction);
    return true;
}

bool
ParallelArrayVisitor::visitLambda(MLambda *ins) {
    if (ins->fun()->hasSingletonType() ||
        types::UseNewTypeForClone(ins->fun())) {
        // slow path: bail on parallel execution.
        return false;
    }

    // fast path: replace with ParLambda op
    MParLambda *parLambdaInstruction = MParLambda::New(parSlice(), ins);
    replace(ins, parLambdaInstruction);
    return true;
}

bool
ParallelArrayVisitor::visitNewObject(MNewObject *newInstruction) {
    if (newInstruction->shouldUseVM()) {
        SpewMIR(newInstruction, "should use VM");
        return false;
    }

    return replaceWithParNew(newInstruction,
                             newInstruction->templateObject());
}

bool
ParallelArrayVisitor::visitNewArray(MNewArray *newInstruction) {
    if (newInstruction->shouldUseVM()) {
        SpewMIR(newInstruction, "should use VM");
        return false;
    }

    return replaceWithParNew(newInstruction,
                             newInstruction->templateObject());
}

bool
ParallelArrayVisitor::replaceWithParNew(MInstruction *newInstruction,
                                        JSObject *templateObject) {
    MParNew *parNewInstruction = new MParNew(parSlice(), templateObject);
    replace(newInstruction, parNewInstruction);
    return true;
}

bool
ParallelArrayVisitor::replace(MInstruction *oldInstruction,
                              MInstruction *replacementInstruction)
{
    MBasicBlock *block = oldInstruction->block();
    block->insertBefore(oldInstruction, replacementInstruction);
    block->discard(oldInstruction);
    oldInstruction->replaceAllUsesWith(replacementInstruction);
    return true;
}

/////////////////////////////////////////////////////////////////////////////
// Write Guards
//
// We only want to permit writes to locally guarded objects.
// Furthermore, we want to avoid PICs and other non-thread-safe things
// (though perhaps we should support PICs at some point).  If we
// cannot determine the origin of an object, we can insert a write
// guard which will check whether the object was allocated from the
// per-thread-arena or not.

bool
ParallelArrayVisitor::insertWriteGuard(MInstruction *writeInstruction,
                                       MDefinition *valueBeingWritten)
{
    // Many of the write operations do not take the JS object
    // but rather something derived from it, such as the elements.
    // So we need to identify the JS object:
    MDefinition *object;
    switch (valueBeingWritten->type()) {
      case MIRType_Object:
        object = valueBeingWritten;
        break;

      case MIRType_Slots:
        switch (valueBeingWritten->op()) {
          case MDefinition::Op_Slots:
            object = valueBeingWritten->toSlots()->object();
            break;

          case MDefinition::Op_NewSlots:
            // Values produced by new slots will ALWAYS be
            // thread-local.
            return true;

          default:
            SpewMIR(writeInstruction, "cannot insert write guard for %s",
                    valueBeingWritten->opName());
            return false;
        }
        break;

      case MIRType_Elements:
        switch (valueBeingWritten->op()) {
          case MDefinition::Op_Elements:
            object = valueBeingWritten->toElements()->object();
            break;

          case MDefinition::Op_TypedArrayElements:
            object = valueBeingWritten->toTypedArrayElements()->object();
            break;

          default:
            SpewMIR(writeInstruction, "cannot insert write guard for %s",
                    valueBeingWritten->opName());
            return false;
        }
        break;

      default:
        SpewMIR(writeInstruction, "cannot insert write guard for MIR Type %d",
                valueBeingWritten->type());
        return false;
    }

    if (object->isUnbox())
        object = object->toUnbox()->input();

    switch (object->op()) {
      case MDefinition::Op_ParNew:
        // MParNew will always be creating something thread-local, omit the guard
        SpewMIR(writeInstruction, "write to ParNew prop does not require guard");
        return true;
      default:
        break;
    }

    MBasicBlock *block = writeInstruction->block();
    MParWriteGuard *writeGuard = MParWriteGuard::New(parSlice(), object);
    block->insertBefore(writeInstruction, writeGuard);
    writeGuard->adjustInputs(writeGuard);
    return true;
}

/////////////////////////////////////////////////////////////////////////////
// Calls
//
// We only support calls to interpreted functions that that have already been
// Ion compiled. If a function has no IonScript, we bail out. The compilation
// is done during warmup of the parallel kernel, see js::RunScript.

static bool
GetPossibleCallees(JSContext *cx, HandleScript script, jsbytecode *pc,
                   types::StackTypeSet *calleeTypes, AutoObjectVector &targets)
{
    JS_ASSERT(calleeTypes);

    if (calleeTypes->baseFlags() != 0)
        return true;

    unsigned objCount = calleeTypes->getObjectCount();

    if (objCount == 0)
        return true;

    RootedFunction fun(cx);
    for (unsigned i = 0; i < objCount; i++) {
        RawObject obj = calleeTypes->getSingleObject(i);
        if (obj && obj->isFunction()) {
            fun = obj->toFunction();
        } else {
            types::TypeObject *typeObj = calleeTypes->getTypeObject(i);
            if (!typeObj)
                continue;
            fun = typeObj->interpretedFunction;
            if (!fun)
                continue;
        }

        if (fun->isCloneAtCallsite()) {
            fun = CloneFunctionAtCallsite(cx, fun, script, pc);
            if (!fun)
                return false;
        }

        if (!targets.append(fun))
            return false;
    }

    return true;
}

bool
ParallelArrayVisitor::visitCall(MCall *ins)
{
    JS_ASSERT(ins->getSingleTarget() || ins->calleeTypes());

    // DOM? Scary.
    if (ins->isDOMFunction()) {
        SpewMIR(ins, "call to dom function");
        return false;
    }

    RootedFunction target(cx_, ins->getSingleTarget());
    if (target) {
        // Native? Scary.
        if (target->isNative()) {
            SpewMIR(ins, "call to native function");
            return false;
        }
        return callTargets.append(target);
    }

    if (ins->isConstructing()) {
        SpewMIR(ins, "call to unknown constructor");
        return false;
    }

    RootedScript script(cx_, ins->block()->info().script());
    return GetPossibleCallees(cx_, script, ins->pc(), ins->calleeTypes(), callTargets);
}

/////////////////////////////////////////////////////////////////////////////
// Stack limit, interrupts
//
// In sequential Ion code, the stack limit is stored in the JSRuntime.
// We store it in the thread context.  We therefore need a separate
// instruction to access it, one parameterized by the thread context.
// Similar considerations apply to checking for interrupts.

bool
ParallelArrayVisitor::visitCheckOverRecursed(MCheckOverRecursed *ins)
{
    MParCheckOverRecursed *replacement = new MParCheckOverRecursed(parSlice());
    return replace(ins, replacement);
}

bool
ParallelArrayVisitor::visitInterruptCheck(MInterruptCheck *ins)
{
    MParCheckInterrupt *replacement = new MParCheckInterrupt(parSlice());
    return replace(ins, replacement);
}

/////////////////////////////////////////////////////////////////////////////
// Specialized ops
//
// Some ops, like +, can be specialized to ints/doubles.  Anything
// else is terrifying.
//
// TODO---Eventually, we should probably permit arbitrary + but bail
// if the operands are not both integers/floats.

bool
ParallelArrayVisitor::visitSpecializedInstruction(MInstruction *ins, MIRType spec,
                                                  uint32_t flags)
{
    uint32_t flag = 1 << spec;
    if (flags & flag)
        return true;

    SpewMIR(ins, "specialized to unacceptable type %d", spec);
    return false;
}

/////////////////////////////////////////////////////////////////////////////
// Throw

bool
ParallelArrayVisitor::visitThrow(MThrow *thr)
{
    MBasicBlock *block = thr->block();
    JS_ASSERT(block->lastIns() == thr);
    block->discardLastIns();
    MParBailout *bailout = new MParBailout();
    if (!bailout)
        return false;
    block->end(bailout);
    return true;
}

}
}

