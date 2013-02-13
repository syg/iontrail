// -*- mode: js2; indent-tabs-mode: nil; -*-

function fillRGBArrayView(view, width, height, fill) {
  var i = 0;
  for (var y=0; y < height; y++) {
    for (var x=0; x < width; x++) {
      a[i] = fill(x,y);
      i++;
    }
  }
}

function buildRGBArray(width, height, fill) {
  var len = width * height;
  var a = new Array(len);
  fillRGBArrayView(a, width, height, fill);
  a.width = width;
  a.height = height;
  return a;
}

function buildRGBTypedArray(width, height, fill) {
  var len = width * height;
  var buf = new ArrayBuffer(len);
  fillRGBArrayView(new Uint8Array(buf), width, height, fill);
  buf.width = width;
  buf.height = height;
  return buf;
}
