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
//	File Name:		TRoster.cpp
//	Author:			Ingo Weinhold (bonefish@users.sf.net)
//	Description:	TRoster is the incarnation of The Roster. It manages the
//					running applications.
//------------------------------------------------------------------------------

#include <new.h>

#include <Application.h>

#include "Debug.h"
#include "RegistrarDefs.h"
#include "RosterAppInfo.h"
#include "TRoster.h"

/*!
	\class TRoster
	\brief Implements the application roster.

	This class handles the BRoster requests. For each kind a hook method is
	implemented to which the registrar looper dispatches the request messages.

	Registered and pre-registered are managed via AppInfoLists.
	\a fEarlyPreRegisteredApps contains the infos for those application that
	are pre-registered and currently have no team ID assigned to them yet,
	whereas the infos of registered and pre-registered applications with a
	team ID are to be found in \a fRegisteredApps.

	When an application asks whether it is pre-registered or not and there
	are one or more instances of the application that are pre-registered, but
	have no team ID assigned yet, the reply to the request has to be
	postponed until the status of the requesting team is clear. The request
	message is dequeued from the registrar's message queue and, with
	additional information (IAPRRequest), added to \a fIAPRRequests for a
	later reply.

	The field \a fActiveApp identifies the currently active application
	and \a fLastToken is a counter used to generate unique tokens for
	pre-registered applications.
*/

// constructor
/*!	\brief Creates a new roster.

	The object is completely initialized and ready to handle requests.
*/
TRoster::TRoster()
	   : fRegisteredApps(),
		 fEarlyPreRegisteredApps(),
		 fIAPRRequests(),
		 fActiveApp(NULL),
		 fLastToken(0)
{
}

// destructor
/*!	\brief Frees all resources associated with this object.
*/
TRoster::~TRoster()
{
}

// HandleAddApplication
/*!	\brief Handles an AddApplication() request.
	\param request The request message
*/
void
TRoster::HandleAddApplication(BMessage *request)
{
	FUNCTION_START();

	status_t error = B_OK;
	// get the parameters
	const char *signature;
	entry_ref ref;
	uint32 flags;
	team_id team;
	thread_id thread;
	port_id port;
	bool fullReg;
	if (request->FindString("signature", &signature) != B_OK)
		signature = NULL;
	if (request->FindRef("ref", &ref) != B_OK)
		SET_ERROR(error, B_BAD_VALUE);
	if (request->FindInt32("flags", (int32*)&flags) != B_OK)
		flags = B_REG_DEFAULT_APP_FLAGS;
	if (request->FindInt32("team", &team) != B_OK)
		team = -1;
	if (request->FindInt32("thread", &thread) != B_OK)
		thread = -1;
	if (request->FindInt32("port", &port) != B_OK)
		port = -1;
	if (request->FindBool("full_registration", &fullReg) != B_OK)
		fullReg = false;
	// check the parameters
	team_id otherTeam = -1;
	uint32 launchFlags = flags & B_LAUNCH_MASK;
	// entry_ref
	if (error == B_OK) {
		// the entry_ref must be valid
		if (BEntry(&ref).Exists()) {
PRINT(("flags: %lx\n", flags));
PRINT(("ref: %ld, %lld, %s\n", ref.device, ref.directory, ref.name));
			// check single/exclusive launchers
			RosterAppInfo *info = NULL;
			if ((launchFlags == B_SINGLE_LAUNCH
				 || launchFlags ==  B_EXCLUSIVE_LAUNCH)
				&& (((info = fRegisteredApps.InfoFor(&ref)))
					|| ((info = fEarlyPreRegisteredApps.InfoFor(&ref))))) {
				SET_ERROR(error, B_ALREADY_RUNNING);
				otherTeam = info->team;
			}
		} else
			SET_ERROR(error, B_ENTRY_NOT_FOUND);
	}
	// signature
	if (error == B_OK && signature) {
		// check exclusive launchers
		RosterAppInfo *info = NULL;
		if (launchFlags == B_EXCLUSIVE_LAUNCH
			&& (((info = fRegisteredApps.InfoFor(signature)))
				|| ((info = fEarlyPreRegisteredApps.InfoFor(signature))))) {
			SET_ERROR(error, B_ALREADY_RUNNING);
			otherTeam = info->team;
		}
	}
	// If no team ID is given, full registration isn't possible.
	if (error == B_OK) {
		if (team < 0) {
			if (fullReg)
				SET_ERROR(error, B_BAD_VALUE);
		} else if (fRegisteredApps.InfoFor(team))
			SET_ERROR(error, B_REG_ALREADY_REGISTERED);
	}
	// Add the application info.
	uint32 token = 0;
	if (error == B_OK) {
		// alloc and init the info
		RosterAppInfo *info = new(nothrow) RosterAppInfo;
		if (info) {
			info->Init(thread, team, port, flags, &ref, signature);
			if (fullReg)
				info->state = APP_STATE_REGISTERED;
			else
				info->state = APP_STATE_PRE_REGISTERED;
			info->registration_time = system_time();
			// add it to the right list
			bool addingSuccess = false;
			if (team >= 0)
{
PRINT(("added ref: %ld, %lld, %s\n", info->ref.device, info->ref.directory, info->ref.name));
				addingSuccess = (AddApp(info) == B_OK);
}
			else {
				token = info->token = _NextToken();
				addingSuccess = fEarlyPreRegisteredApps.AddInfo(info);
			}
			if (!addingSuccess)
				SET_ERROR(error, B_NO_MEMORY);
		} else
			SET_ERROR(error, B_NO_MEMORY);
		// delete the info on failure
		if (error != B_OK && info)
			delete info;
	}
	// reply to the request
	if (error == B_OK) {
		BMessage reply(B_REG_SUCCESS);
		// The token is valid only when no team ID has been supplied.
		if (team < 0)
			reply.AddInt32("token", (int32)token);
		request->SendReply(&reply);
	} else {
		BMessage reply(B_REG_ERROR);
		reply.AddInt32("error", error);
		if (otherTeam >= 0)
			reply.AddInt32("other_team", otherTeam);
		request->SendReply(&reply);
	}

	FUNCTION_END();
}

// HandleCompleteRegistration
/*!	\brief Handles a CompleteRegistration() request.
	\param request The request message
*/
void
TRoster::HandleCompleteRegistration(BMessage *request)
{
	FUNCTION_START();

	status_t error = B_OK;
	// get the parameters
	team_id team;
	thread_id thread;
	port_id port;
	if (request->FindInt32("team", &team) != B_OK)
		team = -1;
	if (request->FindInt32("thread", &thread) != B_OK)
		thread = -1;
	if (request->FindInt32("port", &port) != B_OK)
		port = -1;
	// check the parameters
	// port
	if (error == B_OK && port < 0)
		SET_ERROR(error, B_BAD_VALUE);
	// thread
	if (error == B_OK && thread < 0)
		SET_ERROR(error, B_BAD_VALUE);
	// team
	if (error == B_OK) {
		if (team >= 0) {
			// everything is fine -- set the values
			if (RosterAppInfo *info = fRegisteredApps.InfoFor(team)) {
				info->thread = thread;
				info->port = port;
				info->state = APP_STATE_REGISTERED;
			} else
				SET_ERROR(error, B_REG_APP_NOT_REGISTERED);
		} else
			SET_ERROR(error, B_BAD_VALUE);
	}
	// reply to the request
	if (error == B_OK) {
		BMessage reply(B_REG_SUCCESS);
		request->SendReply(&reply);
	} else {
		BMessage reply(B_REG_ERROR);
		reply.AddInt32("error", error);
		request->SendReply(&reply);
	}

	FUNCTION_END();
}

// HandleIsAppPreRegistered
/*!	\brief Handles an IsAppPreRegistered() request.
	\param request The request message
*/
void
TRoster::HandleIsAppPreRegistered(BMessage *request)
{
	FUNCTION_START();

	status_t error = B_OK;
	// get the parameters
	entry_ref ref;
	team_id team;
	if (request->FindRef("ref", &ref) != B_OK)
		SET_ERROR(error, B_BAD_VALUE);
	if (request->FindInt32("team", &team) != B_OK)
		team = -1;
	// check the parameters
	// entry_ref
	if (error == B_OK & !BEntry(&ref).Exists())
		SET_ERROR(error, B_ENTRY_NOT_FOUND);
	// team
	if (error == B_OK && team < 0)
		SET_ERROR(error, B_BAD_VALUE);
	// loop the information up
	RosterAppInfo *info = NULL;
	if (error == B_OK) {
		if ((info = fRegisteredApps.InfoFor(team)) != NULL) {
			_ReplyToIAPRRequest(request, info);
		} else if ((info = fEarlyPreRegisteredApps.InfoFor(&ref)) != NULL) {
			// pre-registered and has no team ID assigned yet -- queue the
			// request
			be_app->DetachCurrentMessage();
			IAPRRequest queuedRequest = { ref, team, request };
			fIAPRRequests[team] = queuedRequest;
		}
	}
	// reply to the request
	if (error == B_OK)
		_ReplyToIAPRRequest(request, info);
	else {
		BMessage reply(B_REG_ERROR);
		reply.AddInt32("error", error);
		request->SendReply(&reply);
	}

	FUNCTION_END();
}

// HandleRemovePreRegApp
/*!	\brief Handles a RemovePreRegApp() request.
	\param request The request message
*/
void
TRoster::HandleRemovePreRegApp(BMessage *request)
{
	FUNCTION_START();

	status_t error = B_OK;
	// get the parameters
	uint32 token;
	if (request->FindInt32("token", (int32*)&token) != B_OK)
		SET_ERROR(error, B_BAD_VALUE);
	// remove the app
	if (error == B_OK) {
		RosterAppInfo *info = fEarlyPreRegisteredApps.InfoForToken(token);
		if (info) {
			fEarlyPreRegisteredApps.RemoveInfo(info);
			delete info;
		} else
			SET_ERROR(error, B_REG_APP_NOT_PRE_REGISTERED);
	}
	// reply to the request
	if (error == B_OK) {
		BMessage reply(B_REG_SUCCESS);
		request->SendReply(&reply);
	} else {
		BMessage reply(B_REG_ERROR);
		reply.AddInt32("error", error);
		request->SendReply(&reply);
	}

	FUNCTION_END();
}

// HandleRemoveApp
/*!	\brief Handles a RemoveApp() request.
	\param request The request message
*/
void
TRoster::HandleRemoveApp(BMessage *request)
{
	FUNCTION_START();

	status_t error = B_OK;
	// get the parameters
	team_id team;
	if (request->FindInt32("team", &team) != B_OK)
		team = -1;
	// remove the app
	if (error == B_OK) {
		if (RosterAppInfo *info = fRegisteredApps.InfoFor(team)) {
			RemoveApp(info);
			delete info;
		} else
			SET_ERROR(error, B_REG_APP_NOT_REGISTERED);
	}
	// reply to the request
	if (error == B_OK) {
		BMessage reply(B_REG_SUCCESS);
		request->SendReply(&reply);
	} else {
		BMessage reply(B_REG_ERROR);
		reply.AddInt32("error", error);
		request->SendReply(&reply);
	}

	FUNCTION_END();
}

// HandleSetThreadAndTeam
/*!	\brief Handles a SetThreadAndTeam() request.
	\param request The request message
*/
void
TRoster::HandleSetThreadAndTeam(BMessage *request)
{
	FUNCTION_START();

	status_t error = B_OK;
	// get the parameters
	team_id team;
	thread_id thread;
	uint32 token;
	if (request->FindInt32("team", &team) != B_OK)
		team = -1;
	if (request->FindInt32("thread", &thread) != B_OK)
		thread = -1;
	if (request->FindInt32("token", (int32*)&token) != B_OK)
		SET_ERROR(error, B_BAD_VALUE);
	// check the parameters
	// team
	if (error == B_OK && team < 0)
		SET_ERROR(error, B_BAD_VALUE);
	// update the app_info
	if (error == B_OK) {
		RosterAppInfo *info = fEarlyPreRegisteredApps.InfoForToken(token);
		if (info) {
			// Set thread and team, create a port for the application and
			// move the app_info from the list of the early pre-registered
			// apps to the list of the (pre-)registered apps.
			fEarlyPreRegisteredApps.RemoveInfo(info);
			info->team = team;
			info->thread = thread;
			// create and transfer the port
			info->port = create_port(B_REG_APP_LOOPER_PORT_CAPACITY,
									 kRAppLooperPortName);
			if (info->port < 0)
				SET_ERROR(error, info->port);
			if (error == B_OK)
				SET_ERROR(error, set_port_owner(info->port, team));
			// add the info to the registered apps list
			if (error == B_OK)
				SET_ERROR(error, AddApp(info));
			// cleanup on failure
			if (error != B_OK) {
				if (info->port >= 0)
					delete_port(info->port);
				delete info;
			}
			// handle a pending IsAppPreRegistered() request
			IAPRRequestMap::iterator it = fIAPRRequests.find(team);
			if (it != fIAPRRequests.end()) {
				IAPRRequest &request = it->second;
				if (error == B_OK)
					_ReplyToIAPRRequest(request.request, info);
				delete request.request;
				fIAPRRequests.erase(it);
			}
		} else
			SET_ERROR(error, B_REG_APP_NOT_PRE_REGISTERED);
	}
	// reply to the request
	if (error == B_OK) {
		BMessage reply(B_REG_SUCCESS);
		request->SendReply(&reply);
	} else {
		BMessage reply(B_REG_ERROR);
		reply.AddInt32("error", error);
		request->SendReply(&reply);
	}

	FUNCTION_END();
}

// HandleGetAppInfo
/*!	\brief Handles a Get{Running,Active,}AppInfo() request.
	\param request The request message
*/
void
TRoster::HandleGetAppInfo(BMessage *request)
{
	FUNCTION_START();

	status_t error = B_OK;
	// get the parameters
	team_id team;
	entry_ref ref;
	const char *signature;
	bool hasTeam = true;
	bool hasRef = true;
	bool hasSignature = true;
	if (request->FindInt32("team", &team) != B_OK)
		hasTeam = false;
	if (request->FindRef("ref", &ref) != B_OK)
		hasRef = false;
	if (request->FindString("signature", &signature) != B_OK)
		hasSignature = false;
	// check the parameters
	// If neither of those has been supplied, the active application info is
	// requested. We simple set the team ID.
	if (error == B_OK && !hasTeam && !hasRef && !hasSignature) {
	}
	// get the info
	RosterAppInfo *info = NULL;
	if (error == B_OK) {
		if (hasTeam) {
			info = fRegisteredApps.InfoFor(team);
			if (info == NULL)
				error = B_BAD_TEAM_ID;
		} else if (hasRef) {
			info = fRegisteredApps.InfoFor(&ref);
			if (info == NULL)
				error = B_ERROR;
		} else if (hasSignature) {
			info = fRegisteredApps.InfoFor(signature);
			if (info == NULL)
				error = B_ERROR;
		} else {
			// If neither of those has been supplied, the active application
			// info is requested.
			if (fActiveApp)
				info = fActiveApp;
			else
				error = B_ERROR;
		}
	}
	// reply to the request
	if (error == B_OK) {
		BMessage reply(B_REG_SUCCESS);
		_AddMessageAppInfo(&reply, info);
		request->SendReply(&reply);
	} else {
		BMessage reply(B_REG_ERROR);
		reply.AddInt32("error", error);
		request->SendReply(&reply);
	}

	FUNCTION_END();
}

// HandleGetAppList
/*!	\brief Handles a GetAppList() request.
	\param request The request message
*/
void
TRoster::HandleGetAppList(BMessage *request)
{
	FUNCTION_START();

	status_t error = B_OK;
	// get the parameters
	const char *signature;
	if (request->FindString("signature", &signature) != B_OK)
		signature = NULL;
	// reply to the request
	if (error == B_OK) {
		BMessage reply(B_REG_SUCCESS);
		// get the list
		for (int32 i = 0;
			 RosterAppInfo *info = fRegisteredApps.InfoAt(i);
			 i++) {
			if (!signature || !strcmp(signature, info->signature))
				reply.AddInt32("teams", info->team);
		}
		request->SendReply(&reply);
	} else {
		BMessage reply(B_REG_ERROR);
		reply.AddInt32("error", error);
		request->SendReply(&reply);
	}

	FUNCTION_END();
}

// HandleActivateApp
/*!	\brief Handles a ActivateApp() request.
	\param request The request message
*/
void
TRoster::HandleActivateApp(BMessage *request)
{
	FUNCTION_START();

	status_t error = B_OK;
	// get the parameters
	team_id team;
	if (request->FindInt32("team", &team) != B_OK)
		error = B_BAD_VALUE;
	// activate the app
	if (error == B_OK) {
		if (RosterAppInfo *info = fRegisteredApps.InfoFor(team))
			ActivateApp(info);
		else
			error = B_BAD_TEAM_ID;
	}
	// reply to the request
	if (error == B_OK) {
		BMessage reply(B_REG_SUCCESS);
		request->SendReply(&reply);
	} else {
		BMessage reply(B_REG_ERROR);
		reply.AddInt32("error", error);
		request->SendReply(&reply);
	}

	FUNCTION_END();
}

// AddApp
/*!	\brief Add the supplied app info to the list of (pre-)registered apps.

	\param info The app info to be added
*/
status_t
TRoster::AddApp(RosterAppInfo *info)
{
	status_t error = (info ? B_OK : B_BAD_VALUE);
	if (info) {
		if (fRegisteredApps.AddInfo(info))
			_AppAdded(info);
		else
			error = B_NO_MEMORY;
	}
	return error;
}

// RemoveApp
/*!	\brief Removes the supplied app info from the list of (pre-)registered
	apps.

	\param info The app info to be removed
*/
void
TRoster::RemoveApp(RosterAppInfo *info)
{
	if (info) {
		if (fRegisteredApps.RemoveInfo(info)) {
			info->state = APP_STATE_UNREGISTERED;
			_AppRemoved(info);
		}
	}
}

// ActivateApp
/*!	\brief Activates the application identified by \a info.

	The currently activate application is deactivated and the one whose
	info is supplied is activated. \a info may be \c NULL, which only
	deactivates the currently active application.

	\param info The info of the app to be activated
*/
void
TRoster::ActivateApp(RosterAppInfo *info)
{
	if (info != fActiveApp) {
		// deactivate the currently active app
		RosterAppInfo *oldActiveApp = fActiveApp;
		fActiveApp = NULL;
		if (oldActiveApp)
			_AppDeactivated(oldActiveApp);
		// activate the new app
		if (info) {
			info = fActiveApp;
			_AppActivated(info);
		}
	}
}


// _AppAdded
/*!	\brief Hook method invoked, when an application has been added.
	\param info The RosterAppInfo of the added application.
*/
void
TRoster::_AppAdded(RosterAppInfo *info)
{
}

// _AppRemoved
/*!	\brief Hook method invoked, when an application has been removed.
	\param info The RosterAppInfo of the removed application.
*/
void
TRoster::_AppRemoved(RosterAppInfo *info)
{
	if (info) {
		// deactivate the app, if it was the active one
		if (info == fActiveApp)
			ActivateApp(NULL);
	}
}

// _AppActivated
/*!	\brief Hook method invoked, when an application has been activated.
	\param info The RosterAppInfo of the activated application.
*/
void
TRoster::_AppActivated(RosterAppInfo *info)
{
	if (info) {
		if (info->state == APP_STATE_REGISTERED
			|| info->state == APP_STATE_PRE_REGISTERED) {
			// send B_APP_ACTIVATED to the app
			BMessenger messenger(info->team, info->port, 0, true);
			BMessage message(B_APP_ACTIVATED);
			message.AddBool("active", true);
			messenger.SendMessage(&message);
		}
	}
}

// _AppDeactivated
/*!	\brief Hook method invoked, when an application has been deactivated.
	\param info The RosterAppInfo of the deactivated application.
*/
void
TRoster::_AppDeactivated(RosterAppInfo *info)
{
	if (info) {
		if (info->state == APP_STATE_REGISTERED
			|| info->state == APP_STATE_PRE_REGISTERED) {
			// send B_APP_ACTIVATED to the app
			BMessenger messenger(info->team, info->port, 0, true);
			BMessage message(B_APP_ACTIVATED);
			message.AddBool("active", false);
			messenger.SendMessage(&message);
		}
	}
}

// _AddMessageAppInfo
/*!	\brief Adds an app_info to a message.

	The info is added as a flat_app_info to a field "app_info" with the type
	\c B_REG_APP_INFO_TYPE.

	\param message The message
	\param info The app_info.
	\return \c B_OK if everything went fine, an error code otherwise.
*/
status_t
TRoster::_AddMessageAppInfo(BMessage *message, const app_info *info)
{
	// An app_info is not completely flat. The entry_ref contains a string
	// pointer. Therefore we flatten the info.
	flat_app_info flatInfo;
	flatInfo.info = *info;
	// set the ref name to NULL and copy it into the flat structure
	flatInfo.info.ref.name = NULL;
	flatInfo.ref_name[0] = '\0';
	if (info->ref.name)
		strcpy(flatInfo.ref_name, info->ref.name);
	// add the flat info
	return message->AddData("app_info", B_REG_APP_INFO_TYPE, &flatInfo,
							sizeof(flat_app_info));
}

// _NextToken
/*!	\brief Returns the next available token.
	\return The token.
*/
uint32
TRoster::_NextToken()
{
	return ++fLastToken;
}

// _ReplyToIAPRRequest
/*!	\brief Sends a reply message to a IsAppPreRegistered() request.

	The message to be sent is a simple \c B_REG_SUCCESS message containing
	a "pre-registered" field, that sais whether or not the application is
	pre-registered. It will be set to \c false, unless an \a info is supplied
	and the application this info refers to is pre-registered.

	\param request The request message to be replied to
	\param info The RosterAppInfo of the application in question
		   (may be \c NULL)
*/
void
TRoster::_ReplyToIAPRRequest(BMessage *request, const RosterAppInfo *info)
{
	// pre-registered or registered?
	bool preRegistered = false;
	if (info) {
		switch (info->state) {
			case APP_STATE_PRE_REGISTERED:
				preRegistered = true;
				break;
			case APP_STATE_INVALID:
			case APP_STATE_REGISTERED:
				preRegistered = false;
				break;
		}
	}
	// send reply
	BMessage reply(B_REG_SUCCESS);
	reply.AddBool("pre-registered", preRegistered);
	if (preRegistered)
		_AddMessageAppInfo(&reply, info);
	request->SendReply(&reply);
}

