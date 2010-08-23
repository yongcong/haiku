/*
 * Copyright 2010, Haiku, Inc. All rights reserved.
 * Distributed under the terms of the MIT License.
 */
#ifndef _TIME_ZONE_H
#define _TIME_ZONE_H


#include <String.h>


namespace icu_44 {
	class TimeZone;
}


class BTimeZone {
public:
								BTimeZone(const char* zoneID = NULL);
								BTimeZone(const BTimeZone& other);
								~BTimeZone();

			BTimeZone&			operator=(const BTimeZone& source);

			const BString&		ID() const;
			const BString&		Name() const;
			const BString&		DaylightSavingName() const;
			const BString&		ShortName() const;
			const BString&		ShortDaylightSavingName() const;
			int					OffsetFromGMT() const;
			bool				SupportsDaylightSaving() const;

			status_t			InitCheck() const;

			status_t			SetTo(const char* zoneID);

	static  const char*			kNameOfGmtZone;

private:
			icu_44::TimeZone*	fIcuTimeZone;
			status_t			fInitStatus;

	mutable uint32				fInitializedFields;
	mutable BString				fZoneID;
	mutable BString				fName;
	mutable BString				fDaylightSavingName;
	mutable BString				fShortName;
	mutable BString				fShortDaylightSavingName;
	mutable int					fOffsetFromGMT;
	mutable bool				fSupportsDaylightSaving;
};


#endif	// _TIME_ZONE_H
