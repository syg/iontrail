// -*- mode: js2; indent-tabs-mode: nil; -*-

// Adapted from
//
// https://github.com/RiverTrail/RiverTrail/blob/master/examples/liquid-resize/resize-compute-dp.js
//
// which in turn is based on an algorithm from the paper below (which
// also appeared in ACM SIGGRAPH 2007):
// Shai Avidan and Ariel Shamir. 2007. Seam carving for content-aware image resizing.
// ACM Trans. Graph. 26, 3, Article 10 (July 2007).
// DOI=10.1145/1276377.1276390 http://doi.acm.org/10.1145/1276377.1276390

  // Assumption: if MODE defined then running under benchmark script
var benchmarking = (typeof(MODE) != "undefined");

if (benchmarking) {
  // util.js provides interface for benchmark infrastructure.
  load(libdir + "util.js");
}

// rectarray.js provides 2D array abstraction for interactive development in REPL.
load(libdir + "rectarray.js");

// A RectArray is a 2D array class exported by rectarray.js.
// It has width and height properties, and a get(i,j) method.
//
// An RectArray of Nat is thought of as an image, where the natural
// number contents are the "colors" at that point in the image.

// A ParallelArray is assumed to be 2D throughout this code.

// A ParMode is an object with properties mode [, expect]; see ParallelArray.js

// A Path is an Array of indices, related to an origin RectArray R.
// Horizontal paths are of length R.width and have elements in [0,R.height-1];
// vertical paths are of length R.height and have elements in [0,R.width-1].

// A PhaseTimes is an object with various properties naming phases of a
// computation; each property maps to a number.

// Ideally, one needs only change the below constructions
// to build4 to approximate "real image" input.
// (However, this is untested.)

// To see the images, try e.g. smallImage.print()

var tinyImage =
  RectArray.build(20, 5,
    function(x, y, k) {
      var ret;
      if (6 <= x && x < 8 && 0 <= y && y < 4)
        ret = ".";
      else if ((x-15)*(x-15)+(y-1)*(y-1) < 2)
        ret = "^";
      else if ((x-20)*(x-20)+(y-3)*(y-3) < 2)
        ret = "%";
      else if ((x-1)*(x-1)+(y-3)*(y-3) < 2)
        ret = "@";
      else
        ret = " ";
      return ret.charCodeAt(0) - 32;
    });

var smallImage =
  RectArray.build(60, 15,
    function(x, y, k) {
      var ret;
      if (6 <= x && x < 8 && 0 <= y && y < 7)
        ret = ".";
      else if ((x-15)*(x-15)+(y-1)*(y-1) < 2)
        ret = "^";
      else if ((x-40)*(x-40)+(y-6)*(y-6) < 6)
        ret = "%";
      else if ((x-1)*(x-1)+(y-12)*(y-12) < 2)
        ret = "@";
      else
        ret = " ";
      return ret.charCodeAt(0) - 32;
    });

var bigImage =
  RectArray.build(200, 70,
    function(x, y, k) {
      var ret;
      if (4 <= x && x < 7 && 10 <= y && y < 40)
        ret = ".";
      else if ((x-150)*(x-150)+(y-13)*(y-13) < 70)
        ret = "^";
      else if ((x-201)*(x-201)+(y-33)*(y-33) < 200)
        ret = "%";
      else if ((x-15)*(x-15)+(y-3)*(y-3) < 7)
        ret = "@";
      else
        ret = " ";
      return ret.charCodeAt(0) - 32;
    });

// randomImage: Nat Nat Nat Nat -> RectArray
function randomImage(w, h, sparsity, variety) {
  return RectArray.build(w, h, function (x,y) {
      if (Math.random() > 1/sparsity)
        return 0;
      else
      return 1+Math.random()*variety|0;
  });
}

// stripedImage: Nat Nat -> RectArray
function stripedImage(w, h) {
  return RectArray.build(w, h,
                         function (x, y) (Math.abs(x%100-y%100) < 10) ? 32 : 0);
}

var massiveImage =
  RectArray.build(70, 10000, function(x,y) (Math.abs(x%100-y%100) < 10) ? 32 : 0);

// asciiart: Self -> RectArray
RectArray.prototype.asciiart = function asciiart() {
  return this.map(function (x) String.fromCharCode(x+32));
};

// row: Self Nat -> Array
RectArray.prototype.row = function row(i) {
  return this.slice(i*this.width*this.payload, (i+1)*this.width*this.payload);
};

// render: Self -> String
RectArray.prototype.render = function render() {
  var art = this.asciiart();
  var a = new Array(art.height);
  for (var i=0; i < art.height; i++) {
    a[i] = art.row(i);
  }
  return a.map(function(r) r.join("")).join("\n");
};

// print: Self -> void; effect: prints rendered self.
RectArray.prototype.print =
  (function locals() { var pr = print;
      return function print() pr(this.render()); })();

// toRectArray: Self Nat Nat -> RectArray
Array.prototype.toRectArray = function toRectArray(w,h) {
  var p = this;
  return RectArray.build(w, h, function (i, j) p[i+j*w]);
};

// toRectArray: Self -> RectArray
ParallelArray.prototype.toRectArray = function toRectArray() {
  var p = this;
  var w = p.shape[0];
  var h = p.shape[1];
  return RectArray.build(w, h, function (i,j) p.get(i,j));
};

// toParallelArray: Self -> ParallelArray
RectArray.prototype.toParallelArray = function toParallelArray(mode) {
  var r = this;
  var w = this.width;
  var h = this.height;
  return new ParallelArray([w,h], function (i,j) r.get(i,j), mode);
};

// transpose: Self -> RectArray
RectArray.prototype.transpose =
  function transpose() {
    var r = this;
    return RectArray.buildN(r.height, r.width, r.payload,
                            function(x, y, k) r.get(y,x,k));
  };

// grayScale: RectArray [ParMode] -> ParallelArray
function grayScale(ra, mode)
{
  return new ParallelArray([ra.width, ra.height],
                           function (x,y) {
                             var r = ra.get(x,y,0);
                             var g = ra.get(x,y,1);
                             var b = ra.get(x,y,2);
                             var lum = (0.299*r + 0.587*g + 0.114*b);
                             return lum;
                           }, mode);
}

// The detectEdgesSeq function allows edgesSequentially to be
// implemented w/ sequential code directly rather than using a
// ParMode to enforce sequential execution a la buildSequentially.)

// detectEdgesSeq_naive: RectArray -> RectArray
// This version is naive because working directly on an array is
// enormously faster.
function detectEdgesSeq_naive(ra) {
  var sobelX = [[-1.0,  0.0, 1.0],
                [-2.0, 0.0, 2.0],
                [-1.0, 0.0, 1.0]];
  var sobelY = [[1.0,  2.0, 1.0],
                [0.0, 0.0, 0.0],
                [-1.0, -2.0, -1.0]];

  var width = ra.width;
  var height = ra.height;

  return RectArray.build(width, height,
    // The fill functions here and below are begging for refactoring, but leaving as manual clones until performance issues are resolved.
    function (x,y)
    {
      // process pixel
      var totalX = 0;
      var totalY = 0;
      for (var offY = -1; offY <= 1; offY++) {
        var newY = y + offY;
        for (var offX = -1; offX <= 1; offX++) {
          var newX = x + offX;
          if ((newX >= 0) && (newX < width) && (newY >= 0) && (newY < height)) {
            var pointIndex = (x + offX + (y + offY) * width);
            var e = ra.get(x + offX, y + offY);
            totalX += e * sobelX[offY + 1][offX + 1];
            totalY += e * sobelY[offY + 1][offX + 1];
          }
        }
      }
      var total = (Math.abs(totalX) + Math.abs(totalY))/8.0 | 0;
      return total;
    });
}

// detectEdgesSeq_array: Array -> Array
// The input array needs to be carrying width and height properties, like RectArray
function detectEdgesSeq_array(data) {
    var data1 = new Array(data.width*data.height);
    var sobelX =  [[-1.0,  0.0, 1.0],
                    [-2.0, 0.0, 2.0],
                    [-1.0, 0.0, 1.0]];
    var sobelY = [[1.0,  2.0, 1.0],
                    [0.0, 0.0, 0.0],
                    [-1.0, -2.0, -1.0]];

    var height = data.height;
    var width = data.width;

    for (var y = 0; y < height; y++) {
        for (var x = 0; x < width; x++) {
            // process pixel
            var totalX = 0;
            var totalY = 0;
            for (var offY = -1; offY <= 1; offY++) {
                var newY = y + offY;
                for (var offX = -1; offX <= 1; offX++) {
                    var newX = x + offX;
                    if ((newX >= 0) && (newX < width) && (newY >= 0) && (newY < height)) {
                        var pointIndex = x + offX + (y + offY) * data.width;
                        var e = data[pointIndex];
                        totalX += e * sobelX[offY + 1][offX + 1];
                        totalY += e * sobelY[offY + 1][offX + 1];
                    }
                }
            }
            var total = Math.floor((Math.abs(totalX) + Math.abs(totalY))/8.0);
            var index = y*width+x;
            data1[index] = total | 0;
        }
    }
    data1.width = width;
    data1.height = height;
    return data1;
}


// detectEdges: Self -> RectArray
RectArray.prototype.detectEdges2D =
  (function locals () { var detect = detectEdgesSeq_array;
      return function detectEdges()
                        detect(this).toRectArray(this.width, this.height);
      })();

// detectEdgesPar: ParallelArray [ParMode] -> ParallelArray

function detectEdgesPar_2d(pa, mode)
{
  var sobelX = [[-1.0,  0.0, 1.0],
                [-2.0, 0.0, 2.0],
                [-1.0, 0.0, 1.0]];
  var sobelY = [[1.0,  2.0, 1.0],
                [0.0, 0.0, 0.0],
                [-1.0, -2.0, -1.0]];

  var width = pa.shape[0];
  var height = pa.shape[1];

  return new ParallelArray([width, height],
    function (x,y)
    {
      // process pixel
      var totalX = 0;
      var totalY = 0;
      for (var offY = -1; offY <= 1; offY++) {
        var newY = y + offY;
        for (var offX = -1; offX <= 1; offX++) {
          var newX = x + offX;
          if ((newX >= 0) && (newX < width) && (newY >= 0) && (newY < height)) {
            var pointIndex = (x + offX + (y + offY) * width);
            var e = pa.get(x + offX, y + offY);
            totalX += e * sobelX[offY + 1][offX + 1];
            totalY += e * sobelY[offY + 1][offX + 1];
          }
        }
      }
      var total = (Math.abs(totalX) + Math.abs(totalY))/8.0 | 0;
      return total;
    }, mode);
}

function detectEdgesPar_1d(pa, mode)
{
  var sobelX = [[-1.0,  0.0, 1.0],
                [-2.0, 0.0, 2.0],
                [-1.0, 0.0, 1.0]];
  var sobelY = [[1.0,  2.0, 1.0],
                [0.0, 0.0, 0.0],
                [-1.0, -2.0, -1.0]];

  var width = pa.shape[0];
  var height = pa.shape[1];

  return new ParallelArray(width*height,
    function (index)
    {
      var j = index | 0;
      var y = (j / width) | 0;
      var x = (j % width);
      // process pixel
      var totalX = 0;
      var totalY = 0;
      for (var offY = -1; offY <= 1; offY++) {
        var newY = y + offY;
        for (var offX = -1; offX <= 1; offX++) {
          var newX = x + offX;
          if ((newX >= 0) && (newX < width) && (newY >= 0) && (newY < height)) {
            var pointIndex = (x + offX + (y + offY) * width);
            var e = pa.get(x + offX, y + offY);
            totalX += e * sobelX[offY + 1][offX + 1];
            totalY += e * sobelY[offY + 1][offX + 1];
          }
        }
      }
      var total = (Math.abs(totalX) + Math.abs(totalY))/8.0 | 0;
      return total;
    }, mode);
}

// detectEdges: Self [ParMode] -> ParallelArray
ParallelArray.prototype.detectEdges2D =
  (function locals () { var detect = detectEdgesPar_2d;
      return function detectEdges(mode) detect(this, mode); })();

ParallelArray.prototype.detectEdges1D =
  (function locals () { var detect = detectEdgesPar_1d;
      return function detectEdges(mode) detect(this, mode); })();

// computeEnergy: ParallelArray -> RectArray
//
// (The return type is forced upon us, for now at least, until we add
// appropriate API to ParallelArray; there's a dependency from each
// row upon its predecessor, but the contents of each row could be
// computed in parallel.)
function computeEnergy(pa) {
  var width = pa.shape[0];
  var height = pa.shape[1];

  var energy = new RectArray(width, height);
  energy.set(0, 0, 0);
  for (var y = 0; y < height; y++) {
    for (var x = 0; x < width; x++) {
      var e = pa.get(x, y);
      if (y >= 1) {
        var p = energy.get(x, y-1);
        if (x > 0) {
          p = Math.min(p, energy.get(x-1, y-1));
        }
        if (x < (width - 1)) {
          p = Math.min(p, energy.get(x+1, y-1));
        }
        e += p;
      }
      energy.set(x, y, e);
    }
  }
  return energy;
}

// computeEnergy: Self -> RectArray
ParallelArray.prototype.computeEnergy =
  (function locals () { var energy = computeEnergy;
      return function computeEnergy() energy(this); })();

// findPath: RectArray -> Array
// (This is inherently sequential.)
function findPath(energy)
{
  var height = energy.height;
  var width  = energy.width;
  var path = new Array(height);
  var y = height - 1;
  var minPos = 0;
  var minEnergy = energy.get(minPos, y);

  for (var x = 1; x < width; x++) {
    if (energy.get(x,y) < minEnergy) {
      minEnergy = energy.get(x,y);
      minPos = x;
    }
  }

  path[y] = minPos;
  for (y = height - 2; y >= 0; y--) {
    minEnergy = energy.get(minPos, y);
    // var line = energy[y]
    var p = minPos;
    if (p >= 1 && energy.get(p-1, y) < minEnergy) {
      minPos = p-1; minEnergy = energy.get(p-1, y);
    }
    if (p < width - 1 && energy.get(p+1, y) < minEnergy) {
      minPos = p+1; minEnergy = energy.get(p+1, y);
    }
    path[y] = minPos;
  }
  return path;
}

// findPath: Self -> Path
RectArray.prototype.findPath =
  (function locals() { var path = findPath;
      return function findPath() path(this); })();

// cutPathHorizontallyBW : RectArray Array -> RectArray
function cutPathHorizontallyBW(ra, path) {
  return RectArray.build(ra.width-1, ra.height,
                         function (x, y) {
                             if (x < path[y]-1)
                               return ra.get(x, y);
                             if (x == path[y]-1)
                               return (ra.get(x,y)+ra.get(x+1,y))/2|0;
                             else
                               return ra.get(x+1,y);
                         });
}

// cutPathHorizontallyBW: Self -> RectArray
RectArray.prototype.cutPathHorizontallyBW =
  (function locals() { var cut = cutPathHorizontallyBW;
      return function cutPathHorizontallyBW(path) cut(this, path);  })();

// cutPathVerticallyBW: RectArray Path -> RectArray
function cutPathVerticallyBW(ra, path) {
  return RectArray.build(ra.width, ra.height-1,
                         function (x, y) {
                             if (y < path[x]-1)
                               return ra.get(x, y);
                             if (y == path[x]-1)
                               return (ra.get(x,y)+ra.get(x,y+1))/2|0;
                             else
                               return ra.get(x,y+1);
                         });
}

// cutPathVerticallyBW: Self Path -> RectArray
RectArray.prototype.cutPathVerticallyBW =
  (function locals() { var cut = cutPathVerticallyBW;
      return function cutPathVerticallyBW(path) cut(this, path); })();

// cutHorizontalSeamBW: RectArray ParMode -> RectArray
function cutHorizontalSeamBW(r, mode)
{
  var e = r.toParallelArray(mode).detectEdges2D(mode).computeEnergy(mode);
  var p = e.findPath(mode); e = null;
  return r.cutPathHorizontallyBW(p, mode);
}

// cutHorizontalSeamBW: Self ParMode -> RectArray
RectArray.prototype.cutHorizontalSeamBW =
  (function locals() { var cut = cutHorizontalSeamBW;
      return function cutHorizontalSeamBW(mode) cut(this, mode); })();

// cutVerticalSeamBW: RectArray ParMode -> RectArray
function cutVerticalSeamBW(r, mode)
{
  var e = r.transpose(mode).toParallelArray(mode).detectEdges2D(mode).computeEnergy(mode);
  return r.cutPathVerticallyBW(e.findPath());
}

// cutVerticalSeamBW: Self ParMode -> RectArray
RectArray.prototype.cutVerticalSeamBW =
  (function locals() { var cut = cutVerticalSeamBW;
      return function cutVerticalSeamBW(mode) cut(this, mode); })();

// cutVerticalSeamBW: Self Nat Nat ParMode -> Self
RectArray.prototype.shrinkBW = function shrinkBW(w, h, mode) {
  var r = this;
  while (r.height > h || r.width > w) {
    if (r.width > w) 
      r = r.cutHorizontalSeamBW(mode);
    if (r.height > h)
      r = r.cutVerticalSeamBW(mode);
  }
  return r;
};

// timedShrinkBW: Self Nat Nat ParMode -> PhaseTimes
RectArray.prototype.timedShrinkBW = function timedShrinkBW(w, h, mode) {
  var times = {
    "topar": 0, "trans": 0, "edges": 0, "energ": 0, "fpath": 0, "cpath": 0
  };
  var r = this;
  var lasttime = new Date();
  function elapsed() {
    var d = new Date(); var e = d - lasttime; lasttime = d; return e;
  }
  while (r.height > h || r.width > w) {
    if (r.width > w) {
      elapsed();
      var e = r.toParallelArray(mode);
      times.topar += elapsed();
      e = e.detectEdges1D(mode);
      times.edges += elapsed();
      e = e.computeEnergy(mode);
      times.energ += elapsed();
      e = e.findPath(mode);
      times.fpath += elapsed();
      r = r.cutPathHorizontallyBW(e, mode);
      times.cpath += elapsed();
      e = null;
    }
    if (r.height > h) {
      elapsed();
      var e = r.transpose(mode);
      times.trans += elapsed();
      e = e.toParallelArray(mode);
      times.topar += elapsed();
      e = e.detectEdges1D(mode);
      times.edges += elapsed();
      e = e.computeEnergy(mode);
      times.energ += elapsed();
      e = e.findPath(mode);
      times.fpath += elapsed();
      r = r.cutPathVerticallyBW(e, mode);
      times.cpath += elapsed();
      e = null;
    }
  }
  return times;
};

if (benchmarking) {
  // Below functions are to interface with run.sh

  // For simple, repeatable investigation, use: tinyImage, smallImage,
  // bigImage, or massiveImage for expression establishing seqInput
  // below.  stripedImage() is also useful for repeatable inputs.
  //
  // For play, use: randomImage(width, height, sparsity, variety)
  // or stripedImage(width, height).
  //
  // The default tower image from the original benchmarks was 800x542.
  // (Correspondingly, the shrunken versions were 400x271 and 80x54.)
  var seqInput = stripedImage(800, 542, 10, 10);
  var parInput = seqInput.toParallelArray();

  function buildSequentially() {
    return seqInput.toParallelArray({mode:"seq"});
  }
  function buildParallel() {
    return seqInput.toParallelArray({mode:"par"});
  }

  function edgesSequentially() {
    return detectEdgesSeq_array(seqInput);
  }
  function edgesParallel() {
    return detectEdgesPar_1d(parInput);
  }

  if (benchmarking) {
    benchmark("BUILD", 1, DEFAULT_MEASURE,
              buildSequentially, buildParallel);

    benchmark("EDGES", 1, DEFAULT_MEASURE,
              edgesSequentially, edgesParallel);
    }
}
