#ifndef _COUNTRY_H_
#define _COUNTRY_H_


#include <SupportDefs.h>
#include <LocaleStrings.h>
#include <String.h>


class BBitmap;
class BMessage;

namespace icu_44 {
	class DateFormat;
	class Locale;
}


enum {
	B_METRIC = 0,
	B_US
};

typedef enum {
	B_DATE_ELEMENT_INVALID = B_BAD_DATA,
	B_DATE_ELEMENT_YEAR = 0,
	B_DATE_ELEMENT_MONTH,
	B_DATE_ELEMENT_DAY,
	B_DATE_ELEMENT_AM_PM,
	B_DATE_ELEMENT_HOUR,
	B_DATE_ELEMENT_MINUTE,
	B_DATE_ELEMENT_SECOND
} BDateElement;


class BCountry {
	public:
		BCountry(const char* languageCode, const char* countryCode);
		BCountry(const char* languageAndCountryCode = "en_US");
		BCountry(const BCountry& other);
		BCountry& operator=(const BCountry& other);
		virtual 		~BCountry();

		virtual bool 	Name(BString&) const;
		bool			LocaleName(BString&) const;
		const char*		Code() const;
		status_t		GetIcon(BBitmap* result);

		const char*		GetString(uint32 id) const;

		// date & time

		virtual void	FormatDate(char* string, size_t maxSize, time_t time,
			bool longFormat);
		virtual void	FormatDate(BString* string, time_t time,
			bool longFormat);
		status_t		FormatDate(BString* string, int*& fieldPositions,
			int& fieldCount, time_t time, bool longFormat);

		virtual void	FormatTime(char* string, size_t maxSize, time_t time,
			bool longFormat);
		virtual void	FormatTime(BString* string, time_t time,
			bool longFormat);
		status_t		FormatTime(BString* string, int*& fieldPositions,
			int& fieldCount, time_t time, bool longFormat);
		status_t		TimeFields(BDateElement*& fields, int& fieldCount,
			bool longFormat);
		status_t		DateFields(BDateElement*& fields, int& fieldCount,
			bool longFormat);

		bool		DateFormat(BString&, bool longFormat) const;
		void		SetDateFormat(const char* formatString,
						bool longFormat = true);
		void		SetTimeFormat(const char* formatString,
						bool longFormat = true);
		bool		TimeFormat(BString&, bool longFormat) const;

		int			StartOfWeek();

		// numbers

		virtual void FormatNumber(char* string, size_t maxSize, double value);
		virtual status_t FormatNumber(BString* string, double value);
		virtual void FormatNumber(char* string, size_t maxSize, int32 value);
		virtual void FormatNumber(BString* string, int32 value);

		bool		DecimalPoint(BString&) const;
		bool		ThousandsSeparator(BString&) const;
		bool		Grouping(BString&) const;

		bool		PositiveSign(BString&) const;
		bool		NegativeSign(BString&) const;

		// measurements

		virtual int8	Measurement() const;

		// monetary

		virtual ssize_t	FormatMonetary(char* string, size_t maxSize,
			double value);
		virtual ssize_t	FormatMonetary(BString* string, double value);

		bool		CurrencySymbol(BString&) const;
		bool		InternationalCurrencySymbol(BString&) const;
		bool		MonDecimalPoint(BString&) const;
		bool		MonThousandsSeparator(BString&) const;
		bool		MonGrouping(BString&) const;
		virtual int32	MonFracDigits() const;

		// timezones
		status_t GetTimeZones(BMessage* timezones);

	private:
		icu_44::DateFormat* fICULongDateFormatter;
		icu_44::DateFormat* fICUShortDateFormatter;
		icu_44::DateFormat* fICULongTimeFormatter;
		icu_44::DateFormat* fICUShortTimeFormatter;
		icu_44::Locale* fICULocale;
};

#endif	/* _COUNTRY_H_ */
