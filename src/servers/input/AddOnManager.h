/*
 * Copyright 2004-2005, Haiku, Inc. All rights reserved.
 * Distributed under the terms of the MIT License.
 *
 * Authors:
 *		Marcus Overhagen, Axel Dörfler
 *		Jérôme Duval
 */
#ifndef _ADD_ON_MANAGER_H
#define _ADD_ON_MANAGER_H

// Manager for input_server add-ons (devices, filters, methods)


#include <Locker.h>
#include <Looper.h>
#include <InputServerDevice.h>
#include <InputServerFilter.h>
#include <InputServerMethod.h>
#include "AddOnMonitor.h"
#include "AddOnMonitorHandler.h"
#include "TList.h"


class AddOnManager : public BLooper {
	public:
		AddOnManager(bool safeMode);
		~AddOnManager();

		void		LoadState();
		void		SaveState();
		void 		MessageReceived(BMessage *message);

	private:
		status_t	RegisterAddOn(BEntry &entry);
		status_t	UnregisterAddOn(BEntry &entry);
		void		RegisterAddOns();
		void		UnregisterAddOns();

		void		RegisterDevice(BInputServerDevice *isd, const entry_ref &ref, image_id addon_image);
		void		RegisterFilter(BInputServerFilter *isf, const entry_ref &ref, image_id addon_image);
		void		RegisterMethod(BInputServerMethod *ism, const entry_ref &ref, image_id addon_image);
		
		status_t HandleFindDevices(BMessage*, BMessage*);
		status_t HandleWatchDevices(BMessage*, BMessage*);
		status_t HandleIsDeviceRunning(BMessage*, BMessage*);
		status_t HandleStartStopDevices(BMessage*, BMessage*);
		status_t HandleControlDevices(BMessage*, BMessage*);
		status_t HandleSystemShuttingDown(BMessage*, BMessage*);
		status_t HandleMethodReplicant(BMessage*, BMessage*);
		status_t HandleNodeMonitor(BMessage*);
		
		void LoadReplicant();
		void UnloadReplicant();
		int32 GetReplicantAt(BMessenger target, int32 index) const;
		status_t GetReplicantName(BMessenger target, int32 uid, BMessage *reply) const;
		status_t GetReplicantView(BMessenger target, int32 uid, BMessage *reply) const;
	
	private:
		class InputServerMonitorHandler;
		friend class InputServerMonitorHandler;

		struct device_info {
			entry_ref ref;
			image_id addon_image;
			BInputServerDevice *device;
		};
		struct filter_info {
			entry_ref ref;
			image_id addon_image;
			BInputServerFilter *filter;
		};
		struct method_info {
			entry_ref ref;
			image_id addon_image;
			BInputServerMethod *method;
		};

		BLocker fLock;
		List<device_info> fDeviceList;
		List<filter_info> fFilterList;
		List<method_info> fMethodList;

		AddOnMonitorHandler	*fHandler;
		AddOnMonitor		*fAddOnMonitor;

		bool fSafeMode;
};

#endif // _ADD_ON_MANAGER_H
