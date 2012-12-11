// Test for one annoying case of the EliminateUnreachableCode
// optimization.  Here the dominator of print("Goodbye") changes to be
// the print("Hello") after optimization.

function test1(v) {
  if (v) {
    if (v) {
      print("hi");
    } else {
      print("ho");
    }
  } else {
    if (v) {
      print("hum");
    } else {
      print("hee");
    }
  }
  print("goodbye");
}

function test() {
  test1(true);
  test1(false);
}

for (var i = 0; i < 100000; i++)
  test();
