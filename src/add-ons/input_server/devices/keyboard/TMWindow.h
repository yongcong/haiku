// ~ ~ ~ ~ ~ ~ ~ ~ ~ ~ ~ ~ ~ ~ ~ ~ ~ ~ ~ ~ ~ ~ ~ ~ ~ ~ ~ ~ ~ ~ ~ ~ ~ ~ ~ ~ ~ ~
//
//	Copyright (c) 2004-2005, Haiku
//
//  This software is part of the Haiku distribution and is covered 
//  by the Haiku license.
//
//
//  File:        TMWindow.h
//  Author:      Jérôme Duval
//  Description: Keyboard input server addon
//  Created :    October 13, 2004
// 
// ~ ~ ~ ~ ~ ~ ~ ~ ~ ~ ~ ~ ~ ~ ~ ~ ~ ~ ~ ~ ~ ~ ~ ~ ~ ~ ~ ~ ~ ~ ~ ~ ~ ~ ~ ~ ~ ~

#ifndef TMWINDOW_H
#define TMWINDOW_H


#include <Box.h>
#include <Button.h>
#include <ListView.h>
#include <Window.h>

#include "InputServerTypes.h"
#include "TMListItem.h"


class TMDescView;

class TMView : public BBox {
	public:
		TMView(BRect bounds, const char* name = NULL,
			uint32 resizeFlags = B_FOLLOW_LEFT | B_FOLLOW_TOP,
			uint32 flags = B_WILL_DRAW | B_FRAME_EVENTS | B_NAVIGABLE_JUMP,
			border_style border = B_NO_BORDER);

		virtual void AttachedToWindow();
		virtual void Pulse();
		virtual void MessageReceived(BMessage *msg);

		void UpdateList();
		BListView *ListView() { return fListView; }

	private:
		BListView *fListView;
		BButton *fKillApp;
		TMDescView *fDescView;
};

class TMDescView : public BBox {
	public:
		TMDescView(BRect bounds);
		virtual void Draw(BRect bounds);

		void SetItem(TMListItem *item);
		TMListItem *Item() { return fItem; }
	private:
		TMListItem *fItem;
};

class TMWindow : public BWindow {
	public:
		TMWindow();
		virtual ~TMWindow();

		virtual void MessageReceived(BMessage *msg);
		virtual bool QuitRequested();
		void Enable();
		void Disable();

	private:
		bool fQuitting;

		TMView *fView;
};

#endif //TMWINDOW_H
