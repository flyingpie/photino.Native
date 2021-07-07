// For this to build on WSL (Ubuntu 18.04) you need to:
//  sudo apt-get install libgtk-3-dev libwebkit2gtk-4.0-dev
#ifdef __linux__
#include "Photino.h"
#include <mutex>
#include <condition_variable>
#include <X11/Xlib.h>
#include <webkit2/webkit2.h>
#include <JavaScriptCore/JavaScript.h>
#include <sstream>
#include <iomanip>

std::mutex invokeLockMutex;

struct InvokeWaitInfo
{
	ACTION callback;
	std::condition_variable completionNotifier;
	bool isCompleted;
};

struct InvokeJSWaitInfo
{
	bool isCompleted;
};

//window size or position changed
gboolean on_configure_event(GtkWidget* widget, GdkEvent* event, gpointer self);
gboolean on_webview_context_menu (WebKitWebView* web_view,
               GtkWidget* default_menu,
               WebKitHitTestResult* hit_test_result,
               gboolean triggered_with_keyboard,
               gpointer user_data);
gboolean on_permission_request(WebKitWebView* web_view, WebKitPermissionRequest* request, gpointer user_data);

Photino::Photino(PhotinoInitParams* initParams) : _webview(nullptr)
{
	// It makes xlib thread safe.
	// Needed for get_position.
	XInitThreads();
	gtk_init(0, NULL);

	if (initParams->Size != sizeof(PhotinoInitParams))
	{
		GtkWidget* dialog = gtk_message_dialog_new(
			nullptr
			, GTK_DIALOG_DESTROY_WITH_PARENT
			, GTK_MESSAGE_ERROR, GTK_BUTTONS_CLOSE
			, "Initial parameters passed are %i bytes, but expected %lu bytes."
			, initParams->Size
			, sizeof(PhotinoInitParams));
		gtk_dialog_run(GTK_DIALOG(dialog));
		gtk_widget_destroy(dialog);
		exit(0);
	}

	_windowTitle = new char[256];
	if (initParams->Title != NULL)
		strcpy(_windowTitle, initParams->Title);
	else
		_windowTitle[0] = 0;


	_startUrl = NULL;
	if (initParams->StartUrl != NULL)
	{
		_startUrl = new char[2048];
		if (_startUrl == NULL) exit(0);
		strcpy(_startUrl, initParams->StartUrl);
	}

	_startString = NULL;
	if (initParams->StartString != NULL)
	{
		_startString = new char[strlen(initParams->StartString) + 1];
		if (_startString == NULL) exit(0);
		strcpy(_startString, initParams->StartString);
	}

	_temporaryFilesPath = NULL;
	if (initParams->TemporaryFilesPath != NULL)
	{
		_temporaryFilesPath = new char[256];
		if (_temporaryFilesPath == NULL) exit(0);
		strcpy(_temporaryFilesPath, initParams->TemporaryFilesPath);
	}

	_contextMenuEnabled = initParams->ContextMenuEnabled;
	_devToolsEnabled = initParams->DevToolsEnabled;
	_grantBrowserPermissions = initParams->GrantBrowserPermissions;

	_zoom = initParams->Zoom;

	//these handlers are ALWAYS hooked up
	_webMessageReceivedCallback = (WebMessageReceivedCallback)initParams->WebMessageReceivedHandler;
	_resizedCallback = (ResizedCallback)initParams->ResizedHandler;
	_movedCallback = (MovedCallback)initParams->MovedHandler;
	_closingCallback = (ClosingCallback)initParams->ClosingHandler;
	_customSchemeCallback = (WebResourceRequestedCallback)initParams->CustomSchemeHandler;

	//copy strings from the fixed size array passed, but only if they have a value.
	for (int i = 0; i < 16; ++i)
	{
		if (initParams->CustomSchemeNames[i] != NULL)
		{
			char* name = new char[50];
			strcpy(name, initParams->CustomSchemeNames[i]);
			_customSchemeNames.push_back(name);
		}
	}

	_parent = initParams->ParentInstance;

	_window = gtk_window_new(GTK_WINDOW_TOPLEVEL);
	
	if (initParams->FullScreen)
	{
		GdkRectangle geometry = { 0 };
		gdk_monitor_get_geometry(gdk_display_get_primary_monitor(gdk_display_get_default()), &geometry);

		initParams->Left = 0;
		initParams->Top = 0;
		initParams->Width = geometry.width;
		initParams->Height = geometry.height;

		gtk_window_fullscreen(GTK_WINDOW(_window));
	}
	else
	{
		if (initParams->UseOsDefaultSize)
			gtk_window_set_default_size(GTK_WINDOW(_window), -1, -1);
		else
			gtk_window_set_default_size(GTK_WINDOW(_window), initParams->Width, initParams->Height);

		if (initParams->UseOsDefaultLocation)
			gtk_window_set_position(GTK_WINDOW(_window), GTK_WIN_POS_NONE);
		else if (initParams->CenterOnInitialize && !initParams->FullScreen)
			gtk_window_set_position(GTK_WINDOW(_window), GTK_WIN_POS_CENTER);
		else
			gtk_window_move(GTK_WINDOW(_window), initParams->Left, initParams->Top);
	}

	SetTitle(_windowTitle);

	if (initParams->Chromeless)
		gtk_window_set_decorated(GTK_WINDOW(_window), false);

	if (initParams->WindowIconFile != NULL && initParams->WindowIconFile != "")
		Photino::SetIconFile(initParams->WindowIconFile);

	if (initParams->CenterOnInitialize)
		Photino::Center();

	if (initParams->Minimized)
		Photino::Minimize();

	if (initParams->Maximized)
		Photino::Maximize();

	if (!initParams->Resizable)
		Photino::SetResizable(false);

	if (initParams->Topmost)
		Photino::SetTopmost(true);

	if (_parent == NULL)
	{
		g_signal_connect(G_OBJECT(_window), "destroy",
			G_CALLBACK(+[](GtkWidget* w, gpointer arg) { gtk_main_quit(); }),
			this);
	}
	//g_signal_connect(G_OBJECT(_window), "size-allocate",
	//	G_CALLBACK(on_size_allocate),
	//	this);
	g_signal_connect(G_OBJECT(_window), "configure-event",
		G_CALLBACK(on_configure_event),
		this);
	g_signal_connect(G_OBJECT(_webview), "permission-request",
		G_CALLBACK(on_permission_request),
		this);

	Photino::Show();

	//These must be called after the webview control is initialized.
	if (!_contextMenuEnabled)
	{
		g_signal_connect(G_OBJECT(_webview), "context-menu",
			G_CALLBACK(on_webview_context_menu),
			this);
	}

	Photino::AddCustomSchemeHandlers();
	
	if (_zoom != 100.0)
		SetZoom(_zoom);
}

Photino::~Photino()
{
	gtk_widget_destroy(_window);
}




void Photino::Center()
{
	gint windowWidth, windowHeight;
    gtk_window_get_size(GTK_WINDOW(_window), &windowWidth, &windowHeight);
	
	GdkRectangle screen = { 0 };
	gdk_monitor_get_geometry(gdk_display_get_primary_monitor(gdk_display_get_default()), &screen);

    gtk_window_move (GTK_WINDOW(_window), 
		(screen.width - windowWidth) / 2, 
		(screen.height - windowHeight) / 2);
}

void Photino::Close()
{
	gtk_window_close(GTK_WINDOW(_window));
}

void Photino::GetContextMenuEnabled(bool* enabled)
{
    //TODO
}

void Photino::GetDevToolsEnabled(bool* enabled)
{
    //TODO:
}

void Photino::GetGrantBrowserPermissions(bool* grant)
{
    //TODO:
}

void Photino::GetMaximized(bool* isMaximized)
{
	*isMaximized = gtk_window_is_maximized(GTK_WINDOW(_window));
}

void Photino::GetMinimized(bool* isMinimized)
{
	//TODO:
	//GtkStateFlags flags = gtk_widget_get_state_flags(GTK_WINDOW(_window));
	//*isMinimized = GtkStateFlags.
}

void Photino::GetPosition(int* x, int* y)
{
	gtk_window_get_position(GTK_WINDOW(_window), x, y);
}

void Photino::GetResizable(bool* resizable)
{
	*resizable = gtk_window_get_resizable(GTK_WINDOW(_window));
}

unsigned int Photino::GetScreenDpi()
{
	GdkScreen* screen = gtk_window_get_screen(GTK_WINDOW(_window));
	gdouble dpi = gdk_screen_get_resolution(screen);
	if (dpi < 0) return 96;
	else return (unsigned int)dpi;
}

void Photino::GetSize(int* width, int* height)
{
	gtk_window_get_size(GTK_WINDOW(_window), width, height);
}

AutoString Photino::GetTitle()
{
	return (AutoString)gtk_window_get_title(GTK_WINDOW(_window));
}

void Photino::GetTopmost(bool* topmost)
{
	//TODO:
	//GtkStateFlags flags = gtk_widget_get_state_flags(GTK_WINDOW(_window));
	//*topmost = GtkStateFlags.
}

void Photino::GetZoom(int* zoom)
{
	double rawValue = 0;
	rawValue = webkit_web_view_get_zoom_level(WEBKIT_WEB_VIEW(_webview));
	rawValue = (rawValue * 100.0) + 0.5;
	*zoom = (int)rawValue;
}

void Photino::Minimize()
{
	gtk_window_iconify(GTK_WINDOW(_window));
}

void Photino::Maximize()
{
	gtk_window_maximize(GTK_WINDOW(_window));
}

void Photino::NavigateToString(AutoString content)
{
	webkit_web_view_load_html(WEBKIT_WEB_VIEW(_webview), content, NULL);
}

void Photino::NavigateToUrl(AutoString url)
{
	webkit_web_view_load_uri(WEBKIT_WEB_VIEW(_webview), url);
}

void Photino::Restore()
{
	gtk_window_present(GTK_WINDOW(_window));
}

// From https://stackoverflow.com/a/33799784
std::string escape_json(const std::string& s) {
	std::ostringstream o;
	for (auto c = s.cbegin(); c != s.cend(); c++) {
		switch (*c) {
		case '"': o << "\\\""; break;
		case '\\': o << "\\\\"; break;
		case '\b': o << "\\b"; break;
		case '\f': o << "\\f"; break;
		case '\n': o << "\\n"; break;
		case '\r': o << "\\r"; break;
		case '\t': o << "\\t"; break;
		default:
			if ('\x00' <= *c && *c <= '\x1f') {
				o << "\\u"
					<< std::hex << std::setw(4) << std::setfill('0') << (int)*c;
			}
			else {
				o << *c;
			}
		}
	}
	return o.str();
}

static void webview_eval_finished(GObject* object, GAsyncResult* result, gpointer userdata) {
	InvokeJSWaitInfo* waitInfo = (InvokeJSWaitInfo*)userdata;
	waitInfo->isCompleted = true;
}

void Photino::SendWebMessage(AutoString message)
{
	std::string js;
	js.append("__dispatchMessageCallback(\"");
	js.append(escape_json(message));
	js.append("\")");

	InvokeJSWaitInfo invokeJsWaitInfo = {};
	webkit_web_view_run_javascript(WEBKIT_WEB_VIEW(_webview),
		js.c_str(), NULL, webview_eval_finished, &invokeJsWaitInfo);
	while (!invokeJsWaitInfo.isCompleted) {
		g_main_context_iteration(NULL, TRUE);
	}
}

void Photino::SetContextMenuEnabled(bool enabled)
{
    //TODO:
}

void Photino::SetDevToolsEnabled(bool enabled)
{
    //TODO:
}

void Photino::SetGrantBrowserPermissions(bool grant)
{
	//TODO:
}

void Photino::SetIconFile(AutoString filename)
{
	gtk_window_set_icon_from_file(GTK_WINDOW(_window), filename, NULL);
}

void Photino::SetPosition(int x, int y)
{
	gtk_window_move(GTK_WINDOW(_window), x, y);
}

void Photino::SetResizable(bool resizable)
{
	gtk_window_set_resizable(GTK_WINDOW(_window), resizable);
}

void Photino::SetSize(int width, int height)
{
	gtk_window_resize(GTK_WINDOW(_window), width, height);
}

void Photino::SetTitle(AutoString title)
{
	gtk_window_set_title(GTK_WINDOW(_window), title);
}

void Photino::SetTopmost(bool topmost)
{
	gtk_window_set_keep_above(GTK_WINDOW(_window), topmost ? TRUE : FALSE);
}

void Photino::SetZoom(int zoom)
{
	double newZoom = zoom / 100.0;
	webkit_web_view_set_zoom_level(WEBKIT_WEB_VIEW(_webview), newZoom);
}

void Photino::ShowMessage(AutoString title, AutoString body, unsigned int type)
{
	GtkWidget* dialog = gtk_message_dialog_new(GTK_WINDOW(_window),
		GTK_DIALOG_DESTROY_WITH_PARENT,
		GTK_MESSAGE_OTHER,
		GTK_BUTTONS_OK,
		"%s",
		body);
	gtk_window_set_title((GtkWindow*)dialog, title);
	gtk_dialog_run(GTK_DIALOG(dialog));
	gtk_widget_destroy(dialog);
}

void Photino::WaitForExit()
{
	gtk_main();
}





//Callbacks
void Photino::GetAllMonitors(GetAllMonitorsCallback callback)
{
	if (callback)
	{
		GdkScreen* screen = gtk_window_get_screen(GTK_WINDOW(_window));
		GdkDisplay* display = gdk_screen_get_display(screen);
		int n = gdk_display_get_n_monitors(display);
		for (int i = 0; i < n; i++)
		{
			GdkMonitor* monitor = gdk_display_get_monitor(display, i);
			Monitor props = {};
			gdk_monitor_get_geometry(monitor, (GdkRectangle*)&props.monitor);
			gdk_monitor_get_workarea(monitor, (GdkRectangle*)&props.work);
			if (!callback(&props)) break;
		}
	}
}

static gboolean invokeCallback(gpointer data)
{
	InvokeWaitInfo* waitInfo = (InvokeWaitInfo*)data;
	waitInfo->callback();
	{
		std::lock_guard<std::mutex> guard(invokeLockMutex);
		waitInfo->isCompleted = true;
	}
	waitInfo->completionNotifier.notify_one();
	return false;
}

void Photino::Invoke(ACTION callback)
{
	InvokeWaitInfo waitInfo = { };
	waitInfo.callback = callback;
	gdk_threads_add_idle(invokeCallback, &waitInfo);

	// Block until the callback is actually executed and completed
	// TODO: Add return values, exception handling, etc.
	std::unique_lock<std::mutex> uLock(invokeLockMutex);
	waitInfo.completionNotifier.wait(uLock, [&] { return waitInfo.isCompleted; });
}






//Private methods
void HandleWebMessage(WebKitUserContentManager* contentManager, WebKitJavascriptResult* jsResult, gpointer arg)
{
	JSCValue* jsValue = webkit_javascript_result_get_js_value(jsResult);
	if (jsc_value_is_string(jsValue)) {
		AutoString str_value = jsc_value_to_string(jsValue);

		WebMessageReceivedCallback callback = (WebMessageReceivedCallback)arg;
		callback(str_value);
		g_free(str_value);
	}

	webkit_javascript_result_unref(jsResult);
}

void Photino::Show()
{
	if (!_webview)
	{
		WebKitUserContentManager* contentManager = webkit_user_content_manager_new();
		_webview = webkit_web_view_new_with_user_content_manager(contentManager);

		//https://webkit.org/reference/webkit2gtk/unstable/WebKitSettings.html#WebKitSettings--allow-file-access-from-file-urls
		WebKitSettings* settings = webkit_web_view_get_settings(WEBKIT_WEB_VIEW(_webview));

		webkit_settings_set_allow_file_access_from_file_urls(settings, TRUE);
		webkit_settings_set_allow_modal_dialogs(settings, TRUE);
		webkit_settings_set_allow_top_navigation_to_data_urls(settings, TRUE);
		webkit_settings_set_allow_universal_access_from_file_urls(settings, TRUE);

		webkit_settings_set_enable_back_forward_navigation_gestures(settings, TRUE);
		webkit_settings_set_enable_caret_browsing(settings, TRUE);
		webkit_settings_set_enable_developer_extras(settings, _devToolsEnabled);
		webkit_settings_set_enable_media_capabilities(settings, TRUE);
		webkit_settings_set_enable_media_stream(settings, TRUE);

		webkit_settings_set_javascript_can_access_clipboard(settings, TRUE);
		webkit_settings_set_javascript_can_open_windows_automatically(settings, TRUE);

		gtk_container_add(GTK_CONTAINER(_window), _webview);

		WebKitUserScript* script = webkit_user_script_new(
			"window.__receiveMessageCallbacks = [];"
			"window.__dispatchMessageCallback = function(message) {"
			"	window.__receiveMessageCallbacks.forEach(function(callback) { callback(message); });"
			"};"
			"window.external = {"
			"	sendMessage: function(message) {"
			"		window.webkit.messageHandlers.Photinointerop.postMessage(message);"
			"	},"
			"	receiveMessage: function(callback) {"
			"		window.__receiveMessageCallbacks.push(callback);"
			"	}"
			"};", WEBKIT_USER_CONTENT_INJECT_ALL_FRAMES, WEBKIT_USER_SCRIPT_INJECT_AT_DOCUMENT_START, NULL, NULL);
		webkit_user_content_manager_add_script(contentManager, script);
		webkit_user_script_unref(script);

		g_signal_connect(contentManager, "script-message-received::Photinointerop",
			G_CALLBACK(HandleWebMessage), (void*)_webMessageReceivedCallback);
		webkit_user_content_manager_register_script_message_handler(contentManager, "Photinointerop");

		if (_startUrl != NULL)
			Photino::NavigateToUrl(_startUrl);
		else if (_startString != NULL)
			Photino::NavigateToString(_startString);
		else
		{
			GtkWidget* dialog = gtk_message_dialog_new(
				nullptr
				, GTK_DIALOG_DESTROY_WITH_PARENT
				, GTK_MESSAGE_ERROR
				, GTK_BUTTONS_CLOSE
				, "Neither StartUrl not StartString was specified");
			gtk_dialog_run(GTK_DIALOG(dialog));
			gtk_widget_destroy(dialog);
			exit(0);
		}
	}

	gtk_widget_show_all(_window);
}

gboolean on_configure_event(GtkWidget* widget, GdkEvent* event, gpointer self)
{
	if (event->type == GDK_CONFIGURE)
	{
		Photino* instance = ((Photino*)self);

		if (instance->_lastLeft != event->configure.x || instance->_lastTop != event->configure.y)
		{
			instance->InvokeMove(event->configure.x, event->configure.y);
			instance->_lastLeft = event->configure.x;
			instance->_lastTop = event->configure.y;
		}

		if (instance->_lastHeight != event->configure.height || instance->_lastWidth != event->configure.width)
		{
			instance->InvokeResize(event->configure.width, event->configure.height);
			instance->_lastWidth = event->configure.width;
			instance->_lastHeight = event->configure.height;
		}
	}
	return FALSE;
}

gboolean on_webview_context_menu (WebKitWebView* web_view, GtkWidget* default_menu,
    WebKitHitTestResult* hit_test_result, gboolean triggered_with_keyboard, gpointer user_data)
{
	return TRUE;	//disable context menu
}

gboolean on_permission_request(WebKitWebView* web_view, WebKitPermissionRequest* request, gpointer user_data)
{
	GtkWidget* dialog = gtk_message_dialog_new(
		nullptr
		, GTK_DIALOG_DESTROY_WITH_PARENT
		, GTK_MESSAGE_ERROR
		, GTK_BUTTONS_CLOSE
		, "Permission Requested - Allowing!");
	gtk_dialog_run(GTK_DIALOG(dialog));
	gtk_widget_destroy(dialog);

	webkit_permission_request_allow(request);
	return FALSE;
}

//void on_size_allocate(GtkWidget* widget, GdkRectangle* allocation, gpointer self)
//{
//	int width, height;
//	gtk_window_get_size(GTK_WINDOW(widget), &width, &height);
//	((Photino*)self)->InvokeResize(width, height);
//}

void HandleCustomSchemeRequest(WebKitURISchemeRequest* request, gpointer user_data)
{
	WebResourceRequestedCallback webResourceRequestedCallback = (WebResourceRequestedCallback)user_data;

	const gchar* uri = webkit_uri_scheme_request_get_uri(request);
	int numBytes;
	AutoString contentType;
	void* dotNetResponse = webResourceRequestedCallback((AutoString)uri, &numBytes, &contentType);
	GInputStream* stream = g_memory_input_stream_new_from_data(dotNetResponse, numBytes, NULL);
	webkit_uri_scheme_request_finish(request, (GInputStream*)stream, -1, contentType);
	g_object_unref(stream);
	delete[] contentType;
}

void Photino::AddCustomSchemeHandlers()
{
	WebKitWebContext* context = webkit_web_context_get_default();
	for (const auto& value: _customSchemeNames)
	{
		webkit_web_context_register_uri_scheme(
			context
			, value
			, (WebKitURISchemeRequestCallback)HandleCustomSchemeRequest
			, (void*)_customSchemeCallback
			, NULL);
	}
}
#endif

