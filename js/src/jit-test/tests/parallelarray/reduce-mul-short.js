load(libdir + "parallelarray-helpers.js");

function testReduce() {
  // This test is interesting because during warmup v*p remains an
  // integer but this ceases to be true once real execution proceeds.
  // By the end, it will just be some double value.

  function mul(v, p) { return v*p; }

  // Ensure that the array only contains values between 1 and 4.
  var array = range(1, 513).map(function(v) { return (v % 4) + 1; });
  var expected = array.reduce(mul);
  print(expected);

  // The expected pattern is the following:
  //
  // Success #1, Bailout #1, Bailout #2, Fixpoint Success
  //
  // Success #1 is because the overflow doesn't bail out of Ion. I haven't
  // investigated further for why it doesn't.
  //
  // At this point the user function f() gets invalidated due to the following
  // line in ParallelArrayReduce:
  //
  //     a = f(a, subreductions[i]);
  //
  // The 2nd argument is frozen (due to f() having been compiled) to be an
  // int, because in the reduce() helper we have:
  //
  //     a = f(a, self.get(i));
  //
  // Here, the 2nd argument is always an int because the accumulator is the
  // one getting the float.
  //
  // So after we return from the Success #1, type propagation trips
  // recompilation of f().
  //
  // Bailout #1 happens because the fill function is still compiled and has a
  // valid parallelIonScript, so no warmup is attempted. We bail when we hit
  // the CallGeneric to f, which was invalidated after Success #1.
  //
  // At this point the *caller* of f(), reduce(), gets invalidated because it
  // was the innermost script that aborted.
  //
  // Bailout #2 happens because now that reduce() got invalidated and has no
  // parallelIonScript, the fill() function bails on CallGeneric to it.
  //
  // Finally, fill() gets invalidated.
  //
  // Fixpoint Success is because we re-warmup and recompile with the
  // up-to-date type info.
  var parray = new ParallelArray(array);
  var results = ["success", "bailout", "bailout", "success", "success"];
  var actual;
  for (var i = 0; i < results.length; i++)
    actual = parray.reduce(mul, {mode: "par", expect: results[i]});

  assertAlmostEq(actual, expected);
}

testReduce();
