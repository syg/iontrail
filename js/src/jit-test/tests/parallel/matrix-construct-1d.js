load(libdir + "parallelarray-helpers.js");

function constructAny() {
  function kernel(i) { return i+1; };
  var m = new Matrix([256], kernel);
  var a = Array.build(256, kernel);
  assertEqMatrixArray(m, a);

  assertParallelModesCommute(["seq", "par"], function(m) {
    return new Matrix([256], kernel, m);
  });

  assertParallelModesCommute(["seq", "par"], function(m) {
    return new Matrix([256], ["any"], kernel, m);
  });
}

function constructAnyOut() {
  function kernel(i, out) { out.set(i+1); };
  var m = new Matrix([256], kernel);
  var a = Array.build(256, function (i) { return i+1; });
  assertEqMatrixArray(m, a);

  assertParallelModesCommute(["seq", "par"], function(m) {
    return new Matrix([256], kernel, m);
  });

  assertParallelModesCommute(["seq", "par"], function(m) {
    return new Matrix([256], ["any"], kernel, m);
  });
}


if (getBuildConfiguration().parallelJS) {
  constructAny();
/*
  constructInt8();
  constructUint8();
  constructUint8clamped();
  constructInt16();
  constructUint16();
  constructInt32();
  constructUint32();
  constructFloat32();
  constructFloat64();
*/

  constructAnyOut();
/*
  constructInt8Out();
  constructUint8Out();
  constructUint8clampedOut();
  constructInt16Out();
  constructUint16Out();
  constructInt32Out();
  constructUint32Out();
  constructFloat32Out();
  constructFloat64Out();
*/
}
