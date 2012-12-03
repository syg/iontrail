load(libdir + "parallelarray-helpers.js");

function mergeSorted(l1, l2) {
  var result = [];
  var i1 = 0, i2 = 0, j = 0;
  while (i1 < l1.length && i2 < l2.length) {
    if (l1[i1] < l2[i2])
      result[j++] = l1[i1++];
    else
      result[j++] = l2[i2++];
  }
  while (i1 < l1.length) {
    result[j++] = l1[i1++];
  }
  while (i2 < l2.length) {
    result[j++] = l2[i2++];
  }
  return result;
}

function test() {
  var elts = [];
  var ints = range(1, 5), c = 0;
  for (var i = 0; i < 64; i++) {
    entry = [];
    for (var j = 0; j < 3; j++) {
      entry[j] = ints[c++ % ints.length];
    }
    elts[i] = entry;
  }
  
  var scanned1 = seq_scan(elts, mergeSorted);
  var scanned2 = new ParallelArray(elts).scan(mergeSorted);
  assertStructuralEq(scanned1, scanned2);
}

test();