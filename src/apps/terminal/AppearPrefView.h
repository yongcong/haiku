/*
 * Copyright (c) 2003-4 Kian Duffy <myob@users.sourceforge.net>
 * Copyright (c) 1998,99 Kazuho Okui and Takashi Murai. 
 *
 * Distributed unter the terms of the MIT License.
 */
#ifndef APPEARANCE_PREF_VIEW_H
#define APPEARANCE_PREF_VIEW_H


#include "PrefView.h"

class BColorControl;
class BMenu;
class BMenuField;

class TermWindow;
class TTextControl;


class AppearancePrefView : public PrefView {
	public:
		AppearancePrefView(BRect frame, const char *name, 
			TermWindow *window);

		virtual	void	Revert();
		virtual void	MessageReceived(BMessage *message);
		virtual void	AttachedToWindow();

		virtual void	GetPreferredSize(float *_width, float *_height);

	private:
		BMenu*			_MakeFontMenu(uint32 command, const char *defaultFont);
		BMenu*			_MakeSizeMenu(uint32 command, uint8 defaultSize);

		BMenuField		*fFont;
		BMenuField		*fFontSize;

		BMenuField		*fColorField;
		BColorControl	*fColorControl;

		TermWindow		*fTermWindow;
};

#endif	// APPEARANCE_PREF_VIEW_H
