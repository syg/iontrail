/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 4 -*-
 * vim: set ts=4 sw=4 et tw=79:
 *
 * ***** BEGIN LICENSE BLOCK *****
 * Version: MPL 1.1/GPL 2.0/LGPL 2.1
 *
 * The contents of this file are subject to the Mozilla Public License Version
 * 1.1 (the "License"); you may not use this file except in compliance with
 * the License. You may obtain a copy of the License at
 * http://www.mozilla.org/MPL/
 *
 * Software distributed under the License is distributed on an "AS IS" basis,
 * WITHOUT WARRANTY OF ANY KIND, either express or implied. See the License
 * for the specific language governing rights and limitations under the
 * License.
 *
 * The Original Code is Mozilla Communicator client code, released
 * March 31, 1998.
 *
 * The Initial Developer of the Original Code is
 * Netscape Communications Corporation.
 * Portions created by the Initial Developer are Copyright (C) 1998
 * the Initial Developer. All Rights Reserved.
 *
 * Contributor(s):
 *   Niko Matsakis <nmatsakis@mozilla.com>
 *   Shu-yu Guo <shu@mozilla.com>
 *
 * Alternatively, the contents of this file may be used under the terms of
 * either of the GNU General Public License Version 2 or later (the "GPL"),
 * or the GNU Lesser General Public License Version 2.1 or later (the "LGPL"),
 * in which case the provisions of the GPL or the LGPL are applicable instead
 * of those above. If you wish to allow use of your version of this file only
 * under the terms of either the GPL or the LGPL, and not to allow others to
 * use your version of this file under the terms of the MPL, indicate your
 * decision by deleting the provisions above and replace them with the notice
 * and other provisions required by the GPL or the LGPL. If you do not delete
 * the provisions above, a recipient may use your version of this file under
 * the terms of any one of the MPL, the GPL or the LGPL.
 *
 * ***** END LICENSE BLOCK ***** */

#ifndef jsion_parallel_array_analysis_h__
#define jsion_parallel_array_analysis_h__

#include "MIR.h"
#include "CompileInfo.h"

namespace js {

class StackFrame;

namespace ion {

class MIRGraph;
class AutoDestroyAllocator;

class ParallelCompileContext
{
  private:
    JSContext *cx_;

    // Compilation is transitive from some set of root(s).
    AutoObjectVector worklist_;

    // Is a function compilable for parallel execution?
    bool analyzeAndGrowWorklist(MIRGenerator *mir, MIRGraph &graph);

    bool removeResumePointOperands(MIRGenerator *mir, MIRGraph &graph);
    void replaceOperandsOnResumePoint(MResumePoint *resumePoint, MDefinition *withDef);

  public:
    ParallelCompileContext(JSContext *cx)
      : cx_(cx),
        worklist_(cx)
    { }

    // Should we append a function to the worklist?
    bool appendToWorklist(HandleFunction fun);

    ExecutionMode executionMode() {
        return ParallelExecution;
    }

    // Defined in Ion.cpp, so that they can make use of static fns defined there
    MethodStatus compileTransitively();
    bool compile(IonBuilder *builder, MIRGraph *graph, AutoDestroyAllocator &autoDestroy);
};


} // namespace ion
} // namespace js

#endif // jsion_parallel_array_analysis_h
