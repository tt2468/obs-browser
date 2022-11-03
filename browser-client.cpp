/******************************************************************************
 Copyright (C) 2014 by John R. Bradley <jrb@turrettech.com>
 Copyright (C) 2018 by Hugh Bailey ("Jim") <jim@obsproject.com>

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

#include "browser-client.hpp"
#include "obs-browser-source.hpp"
#include "base64/base64.hpp"
#include "json11/json11.hpp"
#include <obs-frontend-api.h>
#include <obs.hpp>
#include <util/platform.h>

using namespace json11;

inline bool BrowserClient::valid() const
{
	return !!bs && !bs->destroying;
}

CefRefPtr<CefLoadHandler> BrowserClient::GetLoadHandler()
{
	return this;
}

CefRefPtr<CefRenderHandler> BrowserClient::GetRenderHandler()
{
	return this;
}

CefRefPtr<CefDisplayHandler> BrowserClient::GetDisplayHandler()
{
	return this;
}

CefRefPtr<CefLifeSpanHandler> BrowserClient::GetLifeSpanHandler()
{
	return this;
}

CefRefPtr<CefContextMenuHandler> BrowserClient::GetContextMenuHandler()
{
	return this;
}

CefRefPtr<CefAudioHandler> BrowserClient::GetAudioHandler()
{
	return this;
}

CefRefPtr<CefRequestHandler> BrowserClient::GetRequestHandler()
{
	return this;
}

CefRefPtr<CefResourceRequestHandler> BrowserClient::GetResourceRequestHandler(
	CefRefPtr<CefBrowser>, CefRefPtr<CefFrame>,
	CefRefPtr<CefRequest> request, bool, bool, const CefString &, bool &)
{
	if (request->GetHeaderByName("origin") == "null") {
		return this;
	}

	return nullptr;
}

CefResourceRequestHandler::ReturnValue
BrowserClient::OnBeforeResourceLoad(CefRefPtr<CefBrowser>, CefRefPtr<CefFrame>,
				    CefRefPtr<CefRequest>,
				    CefRefPtr<CefCallback>)
{
	return RV_CONTINUE;
}

bool BrowserClient::OnBeforePopup(CefRefPtr<CefBrowser>, CefRefPtr<CefFrame>,
				  const CefString &, const CefString &,
				  cef_window_open_disposition_t, bool,
				  const CefPopupFeatures &, CefWindowInfo &,
				  CefRefPtr<CefClient> &, CefBrowserSettings &,
				  CefRefPtr<CefDictionaryValue> &, bool *)
{
	/* block popups */
	return true;
}

void BrowserClient::OnBeforeContextMenu(CefRefPtr<CefBrowser>,
					CefRefPtr<CefFrame>,
					CefRefPtr<CefContextMenuParams>,
					CefRefPtr<CefMenuModel> model)
{
	/* remove all context menu contributions */
	model->Clear();
}

bool BrowserClient::OnProcessMessageReceived(CefRefPtr<CefBrowser> browser, CefRefPtr<CefFrame>, CefProcessId, CefRefPtr<CefProcessMessage> message)
{
	const std::string &name = message->GetName();
	CefRefPtr<CefListValue> input_args = message->GetArgumentList();
	Json json;

	if (!valid()) {
		return false;
	}

	json = Json::object{ {"test", "object"}, {"r", name} };

	CefRefPtr<CefProcessMessage> msg = CefProcessMessage::Create("executeCallback");

	CefRefPtr<CefListValue> execute_args = msg->GetArgumentList();
	execute_args->SetInt(0, input_args->GetInt(0));
	execute_args->SetString(1, json.dump());

	SendBrowserProcessMessage(browser, PID_RENDERER, msg);

	return true;
}

void BrowserClient::GetViewRect(CefRefPtr<CefBrowser>, CefRect &rect)
{
	if (!valid()) {
		rect.Set(0, 0, 16, 16);
		return;
	}

	rect.Set(0, 0, bs->width < 1 ? 1 : bs->width, bs->height < 1 ? 1 : bs->height);
}

bool BrowserClient::OnTooltip(CefRefPtr<CefBrowser>, CefString &text)
{
	UNUSED_PARAMETER(text);
	return false;
}

void BrowserClient::OnPaint(CefRefPtr<CefBrowser>, PaintElementType type, const RectList &, const void *buffer, int width, int height)
{
	if (type != PET_VIEW) {
		// TODO Overlay texture on top of bs->texture
		return;
	}

	if (!valid()) {
		return;
	}

	if (bs->width != width || bs->height != height) {
		obs_enter_graphics();
		bs->DestroyTextures();
		obs_leave_graphics();
	}

	if (!bs->texture && width && height) {
		obs_enter_graphics();
		bs->texture = gs_texture_create(width, height, GS_BGRA, 1, (const uint8_t **)&buffer, GS_DYNAMIC);
		bs->width = width;
		bs->height = height;
		obs_leave_graphics();
	} else {
		obs_enter_graphics();
		gs_texture_set_image(bs->texture, (const uint8_t *)buffer, width * 4, false);
		obs_leave_graphics();
	}
}

static speaker_layout GetSpeakerLayout(CefAudioHandler::ChannelLayout cefLayout)
{
	switch (cefLayout) {
	case CEF_CHANNEL_LAYOUT_MONO:
		return SPEAKERS_MONO; /**< Channels: MONO */
	case CEF_CHANNEL_LAYOUT_STEREO:
		return SPEAKERS_STEREO; /**< Channels: FL, FR */
	case CEF_CHANNEL_LAYOUT_2POINT1:
		return SPEAKERS_2POINT1; /**< Channels: FL, FR, LFE */
	case CEF_CHANNEL_LAYOUT_2_2:
	case CEF_CHANNEL_LAYOUT_QUAD:
	case CEF_CHANNEL_LAYOUT_4_0:
		return SPEAKERS_4POINT0; /**< Channels: FL, FR, FC, RC */
	case CEF_CHANNEL_LAYOUT_4_1:
		return SPEAKERS_4POINT1; /**< Channels: FL, FR, FC, LFE, RC */
	case CEF_CHANNEL_LAYOUT_5_1:
	case CEF_CHANNEL_LAYOUT_5_1_BACK:
		return SPEAKERS_5POINT1; /**< Channels: FL, FR, FC, LFE, RL, RR */
	case CEF_CHANNEL_LAYOUT_7_1:
	case CEF_CHANNEL_LAYOUT_7_1_WIDE_BACK:
	case CEF_CHANNEL_LAYOUT_7_1_WIDE:
		return SPEAKERS_7POINT1; /**< Channels: FL, FR, FC, LFE, RL, RR, SL, SR */
	default:
		return SPEAKERS_UNKNOWN;
	}
}

void BrowserClient::OnAudioStreamStarted(CefRefPtr<CefBrowser> browser, const CefAudioParameters &params_, int channels_)
{
	UNUSED_PARAMETER(browser);
	channels = channels_;
	channel_layout = (ChannelLayout)params_.channel_layout;
	sample_rate = params_.sample_rate;
	frames_per_buffer = params_.frames_per_buffer;
}

void BrowserClient::OnAudioStreamPacket(CefRefPtr<CefBrowser> browser, const float **data, int frames, int64_t pts)
{
	UNUSED_PARAMETER(browser);
	if (!valid()) {
		return;
	}
	struct obs_source_audio audio = {};
	const uint8_t **pcm = (const uint8_t **)data;
	speaker_layout speakers = GetSpeakerLayout(channel_layout);
	int speaker_count = get_audio_channels(speakers);
	for (int i = 0; i < speaker_count; i++)
		audio.data[i] = pcm[i];
	audio.samples_per_sec = sample_rate;
	audio.frames = frames;
	audio.format = AUDIO_FORMAT_FLOAT_PLANAR;
	audio.speakers = speakers;
	audio.timestamp = (uint64_t)pts * 1000000LLU;
	obs_source_output_audio(bs->source, &audio);
}

void BrowserClient::OnAudioStreamStopped(CefRefPtr<CefBrowser> browser)
{
	UNUSED_PARAMETER(browser);
}

void BrowserClient::OnAudioStreamError(CefRefPtr<CefBrowser> browser, const CefString &message)
{
	UNUSED_PARAMETER(browser);
	UNUSED_PARAMETER(message);
}

static CefAudioHandler::ChannelLayout Convert2CEFSpeakerLayout(int channels)
{
	switch (channels) {
	case 1:
		return CEF_CHANNEL_LAYOUT_MONO;
	case 2:
		return CEF_CHANNEL_LAYOUT_STEREO;
	case 3:
		return CEF_CHANNEL_LAYOUT_2_1;
	case 4:
		return CEF_CHANNEL_LAYOUT_4_0;
	case 5:
		return CEF_CHANNEL_LAYOUT_4_1;
	case 6:
		return CEF_CHANNEL_LAYOUT_5_1;
	case 8:
		return CEF_CHANNEL_LAYOUT_7_1;
	default:
		return CEF_CHANNEL_LAYOUT_UNSUPPORTED;
	}
}

bool BrowserClient::GetAudioParameters(CefRefPtr<CefBrowser> browser,
				       CefAudioParameters &params)
{
	UNUSED_PARAMETER(browser);
	int channels = (int)audio_output_get_channels(obs_get_audio());
	params.channel_layout = Convert2CEFSpeakerLayout(channels);
	params.sample_rate = (int)audio_output_get_sample_rate(obs_get_audio());
	params.frames_per_buffer = kFramesPerBuffer;
	return true;
}

void BrowserClient::OnLoadEnd(CefRefPtr<CefBrowser>, CefRefPtr<CefFrame> frame,
			      int)
{
	if (!valid()) {
		return;
	}

	if (frame->IsMain() && bs->css.length()) {
		std::string uriEncodedCSS =
			CefURIEncode(bs->css, false).ToString();

		std::string script;
		script += "const obsCSS = document.createElement('style');";
		script += "obsCSS.innerHTML = decodeURIComponent(\"" + uriEncodedCSS + "\");";
		script += "document.querySelector('head').appendChild(obsCSS);";

		frame->ExecuteJavaScript(script, "", 0);
	}
}

bool BrowserClient::OnConsoleMessage(CefRefPtr<CefBrowser>, cef_log_severity_t level, const CefString &message, const CefString &source, int line)
{
	int errorLevel = LOG_INFO;
	const char *code = "Info";
	switch (level) {
	case LOGSEVERITY_ERROR:
		errorLevel = LOG_WARNING;
		code = "Error";
		break;
	case LOGSEVERITY_FATAL:
		errorLevel = LOG_ERROR;
		code = "Fatal";
		break;
	default:
		return false;
	}

	const char *sourceName = "<unknown>";

	if (bs && bs->source)
		sourceName = obs_source_get_name(bs->source);

	blog(errorLevel, "[obs-browser: '%s'] %s: %s (%s:%d)", sourceName, code,
	     message.ToString().c_str(), source.ToString().c_str(), line);
	return false;
}
