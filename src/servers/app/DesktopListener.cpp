/*
 * Copyright 2010, Haiku.
 * Distributed under the terms of the MIT License.
 *
 * Authors:
 *		Clemens Zeidler <haiku@clemens-zeidler.de>
 */


#include "DesktopListener.h"


DesktopListener::~DesktopListener()
{
	
}


DesktopObservable::DesktopObservable()
	:
	fWeAreInvoking(false)
{

}


void
DesktopObservable::RegisterListener(DesktopListener* listener, Desktop* desktop)
{
	fDesktopListenerList.Add(listener);
	listener->ListenerRegistered(desktop);
}


void
DesktopObservable::UnregisterListener(DesktopListener* listener)
{
	fDesktopListenerList.Remove(listener);
	listener->ListenerUnregistered();
}


const DesktopListenerDLList&
DesktopObservable::GetDesktopListenerList()
{
	return fDesktopListenerList;
}


bool
DesktopObservable::MessageForListener(Window* sender,
	BPrivate::ServerLink& link)
{
	int32 identifier;
	link.Read<int32>(&identifier);
	for (DesktopListener* listener = fDesktopListenerList.First();
		listener != NULL; listener = fDesktopListenerList.GetNext(listener)) {
		if (listener->Identifier() == identifier) {
			if (!listener->HandleMessage(sender, link))
				break;
			return true;
		}
	}
	return false;
}


void
DesktopObservable::WindowAdded(Window* window)
{
	if (fWeAreInvoking)
		return;
	InvokeGuard invokeGuard(fWeAreInvoking);

	for (DesktopListener* listener = fDesktopListenerList.First();
		listener != NULL; listener = fDesktopListenerList.GetNext(listener))
		listener->WindowAdded(window);
}


void
DesktopObservable::WindowRemoved(Window* window)
{
	if (fWeAreInvoking)
		return;
	InvokeGuard invokeGuard(fWeAreInvoking);

	for (DesktopListener* listener = fDesktopListenerList.First();
		listener != NULL; listener = fDesktopListenerList.GetNext(listener))
		listener->WindowRemoved(window);
}


void
DesktopObservable::KeyPressed(uint32 what, int32 key, int32 modifiers)
{
	if (fWeAreInvoking)
		return;
	InvokeGuard invokeGuard(fWeAreInvoking);

	for (DesktopListener* listener = fDesktopListenerList.First();
		listener != NULL; listener = fDesktopListenerList.GetNext(listener))
		listener->KeyPressed(what, key, modifiers);	
}


void
DesktopObservable::MouseEvent(BMessage* message)
{
	if (fWeAreInvoking)
		return;
	InvokeGuard invokeGuard(fWeAreInvoking);

	for (DesktopListener* listener = fDesktopListenerList.First();
		listener != NULL; listener = fDesktopListenerList.GetNext(listener))
		listener->MouseEvent(message);
}


void
DesktopObservable::MouseDown(Window* window, BMessage* message,
	const BPoint& where)
{
	if (fWeAreInvoking)
		return;
	InvokeGuard invokeGuard(fWeAreInvoking);

	for (DesktopListener* listener = fDesktopListenerList.First();
		listener != NULL; listener = fDesktopListenerList.GetNext(listener))
		listener->MouseDown(window, message, where);
}


void
DesktopObservable::MouseUp(Window* window, BMessage* message,
	const BPoint& where)
{
	if (fWeAreInvoking)
		return;
	InvokeGuard invokeGuard(fWeAreInvoking);

	for (DesktopListener* listener = fDesktopListenerList.First();
		listener != NULL; listener = fDesktopListenerList.GetNext(listener))
		listener->MouseUp(window, message, where);
}


void
DesktopObservable::MouseMoved(Window* window, BMessage* message,
	const BPoint& where)
{
	if (fWeAreInvoking)
		return;
	InvokeGuard invokeGuard(fWeAreInvoking);

	for (DesktopListener* listener = fDesktopListenerList.First();
		listener != NULL; listener = fDesktopListenerList.GetNext(listener))
		listener->MouseMoved(window, message, where);
}


void
DesktopObservable::WindowMoved(Window* window)
{
	if (fWeAreInvoking)
		return;
	InvokeGuard invokeGuard(fWeAreInvoking);

	for (DesktopListener* listener = fDesktopListenerList.First();
		listener != NULL; listener = fDesktopListenerList.GetNext(listener))
		listener->WindowMoved(window);
}


void
DesktopObservable::WindowResized(Window* window)
{
	if (fWeAreInvoking)
		return;
	InvokeGuard invokeGuard(fWeAreInvoking);

	for (DesktopListener* listener = fDesktopListenerList.First();
		listener != NULL; listener = fDesktopListenerList.GetNext(listener))
		listener->WindowResized(window);
}


void
DesktopObservable::WindowActitvated(Window* window)
{
	if (fWeAreInvoking)
		return;
	InvokeGuard invokeGuard(fWeAreInvoking);

	for (DesktopListener* listener = fDesktopListenerList.First();
		listener != NULL; listener = fDesktopListenerList.GetNext(listener))
		listener->WindowActitvated(window);
}


void
DesktopObservable::WindowSentBehind(Window* window, Window* behindOf)
{
	if (fWeAreInvoking)
		return;
	InvokeGuard invokeGuard(fWeAreInvoking);

	for (DesktopListener* listener = fDesktopListenerList.First();
		listener != NULL; listener = fDesktopListenerList.GetNext(listener))
		listener->WindowSentBehind(window, behindOf);
}


void
DesktopObservable::WindowWorkspacesChanged(Window* window, uint32 workspaces)
{
	if (fWeAreInvoking)
		return;
	InvokeGuard invokeGuard(fWeAreInvoking);

	for (DesktopListener* listener = fDesktopListenerList.First();
		listener != NULL; listener = fDesktopListenerList.GetNext(listener))
		listener->WindowWorkspacesChanged(window, workspaces);
}


void
DesktopObservable::WindowMinimized(Window* window, bool minimize)
{
	if (fWeAreInvoking)
		return;
	InvokeGuard invokeGuard(fWeAreInvoking);

	for (DesktopListener* listener = fDesktopListenerList.First();
		listener != NULL; listener = fDesktopListenerList.GetNext(listener))
		listener->WindowMinimized(window, minimize);
}


void
DesktopObservable::WindowTabLocationChanged(Window* window, float location)
{
	if (fWeAreInvoking)
		return;
	InvokeGuard invokeGuard(fWeAreInvoking);

	for (DesktopListener* listener = fDesktopListenerList.First();
		listener != NULL; listener = fDesktopListenerList.GetNext(listener))
		listener->WindowTabLocationChanged(window, location);
}


bool
DesktopObservable::SetDecoratorSettings(Window* window,
	const BMessage& settings)
{
	if (fWeAreInvoking)
		return false;
	InvokeGuard invokeGuard(fWeAreInvoking);

	bool changed = false;
	for (DesktopListener* listener = fDesktopListenerList.First();
		listener != NULL; listener = fDesktopListenerList.GetNext(listener))
		changed = changed | listener->SetDecoratorSettings(window, settings);

	return changed;
}


void
DesktopObservable::GetDecoratorSettings(Window* window, BMessage& settings)
{
	if (fWeAreInvoking)
		return;
	InvokeGuard invokeGuard(fWeAreInvoking);

	for (DesktopListener* listener = fDesktopListenerList.First();
		listener != NULL; listener = fDesktopListenerList.GetNext(listener))
		listener->GetDecoratorSettings(window, settings);
}


DesktopObservable::InvokeGuard::InvokeGuard(bool& invoking)
	:
	fInvoking(invoking)
{
	fInvoking = true;
}


DesktopObservable::InvokeGuard::~InvokeGuard()
{
	fInvoking = false;
}
