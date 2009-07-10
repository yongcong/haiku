/*
 * Copyright 2007-2009 Oliver Ruiz Dorantes, oliver.ruiz.dorantes_at_gmail.com
 * Copyright 2008 Mika Lindqvist, monni1995_at_gmail.com
 * All rights reserved. Distributed under the terms of the MIT License.
 */

#include <stdio.h>
#include <fcntl.h>
#include <unistd.h>

#include <Entry.h>
#include <Deskbar.h>
#include <Directory.h>
#include <Message.h>
#include <Path.h>
#include <Roster.h>
#include <String.h>

#include <TypeConstants.h>
#include <syslog.h>

#include <bluetoothserver_p.h>
#include <bluetooth/HCI/btHCI_command.h>
#include <bluetooth/bluetooth_util.h>

#include "BluetoothServer.h"
#include "DeskbarReplicant.h"
#include "LocalDeviceImpl.h"
#include "Output.h"


status_t
DispatchEvent(struct hci_event_header* header, int32 code, size_t size)
{
	// we only handle events
	if (GET_PORTCODE_TYPE(code)!= BT_EVENT) {
		Output::Instance()->Post("Wrong type frame code", BLACKBOARD_KIT);
		return B_OK;
	}
	
	// fetch the LocalDevice who belongs this event 	    
    LocalDeviceImpl* lDeviceImplementation = ((BluetoothServer*)be_app)->
    	LocateLocalDeviceImpl(GET_PORTCODE_HID(code));
    	
	if (lDeviceImplementation == NULL) {
		Output::Instance()->Post("LocalDevice could not be fetched", BLACKBOARD_KIT);
		return B_OK;
	}

	lDeviceImplementation->HandleEvent(header);
	
	return B_OK;
}


BluetoothServer::BluetoothServer() : BApplication(BLUETOOTH_SIGNATURE)
{
	Output::Instance()->Run();
	Output::Instance()->SetTitle("Bluetooth message gathering");

	Output::Instance()->AddTab("General", BLACKBOARD_GENERAL);
	Output::Instance()->AddTab("Device Manager", BLACKBOARD_DEVICEMANAGER);
	Output::Instance()->AddTab("Kit", BLACKBOARD_KIT);

	fDeviceManager = new DeviceManager();
	fLocalDevicesList.MakeEmpty();

	fEventListener2 = new BluetoothPortListener(BT_USERLAND_PORT_NAME,
		(BluetoothPortListener::port_listener_func)&DispatchEvent);
}

bool BluetoothServer::QuitRequested(void)
{
	// Finish quitting
	Output::Instance()->Lock();
	Output::Instance()->Quit();

	LocalDeviceImpl* lDeviceImpl = NULL;
	while ((lDeviceImpl = (LocalDeviceImpl *)fLocalDevicesList.RemoveItem((int32)0))
		!= NULL)
		delete lDeviceImpl;
 
 	_RemoveDeskbarIcon();
 
	printf("Accepting quitting of the application\n");
	return BApplication::QuitRequested();
}


void BluetoothServer::ArgvReceived(int32 argc, char **argv)
{
	if (argc>1) {
		if (strcmp(argv[1], "--finish") == 0)
			PostMessage(B_QUIT_REQUESTED);
	}

}


void BluetoothServer::ReadyToRun(void)
{
	fDeviceManager->StartMonitoringDevice("bluetooth/h2");
	fDeviceManager->StartMonitoringDevice("bluetooth/h3");
	fDeviceManager->StartMonitoringDevice("bluetooth/h4");
	fDeviceManager->StartMonitoringDevice("bluetooth/h5");

	if (fEventListener2->Launch() != B_OK)
		Output::Instance()->Post("Bluetooth port listener failed\n", BLACKBOARD_GENERAL);
	else
		Output::Instance()->Post("Bluetooth server Ready\n", BLACKBOARD_GENERAL);

	ShowWindow(Output::Instance());

	_InstallDeskbarIcon();
}


void BluetoothServer::AppActivated(bool act)
{
	printf("Activated %d\n",act);
}


void BluetoothServer::MessageReceived(BMessage *message)
{
	BMessage reply;
	status_t status = B_WOULD_BLOCK; // mark somehow to do not reply anything

	switch(message->what)
	{
		case BT_MSG_ADD_DEVICE:
		{
			BString str;
			message->FindString("name", &str);
			BPath path(str.String());
			Output::Instance()->Postf(BLACKBOARD_GENERAL,
				"Requested LocalDevice %s\n", str.String());
			LocalDeviceImpl* lDeviceImpl = LocalDeviceImpl::CreateTransportAccessor(&path);

			if (lDeviceImpl->GetID() >= 0) {
				fLocalDevicesList.AddItem(lDeviceImpl);
				Output::Instance()->AddTab("Local Device", BLACKBOARD_LD(lDeviceImpl->GetID()));
				Output::Instance()->Postf(BLACKBOARD_LD(lDeviceImpl->GetID()),
					"LocalDevice %s id=%x added\n", str.String(), lDeviceImpl->GetID());

			} else {
				Output::Instance()->Post("Adding LocalDevice failed\n",
					BLACKBOARD_GENERAL);
			}

			status = B_WOULD_BLOCK;
			/* TODO: This should be by user request only! */
			lDeviceImpl->Launch();
			break;
		}
		
		case BT_MSG_REMOVE_DEVICE:
		{
			LocalDeviceImpl* lDeviceImpl = LocateDelegateFromMessage(message);
			if (lDeviceImpl != NULL) {
				fLocalDevicesList.RemoveItem(lDeviceImpl);
				delete lDeviceImpl;
			}
			break;
		}
		
		case BT_MSG_COUNT_LOCAL_DEVICES: 
			status = HandleLocalDevicesCount(message, &reply);
			break;

		case BT_MSG_ACQUIRE_LOCAL_DEVICE:
			status = HandleAcquireLocalDevice(message, &reply);
			break;

		case BT_MSG_HANDLE_SIMPLE_REQUEST:
			status = HandleSimpleRequest(message, &reply);
			break;

		case BT_MSG_GET_PROPERTY:
			status = HandleGetProperty(message, &reply);
			break;
		
		/* Handle if the bluetooth preferences is running?? */
		case B_SOME_APP_LAUNCHED:
   		{
			const char *signature;
			// TODO: what's this for?
			if (message->FindString("be:signature", &signature) == B_OK) {
				printf("input_server : %s\n", signature);
				if (strcmp(signature, "application/x-vnd.Be-TSKB") == 0) {

				}
			}
			return;
		}
		
		case BT_MSG_SERVER_SHOW_CONSOLE:
			ShowWindow(Output::Instance());
			break;

		default:
			BApplication::MessageReceived(message);
			break;
	}
	
	// Can we reply right now?
	// TOD: review this condition
	if (status != B_WOULD_BLOCK) {
		reply.AddInt32("status", status);
		message->SendReply(&reply);
		printf("Sending reply message for->\n");
		message->PrintToStream();
	}
}

#if 0
#pragma mark -
#endif


LocalDeviceImpl* 
BluetoothServer::LocateDelegateFromMessage(BMessage* message)
{
	LocalDeviceImpl* lDeviceImpl = NULL;
	hci_id hid;

	if (message->FindInt32("hci_id", &hid) == B_OK) {
		/* Try to find out when a ID was specified */
		int index;
		for (index = 0; index < fLocalDevicesList.CountItems(); index ++) {
		    lDeviceImpl = fLocalDevicesList.ItemAt(index);
		    if (lDeviceImpl->GetID() == hid)
		        break;
		}
	}

	return lDeviceImpl;

}

LocalDeviceImpl*
BluetoothServer::LocateLocalDeviceImpl(hci_id hid) 
{
	/* Try to find out when a ID was specified */
	int index;

	for (index = 0; index < fLocalDevicesList.CountItems(); index ++) {
		LocalDeviceImpl* lDeviceImpl = fLocalDevicesList.ItemAt(index);
		if (lDeviceImpl->GetID() == hid) 
			return lDeviceImpl;
	}

	return NULL;
}


#if 0
#pragma - Messages reply
#endif

status_t
BluetoothServer::HandleLocalDevicesCount(BMessage* message, BMessage* reply)
{
	return reply->AddInt32("count", fLocalDevicesList.CountItems());
}


status_t
BluetoothServer::HandleAcquireLocalDevice(BMessage* message, BMessage* reply)
{
	hci_id hid;
	ssize_t size;
	bdaddr_t bdaddr;
	LocalDeviceImpl* lDeviceImpl = NULL;
	static int32 lastIndex = 0;
	
	if (message->FindInt32("hci_id", &hid) == B_OK)
	{
		Output::Instance()->Post("GetLocalDevice requested with id\n", 
						BLACKBOARD_KIT);
		lDeviceImpl = LocateDelegateFromMessage(message);

	} else if (message->FindData("bdaddr", B_ANY_TYPE, (const void**)&bdaddr, &size ) 
			== B_OK) {
		/* Try to find out when the user specified the address */
		Output::Instance()->Post("GetLocalDevice requested with bdaddr\n", 
						BLACKBOARD_KIT);
		for (lastIndex = 0; lastIndex < fLocalDevicesList.CountItems(); lastIndex ++) {
			//TODO: Only possible if the property is available
			//bdaddr_t local;
			//lDeviceImpl = fLocalDevicesList.ItemAt(lastIndex);
			//if ((lDeviceImpl->GetAddress(&local, message) == B_OK) 
			//	&& bacmp(&local, &bdaddr))  {
			//    break;
			//}
		}

	} else	{
		// Careless, any device not performing operations will be fine
		Output::Instance()->Post("GetLocalDevice plain request\n", BLACKBOARD_KIT);
		// from last assigned till end
		for ( int index  = lastIndex + 1; index < fLocalDevicesList.CountItems(); index ++) {
			lDeviceImpl = fLocalDevicesList.ItemAt(index);
			printf("Requesting local device %ld\n", lDeviceImpl->GetID());
			if (lDeviceImpl != NULL && lDeviceImpl->Available())
			{
				Output::Instance()->Postf(BLACKBOARD_KIT, "Device available: %lx\n", lDeviceImpl->GetID());
				lastIndex = index;
				break;
			}
		}	

		// from starting till last assigned if not yet found
		if (lDeviceImpl == NULL) {
			for ( int index = 0; index <= lastIndex ; index ++) {
				lDeviceImpl = fLocalDevicesList.ItemAt(index);
				printf("Requesting local device %ld\n", lDeviceImpl->GetID());
				if (lDeviceImpl != NULL && lDeviceImpl->Available())
				{
					Output::Instance()->Postf(BLACKBOARD_KIT, "Device available: %lx\n", lDeviceImpl->GetID());
					lastIndex = index;
					break;
				}
			}
		}
	}
	
	if (lastIndex <= fLocalDevicesList.CountItems() && lDeviceImpl != NULL && lDeviceImpl->Available()) {
		hid = lDeviceImpl->GetID();
		lDeviceImpl->Acquire();
		
		Output::Instance()->Postf(BLACKBOARD_KIT, "Device acquired %lx\n", hid);
		return reply->AddInt32("hci_id", hid);
	}

	return B_ERROR;

}


status_t
BluetoothServer::HandleSimpleRequest(BMessage* message, BMessage* reply)
{
	LocalDeviceImpl* lDeviceImpl = LocateDelegateFromMessage(message);
	if (lDeviceImpl == NULL) {
		return B_ERROR;
	}
	
	const char* propertyRequested;

	// Find out if there is a property being requested,
	if (message->FindString("property", &propertyRequested) == B_OK) {
		// Check if the property has been already retrieved
		if (lDeviceImpl->IsPropertyAvailable(propertyRequested)) {
			// Dump everything
			reply->AddMessage("properties", lDeviceImpl->GetPropertiesMessage());
			return B_OK;
		}
	}
	
	// we are gonna need issue the command ...
	if (lDeviceImpl->ProcessSimpleRequest(DetachCurrentMessage()) == B_OK)
		return B_WOULD_BLOCK;
	else {
		lDeviceImpl->Unregister();
		return B_ERROR;
	}

}


status_t
BluetoothServer::HandleGetProperty(BMessage* message, BMessage* reply)
{
	/* User side will look for the reply in a result field
	 * and will not care about status fields, therefore we return OK in all cases
	 */
	LocalDeviceImpl* lDeviceImpl = LocateDelegateFromMessage(message);
	if (lDeviceImpl == NULL) {
		return B_ERROR;
	}
	
	const char* propertyRequested;

	// Find out if there is a property being requested,
	if (message->FindString("property", &propertyRequested) == B_OK) {
		
		Output::Instance()->Postf(BLACKBOARD_LD(lDeviceImpl->GetID()), "Searching %s property...\n",
					propertyRequested);

		// Check if the property has been already retrieved
		if (lDeviceImpl->IsPropertyAvailable(propertyRequested)) {
			if (strcmp(propertyRequested, "hci_version") == 0
				|| strcmp(propertyRequested, "lmp_version") == 0
			    || strcmp(propertyRequested, "sco_mtu") == 0) {
			    	
				uint8 result = lDeviceImpl->GetPropertiesMessage()->FindInt8(propertyRequested);
				reply->AddInt32("result", result);
				
			} else if (strcmp(propertyRequested, "hci_revision") == 0
					   || strcmp(propertyRequested, "lmp_subversion") == 0
					   || strcmp(propertyRequested, "manufacturer") == 0
					   || strcmp(propertyRequested, "acl_mtu") == 0
					   || strcmp(propertyRequested, "acl_max_pkt") == 0
					   || strcmp(propertyRequested, "sco_max_pkt") == 0 ) {
					   	
				uint16 result = lDeviceImpl->GetPropertiesMessage()->FindInt16(propertyRequested);
				reply->AddInt32("result", result);
				
			} else {
				Output::Instance()->Postf(BLACKBOARD_LD(lDeviceImpl->GetID()), "Property %s could not be satisfied\n",
						propertyRequested);
			}
		}
	}

	return B_OK;
}

#if 0
#pragma mark -
#endif

int32 
BluetoothServer::sdp_server_Thread(void* data)
{

	return B_NO_ERROR;	
}


void
BluetoothServer::ShowWindow(BWindow* pWindow)
{
	pWindow->Lock();
	if (pWindow->IsHidden())
		pWindow->Show();
	else
		pWindow->Activate();
	pWindow->Unlock();
}


void
BluetoothServer::_InstallDeskbarIcon()
{
	app_info appInfo;
	be_app->GetAppInfo(&appInfo);
	
	BDeskbar deskbar;
	
	if (deskbar.HasItem(kDeskbarItemName)) {
		_RemoveDeskbarIcon();
	}
	
	status_t res = deskbar.AddItem(&appInfo.ref);
	if (res != B_OK) {
		printf("Failed adding deskbar icon: %ld\n", res);
	}
}


void
BluetoothServer::_RemoveDeskbarIcon()
{
	BDeskbar deskbar;
	status_t res = deskbar.RemoveItem(kDeskbarItemName);
	if (res != B_OK) {
		printf("Failed removing Deskbar icon: %ld: \n", res);
	}
}

#if 0
#pragma mark -
#endif

int
main(int /*argc*/, char** /*argv*/)
{
	setbuf(stdout, NULL);

	BluetoothServer* bluetoothServer = new BluetoothServer;

	bluetoothServer->Run();
	delete bluetoothServer;

	return 0;
}

