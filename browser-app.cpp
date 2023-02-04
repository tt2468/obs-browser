/******************************************************************************
 Copyright (C) 2014 by John R. Bradley <jrb@turrettech.com>
 Copyright (C) 2023 by Lain Bailey <lain@obsproject.com>

 This program is free software: you can redistribute it and/or modify
 it under the terms of the GNU General Public License as published by
 the Free Software Foundation, either version 2 of the License, or
 (at your option) any later version.

 This program is distributed in the hope that it will be useful,
 but WITHOUT ANY WARRANTY; without even the implied warranty of
 MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 GNU General Public License for more details.

 You should have received a copy of the GNU General Public License
 along with this program.  If not, see <http://www.gnu.org/licenses/>.
 ******************************************************************************/

#include "browser-app.hpp"
#include "browser-version.h"
#include <nlohmann/json.hpp>

#ifndef UNUSED_PARAMETER
#define UNUSED_PARAMETER(x) \
	{                   \
		(void)x;    \
	}
#endif

CefRefPtr<CefRenderProcessHandler> BrowserApp::GetRenderProcessHandler()
{
	return this;
}

CefRefPtr<CefBrowserProcessHandler> BrowserApp::GetBrowserProcessHandler()
{
	return this;
}

void BrowserApp::OnRegisterCustomSchemes(CefRawPtr<CefSchemeRegistrar> registrar)
{
	registrar->AddCustomScheme("http", CEF_SCHEME_OPTION_STANDARD | CEF_SCHEME_OPTION_CORS_ENABLED);
}

void BrowserApp::OnBeforeChildProcessLaunch(CefRefPtr<CefCommandLine> command_line)
{
	(void)command_line;
}

void BrowserApp::OnBeforeCommandLineProcessing(const CefString &, CefRefPtr<CefCommandLine> command_line)
{
	if (!shared_texture_available) {
		bool enableGPU = command_line->HasSwitch("enable-gpu");
		CefString type = command_line->GetSwitchValue("type");

		if (!enableGPU && type.empty()) {
			command_line->AppendSwitch("disable-gpu-compositing");
		}
	}

	if (command_line->HasSwitch("disable-features")) {
		// Don't override existing, as this can break OSR
		std::string disableFeatures = command_line->GetSwitchValue("disable-features");
		disableFeatures += ",HardwareMediaKeyHandling";
		disableFeatures += ",WebBluetooth";
		command_line->AppendSwitchWithValue("disable-features", disableFeatures);
	} else {
		command_line->AppendSwitchWithValue("disable-features", "WebBluetooth,HardwareMediaKeyHandling");
	}

	command_line->AppendSwitchWithValue("autoplay-policy", "no-user-gesture-required");
}

std::vector<std::string> exposedFunctions = {
	"testFunction",};

void BrowserApp::OnContextCreated(CefRefPtr<CefBrowser> browser, CefRefPtr<CefFrame>, CefRefPtr<CefV8Context> context)
{
	CefRefPtr<CefV8Value> globalObj = context->GetGlobal();

	CefRefPtr<CefV8Value> irltkObj = CefV8Value::CreateObject(nullptr, nullptr);
	globalObj->SetValue("irltk", irltkObj, V8_PROPERTY_ATTRIBUTE_NONE);

	for (std::string name : exposedFunctions) {
		CefRefPtr<CefV8Value> func = CefV8Value::CreateFunction(name, this);
		irltkObj->SetValue(name, func, V8_PROPERTY_ATTRIBUTE_NONE);
	}

	UNUSED_PARAMETER(browser);
}

void BrowserApp::ExecuteJSFunction(CefRefPtr<CefBrowser> browser, const char *functionName, CefV8ValueList arguments)
{
	std::vector<CefString> names;
	browser->GetFrameNames(names);

	for (auto &name : names) {
		CefRefPtr<CefFrame> frame = browser->GetFrame(name);
		CefRefPtr<CefV8Context> context = frame->GetV8Context();

		context->Enter();

		CefRefPtr<CefV8Value> globalObj = context->GetGlobal();
		CefRefPtr<CefV8Value> irltkObj = globalObj->GetValue("irltk");
		CefRefPtr<CefV8Value> jsFunction = irltkObj->GetValue(functionName);

		if (jsFunction && jsFunction->IsFunction())
			jsFunction->ExecuteFunction(nullptr, arguments);

		context->Exit();
	}
}

CefRefPtr<CefV8Value> CefValueToCefV8Value(CefRefPtr<CefValue> value)
{
	CefRefPtr<CefV8Value> result;
	switch (value->GetType()) {
	case VTYPE_INVALID:
		result = CefV8Value::CreateNull();
		break;
	case VTYPE_NULL:
		result = CefV8Value::CreateNull();
		break;
	case VTYPE_BOOL:
		result = CefV8Value::CreateBool(value->GetBool());
		break;
	case VTYPE_INT:
		result = CefV8Value::CreateInt(value->GetInt());
		break;
	case VTYPE_DOUBLE:
		result = CefV8Value::CreateDouble(value->GetDouble());
		break;
	case VTYPE_STRING:
		result = CefV8Value::CreateString(value->GetString());
		break;
	case VTYPE_BINARY:
		result = CefV8Value::CreateNull();
		break;
	case VTYPE_DICTIONARY: {
		result = CefV8Value::CreateObject(nullptr, nullptr);
		CefRefPtr<CefDictionaryValue> dict = value->GetDictionary();
		CefDictionaryValue::KeyList keys;
		dict->GetKeys(keys);
		for (unsigned int i = 0; i < keys.size(); i++) {
			CefString key = keys[i];
			result->SetValue(
				key, CefValueToCefV8Value(dict->GetValue(key)),
				V8_PROPERTY_ATTRIBUTE_NONE);
		}
	} break;
	case VTYPE_LIST: {
		CefRefPtr<CefListValue> list = value->GetList();
		size_t size = list->GetSize();
		result = CefV8Value::CreateArray((int)size);
		for (int i = 0; i < size; i++) {
			result->SetValue(
				i, CefValueToCefV8Value(list->GetValue(i)));
		}
	} break;
	}
	return result;
}

bool BrowserApp::OnProcessMessageReceived(CefRefPtr<CefBrowser> browser, CefRefPtr<CefFrame> frame, CefProcessId source_process, CefRefPtr<CefProcessMessage> message)
{
	UNUSED_PARAMETER(frame);
	DCHECK(source_process == PID_BROWSER);

	CefRefPtr<CefListValue> args = message->GetArgumentList();

	if (message->GetName() == "Visibility") {
		CefV8ValueList arguments;
		arguments.push_back(CefV8Value::CreateBool(args->GetBool(0)));

		ExecuteJSFunction(browser, "onVisibilityChange", arguments);
	} else if (message->GetName() == "Active") {
		CefV8ValueList arguments;
		arguments.push_back(CefV8Value::CreateBool(args->GetBool(0)));

		ExecuteJSFunction(browser, "onActiveChange", arguments);
	} else if (message->GetName() == "DispatchJSEvent") {
		nlohmann::json payloadJson = nlohmann::json::parse(
			args->GetString(1).ToString(), nullptr, false);

		nlohmann::json wrapperJson;
		if (args->GetSize() > 1)
			wrapperJson["detail"] = payloadJson;
		std::string wrapperJsonString = wrapperJson.dump();
		std::string script;

		script += "new CustomEvent('";
		script += args->GetString(0).ToString();
		script += "', ";
		script += wrapperJsonString;
		script += ");";

		std::vector<CefString> names;
		browser->GetFrameNames(names);
		for (auto &name : names) {
			CefRefPtr<CefFrame> frame = browser->GetFrame(name);
			CefRefPtr<CefV8Context> context = frame->GetV8Context();

			context->Enter();

			CefRefPtr<CefV8Value> globalObj = context->GetGlobal();

			CefRefPtr<CefV8Value> returnValue;
			CefRefPtr<CefV8Exception> exception;

			/* Create the CustomEvent object
			* We have to use eval to invoke the new operator */
			context->Eval(script, browser->GetMainFrame()->GetURL(), 0, returnValue, exception);

			CefV8ValueList arguments;
			arguments.push_back(returnValue);

			CefRefPtr<CefV8Value> dispatchEvent = globalObj->GetValue("dispatchEvent");
			dispatchEvent->ExecuteFunction(nullptr, arguments);

			context->Exit();
		}
	} else if (message->GetName() == "executeCallback") {
		CefRefPtr<CefV8Context> context = browser->GetMainFrame()->GetV8Context();

		context->Enter();

		CefRefPtr<CefListValue> arguments = message->GetArgumentList();
		int callbackID = arguments->GetInt(0);
		CefString jsonString = arguments->GetString(1);

		CefRefPtr<CefValue> json = CefParseJSON(arguments->GetString(1).ToString(), {});

		CefRefPtr<CefV8Value> callback = callbackMap[callbackID];
		CefV8ValueList args;

		CefRefPtr<CefV8Value> retval = CefValueToCefV8Value(json);

		args.push_back(retval);

		if (callback)
			callback->ExecuteFunction(nullptr, args);

		context->Exit();

		callbackMap.erase(callbackID);
	} else {
		return false;
	}

	return true;
}

bool IsValidFunction(std::string function)
{
	std::vector<std::string>::iterator iterator;
	iterator = std::find(exposedFunctions.begin(), exposedFunctions.end(), function);
	return iterator != exposedFunctions.end();
}

bool BrowserApp::Execute(const CefString &name, CefRefPtr<CefV8Value>,
			 const CefV8ValueList &arguments,
			 CefRefPtr<CefV8Value> &, CefString &)
{
	if (IsValidFunction(name.ToString())) {
		if (arguments.size() >= 1 && arguments[0]->IsFunction()) {
			callbackId++;
			callbackMap[callbackId] = arguments[0];
		}

		CefRefPtr<CefProcessMessage> msg = CefProcessMessage::Create(name);
		CefRefPtr<CefListValue> args = msg->GetArgumentList();
		args->SetInt(0, callbackId);

		/* Pass on arguments */
		for (u_long l = 0; l < arguments.size(); l++) {
			u_long pos;
			if (arguments[0]->IsFunction())
				pos = l;
			else
				pos = l + 1;

			if (arguments[l]->IsString())
				args->SetString(pos, arguments[l]->GetStringValue());
			else if (arguments[l]->IsInt())
				args->SetInt(pos, arguments[l]->GetIntValue());
			else if (arguments[l]->IsBool())
				args->SetBool(pos, arguments[l]->GetBoolValue());
			else if (arguments[l]->IsDouble())
				args->SetDouble(pos, arguments[l]->GetDoubleValue());
		}

		CefRefPtr<CefBrowser> browser = CefV8Context::GetCurrentContext()->GetBrowser();
		SendBrowserProcessMessage(browser, PID_BROWSER, msg);

	} else {
		/* Function does not exist. */
		return false;
	}

	return true;
}
