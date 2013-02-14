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

load(libdir + "rectarray.js");

var tinyImage =
  RectArray.build(20, 5,
    function(x, y, k) {
      var ret;
      if (6 <= x && x < 7 && 0 <= y && y < 4)
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

var bigImage =
  RectArray.build(200, 50, // change to build4 for a "real image"
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

RectArray.prototype.asciiart = function asciiart() {
  return this.map(function (x) String.fromCharCode(x+32));
}

RectArray.prototype.row = function row(i) {
  return this.slice(i*this.width*this.payload, (i+1)*this.width*this.payload);
};

RectArray.prototype.render = function render() {
  var art = this.asciiart();
  var a = new Array(art.height);
  for (var i=0; i < art.height; i++) {
    a[i] = art.row(i);
  }
  return a.map(function(r) r.join("")).join("\n");
};

RectArray.prototype.print =
  (function locals() { var pr = print;
      return function print() pr(this.render()); })();

ParallelArray.prototype.toRectArray = function toRectArray() {
  var p = this;
  var w = p.shape[0];
  var h = p.shape[1];
  return RectArray.build(w, h, function (i,j) p.get(i,j));
};

RectArray.prototype.toParallelArray = function toParallelArray() {
  var r = this;
  var w = this.width;
  var h = this.height;
  return new ParallelArray([w,h], function (i,j) r.get(i,j));
};

RectArray.prototype.transpose =
  function transpose() {
    var r = this;
    return RectArray.buildN(r.height, r.width, r.payload,
                            function(x, y, k) r.get(y,x,k));
  };


function grayScale(ra)
{
  return new ParallelArray([ra.width, ra.height],
                           function (x,y) {
                             var r = ra.get(x,y,0);
                             var g = ra.get(x,y,1);
                             var b = ra.get(x,y,2);
                             var lum = (0.299*r + 0.587*g + 0.114*b);
                             return lum;
                           });
}

function detectEdges(pa)
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
    });
}

ParallelArray.prototype.detectEdges =
  (function locals () { var detect = detectEdges;
      return function detectEdges() detect(this); })();

Array.build = function build(len, fill) {
  var a = new Array(len);
  for (var i=0; i < len; i++) { a[i] = fill(i); }
  return a;
};

// computeEnergy : ParallelArray -> RectArray
// (for now at least, until we add appropriate API to ParallelArray;
//  there's a dependency from each row upon its predecessor, but
//  the contents of each row could be computed in parallel.)
function computeEnergy(pa) {
  var width = pa.shape[0];
  var height = pa.shape[1];
  // Array.build(height, function (x) Array.build(width));
  var energy = new RectArray(width, height);
  energy.set(0, 0, 0); // energy[0][0] = 0;
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
      energy.set(x, y, e); // energy[y][x] = e;
    }
  }
  return energy;
}

ParallelArray.prototype.computeEnergy =
  (function locals () { var energy = computeEnergy;
      return function computeEnergy() energy(this); })();

// findPath : RectArray -> Array
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

RectArray.prototype.findPath =
  (function locals() { var path = findPath;
      return function findPath() path(this); })();

// cutPathHorizontally : RectArray Array -> RectArray

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

RectArray.prototype.cutPathHorizontallyBW =
  (function locals() { var cut = cutPathHorizontallyBW;
      return function cutPathHorizontallyBW(path) cut(this, path);  })();

function cutPathVerticallyBW(ra, path) {
  return RectArray.build(ra.width, ra.height-1,
                         function (x, y) {
                             if (x < path[x]-1)
                               return ra.get(x, y);
                             if (x == path[x]-1)
                               return (ra.get(x,y)+ra.get(x,y+1))/2|0;
                             else
                               return ra.get(x,y+1);
                         });
}

RectArray.prototype.cutPathVerticallyBW =
  (function locals() { var cut = cutPathVerticallyBW;
      return function cutPathVerticallyBW(path) cut(this, path); })();

function cutHorizontalSeam(r)
{
  var e = r.toParallelArray().detectEdges().computeEnergy();
  return r.cutPathHorizontallyBW(e.findPath());
}

function cutVerticalSeam(r)
{
  var e = r.transpose().toParallelArray().detectEdges().computeEnergy();
  return r.cutPathVerticallyBW(e.findPath());
}

function iterated(count, transform) {
  return function (x) {
    for (var i = 0 ; i < count; i++) { x = transform(x); }
    return x;
  };
}
