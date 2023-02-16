/*
 * Demo gstreamer app for negotiating and streaming a sendrecv webrtc stream
 * with a browser JS app.
 *
 * gcc webrtc-sendrecv.c $(pkg-config --cflags --libs gstreamer-webrtc-1.0 gstreamer-sdp-1.0 libsoup-2.4 json-glib-1.0) -o webrtc-sendrecv
 *
 * Author: Nirbheek Chauhan <nirbheek@centricular.com>
 */

#include "http.h"

#include <gst/gst.h>
#include <gst/sdp/sdp.h>
#include <gst/rtp/rtp.h>

#define GST_USE_UNSTABLE_API
#include <gst/webrtc/webrtc.h>

 /* For signalling */
#include <json-glib/json-glib.h>

#include <string.h>

#include <iostream>
#include <string>
#include <sstream>
#include <vector>
#include <numeric>
#include <algorithm>
#include <thread>
#include <atomic>
#include <future>

enum AppState
{
    APP_STATE_UNKNOWN = 0,
    APP_STATE_ERROR = 1,          /* generic error */
    PEER_CONNECTING = 3000,
    PEER_CONNECTION_ERROR,
    PEER_CONNECTED,
    PEER_CALL_NEGOTIATING = 4000,
    PEER_CALL_STARTED,
    PEER_CALL_STOPPING,
    PEER_CALL_STOPPED,
    PEER_CALL_ERROR,
};

#define GST_CAT_DEFAULT webrtc_sendrecv_debug
GST_DEBUG_CATEGORY_STATIC(GST_CAT_DEFAULT);

static GMainLoop *loop;
static GstElement *pipe1, *webrtc1 = NULL;
static GObject *receive_channel;

static enum AppState app_state = APP_STATE_UNKNOWN;
static const gboolean disable_ssl = FALSE;
static const gboolean remote_is_offerer = FALSE;

const char send_offer_url[] = "https://ntfy.sh/mediaReceiverSendOffer";
const char get_answer_url[] = "https://ntfy.sh/mediaReceiverGetAnswer/sse";

// https://webrtchacks.com/limit-webrtc-bandwidth-sdp/
static std::string setMediaBitrate(const std::string& sdp, const std::string& media, int bitrate)
{
    std::istringstream ss(sdp);

    std::vector<std::string> lines;

    std::string buffer;
    while (std::getline(ss, buffer))
        lines.push_back(buffer);

    auto it = std::find_if(lines.begin(), lines.end(), [&media](const std::string& v) { return v.find("m=" + media) == 0; });

    if (it == lines.end())
        return sdp;

    ++it;

    it = std::find_if(it, lines.end(), [](const std::string& v) { return v.find("i=") != 0 && v.find("c=") != 0; });

    const auto b_line = "b=AS:" + std::to_string(bitrate);
    if (it != lines.end() && it->find("b") == 0)
    {
        *it = b_line;
    }
    else
    {
        lines.insert(it, b_line);
    }

    //return std::accumulate(std::next(lines.begin()), lines.end(), lines[0],
    //    [](std::string a, const std::string& b) { return std::move(a) + '\n' + b; });

    std::string result;
    for (auto& v : lines)
    {
        if (!v.empty())
        {
            result += v;
            result += '\n';
        }
    }

    return result;
}


static gboolean
cleanup_and_quit_loop(const gchar * msg, enum AppState state)
{
    if (msg)
        gst_printerr("%s\n", msg);
    if (state > 0)
        app_state = state;

    if (loop) {
        g_main_loop_quit(loop);
        g_clear_pointer(&loop, g_main_loop_unref);
    }

    /* To allow usage as a GSourceFunc */
    return G_SOURCE_REMOVE;
}

static gchar *
get_string_from_json_object(JsonObject * object)
{
    JsonNode *root;
    JsonGenerator *generator;
    gchar *text;

    /* Make it the root node */
    root = json_node_init_object(json_node_alloc(), object);
    generator = json_generator_new();
    json_generator_set_root(generator, root);
    text = json_generator_to_data(generator, NULL);

    /* Release everything */
    g_object_unref(generator);
    json_node_free(root);
    return text;
}

static void
handle_media_stream(GstPad * pad, GstElement * pipe, const char *convert_name,
    const char *sink_name)
{
    GstPad *qpad;
    GstElement *q, *conv, *resample, *sink;
    GstPadLinkReturn ret;

    gst_println("Trying to handle stream with %s ! %s", convert_name, sink_name);

    q = gst_element_factory_make("queue", NULL);
    g_assert_nonnull(q);
    conv = gst_element_factory_make(convert_name, NULL);
    g_assert_nonnull(conv);
    sink = gst_element_factory_make(sink_name, NULL);
    g_assert_nonnull(sink);

    if (g_strcmp0(convert_name, "audioconvert") == 0) {
        /* Might also need to resample, so add it just in case.
         * Will be a no-op if it's not required. */
        resample = gst_element_factory_make("audioresample", NULL);
        g_assert_nonnull(resample);
        gst_bin_add_many(GST_BIN(pipe), q, conv, resample, sink, NULL);
        gst_element_sync_state_with_parent(q);
        gst_element_sync_state_with_parent(conv);
        gst_element_sync_state_with_parent(resample);
        gst_element_sync_state_with_parent(sink);
        gst_element_link_many(q, conv, resample, sink, NULL);
    }
    else {
        gst_bin_add_many(GST_BIN(pipe), q, conv, sink, NULL);
        gst_element_sync_state_with_parent(q);
        gst_element_sync_state_with_parent(conv);
        gst_element_sync_state_with_parent(sink);
        gst_element_link_many(q, conv, sink, NULL);
    }

    qpad = gst_element_get_static_pad(q, "sink");

    ret = gst_pad_link(pad, qpad);
    g_assert_cmphex(ret, == , GST_PAD_LINK_OK);
}

static void
on_incoming_decodebin_stream(GstElement * decodebin, GstPad * pad,
    GstElement * pipe)
{
    GstCaps *caps;
    const gchar *name;

    if (!gst_pad_has_current_caps(pad)) {
        gst_printerr("Pad '%s' has no caps, can't do anything, ignoring\n",
            GST_PAD_NAME(pad));
        return;
    }

    caps = gst_pad_get_current_caps(pad);
    name = gst_structure_get_name(gst_caps_get_structure(caps, 0));

    if (g_str_has_prefix(name, "video")) {
        handle_media_stream(pad, pipe, "videoconvert", "autovideosink");
    }
    else if (g_str_has_prefix(name, "audio")) {
        handle_media_stream(pad, pipe, "audioconvert", "autoaudiosink");
    }
    else {
        gst_printerr("Unknown pad %s, ignoring", GST_PAD_NAME(pad));
    }
}

static void
on_incoming_stream(GstElement * webrtc, GstPad * pad, GstElement * pipe)
{
    GstElement *decodebin;
    GstPad *sinkpad;

    if (GST_PAD_DIRECTION(pad) != GST_PAD_SRC)
        return;

    decodebin = gst_element_factory_make("decodebin", NULL);
    g_signal_connect(decodebin, "pad-added",
        G_CALLBACK(on_incoming_decodebin_stream), pipe);
    gst_bin_add(GST_BIN(pipe), decodebin);
    gst_element_sync_state_with_parent(decodebin);

    sinkpad = gst_element_get_static_pad(decodebin, "sink");
    gst_pad_link(pad, sinkpad);
    gst_object_unref(sinkpad);
}


static const char* verify_sse_response(CURL* curl) {
#define EXPECTED_CONTENT_TYPE "text/event-stream"

    static const char expected_content_type[] = EXPECTED_CONTENT_TYPE;

    const char* content_type;
    curl_easy_getinfo(curl, CURLINFO_CONTENT_TYPE, &content_type);
    if (!content_type) content_type = "";

    if (!strncmp(content_type, expected_content_type, strlen(expected_content_type)))
        return 0;

    return "Invalid content_type, should be '" EXPECTED_CONTENT_TYPE "'.";
}



static auto getRemoteEcho()
{
    std::promise<bool> startedPromise;
    std::promise<std::string> responsePromise;

    auto startedResult = startedPromise.get_future();
    auto responseResult = responsePromise.get_future();

    auto threadLam = [](
        std::promise<bool> startedPromise,
        std::promise<std::string> responsePromise
        ) {
            const char* headers[] = {
                "Accept: text/event-stream",
                NULL
            };

            bool requestInterrupted = false;

            auto on_data = [&startedPromise, &responsePromise, &requestInterrupted](char *ptr, size_t size, size_t nmemb)->size_t {
                try {
                    const auto ptrEnd = ptr + size * nmemb;

                    const char watch[] = "data:";

                    auto pData = std::search(ptr, ptrEnd, std::begin(watch), std::prev(std::end(watch)));
                    if (pData != ptrEnd) do {
                        pData += sizeof(watch) / sizeof(watch[0]) - 1;

                        JsonParser *parser = json_parser_new();
                        if (!json_parser_load_from_data(parser, pData, ptrEnd - pData, NULL)) {
                            //gst_printerr("Unknown message '%s', ignoring\n", text);
                            g_object_unref(parser);
                            break; //goto out;
                        }

                        auto root = json_parser_get_root(parser);
                        if (!JSON_NODE_HOLDS_OBJECT(root)) {
                            //gst_printerr("Unknown json message '%s', ignoring\n", text);
                            g_object_unref(parser);
                            break; //goto out;
                        }

                        auto child = json_node_get_object(root);

                        if (!json_object_has_member(child, "event")) {
                            g_object_unref(parser);
                            break; //goto out;
                        }

                        auto sdptype = json_object_get_string_member(child, "event");
                        if (g_str_equal(sdptype, "open")) {
                            startedPromise.set_value(true);
                        }
                        else if (g_str_equal(sdptype, "message")) {
                            auto text = json_object_get_string_member(child, "message");
                            responsePromise.set_value(text);
                            requestInterrupted = true;
                        }

                        g_object_unref(parser);

                    } while (false);
                }
                catch (const std::exception&) {
                    startedPromise.set_value(false);
                    requestInterrupted = true;
                }
                return size * nmemb;
            };

            auto progress_callback = [&requestInterrupted](curl_off_t dltotal,
                curl_off_t dlnow,
                curl_off_t ultotal,
                curl_off_t ulnow)->size_t {
                    return requestInterrupted;
            };


            http(HTTP_GET, get_answer_url, headers, 0, 0, on_data, verify_sse_response, progress_callback);
    };

    // https://stackoverflow.com/a/23454840/10472202
    std::thread(threadLam, std::move(startedPromise), std::move(responsePromise)).detach();

    return std::make_tuple(std::move(startedResult), std::move(responseResult));
}


static void
send_sdp_to_peer(GstWebRTCSessionDescription * desc)
{
    if (app_state < PEER_CALL_NEGOTIATING) {
        cleanup_and_quit_loop("Can't send SDP to peer, not in call",
            APP_STATE_ERROR);
        return;
    }

    auto text = gst_sdp_message_as_text(desc->sdp);
    auto correctedText = setMediaBitrate(text, "video", 500);
    g_free(text);

    auto sdp = json_object_new();

    if (desc->type == GST_WEBRTC_SDP_TYPE_OFFER) {
        gst_print("Sending offer:\n%s\n", correctedText.c_str());
        json_object_set_string_member(sdp, "type", "offer");
    }
    else if (desc->type == GST_WEBRTC_SDP_TYPE_ANSWER) {
        gst_print("Sending answer:\n%s\n", correctedText.c_str());
        json_object_set_string_member(sdp, "type", "answer");
    }
    else {
        g_assert_not_reached();
    }

    json_object_set_string_member(sdp, "sdp", correctedText.c_str());

    text = get_string_from_json_object(sdp);

#if 0
    std::cout << text << std::endl;

    g_free(text);

    // interacting with operator

    std::string s;
    std::getline(std::cin, s);
#endif


    auto[startedResult, responseResult] = getRemoteEcho();

    if (!startedResult.get()) {
        std::cerr << "Failed to SSE connect to the server.\n";
        return;
    }

    http(HTTP_POST, send_offer_url, nullptr, text, strlen(text));

    g_free(text);

    std::string s = responseResult.get();


    JsonParser *parser = json_parser_new();
    if (!json_parser_load_from_data(parser, s.c_str(), -1, NULL)) {
        gst_printerr("Unknown message '%s', ignoring\n", text);
        g_object_unref(parser);
        return; //goto out;
    }
    
    auto root = json_parser_get_root(parser);
    if (!JSON_NODE_HOLDS_OBJECT(root)) {
        gst_printerr("Unknown json message '%s', ignoring\n", text);
        g_object_unref(parser);
        return; //goto out;
    }

    auto child = json_node_get_object(root);

    if (!json_object_has_member(child, "type")) {
        cleanup_and_quit_loop("ERROR: received SDP without 'type'",
            PEER_CALL_ERROR);
        g_object_unref(parser);
        return; //goto out;
    }

    auto sdptype = json_object_get_string_member(child, "type");
    /* In this example, we create the offer and receive one answer by default,
        * but it's possible to comment out the offer creation and wait for an offer
        * instead, so we handle either here.
        *
        * See tests/examples/webrtcbidirectional.c in gst-plugins-bad for another
        * example how to handle offers from peers and reply with answers using webrtcbin. */
    {
        auto text = json_object_get_string_member(child, "sdp");
        GstSDPMessage *sdp;
        auto ret = gst_sdp_message_new(&sdp);
        g_assert_cmphex(ret, == , GST_SDP_OK);
        ret = gst_sdp_message_parse_buffer((guint8 *)text, strlen(text), sdp);
        g_assert_cmphex(ret, == , GST_SDP_OK);

        if (g_str_equal(sdptype, "answer")) {
            gst_print("Received answer:\n%s\n", text);
            auto answer = gst_webrtc_session_description_new(GST_WEBRTC_SDP_TYPE_ANSWER,
                sdp);
            g_assert_nonnull(answer);

            /* Set remote description on our pipeline */
            {
                GstPromise *promise = gst_promise_new();
                g_signal_emit_by_name(webrtc1, "set-remote-description", answer,
                    promise);
                gst_promise_interrupt(promise);
                gst_promise_unref(promise);
            }
            app_state = PEER_CALL_STARTED;
        }
    }

    g_object_unref(parser);
}

/* Offer created by our pipeline, to be sent to the peer */
static void
on_offer_created(GstPromise * promise, gpointer user_data)
{
    GstWebRTCSessionDescription *offer = NULL;
    const GstStructure *reply;

    g_assert_cmphex(app_state, == , PEER_CALL_NEGOTIATING);

    g_assert_cmphex(gst_promise_wait(promise), == , GST_PROMISE_RESULT_REPLIED);
    reply = gst_promise_get_reply(promise);
    gst_structure_get(reply, "offer",
        GST_TYPE_WEBRTC_SESSION_DESCRIPTION, &offer, NULL);
    gst_promise_unref(promise);

    promise = gst_promise_new();
    g_signal_emit_by_name(webrtc1, "set-local-description", offer, promise);
    gst_promise_interrupt(promise);
    gst_promise_unref(promise);

    /* Send offer to peer */
    send_sdp_to_peer(offer);
    gst_webrtc_session_description_free(offer);
}

static void
on_negotiation_needed(GstElement * element, gpointer user_data)
{
    gboolean create_offer = GPOINTER_TO_INT(user_data);
    app_state = PEER_CALL_NEGOTIATING;

    //if (remote_is_offerer) {
    //    soup_websocket_connection_send_text(ws_conn, "OFFER_REQUEST");
    //}
    //else 
        if (create_offer) {
        GstPromise *promise =
            gst_promise_new_with_change_func(on_offer_created, NULL, NULL);
        g_signal_emit_by_name(webrtc1, "create-offer", NULL, promise);
    }
}

#define STUN_SERVER " stun-server=stun://stun.l.google.com:19302 "
#define RTP_CAPS_OPUS "application/x-rtp,media=audio,encoding-name=OPUS,payload="
#define RTP_CAPS_VP8 "application/x-rtp,media=video,encoding-name=VP8,payload="

static void
data_channel_on_error(GObject * dc, gpointer user_data)
{
    cleanup_and_quit_loop("Data channel error", APP_STATE_UNKNOWN);
}

static void
data_channel_on_open(GObject * dc, gpointer user_data)
{
    GBytes *bytes = g_bytes_new("data", strlen("data"));
    gst_print("data channel opened\n");
    g_signal_emit_by_name(dc, "send-string", "Hi! from GStreamer");
    g_signal_emit_by_name(dc, "send-data", bytes);
    g_bytes_unref(bytes);
}

static void
data_channel_on_close(GObject * dc, gpointer user_data)
{
    cleanup_and_quit_loop("Data channel closed", APP_STATE_UNKNOWN);
}

static void
data_channel_on_message_string(GObject * dc, gchar * str, gpointer user_data)
{
    gst_print("Received data channel message: %s\n", str);
}

static void
connect_data_channel_signals(GObject * data_channel)
{
    g_signal_connect(data_channel, "on-error",
        G_CALLBACK(data_channel_on_error), NULL);
    g_signal_connect(data_channel, "on-open", G_CALLBACK(data_channel_on_open),
        NULL);
    g_signal_connect(data_channel, "on-close",
        G_CALLBACK(data_channel_on_close), NULL);
    g_signal_connect(data_channel, "on-message-string",
        G_CALLBACK(data_channel_on_message_string), NULL);
}

static void
on_data_channel(GstElement * webrtc, GObject * data_channel,
    gpointer user_data)
{
    connect_data_channel_signals(data_channel);
    receive_channel = data_channel;
}

static void
on_ice_gathering_state_notify(GstElement * webrtcbin, GParamSpec * pspec,
    gpointer user_data)
{
    GstWebRTCICEGatheringState ice_gather_state;
    const gchar *new_state = "unknown";

    g_object_get(webrtcbin, "ice-gathering-state", &ice_gather_state, NULL);
    switch (ice_gather_state) {
    case GST_WEBRTC_ICE_GATHERING_STATE_NEW:
        new_state = "new";
        break;
    case GST_WEBRTC_ICE_GATHERING_STATE_GATHERING:
        new_state = "gathering";
        break;
    case GST_WEBRTC_ICE_GATHERING_STATE_COMPLETE:
        new_state = "complete";
        break;
    }
    gst_print("ICE gathering state changed to %s\n", new_state);
}

static gboolean webrtcbin_get_stats(GstElement * webrtcbin);

static gboolean
on_webrtcbin_stat(GQuark field_id, const GValue * value, gpointer unused)
{
    if (GST_VALUE_HOLDS_STRUCTURE(value)) {
        GST_DEBUG("stat: \'%s\': %" GST_PTR_FORMAT, g_quark_to_string(field_id),
            gst_value_get_structure(value));
    }
    else {
        GST_FIXME("unknown field \'%s\' value type: \'%s\'",
            g_quark_to_string(field_id), g_type_name(G_VALUE_TYPE(value)));
    }

    return TRUE;
}

static void
on_webrtcbin_get_stats(GstPromise * promise, GstElement * webrtcbin)
{
    const GstStructure *stats;

    g_return_if_fail(gst_promise_wait(promise) == GST_PROMISE_RESULT_REPLIED);

    stats = gst_promise_get_reply(promise);
    gst_structure_foreach(stats, on_webrtcbin_stat, NULL);

    g_timeout_add(100, (GSourceFunc)webrtcbin_get_stats, webrtcbin);
}

static gboolean
webrtcbin_get_stats(GstElement * webrtcbin)
{
    GstPromise *promise;

    promise =
        gst_promise_new_with_change_func(
        (GstPromiseChangeFunc)on_webrtcbin_get_stats, webrtcbin, NULL);

    GST_TRACE("emitting get-stats on %" GST_PTR_FORMAT, webrtcbin);
    g_signal_emit_by_name(webrtcbin, "get-stats", NULL, promise);
    gst_promise_unref(promise);

    return G_SOURCE_REMOVE;
}

#define RTP_TWCC_URI "http://www.ietf.org/id/draft-holmer-rmcat-transport-wide-cc-extensions-01"

static gboolean
start_pipeline(gboolean create_offer)
{
    GstStateChangeReturn ret;
    GError *error = NULL;

    webrtc1 = gst_element_factory_make("webrtcbin", "recvonly");
    g_object_set(webrtc1, "bundle-policy", GST_WEBRTC_BUNDLE_POLICY_MAX_BUNDLE, nullptr);


    pipe1 = gst_pipeline_new(nullptr);

    gst_bin_add_many(GST_BIN(pipe1),
        webrtc1,
        nullptr);

    g_object_set(webrtc1, "stun-server", "stun:stun.l.google.com:19302", nullptr);

    {
        GstWebRTCRTPTransceiver *trans;
        //auto video_caps = gst_caps_from_string("application/x-rtp,media=video,encoding-name=vp8,clock-rate=90000,ssrc=1,payload=98,fec-type=ulp-red,do-nack=true");
        auto video_caps = gst_caps_from_string(RTP_CAPS_VP8 "96");
        g_signal_emit_by_name(webrtc1, "add-transceiver", GST_WEBRTC_RTP_TRANSCEIVER_DIRECTION_RECVONLY, video_caps, &trans);
        gst_caps_unref(video_caps);
        gst_object_unref(trans);
    }

    /* This is the gstwebrtc entry point where we create the offer and so on. It
     * will be called when the pipeline goes to PLAYING. */
    g_signal_connect(webrtc1, "on-negotiation-needed",
        G_CALLBACK(on_negotiation_needed), GINT_TO_POINTER(create_offer));
    /* We need to transmit this ICE candidate to the browser via the websockets
     * signalling server. Incoming ice candidates from the browser need to be
     * added by us too, see on_server_message() */

    g_signal_connect(webrtc1, "notify::ice-gathering-state",
        G_CALLBACK(on_ice_gathering_state_notify), NULL);

    gst_element_set_state(pipe1, GST_STATE_READY);

    g_signal_connect(webrtc1, "on-data-channel", G_CALLBACK(on_data_channel),
        NULL);
    /* Incoming streams will be exposed via this signal */
    g_signal_connect(webrtc1, "pad-added", G_CALLBACK(on_incoming_stream),
        pipe1);
    /* Lifetime is the same as the pipeline itself */
    gst_object_unref(webrtc1);

    g_timeout_add(100, (GSourceFunc)webrtcbin_get_stats, webrtc1);

    gst_print("Starting pipeline\n");
    ret = gst_element_set_state(GST_ELEMENT(pipe1), GST_STATE_PLAYING);
    if (ret == GST_STATE_CHANGE_FAILURE)
        goto err;

    return TRUE;

err:
    if (pipe1)
        g_clear_object(&pipe1);
    if (webrtc1)
        webrtc1 = NULL;
    return FALSE;
}

/* Answer created by our pipeline, to be sent to the peer */
static void
on_answer_created(GstPromise * promise, gpointer user_data)
{
    GstWebRTCSessionDescription *answer = NULL;
    const GstStructure *reply;

    g_assert_cmphex(app_state, == , PEER_CALL_NEGOTIATING);

    g_assert_cmphex(gst_promise_wait(promise), == , GST_PROMISE_RESULT_REPLIED);
    reply = gst_promise_get_reply(promise);
    gst_structure_get(reply, "answer",
        GST_TYPE_WEBRTC_SESSION_DESCRIPTION, &answer, NULL);
    gst_promise_unref(promise);

    promise = gst_promise_new();
    g_signal_emit_by_name(webrtc1, "set-local-description", answer, promise);
    gst_promise_interrupt(promise);
    gst_promise_unref(promise);

    /* Send answer to peer */
    send_sdp_to_peer(answer);
    gst_webrtc_session_description_free(answer);
}

static void
on_offer_set(GstPromise * promise, gpointer user_data)
{
    gst_promise_unref(promise);
    promise = gst_promise_new_with_change_func(on_answer_created, NULL, NULL);
    g_signal_emit_by_name(webrtc1, "create-answer", NULL, promise);
}

static void
on_offer_received(GstSDPMessage * sdp)
{
    GstWebRTCSessionDescription *offer = NULL;
    GstPromise *promise;

    offer = gst_webrtc_session_description_new(GST_WEBRTC_SDP_TYPE_OFFER, sdp);
    g_assert_nonnull(offer);

    /* Set remote description on our pipeline */
    {
        promise = gst_promise_new_with_change_func(on_offer_set, NULL, NULL);
        g_signal_emit_by_name(webrtc1, "set-remote-description", offer, promise);
    }
    gst_webrtc_session_description_free(offer);
}

static gboolean
check_plugins(void)
{
    //int i;
    gboolean ret;
    GstPlugin *plugin;
    GstRegistry *registry;
    const gchar *needed[] = { 
        "opus", "vpx", "nice", "webrtc", "dtls", "srtp",
        "rtpmanager", "videotestsrc", "audiotestsrc", NULL
    };

    registry = gst_registry_get();
    ret = TRUE;
    for (unsigned int i = 0; i < g_strv_length((gchar **)needed); i++) {
        plugin = gst_registry_find_plugin(registry, needed[i]);
        if (!plugin) {
            gst_print("Required gstreamer plugin '%s' not found\n", needed[i]);
            ret = FALSE;
            continue;
        }
        gst_object_unref(plugin);
    }
    return ret;
}

int
main(int argc, char *argv[])
{
    GError *error = NULL;
    int ret_code = -1;

    gst_init(&argc, &argv);

    GST_DEBUG_CATEGORY_INIT(GST_CAT_DEFAULT, "webrtc-sendrecv", 0,
        "WebRTC Sending and Receiving example");

    if (!check_plugins()) {
        goto out;
    }

    ret_code = 0;

    app_state = PEER_CONNECTED;
    /* Start negotiation (exchange SDP and ICE candidates) */
    if (!start_pipeline(TRUE))
        cleanup_and_quit_loop("ERROR: failed to start pipeline",
            PEER_CALL_ERROR);

    loop = g_main_loop_new(NULL, FALSE);

    g_main_loop_run(loop);

    if (loop)
        g_main_loop_unref(loop);

    if (pipe1) {
        gst_element_set_state(GST_ELEMENT(pipe1), GST_STATE_NULL);
        gst_print("Pipeline stopped\n");
        gst_object_unref(pipe1);
    }

out:
    return ret_code;
}
