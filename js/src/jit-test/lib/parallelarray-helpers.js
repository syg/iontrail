/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

load(libdir + "eqArrayHelper.js");

function assertEqParallelArray(a, b) {
  assertEq(a instanceof ParallelArray, true);
  assertEq(b instanceof ParallelArray, true);

  var shape = a.shape;
  assertEqArray(shape, b.shape);

  function bump(indices) {
    var d = indices.length - 1;
    while (d >= 0) {
      if (++indices[d] < shape[d])
        break;
      indices[d] = 0;
      d--;
    }
    return d >= 0;
  }

  var iv = shape.map(function () { return 0; });
  do {
    var e1 = a.get(iv);
    var e2 = b.get(iv);
    if (e1 instanceof ParallelArray && e2 instanceof ParallelArray)
      assertEqParallelArray(e1, e2);
    else if (e1 instanceof Array && e2 instanceof Array)
      assertEqArray(e1, e2);
    else
      assertEq(e1, e2);
  } while (bump(iv));
}

function assertParallelArrayModesEq(modes, acc, opFunction) {
    modes.forEach(function (mode) {
        var result = opFunction({ mode: mode, expect: "success" });
        if (acc instanceof ParallelArray)
            assertEqParallelArray(acc, result);
        else
            assertEq(acc, result);
    });
}

function assertParallelArrayModesCommute(modes, opFunction) {
    var acc = opFunction({ mode: modes[0], expect: "success" });
    assertParallelArrayModesEq(modes.slice(1), acc, opFunction);
}

function comparePerformance(opts) {
    var measurements = [];
    for (var i = 0; i < opts.length; i++) {
        var start = new Date();
        opts[i].func();
        var end = new Date();
        var diff = (end.getTime() - start.getTime());
        measurements.push(diff);
        print("Option " + opts[i].name + " took " + diff + "ms");
    }

    for (var i = 1; i < opts.length; i++) {
        var rel = (measurements[i] - measurements[0]) * 100 / measurements[0];
        print("Option " + opts[i].name + " relative to option " +
              opts[0].name + ": " + (rel|0) + "%");
    }
}