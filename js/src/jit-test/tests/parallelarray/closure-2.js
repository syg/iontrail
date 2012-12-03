function testClosureCreation() {
  var a = [1,2,3,4,5];
  var p = new ParallelArray(a);
  var makeadd1 = function (v) { return function (x) { return x+1; }; };
  var m = p.map(makeadd1, {mode: "par", expect: "success"});
  assertEq(m.get(1)(2), 3); // (\x.x+1) 2 == 3
}

testClosureCreation();
