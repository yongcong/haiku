/*
 * Copyright (C) 2010 Stephan Aßmus <superstippi@gmx.de>
 *
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY APPLE COMPUTER, INC. ``AS IS'' AND ANY
 * EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR
 * PURPOSE ARE DISCLAIMED.  IN NO EVENT SHALL APPLE COMPUTER, INC. OR
 * CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL,
 * EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO,
 * PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR
 * PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY
 * OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
 * OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

#ifndef TAB_MANAGER_H
#define TAB_MANAGER_H

#include <Messenger.h>
#include <TabView.h>

enum {
    TAB_CHANGED = 'tcha',
    CLOSE_TAB = 'cltb'
};

class BBitmap;
class BCardLayout;
class BGroupView;
class BMenu;
class TabContainerGroup;
class TabContainerView;
class TabManagerController;

class TabManager {
public:
    							TabManager(const BMessenger& target,
    								BMessage* newTabMessage);
	virtual						~TabManager();

			void				SetTarget(const BMessenger& target);
			const BMessenger&	Target() const;

#if INTEGRATE_MENU_INTO_TAB_BAR
			BMenu*				Menu() const;
#endif

			BView*				TabGroup() const;
			BView*				GetTabContainerView() const;
			BView*				ContainerView() const;

			BView*				ViewForTab(int32 tabIndex) const;
			int32				TabForView(const BView* containedView) const;
			bool				HasView(const BView* containedView) const;

			void				SelectTab(int32 tabIndex);
			void				SelectTab(const BView* containedView);
			int32				SelectedTabIndex() const;
			void				CloseTab(int32 tabIndex);

			void				AddTab(BView* view, const char* label,
									int32 index = -1);
			BView*				RemoveTab(int32 index);
			int32				CountTabs() const;

			void				SetTabLabel(int32 tabIndex, const char* label);
	const	BString&			TabLabel(int32);
			void				SetTabIcon(const BView* containedView,
									const BBitmap* icon);
			void				SetCloseButtonsAvailable(bool available);

private:
#if INTEGRATE_MENU_INTO_TAB_BAR
			BMenu*				fMenu;
#endif
			TabContainerGroup*	fTabContainerGroup;
			TabContainerView*	fTabContainerView;
			BView*				fContainerView;
			BCardLayout*		fCardLayout;
			TabManagerController* fController;

			BMessenger			fTarget;
};

#endif // TAB_MANAGER_H
