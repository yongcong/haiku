/* 
** Copyright 2004, Axel Dörfler, axeld@pinc-software.de. All rights reserved.
** Distributed under the terms of the OpenBeOS License.
*/
#ifndef ATTRIBUTE_WINDOW_H
#define ATTRIBUTE_WINDOW_H


#include "ProbeWindow.h"

class ProbeView;


class AttributeWindow : public ProbeWindow {
	public:
		AttributeWindow(BRect rect, entry_ref *ref, const char *attribute = NULL,
			const BMessage *settings = NULL);
		virtual ~AttributeWindow();

		virtual void MessageReceived(BMessage *message);
		virtual bool Contains(const entry_ref &ref, const char *attribute);

	private:
		ProbeView	*fProbeView;
		char		*fAttribute;
};

#endif	/* ATTRIBUTE_WINDOW_H */
