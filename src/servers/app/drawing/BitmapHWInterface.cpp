//------------------------------------------------------------------------------
//
// Copyright 2002-2005, Haiku, Inc. All rights reserved.
// Distributed under the terms of the MIT License.
//
//
//	File Name:		BitmapHWInterface.cpp
//	Authors:		Michael Lotz <mmlr@mlotz.ch>
//					DarkWyrm <bpmagic@columbus.rr.com>
//					Stephan Aßmus <superstippi@gmx.de>
//	Description:	Accelerant based HWInterface implementation
//  
//------------------------------------------------------------------------------

#include <new>
#include <stdio.h>
#include <string.h>

#include "Bitmap.h"
#include "BitmapBuffer.h"
#include "BBitmapBuffer.h"

#include "BitmapHWInterface.h"

using std::nothrow;

// constructor
BitmapHWInterface::BitmapHWInterface(ServerBitmap* bitmap)
	: HWInterface(),
	  fBackBuffer(NULL),
	  fFrontBuffer(new(nothrow) BitmapBuffer(bitmap))
{
}

// destructor
BitmapHWInterface::~BitmapHWInterface()
{
	delete fBackBuffer;
	delete fFrontBuffer;
}

// Initialize
status_t
BitmapHWInterface::Initialize()
{
	status_t ret = HWInterface::Initialize();
	if (ret < B_OK)
		return ret;

	ret = fFrontBuffer->InitCheck();
	if (ret < B_OK)
		return ret;

// TODO: Remove once unnecessary...
	// fall back to double buffered mode until Painter knows how
	// to draw onto non 32-bit surfaces...
	if (fFrontBuffer->ColorSpace() != B_RGB32 &&
		fFrontBuffer->ColorSpace() != B_RGBA32) {

		BBitmap* backBitmap = new BBitmap(fFrontBuffer->Bounds(),
										  B_BITMAP_NO_SERVER_LINK,
										  B_RGBA32);
		fBackBuffer = new BBitmapBuffer(backBitmap);

		ret = fBackBuffer->InitCheck();
		if (ret < B_OK) {
			delete fBackBuffer;
			fBackBuffer = NULL;
		} else {
			// import the current contents of the bitmap
			// into the back bitmap
			backBitmap->ImportBits(fFrontBuffer->Bits(),
								   fFrontBuffer->BitsLength(),
								   fFrontBuffer->BytesPerRow(),
								   0,
								   fFrontBuffer->ColorSpace());
		}
	}

	return ret;
}

// Shutdown
status_t
BitmapHWInterface::Shutdown()
{
	return B_OK;
}

// SetMode
status_t
BitmapHWInterface::SetMode(const display_mode &mode)
{
	return B_UNSUPPORTED;
}

// GetMode
void
BitmapHWInterface::GetMode(display_mode *mode)
{
	if (mode) {
		memset(mode, 0, sizeof(display_mode));
	}
}

// GetDeviceInfo
status_t
BitmapHWInterface::GetDeviceInfo(accelerant_device_info *info)
{
	return B_UNSUPPORTED;
}

// GetModeList
status_t
BitmapHWInterface::GetModeList(display_mode** modes, uint32 *count)
{
	return B_UNSUPPORTED;
}

// GetPixelClockLimits
status_t
BitmapHWInterface::GetPixelClockLimits(display_mode *mode, uint32 *low, uint32 *high)
{
	return B_UNSUPPORTED;
}

// GetPixelClockLimits
status_t
BitmapHWInterface::GetTimingConstraints(display_timing_constraints *dtc)
{
	return B_UNSUPPORTED;
}

// ProposeMode
status_t
BitmapHWInterface::ProposeMode(display_mode *candidate, const display_mode *low, const display_mode *high)
{
	return B_UNSUPPORTED;
}

// RetraceSemaphore
sem_id
BitmapHWInterface::RetraceSemaphore()
{
	return B_ERROR;
}

// WaitForRetrace
status_t
BitmapHWInterface::WaitForRetrace(bigtime_t timeout)
{
	return B_UNSUPPORTED;
}

// SetDPMSMode
status_t
BitmapHWInterface::SetDPMSMode(const uint32 &state)
{
	return B_UNSUPPORTED;
}

// DPMSMode
uint32
BitmapHWInterface::DPMSMode()
{
	return 0;
}

// DPMSCapabilities
uint32
BitmapHWInterface::DPMSCapabilities()
{
	return 0;
}

// FrontBuffer
RenderingBuffer *
BitmapHWInterface::FrontBuffer() const
{
	return fFrontBuffer;
}

// BackBuffer
RenderingBuffer *
BitmapHWInterface::BackBuffer() const
{
	return fBackBuffer;
}

// IsDoubleBuffered
bool
BitmapHWInterface::IsDoubleBuffered() const
{
	// overwrite double buffered preference
	if (fFrontBuffer)
		return fBackBuffer != NULL;

	return HWInterface::IsDoubleBuffered();
}


