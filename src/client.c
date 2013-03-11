
/* ***** BEGIN LICENSE BLOCK *****
 * Version: MPL 1.1/GPL 2.0/LGPL 2.1
 *
 * The contents of this file are subject to the Mozilla Public License Version
 * 1.1 (the "License"); you may not use this file except in compliance with
 * the License. You may obtain a copy of the License at
 * http://www.mozilla.org/MPL/
 *
 * Software distributed under the License is distributed on an "AS IS" basis,
 * WITHOUT WARRANTY OF ANY KIND, either express or implied. See the License
 * for the specific language governing rights and limitations under the
 * License.
 *
 * The Original Code is libguac-client-rdp.
 *
 * The Initial Developer of the Original Code is
 * Michael Jumper.
 * Portions created by the Initial Developer are Copyright (C) 2011
 * the Initial Developer. All Rights Reserved.
 *
 * Contributor(s):
 * Matt Hortman
 * David PHAM-VAN <d.pham-van@ulteo.com> Ulteo SAS - http://www.ulteo.com
 * Laurent Meunier <laurent@deltalima.net>
 *
 * Alternatively, the contents of this file may be used under the terms of
 * either the GNU General Public License Version 2 or later (the "GPL"), or
 * the GNU Lesser General Public License Version 2.1 or later (the "LGPL"),
 * in which case the provisions of the GPL or the LGPL are applicable instead
 * of those above. If you wish to allow use of your version of this file only
 * under the terms of either the GPL or the LGPL, and not to allow others to
 * use your version of this file under the terms of the MPL, indicate your
 * decision by deleting the provisions above and replace them with the notice
 * and other provisions required by the GPL or the LGPL. If you do not delete
 * the provisions above, a recipient may use your version of this file under
 * the terms of any one of the MPL, the GPL or the LGPL.
 *
 * ***** END LICENSE BLOCK ***** */

#define _XOPEN_SOURCE 500

#include <stdlib.h>
#include <string.h>
#include <pthread.h>

#include <sys/select.h>
#include <errno.h>

#include <freerdp/freerdp.h>
#include <freerdp/cache/bitmap.h>
#include <freerdp/cache/brush.h>
#include <freerdp/cache/glyph.h>
#include <freerdp/cache/palette.h>
#include <freerdp/cache/pointer.h>
#include <freerdp/cache/offscreen.h>
#include <freerdp/channels/channels.h>
#include <freerdp/input.h>
#include <freerdp/constants.h>
#include <freerdp/codec/color.h>
#include <freerdp/client/channels.h>

#include <guacamole/socket.h>
#include <guacamole/protocol.h>
#include <guacamole/client.h>
#include <guacamole/error.h>

#include "audio.h"
#include "wav_encoder.h"

#ifdef ENABLE_OGG
#include "ogg_encoder.h"
#endif

#include "client.h"
#include "guac_handlers.h"
#include "rdp_keymap.h"
#include "rdp_bitmap.h"
#include "rdp_glyph.h"
#include "rdp_pointer.h"
#include "rdp_gdi.h"
#include "default_pointer.h"

/* Client plugin arguments */
const char* GUAC_CLIENT_ARGS[] = {
    "hostname",
    "port",
    "domain",
    "username",
    "password",
    "width",
    "height",
    "initial-program",
    "color-depth",
    "disable-audio",
    "console",
    "console-audio",
    "vmconnect",
    NULL
};

enum ARGS_IDX {
    IDX_HOSTNAME,
    IDX_PORT,
    IDX_DOMAIN,
    IDX_USERNAME,
    IDX_PASSWORD,
    IDX_WIDTH,
    IDX_HEIGHT,
    IDX_INITIAL_PROGRAM,
    IDX_COLOR_DEPTH,
    IDX_DISABLE_AUDIO,
    IDX_CONSOLE,
    IDX_CONSOLE_AUDIO,
    IDX_VMCONNECT,

    RDP_ARGS_COUNT
};

int __guac_receive_channel_data(freerdp* rdp_inst, int channelId, BYTE* data, int size, int flags, int total_size) {
    return freerdp_channels_data(rdp_inst, channelId, data, size, flags, total_size);
}

BOOL rdp_freerdp_pre_connect(freerdp* instance) {

    rdpContext* context = instance->context;
    guac_client* client = ((rdp_freerdp_context*) context)->client;
    rdpChannels* channels = context->channels;
    rdpBitmap* bitmap;
    rdpGlyph* glyph;
    rdpPointer* pointer;
    rdpPrimaryUpdate* primary;
    CLRCONV* clrconv;
    int i;

    freerdp_register_addin_provider(freerdp_channels_load_static_addin_entry, 0);

    rdp_guac_client_data* guac_client_data =
        (rdp_guac_client_data*) client->data;

    /* Load clipboard plugin */
    if (freerdp_channels_load_plugin(channels, instance->settings,
                "cliprdr", NULL))
        guac_client_log_error(client, "Failed to load cliprdr plugin.");

    /* If audio enabled, choose an encoder */
    if (guac_client_data->audio_enabled) {

        /* Choose an encoding */
        for (i=0; client->info.audio_mimetypes[i] != NULL; i++) {

            const char* mimetype = client->info.audio_mimetypes[i];

#ifdef ENABLE_OGG
            /* If Ogg is supported, done. */
            if (strcmp(mimetype, ogg_encoder->mimetype) == 0) {
                guac_client_log_info(client, "Loading Ogg Vorbis encoder.");
                guac_client_data->audio = audio_stream_alloc(client,
                        ogg_encoder);
                break;
            }
#endif

            /* If wav is supported, done. */
            if (strcmp(mimetype, wav_encoder->mimetype) == 0) {
                guac_client_log_info(client, "Loading wav encoder.");
                guac_client_data->audio = audio_stream_alloc(client,
                        wav_encoder);
                break;
            }

        }

        /* If an encoding is available, load the sound plugin */
        if (guac_client_data->audio != NULL) {

            /* Load sound plugin */
            if (freerdp_channels_load_plugin(channels, instance->settings,
                        "guacsnd", guac_client_data->audio))
                guac_client_log_error(client,
                        "Failed to load guacsnd plugin.");

        }
        else
            guac_client_log_info(client,
                    "No available audio encoding. Sound disabled.");

    } /* end if audio enabled */

    /* Init color conversion structure */
    clrconv = freerdp_clrconv_new(CLRCONV_ALPHA);
    ((rdp_freerdp_context*) context)->clrconv = clrconv;

    /* Init FreeRDP cache */
    instance->context->cache = cache_new(instance->settings);

    /* Set up bitmap handling */
    bitmap = (rdpBitmap*) malloc(sizeof(rdpBitmap));
    ZeroMemory(bitmap, sizeof(rdpBitmap));
    bitmap->size = sizeof(guac_rdp_bitmap);
    bitmap->New = guac_rdp_bitmap_new;
    bitmap->Free = guac_rdp_bitmap_free;
    bitmap->Paint = guac_rdp_bitmap_paint;
    bitmap->Decompress = guac_rdp_bitmap_decompress;
    bitmap->SetSurface = guac_rdp_bitmap_setsurface;
    graphics_register_bitmap(context->graphics, bitmap);
    free(bitmap);

    /* Set up glyph handling */
    glyph = (rdpGlyph*) malloc(sizeof(rdpGlyph));
    ZeroMemory(glyph, sizeof(rdpGlyph));
    glyph->size = sizeof(guac_rdp_glyph);
    glyph->New = guac_rdp_glyph_new;
    glyph->Free = guac_rdp_glyph_free;
    glyph->Draw = guac_rdp_glyph_draw;
    glyph->BeginDraw = guac_rdp_glyph_begindraw;
    glyph->EndDraw = guac_rdp_glyph_enddraw;
    graphics_register_glyph(context->graphics, glyph);
    free(glyph);

    /* Set up pointer handling */
    pointer = (rdpPointer*) malloc(sizeof(rdpPointer));
    ZeroMemory(pointer, sizeof(rdpPointer));
    pointer->size = sizeof(guac_rdp_pointer);
    pointer->New = guac_rdp_pointer_new;
    pointer->Free = guac_rdp_pointer_free;
    pointer->Set = guac_rdp_pointer_set;
#ifdef HAVE_RDPPOINTER_SETNULL
    pointer->SetNull = guac_rdp_pointer_set_null;
#endif
#ifdef HAVE_RDPPOINTER_SETDEFAULT
    pointer->SetDefault = guac_rdp_pointer_set_default;
#endif
    graphics_register_pointer(context->graphics, pointer);
    free(pointer);

    /* Set up GDI */
    instance->update->EndPaint = guac_rdp_gdi_end_paint;
    instance->update->Palette = guac_rdp_gdi_palette_update;
    instance->update->SetBounds = guac_rdp_gdi_set_bounds;

    primary = instance->update->primary;
    primary->DstBlt = guac_rdp_gdi_dstblt;
    primary->PatBlt = guac_rdp_gdi_patblt;
    primary->ScrBlt = guac_rdp_gdi_scrblt;
    primary->MemBlt = guac_rdp_gdi_memblt;
    primary->OpaqueRect = guac_rdp_gdi_opaquerect;

    pointer_cache_register_callbacks(instance->update);
    glyph_cache_register_callbacks(instance->update);
    brush_cache_register_callbacks(instance->update);
    bitmap_cache_register_callbacks(instance->update);
    offscreen_cache_register_callbacks(instance->update);
    palette_cache_register_callbacks(instance->update);

    /* Init channels (pre-connect) */
    if (freerdp_channels_pre_connect(channels, instance)) {
        guac_protocol_send_error(client->socket, "Error initializing RDP client channel manager");
        guac_socket_flush(client->socket);
        return FALSE;
    }

    return TRUE;

}

BOOL rdp_freerdp_post_connect(freerdp* instance) {

    rdpContext* context = instance->context;
    guac_client* client = ((rdp_freerdp_context*) context)->client;
    rdpChannels* channels = instance->context->channels;

    /* Init channels (post-connect) */
    if (freerdp_channels_post_connect(channels, instance)) {
        guac_protocol_send_error(client->socket, "Error initializing RDP client channel manager");
        guac_socket_flush(client->socket);
        return FALSE;
    }

    /* Client handlers */
    client->free_handler = rdp_guac_client_free_handler;
    client->handle_messages = rdp_guac_client_handle_messages;
    client->mouse_handler = rdp_guac_client_mouse_handler;
    client->key_handler = rdp_guac_client_key_handler;
    client->clipboard_handler = rdp_guac_client_clipboard_handler;

    return TRUE;

}

void rdp_freerdp_context_new(freerdp* instance, rdpContext* context) {
    context->channels = freerdp_channels_new();
}

void rdp_freerdp_context_free(freerdp* instance, rdpContext* context) {
    /* EMPTY */
}

void __guac_rdp_client_load_keymap(guac_client* client,
        const guac_rdp_keymap* keymap) {

    rdp_guac_client_data* guac_client_data =
        (rdp_guac_client_data*) client->data;

    /* Get mapping */
    const guac_rdp_keysym_desc* mapping = keymap->mapping;

    /* If parent exists, load parent first */
    if (keymap->parent != NULL)
        __guac_rdp_client_load_keymap(client, keymap->parent);

    /* Log load */
    guac_client_log_info(client, "Loading keymap \"%s\"", keymap->name);

    /* Load mapping into keymap */
    while (mapping->keysym != 0) {

        /* Copy mapping */
        GUAC_RDP_KEYSYM_LOOKUP(guac_client_data->keymap, mapping->keysym) =
            *mapping;

        /* Next keysym */
        mapping++;

    }

}

int guac_client_init(guac_client* client, int argc, char** argv) {

    rdp_guac_client_data* guac_client_data;

    freerdp* rdp_inst;
    rdpSettings* settings;

    char* hostname;
    int port = RDP_DEFAULT_PORT;
    BOOL BitmapCacheEnabled;
    BOOL portProvided = FALSE;

    /**
     * Selected server-side keymap. Client will be assumed to also use this
     * keymap. Keys will be sent to server based on client input on a
     * best-effort basis.
     *
     * Currently hard-coded to en-us-qwerty.
     */
    const guac_rdp_keymap* chosen_keymap = &guac_rdp_keymap_en_us;

    if (argc < RDP_ARGS_COUNT) {

        guac_protocol_send_error(client->socket,
                "Wrong argument count received.");
        guac_socket_flush(client->socket);

        guac_error = GUAC_STATUS_BAD_ARGUMENT;
        guac_error_message = "Wrong argument count received";

        return 1;
    }

    /* If port specified, use it */
    if (argv[IDX_PORT][0] != '\0') {
    	port = atoi(argv[IDX_PORT]);
    	portProvided = TRUE;
    }

    hostname = argv[IDX_HOSTNAME];

    /* Allocate client data */
    guac_client_data = malloc(sizeof(rdp_guac_client_data));

    /* Init client */
    freerdp_channels_global_init();
    rdp_inst = freerdp_new();
    rdp_inst->PreConnect = rdp_freerdp_pre_connect;
    rdp_inst->PostConnect = rdp_freerdp_post_connect;
    rdp_inst->ReceiveChannelData = __guac_receive_channel_data;

    /* Allocate FreeRDP context */
    rdp_inst->context_size = sizeof(rdp_freerdp_context);
    rdp_inst->ContextNew  = (pContextNew) rdp_freerdp_context_new;
    rdp_inst->ContextFree = (pContextFree) rdp_freerdp_context_free;
    freerdp_context_new(rdp_inst);

    /* Set settings */
    settings = rdp_inst->settings;

    /* Console */
    settings->ConsoleSession = (strcmp(argv[IDX_CONSOLE], "true") == 0);
    settings->RemoteConsoleAudio   = (strcmp(argv[IDX_CONSOLE_AUDIO], "true") == 0);

    /* Use optimal width unless overridden */
    settings->DesktopWidth = client->info.optimal_width;
    if (argv[IDX_WIDTH][0] != '\0')
        settings->DesktopWidth = atoi(argv[IDX_WIDTH]);

    /* Use default width if given width is invalid. */
    if (settings->DesktopWidth <= 0) {
        settings->DesktopWidth = RDP_DEFAULT_WIDTH;
        guac_client_log_error(client,
                "Invalid width: \"%s\". Using default of %i.",
                argv[IDX_WIDTH], settings->DesktopWidth);
    }

    /* Use optimal height unless overridden */
    settings->DesktopHeight = client->info.optimal_height;
    if (argv[IDX_HEIGHT][0] != '\0')
        settings->DesktopHeight = atoi(argv[IDX_HEIGHT]);

    /* Use default height if given height is invalid. */
    if (settings->DesktopHeight <= 0) {
        settings->DesktopHeight = RDP_DEFAULT_HEIGHT;
        guac_client_log_error(client,
                "Invalid height: \"%s\". Using default of %i.",
                argv[IDX_WIDTH], settings->DesktopHeight);
    }

    /* Set hostname */
    settings->ServerHostname = strdup(hostname);
    settings->WindowTitle = strdup(hostname);

    // Check if doing vmconnect before setting the port to use
    if (argv[IDX_VMCONNECT][0] != '\0') {
    	// If the port was not provided, set to the default vmconnect port
    	if (!portProvided) {
    		port = 2179;
    	}
    	settings->NegotiateSecurityLayer = FALSE;
    	settings->SendPreconnectionPdu = TRUE;
    	settings->PreconnectionBlob = strdup(argv[IDX_VMCONNECT]);
    	guac_client_log_info(client, "Connect to VM %s", settings->PreconnectionBlob);
    }

    settings->ServerPort = port;

    /* Domain */
    if (argv[IDX_DOMAIN][0] != '\0')
        settings->Domain = strdup(argv[IDX_DOMAIN]);

    /* Username */
    if (argv[IDX_USERNAME][0] != '\0')
        settings->Username = strdup(argv[IDX_USERNAME]);

    /* Password */
    if (argv[IDX_PASSWORD][0] != '\0') {
        settings->Password = strdup(argv[IDX_PASSWORD]);
        settings->AutoLogonEnabled = 1;
    }

    /* Initial program */
    if (argv[IDX_INITIAL_PROGRAM][0] != '\0')
        settings->AlternateShell = strdup(argv[IDX_INITIAL_PROGRAM]);

    /* Session color depth */
    settings->ColorDepth = RDP_DEFAULT_DEPTH;
    if (argv[IDX_COLOR_DEPTH][0] != '\0')
        settings->ColorDepth = atoi(argv[IDX_COLOR_DEPTH]);

    /* Use default depth if given depth is invalid. */
    if (settings->ColorDepth == 0) {
        settings->ColorDepth = RDP_DEFAULT_DEPTH;
        guac_client_log_error(client,
                "Invalid color-depth: \"%s\". Using default of %i.",
                argv[IDX_WIDTH], settings->ColorDepth);
    }

    /* Audio enable/disable */
    guac_client_data->audio_enabled =
        (strcmp(argv[IDX_DISABLE_AUDIO], "true") != 0);

    /* Order support */
    BitmapCacheEnabled = settings->BitmapCacheEnabled;
    settings->OsMajorType = OSMAJORTYPE_UNSPECIFIED;
    settings->OsMinorType = OSMINORTYPE_UNSPECIFIED;
    settings->OrderSupport[NEG_DSTBLT_INDEX] = TRUE;
    settings->OrderSupport[NEG_PATBLT_INDEX] = FALSE; /* PATBLT not yet supported */
    settings->OrderSupport[NEG_SCRBLT_INDEX] = TRUE;
    settings->OrderSupport[NEG_OPAQUE_RECT_INDEX] = TRUE;
    settings->OrderSupport[NEG_DRAWNINEGRID_INDEX] = FALSE;
    settings->OrderSupport[NEG_MULTIDSTBLT_INDEX] = FALSE;
    settings->OrderSupport[NEG_MULTIPATBLT_INDEX] = FALSE;
    settings->OrderSupport[NEG_MULTISCRBLT_INDEX] = FALSE;
    settings->OrderSupport[NEG_MULTIOPAQUERECT_INDEX] = FALSE;
    settings->OrderSupport[NEG_MULTI_DRAWNINEGRID_INDEX] = FALSE;
    settings->OrderSupport[NEG_LINETO_INDEX] = FALSE;
    settings->OrderSupport[NEG_POLYLINE_INDEX] = FALSE;
    settings->OrderSupport[NEG_MEMBLT_INDEX] = BitmapCacheEnabled;
    settings->OrderSupport[NEG_MEM3BLT_INDEX] = FALSE;
    settings->OrderSupport[NEG_MEMBLT_V2_INDEX] = BitmapCacheEnabled;
    settings->OrderSupport[NEG_MEM3BLT_V2_INDEX] = FALSE;
    settings->OrderSupport[NEG_SAVEBITMAP_INDEX] = FALSE;
    settings->OrderSupport[NEG_GLYPH_INDEX_INDEX] = TRUE;
    settings->OrderSupport[NEG_FAST_INDEX_INDEX] = TRUE;
    settings->OrderSupport[NEG_FAST_GLYPH_INDEX] = TRUE;
    settings->OrderSupport[NEG_POLYGON_SC_INDEX] = FALSE;
    settings->OrderSupport[NEG_POLYGON_CB_INDEX] = FALSE;
    settings->OrderSupport[NEG_ELLIPSE_SC_INDEX] = FALSE;
    settings->OrderSupport[NEG_ELLIPSE_CB_INDEX] = FALSE;

    /* Store client data */
    guac_client_data->rdp_inst = rdp_inst;
    guac_client_data->mouse_button_mask = 0;
    guac_client_data->current_surface = GUAC_DEFAULT_LAYER;
    guac_client_data->clipboard = NULL;
    guac_client_data->audio = NULL;

    /* Recursive attribute for locks */
    pthread_mutexattr_init(&(guac_client_data->attributes));
    pthread_mutexattr_settype(&(guac_client_data->attributes),
            PTHREAD_MUTEX_RECURSIVE);

    /* Init update lock */
    pthread_mutex_init(&(guac_client_data->update_lock),
           &(guac_client_data->attributes));

    /* Init RDP lock */
    pthread_mutex_init(&(guac_client_data->rdp_lock),
           &(guac_client_data->attributes));

    /* Clear keysym state mapping and keymap */
    memset(guac_client_data->keysym_state, 0,
            sizeof(guac_rdp_keysym_state_map));

    memset(guac_client_data->keymap, 0,
            sizeof(guac_rdp_static_keymap));

    client->data = guac_client_data;
    ((rdp_freerdp_context*) rdp_inst->context)->client = client;

    /* Load keymap into client */
    __guac_rdp_client_load_keymap(client, chosen_keymap);

    /* Set server-side keymap */
    settings->KeyboardLayout = chosen_keymap->freerdp_keyboard_layout;

    /* Connect to RDP server */
    if (!freerdp_connect(rdp_inst)) {

        guac_protocol_send_error(client->socket,
                "Error connecting to RDP server");
        guac_socket_flush(client->socket);

        guac_error = GUAC_STATUS_BAD_STATE;
        guac_error_message = "Error connecting to RDP server";

        return 1;
    }

    /* Send connection name */
    guac_protocol_send_name(client->socket, settings->WindowTitle);

    /* Send size */
    guac_protocol_send_size(client->socket, GUAC_DEFAULT_LAYER,
            settings->DesktopWidth, settings->DesktopHeight);

    /* Create glyph surfaces */
    guac_client_data->opaque_glyph_surface = cairo_image_surface_create(
            CAIRO_FORMAT_RGB24, settings->DesktopWidth, settings->DesktopHeight);

    guac_client_data->trans_glyph_surface = cairo_image_surface_create(
            CAIRO_FORMAT_ARGB32, settings->DesktopWidth, settings->DesktopHeight);

    /* Set default pointer */
    guac_rdp_set_default_pointer(client);

    /* Success */
    return 0;

}

