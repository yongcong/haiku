/*
 * Workspaces - OpenBeOS version by Francois Revol revol@free.fr
 * This file is distributed under the terms of the OpenBeOS license.
 *
 * Workspaces window trick found by Michael "Minox" Paine.
 * (using B_ALL_WORKSPACES as flags in BWindow)
 * Found out that using 0xffffffff as Flags was causing the window not to close on Alt-W
 * hey Workspaces get Flags of Window 0
 * gives 0x00008080 which makes it.
 *
 * Other authors: Axel Dörfler, Oliver "Madison" Kohl, Matt Madia
 */


#include <Application.h>
#include <Alert.h>
#include <File.h>
#include <Window.h>
#include <Screen.h>
#include <Entry.h>
#include <Roster.h>
#include <Path.h>
#include <FindDirectory.h>

#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <ctype.h>


// here is the trick :)
#define B_WORKSPACES_WINDOW 0x00008000

static const char *kWorkspacesSignature = "application/x-vnd.Be-WORK";
static const char *kWorkspacesSettingFile = "Workspace_data";

static const float kScreenBorderOffset = 10.0;


class WorkspacesPreferences {
	public:
		WorkspacesPreferences();
		virtual ~WorkspacesPreferences();

		BRect WindowFrame() const { return fWindowFrame; }
		BRect ScreenFrame() const { return fScreenFrame; }

		void UpdateFramesForScreen(BRect screenFrame);
		void UpdateScreenFrame();
		void SetWindowFrame(BRect);

	private:
		BRect	fWindowFrame, fScreenFrame;
};

class WorkspacesWindow : public BWindow {
	public:
		WorkspacesWindow(WorkspacesPreferences *fPreferences);
		virtual ~WorkspacesWindow();

		virtual void ScreenChanged(BRect frame, color_space mode);
		virtual void FrameMoved(BPoint origin);
		virtual void FrameResized(float width, float height);
		virtual void Zoom(BPoint origin, float width, float height);

		virtual void MessageReceived(BMessage *msg);
		virtual bool QuitRequested(void);

	private:
		WorkspacesPreferences *fPreferences;
		BRect	fPreviousFrame;
};

class WorkspacesApp : public BApplication {
	public:
		WorkspacesApp();
		virtual ~WorkspacesApp();

		virtual void AboutRequested(void);
		virtual void ArgvReceived(int32 argc, char **argv);
		virtual void ReadyToRun(void);

		void Usage(const char *programName);

	private:
		BWindow		*fWindow;
};


WorkspacesPreferences::WorkspacesPreferences()
{
	UpdateScreenFrame();

	bool settingsValid = false;
	BPath path;

	if (find_directory(B_USER_SETTINGS_DIRECTORY, &path) == B_OK) {
		path.Append(kWorkspacesSettingFile);
		BFile file(path.Path(), B_READ_ONLY);
		if (file.InitCheck() == B_OK
			&& file.Read(&fWindowFrame, sizeof(BRect)) == sizeof(BRect)) {
			// we now also store the frame of the screen to know
			// in which context the window frame has been chosen
			BScreen screen;
			BRect frame;
			if (file.Read(&frame, sizeof(BRect)) == sizeof(BRect)) {
				fScreenFrame = frame;
				// if the current screen frame is different from the one
				// just loaded, we need to alter the window frame accordingly
				if (fScreenFrame != screen.Frame())
					UpdateFramesForScreen(screen.Frame());
			}

			// check if loaded values are valid
			if (screen.Frame().right >= fWindowFrame.right
				&& screen.Frame().bottom >= fWindowFrame.bottom) 
				settingsValid = true;	
		}
	}

	if (!settingsValid) {
		// set to some usable defaults
		fWindowFrame = fScreenFrame;
		fWindowFrame.OffsetBy(-kScreenBorderOffset, -kScreenBorderOffset);
		fWindowFrame.left = fWindowFrame.right - 160;
		fWindowFrame.top = fWindowFrame.bottom - 140;
	}
}


WorkspacesPreferences::~WorkspacesPreferences()
{
	// write settings file
	BPath path;
	if (find_directory(B_USER_SETTINGS_DIRECTORY, &path) < B_OK)
		return;

	path.Append(kWorkspacesSettingFile);

	BFile file(path.Path(), B_WRITE_ONLY | B_CREATE_FILE);
	if (file.InitCheck() == B_OK) {
		file.Write(&fWindowFrame, sizeof(BRect));
		file.Write(&fScreenFrame, sizeof(BRect));
	}
}


void
WorkspacesPreferences::UpdateFramesForScreen(BRect newScreenFrame)
{
	// don't change the position if the screen frame hasn't changed
	if (newScreenFrame == fScreenFrame)
		return;

	// adjust horizontal position
	if (fWindowFrame.right > fScreenFrame.right / 2)
		fWindowFrame.OffsetTo(newScreenFrame.right
			- (fScreenFrame.right - fWindowFrame.left), fWindowFrame.top);

	// adjust vertical position
	if (fWindowFrame.bottom > fScreenFrame.bottom / 2)
		fWindowFrame.OffsetTo(fWindowFrame.left,
			newScreenFrame.bottom - (fScreenFrame.bottom - fWindowFrame.top));

	fScreenFrame = newScreenFrame;
}


void
WorkspacesPreferences::UpdateScreenFrame()
{
	BScreen screen;
	fScreenFrame = screen.Frame();
}


void
WorkspacesPreferences::SetWindowFrame(BRect frame)
{
	fWindowFrame = frame;
}


//	#pragma mark -


WorkspacesWindow::WorkspacesWindow(WorkspacesPreferences *preferences)
	: BWindow(preferences->WindowFrame(), "Workspaces", B_TITLED_WINDOW_LOOK,
 			B_NORMAL_WINDOW_FEEL, B_WORKSPACES_WINDOW | B_AVOID_FRONT, B_ALL_WORKSPACES),
 	fPreferences(preferences)
{
	fPreviousFrame = Frame();
}


WorkspacesWindow::~WorkspacesWindow()
{
	delete fPreferences;
}


void
WorkspacesWindow::ScreenChanged(BRect rect, color_space mode)
{
	fPreviousFrame = fPreferences->WindowFrame();
		// work-around for a bug in BeOS, see explanation in FrameMoved()

	fPreferences->UpdateFramesForScreen(rect);
	MoveTo(fPreferences->WindowFrame().LeftTop());
}


void
WorkspacesWindow::FrameMoved(BPoint origin)
{
	if (origin == fPreviousFrame.LeftTop()) {
		// This works around a bug in BeOS; when you change the window
		// position in WorkspaceActivated() or ScreenChanged(), it will
		// send an old repositioning message *after* the FrameMoved()
		// that originated your change has arrived
		return;
	}

	fPreferences->SetWindowFrame(Frame());
}


void
WorkspacesWindow::FrameResized(float width, float height)
{
	fPreferences->SetWindowFrame(Frame());
}


void 
WorkspacesWindow::Zoom(BPoint origin, float width, float height)
{
	BScreen screen;
	origin = screen.Frame().RightBottom();
	origin.x -= kScreenBorderOffset + fPreferences->WindowFrame().Width();
	origin.y -= kScreenBorderOffset + fPreferences->WindowFrame().Height();

	MoveTo(origin);
}


void
WorkspacesWindow::MessageReceived(BMessage *msg)
{
	if (msg->what == 'DATA') {
		// Drop from Tracker
		entry_ref ref;
		for (int i = 0; (msg->FindRef("refs", i, &ref) == B_OK); i++)
			be_roster->Launch(&ref);
	} else
		BWindow::MessageReceived(msg);
}


bool
WorkspacesWindow::QuitRequested(void)
{
	be_app->PostMessage(B_QUIT_REQUESTED);
	return true;
}


//	#pragma mark -


WorkspacesApp::WorkspacesApp()
	: BApplication(kWorkspacesSignature)
{	
	fWindow = new WorkspacesWindow(new WorkspacesPreferences());
}


WorkspacesApp::~WorkspacesApp()
{
}


void
WorkspacesApp::AboutRequested(void)
{
	// blocking !! 
	// the original does the same by the way =)
	(new BAlert("about", "OpenBeOS Workspaces\n"
		"\tby François Revol, Axel Dörfler, Matt Madia.\n\n"
		"original Be version by Robert Polic", "Big Deal"))->Go();
}


void
WorkspacesApp::Usage(const char *programName)
{
	printf("Usage: %s [options] [workspace]\n"
		"where \"options\" is one of:\n"
		"  --notitle\t\ttitle bar removed.  border and resize kept.\n"
		"  --noborder\t\ttitle, border, and resize removed.\n"
		"  --acceptfirstclick\tforces workspace switch on first click.\n"
		"  --avoidfocus\t\tprevents the window from being the target of keyboard events.\n"
		"  --alwaysontop\t\tkeeps window on top\n"
		"  --help\t\tdisplay this help and exit\n"
		"and \"workspace\" is the number of the Workspace to which to switch (0-31)\n",
		programName);

	// quit only if we aren't running already
	if (IsLaunching())
		Quit();
}


void
WorkspacesApp::ArgvReceived(int32 argc, char **argv)
{
	for (int i = 1;  i < argc;  i++) {
		if (argv[i][0] == '-' && argv[i][1] == '-') {
			// evaluate --arguments
			if (!strcmp(argv[i], "--notitle")) {
				fWindow->SetLook(B_MODAL_WINDOW_LOOK);
				fWindow->SetFlags(B_OUTLINE_RESIZE);
			} else if (!strcmp(argv[i], "--noborder")) 
				fWindow->SetLook(B_NO_BORDER_WINDOW_LOOK);
			else if (!strcmp(argv[i], "--acceptfirstclick"))
				fWindow->SetFlags(B_WILL_ACCEPT_FIRST_CLICK);
			else if (!strcmp(argv[i], "--avoidfocus"))
				fWindow->SetFlags(B_AVOID_FOCUS);
			else if (!strcmp(argv[i], "--alwaysontop"))
				fWindow->SetFeel(B_FLOATING_ALL_WINDOW_FEEL);
			else {
				const char *programName = strrchr(argv[0], '/');
				programName = programName ? programName + 1 : argv[0];

				Usage(programName);
			}
		} else if (isdigit(*argv[i])) {
			// check for a numeric arg, if not already given
			activate_workspace(atoi(argv[i]));

			// if the app is running, don't quit
			// but if it isn't, cancel the complete run, so it doesn't
			// open any window
			if (IsLaunching())
				Quit();
		}
	}
}


void
WorkspacesApp::ReadyToRun(void)
{
	fWindow->Show();
}


//	#pragma mark -


int
main(int32 argc, char **argv)
{
	WorkspacesApp app;
	app.Run();

	return 0;
}
