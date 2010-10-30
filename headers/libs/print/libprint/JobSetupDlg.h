/*
 * JobSetupDlg.cpp
 * Copyright 1999-2000 Y.Takagi. All Rights Reserved.
 */

#ifndef __JOBSETUPDLG_H
#define __JOBSETUPDLG_H

#include <View.h>
#include <map>

#include "DialogWindow.h"

#include "JobData.h"
#include "Halftone.h"
#include "JSDSlider.h"
#include "PrinterCap.h"

class BCheckBox;
class BGridLayout;
class BPopUpMenu;
class BRadioButton;
class BTextControl;
class BTextView;
class HalftoneView;
class JobData;
class PagesView;
class PrinterCap;
class PrinterData;

class JobSetupView : public BView {
public:
					JobSetupView(JobData* jobData, PrinterData* printerData,
						const PrinterCap* printerCap);
	virtual	void	AttachedToWindow();
	virtual void	MessageReceived(BMessage* message);
			bool	UpdateJobData(bool showPreview);

private:
			void	UpdateButtonEnabledState();
			bool	IsHalftoneConfigurationNeeded();
			void	CreateHalftoneConfigurationUI();
			void	AddDriverSpecificSettings(BGridLayout* gridLayout, int row);
			string	GetDriverSpecificValue(PrinterCap::CapID category,
						const char* key);
			template<typename Predicate>
			void	FillCapabilityMenu(BPopUpMenu* menu, uint32 message,
						const BaseCap** capabilities, int count,
						Predicate& predicate);
			void	FillCapabilityMenu(BPopUpMenu* menu, uint32 message,
						PrinterCap::CapID category, int id);
			void	FillCapabilityMenu(BPopUpMenu* menu, uint32 message,
						const BaseCap** capabilities, int count, int id);
			int		GetID(const BaseCap** capabilities, int count,
						const char* label, int defaultValue);
			BRadioButton* CreatePageSelectionItem(const char* name,
						const char* label,
						JobData::PageSelection pageSelection);
			void	AllowOnlyDigits(BTextView* textView, int maxDigits);
			void	UpdateHalftonePreview();

			JobData::Color			Color();
			Halftone::DitherType	DitherType();
			float					Gamma();
			float					InkDensity();
			JobData::PaperSource	PaperSource();

	BTextControl*		fCopies;
	BTextControl*		fFromPage;
	BTextControl*		fToPage;
	JobData*			fJobData;
	PrinterData*		fPrinterData;
	const PrinterCap*	fPrinterCap;
	BPopUpMenu*			fColorType;
	BPopUpMenu*			fDitherType;
	BMenuField*			fDitherMenuField;
	JSDSlider*			fGamma;
	JSDSlider*			fInkDensity;
	HalftoneView*		fHalftone;
	BBox*				fHalftoneBox;
	BRadioButton*		fAll;
	BCheckBox*			fCollate;
	BCheckBox*			fReverse;
	PagesView*			fPages;
	BPopUpMenu*			fPaperFeed;
	BCheckBox*			fDuplex;
	BPopUpMenu*			fNup;
	BRadioButton*		fAllPages;
	BRadioButton*		fOddNumberedPages;
	BRadioButton*		fEvenNumberedPages;
	std::map<PrinterCap::CapID, BPopUpMenu*> fDriverSpecificLists;
};

class JobSetupDlg : public DialogWindow {
public:
					JobSetupDlg(JobData* jobData, PrinterData* printerData,
						const PrinterCap* printerCap);
	virtual	void	MessageReceived(BMessage* message);

private:
	JobSetupView*	fJobSetup;
};

#endif	/* __JOBSETUPDLG_H */
