/* 
** Copyright 2004, Axel Dörfler, axeld@pinc-software.de. All rights reserved.
** Distributed under the terms of the OpenBeOS License.
*/


#include <ScrollView.h>
#include <Message.h>


static const float kFancyBorderSize = 2;
static const float kPlainBorderSize = 1;


BScrollView::BScrollView(const char *name, BView *target, uint32 resizeMask,
	uint32 flags, bool horizontal, bool vertical, border_style border)
	: BView(CalcFrame(target, horizontal, vertical, border), name,
		flags | (border != B_NO_BORDER ? B_WILL_DRAW : 0), resizeMask),
	fTarget(target),
	fHorizontalScrollBar(NULL),
	fVerticalScrollBar(NULL),
	fBorder(border),
	fHighlighted(false)
{
	fTarget->TargetedByScrollView(this);
	fTarget->MoveTo(B_ORIGIN);

	if (border == B_FANCY_BORDER)
		fTarget->MoveBy(kFancyBorderSize, kFancyBorderSize);
	else if (border == B_PLAIN_BORDER)
		fTarget->MoveBy(kPlainBorderSize, kPlainBorderSize);

	AddChild(fTarget);

	BRect frame = fTarget->Frame();
	// ToDo: find out the original scroll bar names
	if (horizontal) {
		BRect rect = frame;
		rect.top = rect.bottom + 1;
		rect.bottom += B_H_SCROLL_BAR_HEIGHT + 1;
		fHorizontalScrollBar = new BScrollBar(rect, "horizontal", fTarget, 0, 100, B_HORIZONTAL);
		AddChild(fHorizontalScrollBar);
	}

	if (vertical) {
		BRect rect = frame;
		rect.left = rect.right + 1;
		rect.right += B_V_SCROLL_BAR_WIDTH + 1;
		fVerticalScrollBar = new BScrollBar(rect, "vertical", fTarget, 0, 100, B_VERTICAL);
		AddChild(fVerticalScrollBar);
	}

	fPreviousWidth = uint16(Bounds().Width());
	fPreviousHeight = uint16(Bounds().Height());
}


BScrollView::BScrollView(BMessage *data)
	: BView(data)
{
	// ToDo
}


BScrollView::~BScrollView()
{
}


BArchivable *
BScrollView::Instantiate(BMessage *data)
{
	// ToDo
	return NULL;
}


status_t
BScrollView::Archive(BMessage *data, bool deep) const
{
	// ToDo
	return B_ERROR;
}


void
BScrollView::AttachedToWindow()
{
	// ToDo: check for the document knob and if we have two scrollers
}


void
BScrollView::DetachedFromWindow()
{
	BView::DetachedFromWindow();
}


void
BScrollView::AllAttached()
{
	BView::AllAttached();
}


void
BScrollView::AllDetached()
{
	BView::AllDetached();
}


void
BScrollView::Draw(BRect updateRect)
{
	if (fBorder == B_PLAIN_BORDER) {
		SetHighColor(tint_color(ui_color(B_PANEL_BACKGROUND_COLOR), B_DARKEN_2_TINT));
		StrokeRect(Bounds());
		return;
	} else if (fBorder != B_FANCY_BORDER)
		return;

	BRect bounds = Bounds();
	SetHighColor(tint_color(ui_color(B_PANEL_BACKGROUND_COLOR), B_DARKEN_2_TINT));
	StrokeRect(bounds.InsetByCopy(1, 1));

	SetHighColor(tint_color(ui_color(B_PANEL_BACKGROUND_COLOR), B_DARKEN_1_TINT));
	StrokeLine(bounds.LeftBottom(), bounds.LeftTop());
	bounds.left++;
	StrokeLine(bounds.LeftTop(), bounds.RightTop());

	SetHighColor(ui_color(B_SHINE_COLOR));
	StrokeLine(bounds.LeftBottom(), bounds.RightBottom());
	bounds.top++;
	bounds.bottom--;
	StrokeLine(bounds.RightBottom(), bounds.RightTop());
}


BScrollBar *
BScrollView::ScrollBar(orientation posture) const
{
	if (posture == B_HORIZONTAL)
		return fHorizontalScrollBar;

	return fVerticalScrollBar;
}


void
BScrollView::SetBorder(border_style border)
{
	// ToDo: update drawing
	fBorder = border;
}


border_style
BScrollView::Border() const
{
	return fBorder;
}


status_t
BScrollView::SetBorderHighlighted(bool state)
{
	if (fBorder != B_FANCY_BORDER)
		return B_ERROR;

	fHighlighted = state;

	// ToDo: update drawing

	return B_OK;
}


bool
BScrollView::IsBorderHighlighted() const
{
	return fHighlighted;
}


void
BScrollView::SetTarget(BView *target)
{
	fTarget = target;
}


BView *
BScrollView::Target() const
{
	return fTarget;
}


void
BScrollView::MessageReceived(BMessage *message)
{
	switch (message->what) {
		default:
			BView::MessageReceived(message);
	}
}


void
BScrollView::MouseDown(BPoint point)
{
	BView::MouseDown(point);
}


void
BScrollView::MouseUp(BPoint point)
{
	BView::MouseUp(point);
}


void
BScrollView::MouseMoved(BPoint point, uint32 code, const BMessage *dragMessage)
{
	BView::MouseMoved(point, code, dragMessage);
}


void
BScrollView::FrameMoved(BPoint position)
{
	BView::FrameMoved(position);
}


void
BScrollView::FrameResized(float width, float height)
{
	BView::FrameResized(width, height);
}


void 
BScrollView::ResizeToPreferred()
{
	BView::ResizeToPreferred();
}


void 
BScrollView::GetPreferredSize(float *_width, float *_height)
{
	float width, height;
	fTarget->GetPreferredSize(&width, &height);

	BRect frame = CalcFrame(fTarget, fHorizontalScrollBar, fVerticalScrollBar, fBorder);
	frame.right += width - fTarget->Frame().Width();
	frame.bottom += height - fTarget->Frame().Height();

	if (_width)
		*_width = frame.Width();
	if (_height)
		*_height = frame.Height();
}


void
BScrollView::WindowActivated(bool active)
{
	BView::WindowActivated(active);
}


void 
BScrollView::MakeFocus(bool state)
{
	BView::MakeFocus(state);
}


BHandler *
BScrollView::ResolveSpecifier(BMessage *msg, int32 index, BMessage *specifier, int32 form, const char *property)
{
	return BView::ResolveSpecifier(msg, index, specifier, form, property);
}


status_t 
BScrollView::GetSupportedSuites(BMessage *data)
{
	return BView::GetSupportedSuites(data);
}


status_t
BScrollView::Perform(perform_code d, void *arg)
{
	return BView::Perform(d, arg);
}


BScrollView &
BScrollView::operator=(const BScrollView &)
{
	return *this;
}


/** This static method is used to calculate the frame that the
 *	ScrollView will cover depending on the frame of its target
 *	and which border style is used.
 */

BRect
BScrollView::CalcFrame(BView *target, bool horizontal, bool vertical, border_style border)
{
	BRect frame = target->Frame();
	if (vertical)
		frame.right += B_V_SCROLL_BAR_WIDTH;
	if (horizontal)
		frame.bottom += B_H_SCROLL_BAR_HEIGHT;

	float borderSize = 0;
	if (border == B_PLAIN_BORDER)
		borderSize = kPlainBorderSize;
	else if (border == B_FANCY_BORDER)
		borderSize = kFancyBorderSize;

	frame.InsetBy(-borderSize, -borderSize);

	if (borderSize == 0) {
		if (vertical)
			frame.right++;
		if (horizontal)
			frame.bottom++;
	}

	return frame;
}


int32
BScrollView::ModFlags(int32, border_style)
{
	// ToDo: I have no idea what this is for
	return 0;
}


void
BScrollView::InitObject()
{
	// ToDo: and what should go in here?
}


//	#pragma mark -
//	Reserved virtuals


void BScrollView::_ReservedScrollView1() {}
void BScrollView::_ReservedScrollView2() {}
void BScrollView::_ReservedScrollView3() {}
void BScrollView::_ReservedScrollView4() {}

