/* -*- Mode: C++; tab-width: 2; indent-tabs-mode: nil; c-basic-offset: 2 -*-
 *
 * The contents of this file are subject to the Netscape Public License
 * Version 1.0 (the "NPL"); you may not use this file except in
 * compliance with the NPL.  You may obtain a copy of the NPL at
 * http://www.mozilla.org/NPL/
 *
 * Software distributed under the NPL is distributed on an "AS IS" basis,
 * WITHOUT WARRANTY OF ANY KIND, either express or implied. See the NPL
 * for the specific language governing rights and limitations under the
 * NPL.
 *
 * The Initial Developer of this code under the NPL is Netscape
 * Communications Corporation.  Portions created by Netscape are
 * Copyright (C) 1998 Netscape Communications Corporation.  All Rights
 * Reserved.
 */

#ifndef nsIRegion_h___
#define nsIRegion_h___

#include "nscore.h"
#include "nsISupports.h"
#include "nsRect.h"

// Function type passed into nsIRegion::forEachRect, invoked
// for each rectangle in a region
typedef void (*nsRectInRegionFunc)(void *closure, nsRect& rect);

// An implementation of a region primitive that can be used to
// represent arbitrary pixel areas. Probably implemented on top
// of the native region primitive. The assumption is that, at worst,
// it is a rectangle list.

#define NS_IREGION_IID   \
{ 0x8ef366e0, 0xee94, 0x11d1,    \
{ 0xa8, 0x2a, 0x00, 0x40, 0x95, 0x9a, 0x28, 0xc9 } }

class nsIRegion : public nsISupports
{
public:
  virtual nsresult Init() = 0;

  /**
  * copy operator equivalent that takes another region
  *
  * @param      region to copy
  * @return     void
  *
  **/

  virtual void SetTo(const nsIRegion &aRegion) = 0;

  /**
  * copy operator equivalent that takes a rect
  *
  * @param      aX xoffset of rect to set region to
  * @param      aY yoffset of rect to set region to
  * @param      aWidth width of rect to set region to
  * @param      aHeight height of rect to set region to
  * @return     void
  *
  **/

  virtual void SetTo(PRInt32 aX, PRInt32 aY, PRInt32 aWidth, PRInt32 aHeight) = 0;

  /**
  * destructively intersect another region with this one
  *
  * @param      region to intersect
  * @return     void
  *
  **/

  virtual void Intersect(const nsIRegion &aRegion) = 0;

  /**
  * destructively intersect a rect with this region
  *
  * @param      aX xoffset of rect to intersect with region
  * @param      aY yoffset of rect to intersect with region
  * @param      aWidth width of rect to intersect with region
  * @param      aHeight height of rect to intersect with region
  * @return     void
  *
  **/

  virtual void Intersect(PRInt32 aX, PRInt32 aY, PRInt32 aWidth, PRInt32 aHeight) = 0;

  /**
  * destructively union another region with this one
  *
  * @param      region to union
  * @return     void
  *
  **/

  virtual void Union(const nsIRegion &aRegion) = 0;

  /**
  * destructively union a rect with this region
  *
  * @param      aX xoffset of rect to union with region
  * @param      aY yoffset of rect to union with region
  * @param      aWidth width of rect to union with region
  * @param      aHeight height of rect to union with region
  * @return     void
  *
  **/

  virtual void Union(PRInt32 aX, PRInt32 aY, PRInt32 aWidth, PRInt32 aHeight) = 0;

  /**
  * destructively subtract another region with this one
  *
  * @param      region to subtract
  * @return     void
  *
  **/

  virtual void Subtract(const nsIRegion &aRegion) = 0;

  /**
  * destructively subtract a rect from this region
  *
  * @param      aX xoffset of rect to subtract with region
  * @param      aY yoffset of rect to subtract with region
  * @param      aWidth width of rect to subtract with region
  * @param      aHeight height of rect to subtract with region
  * @return     void
  *
  **/

  virtual void Subtract(PRInt32 aX, PRInt32 aY, PRInt32 aWidth, PRInt32 aHeight) = 0;
  
  /**
  * is this region empty? i.e. does it contain any pixels
  *
  * @param      none
  * @return     returns whether the region is empty
  *
  **/

  virtual PRBool IsEmpty(void) = 0;

  /**
  * == operator equivalent i.e. do the regions contain exactly
  * the same pixels
  *
  * @param      region to compare
  * @return     whether the regions are identical
  *
  **/

  virtual PRBool IsEqual(const nsIRegion &aRegion) = 0;

  /**
  * returns the bounding box of the region i.e. the smallest
  * rectangle that completely contains the region.        
  *
  * @param      aX out parameter for xoffset of bounding rect for region
  * @param      aY out parameter for yoffset of bounding rect for region
  * @param      aWidth out parameter for width of bounding rect for region
  * @param      aHeight out parameter for height of bounding rect for region
  * @return     void
  *
  **/
  virtual void GetBoundingBox(PRInt32 *aX, PRInt32 *aY, PRInt32 *aWidth, PRInt32 *aHeight) = 0;

  /**
  * offsets the region in x and y
  *
  * @param  xoffset  pixel offset in x
  * @param  yoffset  pixel offset in y
  * @return          void
  *
  **/
  virtual void Offset(PRInt32 aXOffset, PRInt32 aYOffset) = 0;

  /**
  * does the region completely contain the rectangle?
  *
  * @param      rect to check for containment
  * @return     true iff the rect is completely contained
  *
  **/

  virtual PRBool ContainsRect(PRInt32 aX, PRInt32 aY, PRInt32 aWidth, PRInt32 aHeight) = 0;
  
  /**
  * invoke a function for each rectangle in the region
  *
  * @param  func    Function to invoke for each rectangle
  * @param  closure Arbitrary data to pass to the function
  * @return          void
  *
  **/
  virtual PRBool ForEachRect(nsRectInRegionFunc *func, void *closure) = 0;
};

#endif  // nsRIegion_h___ 
