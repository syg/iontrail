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

var anImage =
  RectArray.build(26, 20, // change to build4 for a "real image"
    function(x, y, k) {
      if (4 <= x && x < 7 && 3 <= y && y < 8)
        return "X";
      else if ((x-15)*(x-15)+(y-13)*(y-13) < 12)
        return "Y";
      else if ((x-17)*(x-17)+(y-3)*(y-3) < 7)
        return "Z";
      else
        return undefined;
    });

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

  var width = pa.width;
  var height = pa.height;

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
                             var total = Math.floor((Math.abs(totalX) + Math.abs(totalY))/8.0);
                             return total;
                           });
}