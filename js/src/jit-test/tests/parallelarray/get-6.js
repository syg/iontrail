function testGet() {
  // Test getting higher dimension inferred shape
  var p0 = new ParallelArray([0,1]);
  var p1 = new ParallelArray([2,3]);
  var p = new ParallelArray([p0, p1]);
  assertEq(p.get(0,0), 0);
  assertEq(p.get(0,1), 1);
  assertEq(p.get(1,0), 2);
  assertEq(p.get(1,1), 3);
}

// FIXME---logical shape does not work in self-hosted code yet
// testGet();
