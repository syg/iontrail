function testElement() {
  var p = new ParallelArray([9]);
  var desc = Object.getOwnPropertyDescriptor(p, "0");
  assertEq(desc.enumerable, true);
  assertEq(desc.configurable, false);
  assertEq(desc.writable, false);
  assertEq(desc.value, 9);
}

// FIXME---getOwnPropertyDescriptor on an element
//         doesn't work right now with self-hosted impl
// testElement();
