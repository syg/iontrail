function testClosureCreationAndInvocation() {
  var a = [1,2,3,4,5];
  var p = new ParallelArray(a);
  function etaadd1(v) { return (function (x) { return x+1; })(v); };
  // eta-expansion is (or at least can be) treated as call with unknown target
  var m = p.map(etaadd1, {mode: "par", expect: "disqualified"});
  assertEq(m.get(1), 3); // (\x.x+1) 2 == 3
}

testClosureCreationAndInvocation();
