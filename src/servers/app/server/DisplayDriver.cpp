//------------------------------------------------------------------------------
//	Copyright (c) 2001-2002, OpenBeOS
//
//	Permission is hereby granted, free of charge, to any person obtaining a
//	copy of this software and associated documentation files (the "Software"),
//	to deal in the Software without restriction, including without limitation
//	the rights to use, copy, modify, merge, publish, distribute, sublicense,
//	and/or sell copies of the Software, and to permit persons to whom the
//	Software is furnished to do so, subject to the following conditions:
//
//	The above copyright notice and this permission notice shall be included in
//	all copies or substantial portions of the Software.
//
//	THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
//	IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
//	FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
//	AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
//	LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
//	FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER
//	DEALINGS IN THE SOFTWARE.
//
//	File Name:		DisplayDriver.cpp
//	Author:			DarkWyrm <bpmagic@columbus.rr.com>
//				Gabe Yoder <gyoder@stny.rr.com>
//	Description:	Mostly abstract class which handles all graphics output
//					for the server
//  
//------------------------------------------------------------------------------
#include <Accelerant.h>
#include "DisplayDriver.h"
#include "LayerData.h"
#include "ServerCursor.h"

// TODO: Major cleanup is left.  Public functions should be repsonsible for locking.
// Clean the locking code from the protected functions.  Remove bounds checking stuff
// since clipper must handle all needed boundary checks (otherwise we could have windows
// bleeding into other windows).
// TODO: Add pixel, line, and rect functions for handling all of the options from LayerData

LineCalc::LineCalc()
{
}

LineCalc::LineCalc(const BPoint &pta, const BPoint &ptb)
{
	start=pta;
	end=ptb;
	slope=(start.y-end.y)/(start.x-end.x);
	offset=start.y-(slope * start.x);
	minx = MIN(start.x,end.x);
	maxx = MAX(start.x,end.x);
	miny = MIN(start.y,end.y);
	maxy = MAX(start.y,end.y);
}

void LineCalc::SetPoints(const BPoint &pta, const BPoint &ptb)
{
	start=pta;
	end=ptb;
	slope=(start.y-end.y)/(start.x-end.x);
	offset=start.y-(slope * start.x);
	minx = MIN(start.x,end.x);
	maxx = MAX(start.x,end.x);
	miny = MIN(start.y,end.y);
	maxy = MAX(start.y,end.y);
}

bool LineCalc::ClipToRect(const BRect& rect)
{
	if ( (maxx < rect.left) || (minx > rect.right) || (miny < rect.top) || (maxy > rect.bottom) )
		return false;
	BPoint newStart(-1,-1);
	BPoint newEnd(-1,-1);
	if ( maxx == minx )
	{
		newStart.x = newEnd.x = minx;
		if ( miny < rect.top )
			newStart.y = rect.top;
		else
			newStart.y = miny;
		if ( maxy > rect.bottom )
			newEnd.y = rect.bottom;
		else
			newEnd.y = maxy;
	}
	else if ( maxy == miny )
	{
		newStart.y = newEnd.y = miny;
		if ( minx < rect.left )
			newStart.x = rect.left;
		else
			newStart.x = minx;
		if ( maxx > rect.right )
			newEnd.x = rect.right;
		else
			newEnd.x = maxx;
	}
	else
	{
		float leftInt, rightInt, topInt, bottomInt;
		BPoint tempPoint;
		leftInt = GetY(rect.left);
		rightInt = GetY(rect.right);
		topInt = GetX(rect.top);
		bottomInt = GetX(rect.bottom);
		if ( end.x < start.x )
		{
			tempPoint = start;
			start = end;
			end = tempPoint;
		}
		if ( start.x < rect.left )
		{
			if ( (leftInt >= rect.top) && (leftInt <= rect.bottom) )
			{
				newStart.x = rect.left;
				newStart.y = leftInt;
			}
			if ( start.y < end.y )
			{
				if ( (topInt >= rect.left) && (topInt <= rect.right) )
				{
					newStart.x = topInt;
					newStart.y = rect.top;
				}
			}
			else
			{
				if ( (bottomInt >= rect.left) && (bottomInt <= rect.right) )
				{
					newStart.x = bottomInt;
					newStart.y = rect.bottom;
				}
			}
		}
		else
		{
			if ( start.y < rect.top )
			{
				if ( (topInt >= rect.left) && (topInt <= rect.right) )
				{
					newStart.x = topInt;
					newStart.y = rect.top;
				}
			}
			else if ( start.y > rect.bottom )
			{
				if ( (bottomInt >= rect.left) && (bottomInt <= rect.right) )
				{
					newStart.x = bottomInt;
					newStart.y = rect.bottom;
				}
			}
			else
				newStart = start;
		}
		if ( end.x > rect.right )
		{
			if ( (rightInt >= rect.top) && (rightInt <= rect.bottom) )
			{
				newEnd.x = rect.right;
				newEnd.y = rightInt;
			}
			if ( start.y < end.y )
			{
				if ( (bottomInt >= rect.left) && (bottomInt <= rect.right) )
				{
					newEnd.x = bottomInt;
					newEnd.y = rect.bottom;
				}
			}
			else
			{
				if ( (topInt >= rect.left) && (topInt <= rect.right) )
				{
					newEnd.x = topInt;
					newEnd.y = rect.top;
				}
			}
		}
		else
		{
			if ( end.y < rect.top )
			{
				if ( (topInt >= rect.left) && (topInt <= rect.right) )
				{
					newEnd.x = topInt;
					newEnd.y = rect.top;
				}
			}
			else if ( end.y > rect.bottom )
			{
				if ( (bottomInt >= rect.left) && (bottomInt <= rect.right) )
				{
					newEnd.x = bottomInt;
					newEnd.y = rect.bottom;
				}
			}
			else
				newEnd = end;
		}
	}
	if ( (newStart.x == -1) || (newStart.y == -1) || (newEnd.x == -1) || (newEnd.y == -1) )
		return false;
	SetPoints(newStart,newEnd);

	return true;
}

void LineCalc::Swap(LineCalc &from)
{
	BPoint pta, ptb;
	pta = start;
	ptb = end;
	SetPoints(from.start,from.end);
	from.SetPoints(pta,ptb);
}

float LineCalc::GetX(float y)
{
	if (start.x == end.x)
		return start.x;
	return ( (y-offset)/slope );
}

float LineCalc::GetY(float x)
{
	if ( start.x == end.x )
		return start.y;
	return ( (slope * x) + offset );
}

/*!
	\brief Sets up internal variables needed by all DisplayDriver subclasses
	
	Subclasses should follow DisplayDriver's lead and use this function mostly
	for initializing data members.
*/
DisplayDriver::DisplayDriver(void)
{
	_locker=new BLocker();

	_buffer_depth=0;
	_buffer_width=0;
	_buffer_height=0;
	_buffer_mode=-1;
	
	_is_cursor_hidden=false;
	_is_cursor_obscured=false;
	_cursor=NULL;
	_dpms_caps=B_DPMS_ON;
	_dpms_state=B_DPMS_ON;
}


/*!
	\brief Deletes the locking semaphore
	
	Subclasses should use the destructor mostly for freeing allocated heap space.
*/
DisplayDriver::~DisplayDriver(void)
{
	delete _locker;
}

/*!
	\brief Initializes the driver object.
	\return true if successful, false if not
	
	Initialize sets up the driver for display, including the initial clearing
	of the screen. If things do not go as they should, false should be returned.
*/
bool DisplayDriver::Initialize(void)
{
	return false;
}

/*!
	\brief Shuts down the driver's video subsystem
	
	Any work done by Initialize() should be undone here. Note that Shutdown() is
	called even if Initialize() was unsuccessful.
*/
void DisplayDriver::Shutdown(void)
{
}

/*!
	\brief Called for all BView::CopyBits calls
	\param src Source rectangle.
	\param dest Destination rectangle.
	
	If the destination is not the same size as the source, the source should be scaled to fit.
*/
void DisplayDriver::CopyBits(BRect src, BRect dest)
{
}

/*!
	\brief A screen-to-screen blit (of sorts) which copies a BRegion
	\param src Source region
	\param lefttop Offset to which the region will be copied
*/
void DisplayDriver::CopyRegion(BRegion *src, const BPoint &lefttop)
{
}

/*!
	\brief Called for all BView::DrawBitmap calls
	\param bmp Bitmap to be drawn. It will always be non-NULL and valid. The color 
	space is not guaranteed to match.
	\param src Source rectangle
	\param dest Destination rectangle. Source will be scaled to fit if not the same size.
	\param d Data structure containing any other data necessary for the call. Always non-NULL.
*/
void DisplayDriver::DrawBitmap(ServerBitmap *bmp, BRect src, BRect dest, LayerData *d)
{
}

/*!
	\brief Utilizes the font engine to draw a string to the frame buffer
	\param string String to be drawn. Always non-NULL.
	\param length Number of characters in the string to draw. Always greater than 0. If greater
	than the number of characters in the string, draw the entire string.
	\param pt Point at which the baseline starts. Characters are to be drawn 1 pixel above
	this for backwards compatibility. While the point itself is guaranteed to be inside
	the frame buffers coordinate range, the clipping of each individual glyph must be
	performed by the driver itself.
	\param d Data structure containing any other data necessary for the call. Always non-NULL.
*/
void DisplayDriver::DrawString(const char *string, int32 length, BPoint pt, LayerData *d, escapement_delta *delta)
{
}

void DisplayDriver::FillArc(const BRect r, float angle, float span, const RGBColor& color)
{
}

void DisplayDriver::FillArc(const BRect r, float angle, float span, const Pattern& pattern, const RGBColor& high_color, const RGBColor& low_color)
{
}

/*!
	\brief Called for all BView::FillArc calls
	\param r Rectangle enclosing the entire arc
	\param angle Starting angle for the arc in degrees
	\param span Span of the arc in degrees. Ending angle = angle+span.
	\param setLIne The horizontal line drawing function which handles needed things like pattern,
	               color, and line thickness
*/
void DisplayDriver::FillArc(const BRect r, float angle, float span, DisplayDriver* driver, SetHorizontalLineFuncType setLine)
{
	float xc = (r.left+r.right)/2;
	float yc = (r.top+r.bottom)/2;
	float rx = r.Width()/2;
	float ry = r.Height()/2;
	int Rx2 = ROUND(rx*rx);
	int Ry2 = ROUND(ry*ry);
	int twoRx2 = 2*Rx2;
	int twoRy2 = 2*Ry2;
	int p;
	int x=0;
	int y = (int)ry;
	int px = 0;
	int py = twoRx2 * y;
	int startx, endx;
	int starty, endy;
	int xclip, startclip, endclip;
	int startQuad, endQuad;
	bool useQuad1, useQuad2, useQuad3, useQuad4;
	bool shortspan = false;
	//BPoint center(xc,yc);

	// Watch out for bozos giving us whacko spans
	if ( (span >= 360) || (span <= -360) )
	{
	  FillEllipse(r,driver,setLine);
	  return;
	}

	Lock();

	if ( span > 0 )
	{
		startQuad = (int)(angle/90)%4+1;
		endQuad = (int)((angle+span)/90)%4+1;
		startx = ROUND(.5*r.Width()*fabs(cos(angle*M_PI/180)));
		endx = ROUND(.5*r.Width()*fabs(cos((angle+span)*M_PI/180)));
	}
	else
	{
		endQuad = (int)(angle/90)%4+1;
		startQuad = (int)((angle+span)/90)%4+1;
		endx = ROUND(.5*r.Width()*fabs(cos(angle*M_PI/180)));
		startx = ROUND(.5*r.Width()*fabs(cos((angle+span)*M_PI/180)));
	}

	starty = ROUND(ry*sqrt(1-(double)startx*startx/(rx*rx)));
	endy = ROUND(ry*sqrt(1-(double)endx*endx/(rx*rx)));

	if ( startQuad != endQuad )
	{
		useQuad1 = (endQuad > 1) && (startQuad > endQuad);
		useQuad2 = ((startQuad == 1) && (endQuad > 2)) || ((startQuad > endQuad) && (endQuad > 2));
		useQuad3 = ((startQuad < 3) && (endQuad == 4)) || ((startQuad < 3) && (endQuad < startQuad));
		useQuad4 = (startQuad < 4) && (startQuad > endQuad);
	}
	else
	{
		if ( (span < 90) && (span > -90) )
		{
			useQuad1 = false;
			useQuad2 = false;
			useQuad3 = false;
			useQuad4 = false;
			shortspan = true;
		}
		else
		{
			useQuad1 = (startQuad != 1);
			useQuad2 = (startQuad != 2);
			useQuad3 = (startQuad != 3);
			useQuad4 = (startQuad != 4);
		}
	}

	if ( useQuad1 )
		(driver->*setLine)(ROUND(xc),ROUND(xc+x),ROUND(yc-y));
	if ( useQuad2 )
		(driver->*setLine)(ROUND(xc),ROUND(xc-x),ROUND(yc-y));
	if ( useQuad3 )
		(driver->*setLine)(ROUND(xc),ROUND(xc-x),ROUND(yc+y));
	if ( useQuad4 )
		(driver->*setLine)(ROUND(xc),ROUND(xc+x),ROUND(yc+y));
	/*
	if ( (!shortspan && (((startQuad == 1) && (x <= startx)) || ((endQuad == 1) && (x >= endx)))) || 
	     (shortspan && (startQuad == 1) && (x <= startx) && (x >= endx)) ) 
		StrokeLine(BPoint(xc+x,yc-y),center,d,pat);
	if ( (!shortspan && (((startQuad == 2) && (x >= startx)) || ((endQuad == 2) && (x <= endx)))) || 
	     (shortspan && (startQuad == 2) && (x >= startx) && (x <= endx)) ) 
		StrokeLine(BPoint(xc-x,yc-y),center,d,pat);
	if ( (!shortspan && (((startQuad == 3) && (x <= startx)) || ((endQuad == 3) && (x >= endx)))) || 
	     (shortspan && (startQuad == 3) && (x <= startx) && (x >= endx)) ) 
		StrokeLine(BPoint(xc-x,yc+y),center,d,pat);
	if ( (!shortspan && (((startQuad == 4) && (x >= startx)) || ((endQuad == 4) && (x <= endx)))) || 
	     (shortspan && (startQuad == 4) && (x >= startx) && (x <= endx)) ) 
		StrokeLine(BPoint(xc+x,yc+y),center,d,pat);
		*/

	p = ROUND (Ry2 - (Rx2 * ry) + (.25 * Rx2));
	while (px < py)
	{
		x++;
		px += twoRy2;
		if ( p < 0 )
			p += Ry2 + px;
		else
		{
			y--;
			py -= twoRx2;
			p += Ry2 + px - py;
		}

		if ( useQuad1 )
			(driver->*setLine)(ROUND(xc),ROUND(xc+x),ROUND(yc-y));
		if ( useQuad2 )
			(driver->*setLine)(ROUND(xc),ROUND(xc-x),ROUND(yc-y));
		if ( useQuad3 )
			(driver->*setLine)(ROUND(xc),ROUND(xc-x),ROUND(yc+y));
		if ( useQuad4 )
			(driver->*setLine)(ROUND(xc),ROUND(xc+x),ROUND(yc+y));
		if ( !shortspan )
		{
			if ( startQuad == 1 )
			{
				if ( x <= startx )
					(driver->*setLine)(ROUND(xc),ROUND(xc+x),ROUND(yc-y));
				else
				{
					xclip = ROUND(y*startx/(double)starty);
					(driver->*setLine)(ROUND(xc),ROUND(xc+xclip),ROUND(yc-y));
				}
			}
			else if ( startQuad == 2 )
			{
				if ( x >= startx )
				{
					xclip = ROUND(y*startx/(double)starty);
					(driver->*setLine)(ROUND(xc-x),ROUND(xc-xclip),ROUND(yc-y));
				}
			}
			else if ( startQuad == 3 )
			{
				if ( x <= startx )
					(driver->*setLine)(ROUND(xc-x),ROUND(xc),ROUND(yc+y));
				else
				{
					xclip = ROUND(y*startx/(double)starty);
					(driver->*setLine)(ROUND(xc-xclip),ROUND(xc),ROUND(yc+y));
				}
			}
			else if ( startQuad == 4 )
			{
				if ( x >= startx )
				{
					xclip = ROUND(y*startx/(double)starty);
					(driver->*setLine)(ROUND(xc+xclip),ROUND(xc+x),ROUND(yc+y));
				}
			}

			if ( endQuad == 1 )
			{
				if ( x >= endx )
				{
					xclip = ROUND(y*endx/(double)endy);
					(driver->*setLine)(ROUND(xc+xclip),ROUND(xc+x),ROUND(yc-y));
				}
			}
			else if ( endQuad == 2 )
			{
				if ( x <= endx )
					(driver->*setLine)(ROUND(xc-x),ROUND(xc),ROUND(yc-y));
				else
				{
					xclip = ROUND(y*endx/(double)endy);
					(driver->*setLine)(ROUND(xc-xclip),ROUND(xc),ROUND(yc-y));
				}
			}
			else if ( endQuad == 3 )
			{
				if ( x >= endx )
				{
					xclip = ROUND(y*endx/(double)endy);
					(driver->*setLine)(ROUND(xc-x),ROUND(xc-xclip),ROUND(yc+y));
				}
			}
			else if ( endQuad == 4 )
			{
				if ( x <= endx )
					(driver->*setLine)(ROUND(xc),ROUND(xc+x),ROUND(yc+y));
				else
				{
					xclip = ROUND(y*endx/(double)endy);
					(driver->*setLine)(ROUND(xc),ROUND(xc+xclip),ROUND(yc+y));
				}
			}
		}
		else
		{
			startclip = ROUND(y*startx/(double)starty);
			endclip = ROUND(y*endx/(double)endy);
			if ( startQuad == 1 )
			{
				if ( (x <= startx) && (x >= endx) )
					(driver->*setLine)(ROUND(xc+endclip),ROUND(xc+x),ROUND(yc-y));
				else
					(driver->*setLine)(ROUND(xc+endclip),ROUND(xc+startclip),ROUND(yc-y));
			}
			else if ( startQuad == 2 )
			{
				if ( (x <= startx) && (x >= endx) )
					(driver->*setLine)(ROUND(xc-x),ROUND(xc-startclip),ROUND(yc-y));
				else
					(driver->*setLine)(ROUND(xc-endclip),ROUND(xc-startclip),ROUND(yc-y));
			}
			else if ( startQuad == 3 )
			{
				if ( (x <= startx) && (x >= endx) )
					(driver->*setLine)(ROUND(xc-x),ROUND(xc-endclip),ROUND(yc+y));
				else
					(driver->*setLine)(ROUND(xc-startclip),ROUND(xc-endclip),ROUND(yc+y));
			}
			else if ( startQuad == 4 )
			{
				if ( (x <= startx) && (x >= endx) )
					(driver->*setLine)(ROUND(xc+startclip),ROUND(xc+x),ROUND(yc+y));
				else
					(driver->*setLine)(ROUND(xc+startclip),ROUND(xc+endclip),ROUND(yc+y));
			}
		}
	}

	p = ROUND(Ry2*(x+.5)*(x+.5) + Rx2*(y-1)*(y-1) - Rx2*Ry2);
	while (y>0)
	{
		y--;
		py -= twoRx2;
		if (p>0)
			p += Rx2 - py;
		else
		{
			x++;
			px += twoRy2;
			p += Rx2 - py +px;
		}

		if ( useQuad1 )
			(driver->*setLine)(ROUND(xc),ROUND(xc+x),ROUND(yc-y));
		if ( useQuad2 )
			(driver->*setLine)(ROUND(xc),ROUND(xc-x),ROUND(yc-y));
		if ( useQuad3 )
			(driver->*setLine)(ROUND(xc),ROUND(xc-x),ROUND(yc+y));
		if ( useQuad4 )
			(driver->*setLine)(ROUND(xc),ROUND(xc+x),ROUND(yc+y));
		if ( !shortspan )
		{
			if ( startQuad == 1 )
			{
				if ( x <= startx )
					(driver->*setLine)(ROUND(xc),ROUND(xc+x),ROUND(yc-y));
				else
				{
					xclip = ROUND(y*startx/(double)starty);
					(driver->*setLine)(ROUND(xc),ROUND(xc+xclip),ROUND(yc-y));
				}
			}
			else if ( startQuad == 2 )
			{
				if ( x >= startx )
				{
					xclip = ROUND(y*startx/(double)starty);
					(driver->*setLine)(ROUND(xc-x),ROUND(xc-xclip),ROUND(yc-y));
				}
			}
			else if ( startQuad == 3 )
			{
				if ( x <= startx )
					(driver->*setLine)(ROUND(xc-x),ROUND(xc),ROUND(yc+y));
				else
				{
					xclip = ROUND(y*startx/(double)starty);
					(driver->*setLine)(ROUND(xc-xclip),ROUND(xc),ROUND(yc+y));
				}
			}
			else if ( startQuad == 4 )
			{
				if ( x >= startx )
				{
					xclip = ROUND(y*startx/(double)starty);
					(driver->*setLine)(ROUND(xc+xclip),ROUND(xc+x),ROUND(yc+y));
				}
			}

			if ( endQuad == 1 )
			{
				if ( x >= endx )
				{
					xclip = ROUND(y*endx/(double)endy);
					(driver->*setLine)(ROUND(xc+xclip),ROUND(xc+x),ROUND(yc-y));
				}
			}
			else if ( endQuad == 2 )
			{
				if ( x <= endx )
					(driver->*setLine)(ROUND(xc-x),ROUND(xc),ROUND(yc-y));
				else
				{
					xclip = ROUND(y*endx/(double)endy);
					(driver->*setLine)(ROUND(xc-xclip),ROUND(xc),ROUND(yc-y));
				}
			}
			else if ( endQuad == 3 )
			{
				if ( x >= endx )
				{
					xclip = ROUND(y*endx/(double)endy);
					(driver->*setLine)(ROUND(xc-x),ROUND(xc-xclip),ROUND(yc+y));
				}
			}
			else if ( endQuad == 4 )
			{
				if ( x <= endx )
					(driver->*setLine)(ROUND(xc),ROUND(xc+x),ROUND(yc+y));
				else
				{
					xclip = ROUND(y*endx/(double)endy);
					(driver->*setLine)(ROUND(xc),ROUND(xc+xclip),ROUND(yc+y));
				}
			}
		}
		else
		{
			startclip = ROUND(y*startx/(double)starty);
			endclip = ROUND(y*endx/(double)endy);
			if ( startQuad == 1 )
			{
				if ( (x <= startx) && (x >= endx) )
					(driver->*setLine)(ROUND(xc+endclip),ROUND(xc+x),ROUND(yc-y));
				else
					(driver->*setLine)(ROUND(xc+endclip),ROUND(xc+startclip),ROUND(yc-y));
			}
			else if ( startQuad == 2 )
			{
				if ( (x <= startx) && (x >= endx) )
					(driver->*setLine)(ROUND(xc-x),ROUND(xc-startclip),ROUND(yc-y));
				else
					(driver->*setLine)(ROUND(xc-endclip),ROUND(xc-startclip),ROUND(yc-y));
			}
			else if ( startQuad == 3 )
			{
				if ( (x <= startx) && (x >= endx) )
					(driver->*setLine)(ROUND(xc-x),ROUND(xc-endclip),ROUND(yc+y));
				else
					(driver->*setLine)(ROUND(xc-startclip),ROUND(xc-endclip),ROUND(yc+y));
			}
			else if ( startQuad == 4 )
			{
				if ( (x <= startx) && (x >= endx) )
					(driver->*setLine)(ROUND(xc+startclip),ROUND(xc+x),ROUND(yc+y));
				else
					(driver->*setLine)(ROUND(xc+startclip),ROUND(xc+endclip),ROUND(yc+y));
			}
		}
	}
	Unlock();
}

void DisplayDriver::FillBezier(BPoint *pts, const RGBColor& color)
{
}

void DisplayDriver::FillBezier(BPoint *pts, const Pattern& pattern, const RGBColor& high_color, const RGBColor& low_color)
{
}

/*!
	\brief Called for all BView::FillBezier calls.
	\param pts 4-element array of BPoints in the order of start, end, and then the two control
	points. 
	\param setLine Horizontal ine drawing routine which handles things like color and pattern.

	Does not work quite right, need to redo this.
*/
void DisplayDriver::FillBezier(BPoint *pts, DisplayDriver* driver, SetHorizontalLineFuncType setLine)
{
  /*
	double Ax, Bx, Cx, Dx;
	double Ay, By, Cy, Dy;
	int x, y;
	int lastx=-1, lasty=-1;
	double t;
	double dt = .0002;
	double dt2, dt3;
	double X, Y, dx, ddx, dddx, dy, ddy, dddy;
	float oldpensize;
	bool steep = false;

	Lock();
	if ( fabs(pts[3].y-pts[0].y) > fabs(pts[3].x-pts[0].x) )
		steep = true;
	
	LineCalc line(pts[0], pts[3]);
	oldpensize = d->pensize;
	d->pensize = 1;

	Ax = -pts[0].x + 3*pts[1].x - 3*pts[2].x + pts[3].x;
	Bx = 3*pts[0].x - 6*pts[1].x + 3*pts[2].x;
	Cx = -3*pts[0].x + 3*pts[1].x;
	Dx = pts[0].x;

	Ay = -pts[0].y + 3*pts[1].y - 3*pts[2].y + pts[3].y;
	By = 3*pts[0].y - 6*pts[1].y + 3*pts[2].y;
	Cy = -3*pts[0].y + 3*pts[1].y;
	Dy = pts[0].y;
	
	dt2 = dt * dt;
	dt3 = dt2 * dt;
	X = Dx;
	dx = Ax*dt3 + Bx*dt2 + Cx*dt;
	ddx = 6*Ax*dt3 + 2*Bx*dt2;
	dddx = 6*Ax*dt3;
	Y = Dy;
	dy = Ay*dt3 + By*dt2 + Cy*dt;
	ddy = 6*Ay*dt3 + 2*By*dt2;
	dddy = 6*Ay*dt3;

	lastx = -1;
	lasty = -1;

	for (t=0; t<=1; t+=dt)
	{
		x = ROUND(X);
		y = ROUND(Y);
		if ( (x!=lastx) || (y!=lasty) )
		{
			if ( steep )
				StrokeLine(BPoint(x,y),BPoint(line.GetX(y),y),d,pat);
			else
				StrokeLine(BPoint(x,y),BPoint(x,line.GetY(x)),d,pat);
		}
		lastx = x;
		lasty = y;

		X += dx;
		dx += ddx;
		ddx += dddx;
		Y += dy;
		dy += ddy;
		ddy += dddy;
	}
	d->pensize = oldpensize;
	Unlock();
	*/
}

void DisplayDriver::FillEllipse(BRect r, const RGBColor& color)
{
}

void DisplayDriver::FillEllipse(BRect r, const Pattern& pattern, const RGBColor& high_color, const RGBColor& low_color)
{
}

/*!
	\brief Called for all BView::FillEllipse calls
	\param r BRect enclosing the ellipse to be drawn.
	\param setLine Horizontal line drawing routine which handles things like color and pattern.
*/
void DisplayDriver::FillEllipse(BRect r, DisplayDriver* driver, SetHorizontalLineFuncType setLine)
{
	float xc = (r.left+r.right)/2;
	float yc = (r.top+r.bottom)/2;
	float rx = r.Width()/2;
	float ry = r.Height()/2;
	int Rx2 = ROUND(rx*rx);
	int Ry2 = ROUND(ry*ry);
	int twoRx2 = 2*Rx2;
	int twoRy2 = 2*Ry2;
	int p;
	int x=0;
	int y = (int)ry;
	int px = 0;
	int py = twoRx2 * y;

	Lock();

	//SetPixel(ROUND(xc),ROUND(yc-y),pattern.GetColor(xc,yc-y));
	(driver->*setLine)(ROUND(xc),ROUND(xc),ROUND(yc-y));
	//SetPixel(ROUND(xc),ROUND(yc+y),pattern.GetColor(xc,yc+y));
	(driver->*setLine)(ROUND(xc),ROUND(xc),ROUND(yc+y));

	p = ROUND (Ry2 - (Rx2 * ry) + (.25 * Rx2));
	while (px < py)
	{
		x++;
		px += twoRy2;
		if ( p < 0 )
			p += Ry2 + px;
		else
		{
			y--;
			py -= twoRx2;
			p += Ry2 + px - py;
		}

               	(driver->*setLine)(ROUND(xc-x),ROUND(xc+x),ROUND(yc-y));
                (driver->*setLine)(ROUND(xc-x),ROUND(xc+x),ROUND(yc+y));
	}

	p = ROUND(Ry2*(x+.5)*(x+.5) + Rx2*(y-1)*(y-1) - Rx2*Ry2);
	while (y>0)
	{
		y--;
		py -= twoRx2;
		if (p>0)
			p += Rx2 - py;
		else
		{
			x++;
			px += twoRy2;
			p += Rx2 - py +px;
		}

               	(driver->*setLine)(ROUND(xc-x),ROUND(xc+x),ROUND(yc-y));
                (driver->*setLine)(ROUND(xc-x),ROUND(xc+x),ROUND(yc+y));
	}
	Unlock();
}

void DisplayDriver::FillPolygon(BPoint *ptlist, int32 numpts, const RGBColor& color)
{
}

void DisplayDriver::FillPolygon(BPoint *ptlist, int32 numpts, const Pattern& pattern, const RGBColor& high_color, const RGBColor& low_color)
{
}

/*!
	\brief Called for all BView::FillPolygon calls
	\param ptlist Array of BPoints defining the polygon.
	\param numpts Number of points in the BPoint array.
	\param setLine Horizontal line drawing routine which handles things like color and pattern.

	The points in the array are not guaranteed to be within the framebuffer's 
	coordinate range.
*/
void DisplayDriver::FillPolygon(BPoint *ptlist, int32 numpts, DisplayDriver* driver, SetHorizontalLineFuncType setLine)
{
	/* Here's the plan.  Record all line segments in polygon.  If a line segments crosses
	   the y-value of a point not in the segment, split the segment into 2 segments.
	   Once we have gone through all of the segments, sort them primarily on y-value
	   and secondarily on x-value.  Step through each y-value in the bounding rectangle
	   and look for intersections with line segments.  First intersection is start of
	   horizontal line, second intersection is end of horizontal line.  Continue for
	   all pairs of intersections.  Watch out for horizontal line segments.
	*/
	Lock();
	if ( !ptlist || (numpts < 3) )
	{
		Unlock();
		return;
	}

	BPoint *currentPoint, *nextPoint;
	BPoint tempNextPoint;
	BPoint tempCurrentPoint;
	int currentIndex, bestIndex, i, j, y;
	LineCalc *segmentArray = new LineCalc[2*numpts];
	int numSegments = 0;

	/* Generate the segment list */
	currentPoint = ptlist;
	currentIndex = 0;
	nextPoint = &ptlist[1];
	while (currentPoint)
	{
		if ( numSegments >= 2*numpts )
		{
			printf("ERROR: Insufficient memory allocated to segment array\n");
			delete[] segmentArray;
			return;
		}

		for (i=0; i<numpts; i++)
		{
			if ( ((ptlist[i].y > currentPoint->y) && (ptlist[i].y < nextPoint->y)) ||
				((ptlist[i].y < currentPoint->y) && (ptlist[i].y > nextPoint->y)) )
			{
				segmentArray[numSegments].SetPoints(*currentPoint,*nextPoint);
				tempNextPoint.x = segmentArray[numSegments].GetX(ptlist[i].y);
				tempNextPoint.y = ptlist[i].y;
				nextPoint = &tempNextPoint;
			}
		}

		segmentArray[numSegments].SetPoints(*currentPoint,*nextPoint);
		numSegments++;
		if ( nextPoint == &tempNextPoint )
		{
			tempCurrentPoint = tempNextPoint;
			currentPoint = &tempCurrentPoint;
			nextPoint = &ptlist[(currentIndex+1)%numpts];
		}
		else if ( nextPoint == ptlist )
		{
			currentPoint = NULL;
		}
		else
		{
			currentPoint = nextPoint;
			currentIndex++;
			nextPoint = &ptlist[(currentIndex+1)%numpts];
		}
	}

	/* Selection sort the segments.  Probably should replace this later. */
	for (i=0; i<numSegments; i++)
	{
		bestIndex = i;
		for (j=i+1; j<numSegments; j++)
		{
			if ( (segmentArray[j].MinY() < segmentArray[bestIndex].MinY()) ||
				((segmentArray[j].MinY() == segmentArray[bestIndex].MinY()) &&
				 (segmentArray[j].MinX() < segmentArray[bestIndex].MinX())) )
				bestIndex = j;
		}
		if (bestIndex != i)
			segmentArray[i].Swap(segmentArray[bestIndex]);
	}

	/* Draw the lines */
	for (y=(int)segmentArray[0].MinY(); y<=segmentArray[numSegments-1].MaxY(); y++)
	{
		i = 0;
		while (i<numSegments)
		{
			if (segmentArray[i].MinY() > y)
				break;
			if (segmentArray[i].MaxY() < y)
			{
				i++;
				continue;
			}
			if (segmentArray[i].MinY() == segmentArray[i].MaxY())
			{
				if ( (segmentArray[i].MinX() < _buffer_width) &&
					(segmentArray[i].MaxX() >= 0) )
					(driver->*setLine)(ROUND(segmentArray[i].MinX()),
					      ROUND(segmentArray[i].MaxX()), y);
				i++;
			}
			else
			{
				if ( (segmentArray[i].GetX(y) < _buffer_width) &&
					(segmentArray[i+1].GetX(y) >= 0) )
					(driver->*setLine)(ROUND(segmentArray[i].GetX(y)),
					      ROUND(segmentArray[i+1].GetX(y)), y);
				i+=2;
			}
		}
	}
	delete[] segmentArray;
	Unlock();
}

/*!
	\brief Called for all BView::FillRect calls
	\param r BRect to be filled. Guaranteed to be in the frame buffer's coordinate space
	\param color The color used to fill the rectangle
*/
void DisplayDriver::FillRect(const BRect r, const RGBColor& color)
{
}

/*!
	\brief Called for all BView::FillRect calls
	\param r BRect to be filled. Guaranteed to be in the frame buffer's coordinate space
	\param pattern The pattern used to fill the rectangle
	\param high_color The high color of the pattern
	\param low_color The low color of the pattern
*/
void DisplayDriver::FillRect(const BRect r, const Pattern& pattern, const RGBColor& high_color, const RGBColor& low_color)
{
}

/*!
	\brief Convenience function for server use
	\param r BRegion to be filled
	\param color The color used to fill the region
*/
void DisplayDriver::FillRegion(BRegion& r, const RGBColor& color)
{
	Lock();

	for(int32 i=0; i<r.CountRects();i++)
		FillRect(r.RectAt(i),color);

	Unlock();
}

/*!
	\brief Convenience function for server use
	\param r BRegion to be filled
	\param pattern The pattern used to fill the region
	\param high_color The high color of the pattern
	\param low_color The low color of the pattern
*/
void DisplayDriver::FillRegion(BRegion& r, const Pattern& pattern, const RGBColor& high_color, const RGBColor& low_color)
{
	Lock();

	for(int32 i=0; i<r.CountRects();i++)
		FillRect(r.RectAt(i),pattern, high_color, low_color);

	Unlock();
}

void DisplayDriver::FillRoundRect(BRect r, float xrad, float yrad, const RGBColor& color)
{
}

void DisplayDriver::FillRoundRect(BRect r, float xrad, float yrad, const Pattern& pattern, const RGBColor& high_color, const RGBColor& low_color)
{
}

/*!
	\brief Called for all BView::FillRoundRect calls
	\param r The rectangle itself
	\param xrad X radius of the corner arcs
	\param yrad Y radius of the corner arcs
	\param setRect Rectangle filling routine which handles things like color and pattern
	\param setLine Horizontal line drawing function which handles things like color and pattern
*/
void DisplayDriver::FillRoundRect(BRect r, float xrad, float yrad, DisplayDriver* driver, SetRectangleFuncType setRect, SetHorizontalLineFuncType setLine)
{
	float arc_x;
	float yrad2 = yrad*yrad;
	int i;

	for (i=0; i<=(int)yrad; i++)
	{
		arc_x = xrad*sqrt(1-i*i/yrad2);
		(driver->*setLine)(ROUND(r.left+xrad-arc_x), ROUND(r.right-xrad+arc_x),
			ROUND(r.top+yrad-i));
		(driver->*setLine)(ROUND(r.left+xrad-arc_x), ROUND(r.right-xrad+arc_x),
			ROUND(r.bottom-yrad+i));
	}
	(driver->*setRect)((int)(r.left),(int)(r.top+yrad),(int)(r.right),(int)(r.bottom-yrad));
}

//void DisplayDriver::FillShape(SShape *sh, LayerData *d, const Pattern &pat)
//{
//}

void DisplayDriver::FillTriangle(BPoint *pts, const RGBColor& color)
{
}

void DisplayDriver::FillTriangle(BPoint *pts, const Pattern& pattern, const RGBColor& high_color, const RGBColor& low_color)
{
}

/*!
	\brief Called for all BView::FillTriangle calls
	\param pts Array of 3 BPoints. Always non-NULL.
	\param setLine Horizontal line drawing routine which handles things like color and pattern.
*/
void DisplayDriver::FillTriangle(BPoint *pts, DisplayDriver* driver, SetHorizontalLineFuncType setLine)
{
	if ( !pts )
		return;

	BPoint first, second, third;

	// Sort points according to their y values and x values (y is primary)
	if ( (pts[0].y < pts[1].y) ||
		((pts[0].y == pts[1].y) && (pts[0].x <= pts[1].x)) )
	{
		first=pts[0];
		second=pts[1];
	}
	else
	{
		first=pts[1];
		second=pts[0];
	}
	
	if ( (second.y<pts[2].y) ||
		((second.y == pts[2].y) && (second.x <= pts[2].x)) )
	{
		third=pts[2];
	}
	else
	{
		// second is lower than "third", so we must ensure that this third point
		// isn't higher than our first point
		third=second;
		if ( (first.y<pts[2].y) ||
			((first.y == pts[2].y) && (first.x <= pts[2].x)) )
			second=pts[2];
		else
		{
			second=first;
			first=pts[2];
		}
	}
	
	// Now that the points are sorted, check to see if they all have the same
	// y value
	if(first.y==second.y && second.y==third.y)
	{
		BPoint start,end;
		start.y=first.y;
		end.y=first.y;
		start.x=MIN(first.x,MIN(second.x,third.x));
		end.x=MAX(first.x,MAX(second.x,third.x));
		(driver->*setLine)(ROUND(start.x), ROUND(end.x), ROUND(start.y));
		return;
	}

	int32 i;

	// Special case #1: first and second in the same row
	if(first.y==second.y)
	{
		LineCalc lineA(first, third);
		LineCalc lineB(second, third);
		
		(driver->*setLine)(ROUND(first.x), ROUND(second.x), ROUND(first.y));
		for(i=(int32)first.y+1; i<=third.y; i++)
			(driver->*setLine)(ROUND(lineA.GetX(i)), ROUND(lineB.GetX(i)), i);
		return;
	}
	
	// Special case #2: second and third in the same row
	if(second.y==third.y)
	{
		LineCalc lineA(first, second);
		LineCalc lineB(first, third);
		
		(driver->*setLine)(ROUND(second.x), ROUND(third.x), ROUND(second.y));
		for(i=(int32)first.y; i<third.y; i++)
			(driver->*setLine)(ROUND(lineA.GetX(i)), ROUND(lineB.GetX(i)), i);
		return;
	}
	
	// Normal case.	
	LineCalc lineA(first, second);
	LineCalc lineB(first, third);
	LineCalc lineC(second, third);
	
	for(i=(int32)first.y; i<(int32)second.y; i++)
		(driver->*setLine)(ROUND(lineA.GetX(i)), ROUND(lineB.GetX(i)), i);

	for(i=(int32)second.y; i<=third.y; i++)
		(driver->*setLine)(ROUND(lineC.GetX(i)), ROUND(lineB.GetX(i)), i);
}

/*!
	\brief Hides the cursor.
	
	Hide calls are not nestable, unlike that of the BApplication class. Subclasses should
	call _SetCursorHidden(true) somewhere within this function to ensure that data is
	maintained accurately. Subclasses must include a call to DisplayDriver::HideCursor
	for proper state tracking.
*/
void DisplayDriver::HideCursor(void)
{
	_is_cursor_hidden=true;
}

/*!
	\brief Returns whether the cursor is visible or not.
	\return true if hidden or obscured, false if not.

*/
bool DisplayDriver::IsCursorHidden(void)
{
	Lock();

	bool value=(_is_cursor_hidden || _is_cursor_obscured);

	Unlock();

	return value;
}

/*!
	\brief Moves the cursor to the given point.

	The coordinates passed to MoveCursorTo are guaranteed to be within the frame buffer's
	range, but the cursor data itself will need to be clipped. A check to see if the 
	cursor is obscured should be made and if so, a call to _SetCursorObscured(false) 
	should be made the cursor in addition to displaying at the passed coordinates.
*/
void DisplayDriver::MoveCursorTo(float x, float y)
{
}

/*!
	\brief Inverts the colors in the rectangle.
	\param r Rectangle of the area to be inverted. Guaranteed to be within bounds.
*/
void DisplayDriver::InvertRect(BRect r)
{
}

/*!
	\brief Shows the cursor.
	
	Show calls are not nestable, unlike that of the BApplication class. Subclasses should
	call _SetCursorHidden(false) somewhere within this function to ensure that data is
	maintained accurately. Subclasses must call DisplayDriver::ShowCursor at some point
	to ensure proper state tracking.
*/
void DisplayDriver::ShowCursor(void)
{
	_is_cursor_hidden=false;
	_is_cursor_obscured=false;
}

/*!
	\brief Obscures the cursor.
	
	Obscure calls are not nestable. Subclasses should call DisplayDriver::ObscureCursor
	somewhere within this function to ensure that data is maintained accurately. A check
	will be made by the system before the next MoveCursorTo call to show the cursor if
	it is obscured.
*/
void DisplayDriver::ObscureCursor(void)
{
	_is_cursor_obscured=true;
}

/*!
	\brief Changes the cursor.
	\param cursor The new cursor. Guaranteed to be non-NULL.
	
	The driver does not take ownership of the given cursor. Subclasses should make
	a copy of the cursor passed to it. The default version of this function hides the
	cursory, replaces it, and shows the cursor if previously visible.
*/
void DisplayDriver::SetCursor(ServerCursor *cursor)
{
	Lock();

	bool hidden=_is_cursor_hidden;
	bool obscured=_is_cursor_obscured;
	if(_cursor)
		delete _cursor;
	_cursor=new ServerCursor(cursor);

	if(!hidden && !obscured)
		ShowCursor();

	Unlock();
}

void DisplayDriver::StrokeArc(BRect r, float angle, float span, float pensize, const RGBColor& color)
{
}

void DisplayDriver::StrokeArc(BRect r, float angle, float span, float pensize, const Pattern& pattern, const RGBColor& high_color, const RGBColor& low_color)
{
}

/*!
	\brief Called for all BView::StrokeArc calls
	\param r Rectangle enclosing the entire arc
	\param angle Starting angle for the arc in degrees
	\param span Span of the arc in degrees. Ending angle = angle+span.
	\param setPixel Pixel drawing function which handles things like size and pattern.
*/
void DisplayDriver::StrokeArc(BRect r, float angle, float span, DisplayDriver* driver, SetPixelFuncType setPixel)
{
	float xc = (r.left+r.right)/2;
	float yc = (r.top+r.bottom)/2;
	float rx = r.Width()/2;
	float ry = r.Height()/2;
	int Rx2 = ROUND(rx*rx);
	int Ry2 = ROUND(ry*ry);
	int twoRx2 = 2*Rx2;
	int twoRy2 = 2*Ry2;
	int p;
	int x=0;
	int y = (int)ry;
	int px = 0;
	int py = twoRx2 * y;
	int startx, endx;
	int startQuad, endQuad;
	bool useQuad1, useQuad2, useQuad3, useQuad4;
	bool shortspan = false;

	// Watch out for bozos giving us whacko spans
	if ( (span >= 360) || (span <= -360) )
	{
	  StrokeEllipse(r,driver,setPixel);
	  return;
	}

	if ( span > 0 )
	{
		startQuad = (int)(angle/90)%4+1;
		endQuad = (int)((angle+span)/90)%4+1;
		startx = ROUND(.5*r.Width()*fabs(cos(angle*M_PI/180)));
		endx = ROUND(.5*r.Width()*fabs(cos((angle+span)*M_PI/180)));
	}
	else
	{
		endQuad = (int)(angle/90)%4+1;
		startQuad = (int)((angle+span)/90)%4+1;
		endx = ROUND(.5*r.Width()*fabs(cos(angle*M_PI/180)));
		startx = ROUND(.5*r.Width()*fabs(cos((angle+span)*M_PI/180)));
	}

	if ( startQuad != endQuad )
	{
		useQuad1 = (endQuad > 1) && (startQuad > endQuad);
		useQuad2 = ((startQuad == 1) && (endQuad > 2)) || ((startQuad > endQuad) && (endQuad > 2));
		useQuad3 = ((startQuad < 3) && (endQuad == 4)) || ((startQuad < 3) && (endQuad < startQuad));
		useQuad4 = (startQuad < 4) && (startQuad > endQuad);
	}
	else
	{
		if ( (span < 90) && (span > -90) )
		{
			useQuad1 = false;
			useQuad2 = false;
			useQuad3 = false;
			useQuad4 = false;
			shortspan = true;
		}
		else
		{
			useQuad1 = (startQuad != 1);
			useQuad2 = (startQuad != 2);
			useQuad3 = (startQuad != 3);
			useQuad4 = (startQuad != 4);
		}
	}

	if ( useQuad1 || 
	     (!shortspan && (((startQuad == 1) && (x <= startx)) || ((endQuad == 1) && (x >= endx)))) || 
	     (shortspan && (startQuad == 1) && (x <= startx) && (x >= endx)) ) 
		(driver->*setPixel)(ROUND(xc+x),ROUND(yc-y));
	if ( useQuad2 || 
	     (!shortspan && (((startQuad == 2) && (x >= startx)) || ((endQuad == 2) && (x <= endx)))) || 
	     (shortspan && (startQuad == 2) && (x >= startx) && (x <= endx)) ) 
		(driver->*setPixel)(ROUND(xc-x),ROUND(yc-y));
	if ( useQuad3 || 
	     (!shortspan && (((startQuad == 3) && (x <= startx)) || ((endQuad == 3) && (x >= endx)))) || 
	     (shortspan && (startQuad == 3) && (x <= startx) && (x >= endx)) ) 
		(driver->*setPixel)(ROUND(xc-x),ROUND(yc+y));
	if ( useQuad4 || 
	     (!shortspan && (((startQuad == 4) && (x >= startx)) || ((endQuad == 4) && (x <= endx)))) || 
	     (shortspan && (startQuad == 4) && (x >= startx) && (x <= endx)) ) 
		(driver->*setPixel)(ROUND(xc+x),ROUND(yc+y));

	p = ROUND (Ry2 - (Rx2 * ry) + (.25 * Rx2));
	while (px < py)
	{
		x++;
		px += twoRy2;
		if ( p < 0 )
			p += Ry2 + px;
		else
		{
			y--;
			py -= twoRx2;
			p += Ry2 + px - py;
		}

		if ( useQuad1 || 
		     (!shortspan && (((startQuad == 1) && (x <= startx)) || ((endQuad == 1) && (x >= endx)))) || 
		     (shortspan && (startQuad == 1) && (x <= startx) && (x >= endx)) ) 
			(driver->*setPixel)(ROUND(xc+x),ROUND(yc-y));
		if ( useQuad2 || 
		     (!shortspan && (((startQuad == 2) && (x >= startx)) || ((endQuad == 2) && (x <= endx)))) || 
		     (shortspan && (startQuad == 2) && (x >= startx) && (x <= endx)) ) 
			(driver->*setPixel)(ROUND(xc-x),ROUND(yc-y));
		if ( useQuad3 || 
		     (!shortspan && (((startQuad == 3) && (x <= startx)) || ((endQuad == 3) && (x >= endx)))) || 
		     (shortspan && (startQuad == 3) && (x <= startx) && (x >= endx)) ) 
			(driver->*setPixel)(ROUND(xc-x),ROUND(yc+y));
		if ( useQuad4 || 
		     (!shortspan && (((startQuad == 4) && (x >= startx)) || ((endQuad == 4) && (x <= endx)))) || 
		     (shortspan && (startQuad == 4) && (x >= startx) && (x <= endx)) ) 
			(driver->*setPixel)(ROUND(xc+x),ROUND(yc+y));
	}

	p = ROUND(Ry2*(x+.5)*(x+.5) + Rx2*(y-1)*(y-1) - Rx2*Ry2);
	while (y>0)
	{
		y--;
		py -= twoRx2;
		if (p>0)
			p += Rx2 - py;
		else
		{
			x++;
			px += twoRy2;
			p += Rx2 - py +px;
		}

		if ( useQuad1 || 
		     (!shortspan && (((startQuad == 1) && (x <= startx)) || ((endQuad == 1) && (x >= endx)))) || 
		     (shortspan && (startQuad == 1) && (x <= startx) && (x >= endx)) ) 
			(driver->*setPixel)(ROUND(xc+x),ROUND(yc-y));
		if ( useQuad2 || 
		     (!shortspan && (((startQuad == 2) && (x >= startx)) || ((endQuad == 2) && (x <= endx)))) || 
		     (shortspan && (startQuad == 2) && (x >= startx) && (x <= endx)) ) 
			(driver->*setPixel)(ROUND(xc-x),ROUND(yc-y));
		if ( useQuad3 || 
		     (!shortspan && (((startQuad == 3) && (x <= startx)) || ((endQuad == 3) && (x >= endx)))) || 
		     (shortspan && (startQuad == 3) && (x <= startx) && (x >= endx)) ) 
			(driver->*setPixel)(ROUND(xc-x),ROUND(yc+y));
		if ( useQuad4 || 
		     (!shortspan && (((startQuad == 4) && (x >= startx)) || ((endQuad == 4) && (x <= endx)))) || 
		     (shortspan && (startQuad == 4) && (x >= startx) && (x <= endx)) ) 
			(driver->*setPixel)(ROUND(xc+x),ROUND(yc+y));
	}
}


void DisplayDriver::StrokeBezier(BPoint *pts, float pensize, const RGBColor& color)
{
}

void DisplayDriver::StrokeBezier(BPoint *pts, float pensize, const Pattern& pattern, const RGBColor& high_color, const RGBColor& low_color)
{
}

/*!
	\brief Called for all BView::StrokeBezier calls.
	\param pts 4-element array of BPoints in the order of start, end, and then the two control
	points. 
	\param setPixel Pixel drawing function which handles things like size and pattern.
*/
void DisplayDriver::StrokeBezier(BPoint *pts, DisplayDriver* driver, SetPixelFuncType setPixel)
{
	double Ax, Bx, Cx, Dx;
	double Ay, By, Cy, Dy;
	int x, y;
	int lastx=-1, lasty=-1;
	double t;
	double dt = .0005;
	double dt2, dt3;
	double X, Y, dx, ddx, dddx, dy, ddy, dddy;

	Ax = -pts[0].x + 3*pts[1].x - 3*pts[2].x + pts[3].x;
	Bx = 3*pts[0].x - 6*pts[1].x + 3*pts[2].x;
	Cx = -3*pts[0].x + 3*pts[1].x;
	Dx = pts[0].x;

	Ay = -pts[0].y + 3*pts[1].y - 3*pts[2].y + pts[3].y;
	By = 3*pts[0].y - 6*pts[1].y + 3*pts[2].y;
	Cy = -3*pts[0].y + 3*pts[1].y;
	Dy = pts[0].y;
	
	dt2 = dt * dt;
	dt3 = dt2 * dt;
	X = Dx;
	dx = Ax*dt3 + Bx*dt2 + Cx*dt;
	ddx = 6*Ax*dt3 + 2*Bx*dt2;
	dddx = 6*Ax*dt3;
	Y = Dy;
	dy = Ay*dt3 + By*dt2 + Cy*dt;
	ddy = 6*Ay*dt3 + 2*By*dt2;
	dddy = 6*Ay*dt3;

	lastx = -1;
	lasty = -1;

	for (t=0; t<=1; t+=dt)
	{
		x = ROUND(X);
		y = ROUND(Y);
		if ( (x!=lastx) || (y!=lasty) )
			(driver->*setPixel)(x,y);
		lastx = x;
		lasty = y;

		X += dx;
		dx += ddx;
		ddx += dddx;
		Y += dy;
		dy += ddy;
		ddy += dddy;
	}
}

void DisplayDriver::StrokeEllipse(BRect r, float pensize, const RGBColor& color)
{
}

void DisplayDriver::StrokeEllipse(BRect r, float pensize, const Pattern& pattern, const RGBColor& high_color, const RGBColor& low_color)
{
}

/*!
	\brief Called for all BView::StrokeEllipse calls
	\param r BRect enclosing the ellipse to be drawn.
	\param setPixel Pixel drawing function which handles things like size and pattern.
*/
void DisplayDriver::StrokeEllipse(BRect r, DisplayDriver* driver, SetPixelFuncType setPixel)
{
	float xc = (r.left+r.right)/2;
	float yc = (r.top+r.bottom)/2;
	float rx = r.Width()/2;
	float ry = r.Height()/2;
	int Rx2 = ROUND(rx*rx);
	int Ry2 = ROUND(ry*ry);
	int twoRx2 = 2*Rx2;
	int twoRy2 = 2*Ry2;
	int p;
	int x=0;
	int y = (int)ry;
	int px = 0;
	int py = twoRx2 * y;

	(driver->*setPixel)(ROUND(xc+x),ROUND(yc-y));
	(driver->*setPixel)(ROUND(xc-x),ROUND(yc-y));
	(driver->*setPixel)(ROUND(xc-x),ROUND(yc+y));
	(driver->*setPixel)(ROUND(xc+x),ROUND(yc+y));

	p = ROUND (Ry2 - (Rx2 * ry) + (.25 * Rx2));
	while (px < py)
	{
		x++;
		px += twoRy2;
		if ( p < 0 )
			p += Ry2 + px;
		else
		{
			y--;
			py -= twoRx2;
			p += Ry2 + px - py;
		}

		(driver->*setPixel)(ROUND(xc+x),ROUND(yc-y));
		(driver->*setPixel)(ROUND(xc-x),ROUND(yc-y));
		(driver->*setPixel)(ROUND(xc-x),ROUND(yc+y));
		(driver->*setPixel)(ROUND(xc+x),ROUND(yc+y));
	}

	p = ROUND(Ry2*(x+.5)*(x+.5) + Rx2*(y-1)*(y-1) - Rx2*Ry2);
	while (y>0)
	{
		y--;
		py -= twoRx2;
		if (p>0)
			p += Rx2 - py;
		else
		{
			x++;
			px += twoRy2;
			p += Rx2 - py +px;
		}

		(driver->*setPixel)(ROUND(xc+x),ROUND(yc-y));
		(driver->*setPixel)(ROUND(xc-x),ROUND(yc-y));
		(driver->*setPixel)(ROUND(xc-x),ROUND(yc+y));
		(driver->*setPixel)(ROUND(xc+x),ROUND(yc+y));
	}
}

void DisplayDriver::StrokeLine(BPoint start, BPoint end, float pensize, const RGBColor& color)
{
}

void DisplayDriver::StrokeLine(BPoint start, BPoint end, float pensize, const Pattern& pattern, const RGBColor& high_color, const RGBColor& low_color)
{
}

/*!
	\brief Draws a line. Really.
	\param start Starting point
	\param end Ending point
	\param setPixel Pixel drawing function which handles things like size and pattern.
*/
void DisplayDriver::StrokeLine(BPoint start, BPoint end, DisplayDriver* driver, SetPixelFuncType setPixel)
{
	int x1 = ROUND(start.x);
	int y1 = ROUND(start.y);
	int x2 = ROUND(end.x);
	int y2 = ROUND(end.y);
	int dx = x2 - x1;
	int dy = y2 - y1;
	int steps, k;
	double xInc, yInc;
	double x = x1;
	double y = y1;

	if ( abs(dx) > abs(dy) )
		steps = abs(dx);
	else
		steps = abs(dy);
	xInc = dx / (double) steps;
	yInc = dy / (double) steps;

	(driver->*setPixel)(ROUND(x),ROUND(y));
	for (k=0; k<steps; k++)
	{
		x += xInc;
		y += yInc;
		(driver->*setPixel)(ROUND(x),ROUND(y));
	}
}

void DisplayDriver::StrokePoint(BPoint& pt, const RGBColor& color)
{
}

void DisplayDriver::StrokePolygon(BPoint *ptlist, int32 numpts, float pensize, const RGBColor& color, bool is_closed)
{
}

void DisplayDriver::StrokePolygon(BPoint *ptlist, int32 numpts, float pensize, const Pattern& pattern, const RGBColor& high_color, const RGBColor& low_color, bool is_closed)
{
}

/*!
	\brief Called for all BView::StrokePolygon calls
	\param ptlist Array of BPoints defining the polygon.
	\param numpts Number of points in the BPoint array.
	\param setPixel Pixel drawing function which handles things like size and pattern.
*/
void DisplayDriver::StrokePolygon(BPoint *ptlist, int32 numpts, DisplayDriver* driver, SetPixelFuncType setPixel, bool is_closed)
{
	for(int32 i=0; i<(numpts-1); i++)
		StrokeLine(ptlist[i],ptlist[i+1],driver,setPixel);
	if(is_closed)
		StrokeLine(ptlist[numpts-1],ptlist[0],driver,setPixel);
}

/*!
	\brief Called for all BView::StrokeRect calls
	\param r BRect to be drawn
	\param pensize Thickness of the lines
	\param color The color of the rectangle
*/
void DisplayDriver::StrokeRect(BRect r, float pensize, const RGBColor& color)
{
}

void DisplayDriver::StrokeRect(BRect r, float pensize, const Pattern& pattern, const RGBColor& high_color, const RGBColor& low_color)
{
}

void DisplayDriver::StrokeRect(BRect r, DisplayDriver* driver, SetHorizontalLineFuncType setHLine, SetVerticalLineFuncType setVLine)
{
	(driver->*setHLine)((int)ROUND(r.left), (int)ROUND(r.right), (int)ROUND(r.top));
	(driver->*setVLine)((int)ROUND(r.right), (int)ROUND(r.top), (int)ROUND(r.bottom));
	(driver->*setHLine)((int)ROUND(r.left), (int)ROUND(r.right), (int)ROUND(r.bottom));
	(driver->*setVLine)((int)ROUND(r.left), (int)ROUND(r.top), (int)ROUND(r.bottom));
}

/*!
	\brief Convenience function for server use
	\param r BRegion to be stroked
	\param d Data structure containing any other data necessary for the call. Always non-NULL.
	\param pat 8-byte array containing the const Pattern &to use. Always non-NULL.

*/
void DisplayDriver::StrokeRegion(BRegion& r, float pensize, const RGBColor& color)
{
	Lock();

	for(int32 i=0; i<r.CountRects();i++)
		StrokeRect(r.RectAt(i),pensize,color);

	Unlock();
}

void DisplayDriver::StrokeRegion(BRegion& r, float pensize, const Pattern& pattern, const RGBColor& high_color, const RGBColor& low_color)
{
	Lock();

	for(int32 i=0; i<r.CountRects();i++)
		StrokeRect(r.RectAt(i),pensize,pattern,high_color,low_color);

	Unlock();
}

void DisplayDriver::StrokeRoundRect(BRect r, float xrad, float yrad, float pensize, const RGBColor& color)
{
}

void DisplayDriver::StrokeRoundRect(BRect r, float xrad, float yrad, float pensize, const Pattern& pattern, const RGBColor& high_color, const RGBColor& low_color)
{
}

/*!
	\brief Called for all BView::StrokeRoundRect calls
	\param r The rect itself
	\param xrad X radius of the corner arcs
	\param yrad Y radius of the corner arcs
	\param setHLine Horizontal line drawing function
	\param setVLine Vertical line drawing function
	\param setPixel Pixel drawing function
*/
void DisplayDriver::StrokeRoundRect(BRect r, float xrad, float yrad, DisplayDriver* driver, SetHorizontalLineFuncType setHLine, SetVerticalLineFuncType setVLine, SetPixelFuncType setPixel)
{
	int hLeft, hRight;
	int vTop, vBottom;
	float bLeft, bRight, bTop, bBottom;

	hLeft = (int)ROUND(r.left + xrad);
	hRight = (int)ROUND(r.right - xrad);
	vTop = (int)ROUND(r.top + yrad);
	vBottom = (int)ROUND(r.bottom - yrad);
	bLeft = hLeft + xrad;
	bRight = hRight -xrad;
	bTop = vTop + yrad;
	bBottom = vBottom - yrad;
	StrokeArc(BRect(bRight, r.top, r.right, bTop), 0, 90, driver, setPixel);
	(driver->*setHLine)(hRight, hLeft, (int)ROUND(r.top));
	
	StrokeArc(BRect(r.left,r.top,bLeft,bTop), 90, 90, driver, setPixel);
	(driver->*setVLine)((int)ROUND(r.left),vTop,vBottom);

	StrokeArc(BRect(r.left,bBottom,bLeft,r.bottom), 180, 90, driver, setPixel);
	(driver->*setHLine)(hLeft, hRight, ROUND(r.bottom));

	StrokeArc(BRect(bRight,bBottom,r.right,r.bottom), 270, 90, driver, setPixel);
	(driver->*setVLine)((int)ROUND(r.right),vBottom,vTop);
}

//void DisplayDriver::StrokeShape(SShape *sh, LayerData *d, const Pattern &pat)
//{
//}

/*!
	\brief Called for all BView::StrokeTriangle calls
	\param pts Array of 3 BPoints. Always non-NULL.
	\param pensize The line thickness
	\param color The color of the lines
*/
void DisplayDriver::StrokeTriangle(BPoint *pts, float pensize, const RGBColor& color)
{
	Lock();
	StrokeLine(pts[0],pts[1],pensize,color);
	StrokeLine(pts[1],pts[2],pensize,color);
	StrokeLine(pts[2],pts[0],pensize,color);
	Unlock();
}

void DisplayDriver::StrokeTriangle(BPoint *pts, float pensize, const Pattern& pattern, const RGBColor& high_color, const RGBColor& low_color)
{
	Lock();
	StrokeLine(pts[0],pts[1],pensize,pattern,high_color,low_color);
	StrokeLine(pts[1],pts[2],pensize,pattern,high_color,low_color);
	StrokeLine(pts[2],pts[0],pensize,pattern,high_color,low_color);
	Unlock();
}

/*!
	\brief Draws a series of lines - optimized for speed
	\param pts Array of BPoints pairs
	\param numlines Number of lines to be drawn
	\param pensize The thickness of the lines
	\param colors Array of colors for each respective line
*/
void DisplayDriver::StrokeLineArray(BPoint *pts, int32 numlines, float pensize, RGBColor *colors)
{
}

/*!
	\brief Sets the screen mode to specified resolution and color depth.
	\param mode constant as defined in GraphicsDefs.h
	
	Subclasses must include calls to _SetDepth, _SetHeight, _SetWidth, and _SetMode
	to update the state variables kept internally by the DisplayDriver class.
*/
void DisplayDriver::SetMode(int32 mode)
{
}

/*!
	\brief Dumps the contents of the frame buffer to a file.
	\param path Path and leaf of the file to be created without an extension
	\return False if unimplemented or unsuccessful. True if otherwise.
	
	Subclasses should add an extension based on what kind of file is saved
*/
bool DisplayDriver::DumpToFile(const char *path)
{
	return false;
}

/*!
	\brief Returns a new ServerBitmap containing the contents of the frame buffer
	\return A new ServerBitmap containing the contents of the frame buffer or NULL if unsuccessful
*/
ServerBitmap *DisplayDriver::DumpToBitmap(void)
{
	return NULL;
}

/*!
	\brief Gets the width of a string in pixels
	\param string Source null-terminated string
	\param length Number of characters in the string
	\param d Data structure containing any other data necessary for the call. Always non-NULL.
	\return Width of the string in pixels
	
	This corresponds to BView::StringWidth.
*/
float DisplayDriver::StringWidth(const char *string, int32 length, LayerData *d)
{
	return 0.0;
}

/*!
	\brief Gets the height of a string in pixels
	\param string Source null-terminated string
	\param length Number of characters in the string
	\param d Data structure containing any other data necessary for the call. Always non-NULL.
	\return Height of the string in pixels
	
	The height calculated in this function does not include any padding - just the
	precise maximum height of the characters within and does not necessarily equate
	with a font's height, i.e. the strings 'case' and 'alps' will have different values
	even when called with all other values equal.
*/
float DisplayDriver::StringHeight(const char *string, int32 length, LayerData *d)
{
	return 0.0;
}

/*!
	\brief Retrieves the bounding box each character in the string
	\param string Source null-terminated string
	\param count Number of characters in the string
	\param mode Metrics mode for either screen or printing
	\param delta Optional glyph padding. This value may be NULL.
	\param rectarray Array of BRect objects which will have at least count elements
	\param d Data structure containing any other data necessary for the call. Always non-NULL.

	See BFont::GetBoundingBoxes for more details on this function.
*/
void DisplayDriver::GetBoundingBoxes(const char *string, int32 count, 
		font_metric_mode mode, escapement_delta *delta, BRect *rectarray, LayerData *d)
{
}

/*!
	\brief Retrieves the escapements for each character in the string
	\param string Source null-terminated string
	\param charcount Number of characters in the string
	\param delta Optional glyph padding. This value may be NULL.
	\param escapements Array of escapement_delta objects which will have at least charcount elements
	\param offsets Actual offset values when iterating over the string. This array will also 
		have at least charcount elements and the values placed therein will reflect 
		the current kerning/spacing mode.
	\param d Data structure containing any other data necessary for the call. Always non-NULL.
	
	See BFont::GetEscapements for more details on this function.
*/
void DisplayDriver::GetEscapements(const char *string, int32 charcount, 
		escapement_delta *delta, escapement_delta *escapements, escapement_delta *offsets, LayerData *d)
{
}

/*!
	\brief Retrieves the inset values of each glyph from its escapement values
	\param string Source null-terminated string
	\param charcount Number of characters in the string
	\param edgearray Array of edge_info objects which will have at least charcount elements
	\param d Data structure containing any other data necessary for the call. Always non-NULL.

	See BFont::GetEdges for more details on this function.
*/
void DisplayDriver::GetEdges(const char *string, int32 charcount, edge_info *edgearray, LayerData *d)
{
}

/*!
	\brief Determines whether a font contains a certain string of characters
	\param string Source null-terminated string
	\param charcount Number of characters in the string
	\param hasarray Array of booleans which will have at least charcount elements

	See BFont::GetHasGlyphs for more details on this function.
*/
void DisplayDriver::GetHasGlyphs(const char *string, int32 charcount, bool *hasarray)
{
}

/*!
	\brief Truncates an array of strings to a certain width
	\param instrings Array of null-terminated strings
	\param stringcount Number of strings passed to the function
	\param mode Truncation mode
	\param maxwidth Maximum width for all strings
	\param outstrings String array provided by the caller into which the truncated strings are
		to be placed.

	See BFont::GetTruncatedStrings for more details on this function.
*/
void DisplayDriver::GetTruncatedStrings( const char **instrings, int32 stringcount, 
	uint32 mode, float maxwidth, char **outstrings)
{
}

/*!
	\brief Returns the bit depth for the current screen mode
	\return Current number of bits per pixel
*/
uint8 DisplayDriver::GetDepth(void)
{
	return _buffer_depth;
}

/*!
	\brief Returns the height for the current screen mode
	\return Height of the screen
*/
uint16 DisplayDriver::GetHeight(void)
{
	return _buffer_height;
}

/*!
	\brief Returns the width for the current screen mode
	\return Width of the screen
*/
uint16 DisplayDriver::GetWidth(void)
{
	return _buffer_width;
}

/*!
	\brief Returns the number of bytes used in each row of the frame buffer
	\return The number of bytes used in each row of the frame buffer
*/
uint32 DisplayDriver::GetBytesPerRow(void)
{
	return _bytes_per_row;
}

/*!
	\brief Returns the screen mode constant in use by the driver
	\return Current screen mode
*/
int32 DisplayDriver::GetMode(void)
{
	return _buffer_mode;
}

/*!
	\brief Returns whether or not the cursor is currently obscured
	\return True if obscured, false if not.
*/
bool DisplayDriver::IsCursorObscured(bool state)
{
	return _is_cursor_obscured;
}

// Protected Internal Functions

/*!
	\brief Locks the driver
	\param timeout Optional timeout specifier
	\return True if the lock was successful, false if not.
	
	The return value need only be checked if a timeout was specified. Each public
	member function should lock the driver before doing anything else. Functions
	internal to the driver (protected/private) need not do this.
*/
bool DisplayDriver::Lock(bigtime_t timeout)
{
	if(timeout==B_INFINITE_TIMEOUT)
		return _locker->Lock();
	
	return (_locker->LockWithTimeout(timeout)==B_OK)?true:false;
}

/*!
	\brief Unlocks the driver
*/
void DisplayDriver::Unlock(void)
{
	_locker->Unlock();
}

/*!
	\brief Sets the driver's Display Power Management System state
	\param state The state which the driver should enter
	\return B_OK if successful, B_ERROR for failure
	
	This function will fail if the driver's rendering context does not support a 
	particular DPMS state. Use DPMSCapabilities to find out the supported states.
	The default implementation supports only B_DPMS_ON.
*/
status_t DisplayDriver::SetDPMSMode(const uint32 &state)
{
	if(state!=B_DPMS_ON)
		return B_ERROR;

	return B_OK;
}

/*!
	\brief Returns the driver's current DPMS state
	\return The driver's current DPMS state
*/
uint32 DisplayDriver::DPMSMode(void) const
{
	return _dpms_state;
}

/*!
	\brief Returns the driver's DPMS capabilities
	\return The driver's DPMS capabilities
	
	The capabilities are the modes supported by the driver. The default implementation 
	allows only B_DPMS_ON. Other possible states are B_DPMS_STANDBY, SUSPEND, and OFF.
*/
uint32 DisplayDriver::DPMSCapabilities(void) const
{
	return _dpms_caps;
}

/*!
	\brief Returns data about the rendering device
	\param info Pointer to an object to receive the device info
	\return B_OK if this function is supported, B_UNSUPPORTED if not
	
	The default implementation of this returns B_UNSUPPORTED and does nothing.

	From Accelerant.h:
	
	uint32	version;			// structure version number
	char 	name[32];			// a name the user will recognize the device by
	char	chipset[32];		// the chipset used by the device
	char	serial_no[32];		// serial number for the device
	uint32	memory;				// amount of memory on the device, in bytes
	uint32	dac_speed;			// nominal DAC speed, in MHz

*/
status_t DisplayDriver::GetDeviceInfo(accelerant_device_info *info)
{
	return B_ERROR;
}

/*!
	\brief Returns data about the rendering device
	\param mode_list Pointer to receive a list of modes.
	\param count The number of modes in mode_list
	\return B_OK if this function is supported, B_UNSUPPORTED if not
	
	The default implementation of this returns B_UNSUPPORTED and does nothing.
*/
status_t DisplayDriver::GetModeList(display_mode **mode_list, uint32 *count)
{
	return B_UNSUPPORTED;
}

/*!
	\brief Obtains the minimum and maximum pixel throughput
	\param mode Structure to receive the data for the given mode
	\param low Recipient of the minimum clock rate
	\param high Recipient of the maximum clock rate
	\return 
	- \c B_OK: Everything is kosher
	- \c B_UNSUPPORTED: The function is unsupported
	- \c B_ERROR: No known pixel clock limits
	
	
	This function returns the minimum and maximum "pixel clock" rates, in 
	thousands-of-pixels per second, that are possible for the given mode. See 
	BScreen::GetPixelClockLimits() for more information.

	The default implementation of this returns B_UNSUPPORTED and does nothing.
*/
status_t DisplayDriver::GetPixelClockLimits(display_mode *mode, uint32 *low, uint32 *high)
{
	return B_UNSUPPORTED;
}

/*!
	\brief Obtains the timing constraints of the current display mode. 
	\param dtc Object to receive the constraints
	\return 
	- \c B_OK: Everything is kosher
	- \c B_UNSUPPORTED: The function is unsupported
	- \c B_ERROR: No known timing constraints
	
	The default implementation of this returns B_UNSUPPORTED and does nothing.
*/
status_t DisplayDriver::GetTimingConstraints(display_timing_constraints *dtc)
{
	return B_UNSUPPORTED;
}

/*!
	\brief Obtains the timing constraints of the current display mode. 
	\param dtc Object to receive the constraints
	\return 
	- \c B_OK: Everything is kosher
	- \c B_UNSUPPORTED: The function is unsupported
	
	The default implementation of this returns B_UNSUPPORTED and does nothing. 
	This is mostly the responsible of the hardware driver if the DisplayDriver 
	interfaces with	actual hardware.
*/
status_t DisplayDriver::ProposeMode(display_mode *candidate, const display_mode *low, const display_mode *high)
{
	return B_UNSUPPORTED;
}

/*!
	\brief Waits for the device's vertical retrace
	\param timeout Amount of time to wait until retrace. Default is B_INFINITE_TIMEOUT
	\return 
	- \c B_OK: Everything is kosher
	- \c B_ERROR: The function timed out before retrace
	- \c B_UNSUPPORTED: The function is unsupported
	
	The default implementation of this returns B_UNSUPPORTED and does nothing. 
*/
status_t DisplayDriver::WaitForRetrace(bigtime_t timeout)
{
	return B_UNSUPPORTED;
}


/*!
	\brief Internal depth-setting function
	\param d Number of bits per pixel in use
	
	_SetDepth must be called from within any implementation of SetMode
*/
void DisplayDriver::_SetDepth(uint8 d)
{
	_buffer_depth=d;
}

/*!
	\brief Internal height-setting function
	\param h Height of the frame buffer
	
	_SetHeight must be called from within any implementation of SetMode
*/
void DisplayDriver::_SetHeight(uint16 h)
{
	_buffer_height=h;
}

/*!
	\brief Internal width-setting function
	\param w Width of the frame buffer
	
	_SetWidth must be called from within any implementation of SetMode
*/
void DisplayDriver::_SetWidth(uint16 w)
{
	_buffer_width=w;
}

/*!
	\brief Internal mode-setting function.
	\param m Screen mode in use as defined in GraphicsDefs.h
	
	_SetMode must be called from within any implementation of SetMode. Note that this
	does not actually change the screen mode; it just updates the state variable used
	to talk with the outside world.
*/
void DisplayDriver::_SetMode(int32 m)
{
	_buffer_mode=m;
}

/*!
	\brief Internal row size-setting function
	\param bpr Number of bytes per row in the frame buffer

	_SetBytesPerRow must be called from within any implementation of SetMode. Note that this
	does not actually change the size of the row; it just updates the state variable used
	to talk with the outside world.
*/
void DisplayDriver::_SetBytesPerRow(uint32 bpr)
{
	_bytes_per_row=bpr;
}

/*!
	\brief Internal DPMS value-setting function
	\param state The new capabilities of the driver

	_SetDPMSState must be called from within any implementation of SetDPMSState. Note that this
	does not actually change the state itself; it just updates the state variable used
	to talk with the outside world.
*/
void DisplayDriver::_SetDPMSState(uint32 state)
{
	_dpms_caps=state;
}

/*!
	\brief Internal DPMS value-setting function
	\param state The new capabilities of the driver

	_SetDPMSCapabilities must be called at the initialization of the driver so that
	GetDPMSCapabilities returns the proper values.
*/
void DisplayDriver::_SetDPMSCapabilities(uint32 caps)
{
	_dpms_caps=caps;
}

/*!
	\brief Internal device info value-setting function
	\param state The new capabilities of the driver

	_SetDeviceInfo must be called at the initialization of the driver so that
	GetDeviceInfo returns the proper values.
*/
void _SetDeviceInfo(const accelerant_device_info &infO)
{
}

/*!
	\brief Obtains the current cursor for the driver.
	\return Pointer to the current cursor object.
	
	Do NOT delete this pointer - change pointers via SetCursor. This call will be 
	necessary for blitting the cursor to the screen and other such tasks.
*/
ServerCursor *DisplayDriver::_GetCursor(void)
{
	return _cursor;
}

/*!
	\brief Draws a pixel in the specified color
	\param x The x coordinate (guaranteed to be in bounds)
	\param y The y coordinate (guaranteed to be in bounds)
	\param col The color to draw
	Must be implemented in subclasses
*/
/*
void DisplayDriver::SetPixel(int x, int y, RGBColor col)
{
}
*/

/*!
	\brief Draws a point of a specified thickness
	\param x The x coordinate (not guaranteed to be in bounds)
	\param y The y coordinate (not guaranteed to be in bounds)
	\param thick The thickness of the point
	\param pat The PatternHandler which detemines pixel colors
	Must be implemented in subclasses
*/
/*
void DisplayDriver::SetThickPixel(int x, int y, int thick, PatternHandler *pat)
{
}
*/
void DisplayDriver::SetThickPatternPixel(int x, int y)
{
}

/*!
	\brief Draws a horizontal line
	\param x1 The first x coordinate (guaranteed to be in bounds)
	\param x2 The second x coordinate (guaranteed to be in bounds)
	\param y The y coordinate (guaranteed to be in bounds)
	\param pat The PatternHandler which detemines pixel colors
	Must be implemented in subclasses
*/
/*
void DisplayDriver::HLine(int32 x1, int32 x2, int32 y, PatternHandler *pat)
{
}
*/

/*!
	\brief Draws a horizontal line
	\param x1 The first x coordinate (not guaranteed to be in bounds)
	\param x2 The second x coordinate (not guaranteed to be in bounds)
	\param y The y coordinate (not guaranteed to be in bounds)
	\param thick The thickness of the line
	\param pat The PatternHandler which detemines pixel colors
	Must be implemented in subclasses
*/
/*
void DisplayDriver::HLineThick(int32 x1, int32 x2, int32 y, int32 thick, PatternHandler *pat)
{
}
*/

void DisplayDriver::HLinePatternThick(int32 x1, int32 x2, int32 y)
{
}

void DisplayDriver::VLinePatternThick(int32 x1, int32 x2, int32 y)
{
}

void DisplayDriver::FillSolidRect(int32 left, int32 top, int32 right, int32 bottom)
{
}

void DisplayDriver::FillPatternRect(int32 left, int32 top, int32 right, int32 bottom)
{
}
