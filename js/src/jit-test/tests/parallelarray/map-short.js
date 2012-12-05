function test() {
  // Test what happens if the length of the array is very short (i.e.,
  // less than the number of cores).  There used to be a bug in this
  // case that led to crashes or other undefined behavior.
  var a = [1,2];
  var p = new ParallelArray(a);
  var makeadd1 = function (v) { return [v]; }
  p.map(makeadd1, {mode: "par", expect: "disqualified"});
}

test();
