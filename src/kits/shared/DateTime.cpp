/*
 * Copyright 2004-2008, Haiku, Inc. All Rights Reserved.
 * Distributed under the terms of the MIT License.
 *
 * Authors:
 *		Julun <host.haiku@gmx.de>
 *		Stephan Aßmus <superstippi@gmx.de>
 */

#include <DateTime.h>


#include <time.h>


namespace BPrivate {


BTime::BTime()
	: fHour(-1),
	  fMinute(-1),
	  fSecond(-1)
{
}


BTime::BTime(int32 hour, int32 minute, int32 second)
	: fHour(hour),
	  fMinute(minute),
	  fSecond(second)
{
}


BTime::~BTime()
{
}


bool
BTime::IsValid() const
{
	if (fHour < 0 || fHour >= 24)
		return false;

	if (fMinute < 0 || fMinute >= 60)
		return false;

	if (fSecond < 0 || fSecond >= 60)
		return false;

	return true;
}


BTime
BTime::CurrentTime(time_type type)
{
	time_t timer;
	struct tm result;
	struct tm* timeinfo;

	time(&timer);

	if (type == B_GMT_TIME)
		timeinfo = gmtime_r(&timer, &result);
	else
		timeinfo = localtime_r(&timer, &result);

	int32 sec = timeinfo->tm_sec;
	return BTime(timeinfo->tm_hour, timeinfo->tm_min, (sec > 59) ? 59 : sec);
}


bool
BTime::SetTime(int32 hour, int32 minute, int32 second)
{
	fHour = hour;
	fMinute = minute;
	fSecond = second;

	return IsValid();
}


int32
BTime::Hour() const
{
	return fHour;
}


int32
BTime::Minute() const
{
	return fMinute;
}


int32
BTime::Second() const
{
	return fSecond;
}


//	#pragma mark - BDate


BDate::BDate()
	: fDay(-1),
	  fYear(-1),
	  fMonth(-1)
{
}


BDate::BDate(int32 year, int32 month, int32 day)
{
	_SetDate(year, month, day);
}


BDate::~BDate()
{
}


bool
BDate::IsValid() const
{
	return IsValid(fYear, fMonth, fDay);
}


bool
BDate::IsValid(const BDate& date) const
{
	return IsValid(date.fYear, date.fMonth, date.fDay);
}


bool
BDate::IsValid(int32 year, int32 month, int32 day) const
{
	// no year 0 in Julian and we can't handle nothing before 1.1.4713 BC
	if (year == 0 || year < -4713
		|| (year == -4713 && month < 1)
		|| (year == -4713 && month < 1 && day < 1))
		return false;

	// 'missing' days between switch julian - gregorian
	if (year == 1582 && month == 10 && day > 4 && day < 15)
		return false;

	if (month < 1 || month > 12)
		return false;

	if (day < 1 || day > _DaysInMonth(year, month))
		return false;

	return true;
}


BDate
BDate::CurrentDate(time_type type)
{
	time_t timer;
	struct tm result;
	struct tm* timeinfo;

	time(&timer);

	if (type == B_GMT_TIME)
		timeinfo = gmtime_r(&timer, &result);
	else
		timeinfo = localtime_r(&timer, &result);

	return BDate(timeinfo->tm_year + 1900, timeinfo->tm_mon +1, timeinfo->tm_mday);
}


BDate
BDate::Date() const
{
	return BDate(fYear, fMonth, fDay);
}


bool
BDate::SetDate(const BDate& date)
{
	return _SetDate(date.fYear, date.fMonth, date.fDay);
}


bool
BDate::SetDate(int32 year, int32 month, int32 day)
{
	return _SetDate(year, month, day);
}


void
BDate::GetDate(int32* year, int32* month, int32* day)
{
	if (year)
		*year = fYear;

	if (month)
		*month = fMonth;

	if (day)
		*day = fDay;
}


void
BDate::AddDays(int32 days)
{
	*this = JulianDayToDate(DateToJulianDay() + days);
}


void
BDate::AddYears(int32 years)
{
	fYear += years;
	fDay = min_c(fDay, _DaysInMonth(fYear, fMonth));
}


void
BDate::AddMonths(int32 months)
{
	fYear += months / 12;
	fMonth +=  months % 12;

	if (fMonth > 12) {
		fYear++;
		fMonth -= 12;
	} else if (fMonth < 1) {
		fYear--;
		fMonth += 12;
	}
	fDay = min_c(fDay, DaysInMonth());
}


int32
BDate::Day() const
{
	return fDay;
}


int32
BDate::Year() const
{
	return fYear;
}


int32
BDate::Month() const
{
	return fMonth;
}


int32
BDate::Difference(const BDate& date) const
{
	return DateToJulianDay() - date.DateToJulianDay();
}


int32
BDate::WeekNumber() const
{
	/*
		This algorithm is taken from:
		Frequently Asked Questions about Calendars
		Version 2.8 Claus Tøndering 15 December 2005

		Note: it will work only within the Gregorian Calendar
	*/

	if (!IsValid() || fYear < 1582
		|| (fYear == 1582 && fMonth < 10)
		|| (fYear == 1582 && fMonth == 10 && fDay < 15))
		return 0;

	int32 a;
	int32 b;
	int32 s;
	int32 e;
	int32 f;

	if (fMonth > 0 && fMonth < 3) {
		a = fYear - 1;
		b = (a / 4) - (a / 100) + (a / 400);
		int32 c = ((a - 1) / 4) - ((a - 1) / 100) + ((a -1) / 400);
		s = b - c;
		e = 0;
		f = fDay - 1 + 31 * (fMonth - 1);
	} else if (fMonth >= 3 && fMonth <= 12) {
		a = fYear;
		b = (a / 4) - (a / 100) + (a / 400);
		int32 c = ((a - 1) / 4) - ((a - 1) / 100) + ((a -1) / 400);
		s = b - c;
		e = s + 1;
		f = fDay + ((153 * (fMonth - 3) + 2) / 5) + 58 + s;
	} else
		return 0;

	int32 g = (a + b) % 7;
	int32 d = (f + g - e) % 7;
	int32 n = f + 3 - d;

	int32 weekNumber;
	if (n < 0)
		weekNumber = 53 - (g -s) / 5;
	else if (n > 364 + s)
		weekNumber = 1;
	else
		weekNumber = n / 7 + 1;

	return weekNumber;
}


int32
BDate::DayOfWeek() const
{
	// http://en.wikipedia.org/wiki/Julian_day#Calculation
	return (DateToJulianDay() % 7) + 1;
}


int32
BDate::DayOfYear() const
{
	return DateToJulianDay() - _DateToJulianDay(fYear, 1, 1) + 1;
}


bool
BDate::IsLeapYear(int32 year) const
{
	if (year < 1582) {
		if (year < 0) year++;
		return (year % 4) == 0;
	}
	return year % 400 == 0 || year % 4 == 0 && year % 100 != 0;
}


int32
BDate::DaysInYear() const
{
	if (!IsValid())
		return 0;

	return IsLeapYear(fYear) ? 366 : 365;
}


int32
BDate::DaysInMonth() const
{
	return _DaysInMonth(fYear, fMonth);
}


BString
BDate::ShortDayName(int32 day) const
{
	if (day < 1 || day > 7)
		return BString();

	tm tm_struct;
	memset(&tm_struct, 0, sizeof(tm));
	tm_struct.tm_wday = day == 7 ? 0 : day;

	char buffer[256];
	strftime(buffer, sizeof(buffer), "%a", &tm_struct);

	return BString(buffer);
}


BString
BDate::ShortMonthName(int32 month) const
{
	if (month < 1 || month > 12)
		return BString();

	tm tm_struct;
	memset(&tm_struct, 0, sizeof(tm));
	tm_struct.tm_mon = month -1;

	char buffer[256];
	strftime(buffer, sizeof(buffer), "%b", &tm_struct);

	return BString(buffer);
}


BString
BDate::LongDayName(int32 day) const
{
	if (day < 1 || day > 7)
		return BString();

	tm tm_struct;
	memset(&tm_struct, 0, sizeof(tm));
	tm_struct.tm_wday = day == 7 ? 0 : day;

	char buffer[256];
	strftime(buffer, sizeof(buffer), "%A", &tm_struct);

	return BString(buffer);
}


BString
BDate::LongMonthName(int32 month) const
{
	if (month < 1 || month > 12)
		return BString();

	tm tm_struct;
	memset(&tm_struct, 0, sizeof(tm));
	tm_struct.tm_mon = month -1;

	char buffer[256];
	strftime(buffer, sizeof(buffer), "%B", &tm_struct);

	return BString(buffer);
}


int32
BDate::DateToJulianDay() const
{
	return _DateToJulianDay(fYear, fMonth, fDay);
}


BDate
BDate::JulianDayToDate(int32 julianDay)
{
	BDate date;
	const int32 kGregorianCalendarStart = 2299161;
	if (julianDay >= kGregorianCalendarStart) {
		// http://en.wikipedia.org/wiki/Julian_day#Gregorian_calendar_from_Julian_day_number
		int32 j = julianDay + 32044;
		int32 dg = j % 146097;
		int32 c = (dg / 36524 + 1) * 3 / 4;
		int32 dc = dg - c * 36524;
		int32 db = dc % 1461;
		int32 a = (db / 365 + 1) * 3 / 4;
		int32 da = db - a * 365;
		int32 m = (da * 5 + 308) / 153 - 2;
		date.fYear = ((j / 146097) * 400 + c * 100 + (dc / 1461) * 4 + a) - 4800 +
			(m + 2) / 12;
		date.fMonth = (m + 2) % 12 + 1;
		date.fDay = int32((da - (m + 4) * 153 / 5 + 122) + 1.5);
	} else {
		// http://en.wikipedia.org/wiki/Julian_day#Calculation
		julianDay += 32082;
		int32 d = (4 * julianDay + 3) / 1461;
		int32 e = julianDay - (1461 * d) / 4;
		int32 m = ((5 * e) + 2) / 153;
		date.fDay = e - (153 * m + 2) / 5 + 1;
		date.fMonth = m + 3 - 12 * (m / 10);
		int32 year = d - 4800 + (m / 10);
		if (year <= 0)
			year--;
		date.fYear = year;
	}
	return date;
}


bool
BDate::operator!=(const BDate& date) const
{
	return DateToJulianDay() != date.DateToJulianDay();
}


bool
BDate::operator==(const BDate& date) const
{
	return DateToJulianDay() == date.DateToJulianDay();
}


bool
BDate::operator<(const BDate& date) const
{
	return DateToJulianDay() < date.DateToJulianDay();
}


bool
BDate::operator<=(const BDate& date) const
{
	return DateToJulianDay() <= date.DateToJulianDay();
}


bool
BDate::operator>(const BDate& date) const
{
	return DateToJulianDay() > date.DateToJulianDay();
}


bool
BDate::operator>=(const BDate& date) const
{
	return DateToJulianDay() >= date.DateToJulianDay();
}


bool
BDate::_SetDate(int32 year, int32 month, int32 day)
{
	fDay = -1;
	fYear = -1;
	fMonth = -1;

	bool valid = IsValid(year, month, day);
	if (valid) {
		fDay = day;
		fYear = year;
		fMonth = month;
	}

	return valid;
}


int32
BDate::_DaysInMonth(int32 year, int32 month) const
{
	if (month == 2 && IsLeapYear(year))
		return 29;

	const int32 daysInMonth[12] =
		{31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};

	return daysInMonth[month -1];
}


int32
BDate::_DateToJulianDay(int32 _year, int32 month, int32 day) const
{
	int32 year = _year;
	if (year < 0) year++;

	int32 a = (14 - month) / 12;
	int32 y = year + 4800 - a;
	int32 m = month + (12 * a) - 3;

	// http://en.wikipedia.org/wiki/Julian_day#Calculation
	if (year > 1582
		|| (year == 1582 && month > 10)
		|| (year == 1582 && month == 10 && day >= 15)) {
		return day + (((153 * m) + 2) / 5) + (365 * y) + (y / 4) -
					(y / 100) + (y / 400) - 32045;
	} else if (year < 1582
		|| (year == 1582 && month < 10)
		|| (year == 1582 && month == 10 && day <= 4)) {
		return day + (((153 * m) + 2) / 5) + (365 * y) + (y / 4) - 32083;
	}

	// http://en.wikipedia.org/wiki/Gregorian_calendar:
	//		The last day of the Julian calendar was Thursday October 4, 1582
	//		and this was followed by the first day of the Gregorian calendar,
	//		Friday October 15, 1582 (the cycle of weekdays was not affected).
	return -1;
}


//	#pragma mark - BDateTime


BDateTime::BDateTime(const BDate &date, const BTime &time)
	: fDate(date),
	  fTime(time)
{
}


BDateTime::~BDateTime()
{
}


bool
BDateTime::IsValid() const
{
	return fDate.IsValid() && fTime.IsValid();
}


BDateTime
BDateTime::CurrentDateTime(time_type type)
{
	BDate date = BDate::CurrentDate(type);
	BTime time = BTime::CurrentTime(type);

	return BDateTime(date, time);
}


void
BDateTime::SetDateTime(const BDate &date, const BTime &time)
{
	fDate = date;
	fTime = time;
}


BDate
BDateTime::Date() const
{
	return fDate;
}


void
BDateTime::SetDate(const BDate &date)
{
	fDate = date;
}


BTime
BDateTime::Time() const
{
	return fTime;
}


void
BDateTime::SetTime(const BTime &time)
{
	fTime = time;
}


uint32
BDateTime::Time_t() const
{
	tm tm_struct;

	tm_struct.tm_hour = fTime.Hour();
	tm_struct.tm_min = fTime.Minute();
	tm_struct.tm_sec = fTime.Second();

	tm_struct.tm_year = fDate.Year() - 1900;
	tm_struct.tm_mon = fDate.Month() - 1;
	tm_struct.tm_mday = fDate.Day();

	// set less 0 as we wan't use it
	tm_struct.tm_isdst = -1;

	// return secs_since_jan1_1970 or -1 on error
	return uint32(mktime(&tm_struct));
}

}	//namespace BPrivate
