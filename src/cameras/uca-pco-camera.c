/* Copyright (C) 2011, 2012 Matthias Vogelgesang <matthias.vogelgesang@kit.edu>
   (Karlsruhe Institute of Technology)

   This library is free software; you can redistribute it and/or modify it
   under the terms of the GNU Lesser General Public License as published by the
   Free Software Foundation; either version 2.1 of the License, or (at your
   option) any later version.

   This library is distributed in the hope that it will be useful, but WITHOUT
   ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS
   FOR A PARTICULAR PURPOSE. See the GNU Lesser General Public License for more
   details.

   You should have received a copy of the GNU Lesser General Public License along
   with this library; if not, write to the Free Software Foundation, Inc., 51
   Franklin St, Fifth Floor, Boston, MA 02110, USA */

#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <libpco/libpco.h>
#include <libpco/sc2_defs.h>
#include <fgrab_struct.h>
#include <fgrab_prototyp.h>
#include "uca-camera.h"
#include "uca-pco-camera.h"

#define FG_TRY_PARAM(fg, camobj, param, val_addr, port)     \
    { int r = Fg_setParameter(fg, param, val_addr, port);   \
        if (r != FG_OK) {                                   \
            g_set_error(error, UCA_PCO_CAMERA_ERROR,        \
                    UCA_PCO_CAMERA_ERROR_FG_GENERAL,        \
                    "%s", Fg_getLastErrorDescription(fg));  \
            g_object_unref(camobj);                         \
            return NULL;                                    \
        } }

#define FG_SET_ERROR(err, fg, err_type)                 \
    if (err != FG_OK) {                                 \
        g_set_error(error, UCA_PCO_CAMERA_ERROR,        \
                err_type,                               \
                "%s", Fg_getLastErrorDescription(fg));  \
        return;                                         \
    }

#define HANDLE_PCO_ERROR(err)                       \
    if ((err) != PCO_NOERROR) {                     \
        g_set_error(error, UCA_PCO_CAMERA_ERROR,    \
                UCA_PCO_CAMERA_ERROR_LIBPCO_GENERAL,\
                "libpco error %i", err);            \
        return;                                     \
    }
    
#define UCA_PCO_CAMERA_GET_PRIVATE(obj) (G_TYPE_INSTANCE_GET_PRIVATE((obj), UCA_TYPE_PCO_CAMERA, UcaPcoCameraPrivate))

G_DEFINE_TYPE(UcaPcoCamera, uca_pco_camera, UCA_TYPE_CAMERA)

/**
 * UcaPcoCameraError:
 * @UCA_PCO_CAMERA_ERROR_LIBPCO_INIT: Initializing libpco failed
 * @UCA_PCO_CAMERA_ERROR_LIBPCO_GENERAL: General libpco error
 * @UCA_PCO_CAMERA_ERROR_UNSUPPORTED: Camera type is not supported
 * @UCA_PCO_CAMERA_ERROR_FG_INIT: Framegrabber initialization failed
 * @UCA_PCO_CAMERA_ERROR_FG_GENERAL: General framegrabber error
 * @UCA_PCO_CAMERA_ERROR_FG_ACQUISITION: Framegrabber acquisition error
 */
GQuark uca_pco_camera_error_quark()
{
    return g_quark_from_static_string("uca-pco-camera-error-quark");
}

enum {
    PROP_0,
    PROP_SENSOR_WIDTH,
    PROP_SENSOR_HEIGHT,
    PROP_SENSOR_BITDEPTH,
    PROP_SENSOR_HORIZONTAL_BINNING,
    PROP_SENSOR_HORIZONTAL_BINNINGS,
    PROP_SENSOR_VERTICAL_BINNING,
    PROP_SENSOR_VERTICAL_BINNINGS,
    PROP_SENSOR_MAX_FRAME_RATE,
    PROP_ROI_X,
    PROP_ROI_Y,
    PROP_ROI_WIDTH,
    PROP_ROI_HEIGHT,
    PROP_HAS_STREAMING,
    PROP_HAS_CAMRAM_RECORDING,
    N_INTERFACE_PROPERTIES,

    PROP_NAME,
    PROP_COOLING_POINT,
    N_PROPERTIES
};

static const gchar *base_overrideables[N_PROPERTIES] = {
    "sensor-width",
    "sensor-height",
    "sensor-bitdepth",
    "sensor-horizontal-binning",
    "sensor-horizontal-binnings",
    "sensor-vertical-binning",
    "sensor-vertical-binnings",
    "max-frame-rate",
    "roi-x",
    "roi-y",
    "roi-width",
    "roi-height",
    "has-streaming",
    "has-camram-recording"
};

static GParamSpec *pco_properties[N_PROPERTIES - N_INTERFACE_PROPERTIES - 1] = { NULL, };

/*
 * This structure defines type-specific properties of PCO cameras.
 */
typedef struct {
    int camera_type;
    const char *so_file;
    int cl_type;
    int cl_format;
    gfloat max_frame_rate;
    gboolean has_camram;
} pco_cl_map_entry;

struct _UcaPcoCameraPrivate {
    pco_handle pco;
    pco_cl_map_entry *camera_description;

    Fg_Struct *fg;
    guint fg_port;
    dma_mem *fg_mem;

    guint frame_width;
    guint frame_height;
    gsize num_bytes;

    guint16 width;
    guint16 height;
    GValueArray *horizontal_binnings;
    GValueArray *vertical_binnings;

    guint16 active_segment;
    guint num_recorded_images;
    guint current_image;
};

static pco_cl_map_entry pco_cl_map[] = { 
    { CAMERATYPE_PCO_EDGE,       "libFullAreaGray8.so",  FG_CL_8BIT_FULL_10,        FG_GRAY,     30.0f, FALSE },
    { CAMERATYPE_PCO4000,        "libDualAreaGray16.so", FG_CL_SINGLETAP_16_BIT,    FG_GRAY16,    5.0f, TRUE },
    { CAMERATYPE_PCO_DIMAX_STD,  "libFullAreaGray16.so", FG_CL_SINGLETAP_8_BIT,     FG_GRAY16, 1279.0f, TRUE },
    { 0, NULL, 0, 0, 0.0f, FALSE }
};

static pco_cl_map_entry *get_pco_cl_map_entry(int camera_type)
{
    pco_cl_map_entry *entry = pco_cl_map;

    while (entry->camera_type != 0) {
        if (entry->camera_type == camera_type)
            return entry;
        entry++; 
    }

    return NULL;
}

static guint fill_binnings(UcaPcoCameraPrivate *priv)
{
    uint16_t *horizontal = NULL;
    uint16_t *vertical = NULL;
    guint num_horizontal, num_vertical;

    guint err = pco_get_possible_binnings(priv->pco, 
            &horizontal, &num_horizontal, 
            &vertical, &num_vertical);

    GValue val = {0};
    g_value_init(&val, G_TYPE_UINT);

    if (err == PCO_NOERROR) {
        priv->horizontal_binnings = g_value_array_new(num_horizontal);
        priv->vertical_binnings = g_value_array_new(num_vertical);

        for (guint i = 0; i < num_horizontal; i++) {
            g_value_set_uint(&val, horizontal[i]);
            g_value_array_append(priv->horizontal_binnings, &val);
        }

        for (guint i = 0; i < num_vertical; i++) {
            g_value_set_uint(&val, vertical[i]);
            g_value_array_append(priv->vertical_binnings, &val);
        }
    }

    free(horizontal);
    free(vertical);
    return err;
}

static guint override_temperature_range(UcaPcoCameraPrivate *priv)
{
    int16_t default_temp, min_temp, max_temp;
    guint err = pco_get_cooling_range(priv->pco, &default_temp, &min_temp, &max_temp);

    if (err == PCO_NOERROR) {
        GParamSpecInt *spec = (GParamSpecInt *) pco_properties[PROP_COOLING_POINT];
        spec->minimum = min_temp;
        spec->maximum = max_temp;
        spec->default_value = default_temp;
    }

    return err;
}

UcaPcoCamera *uca_pco_camera_new(GError **error)
{
    pco_handle pco = pco_init();

    if (pco == NULL) {
        g_set_error(error, UCA_PCO_CAMERA_ERROR, UCA_PCO_CAMERA_ERROR_LIBPCO_INIT,
                "Initializing libpco failed");
        return NULL;
    }

    UcaPcoCamera *camera = g_object_new(UCA_TYPE_PCO_CAMERA, NULL);
    UcaPcoCameraPrivate *priv = UCA_PCO_CAMERA_GET_PRIVATE(camera);
    priv->pco = pco;

    guint16 width_ex, height_ex;
    pco_get_resolution(priv->pco, &priv->width, &priv->height, &width_ex, &height_ex);
    pco_set_storage_mode(pco, STORAGE_MODE_RECORDER);
    pco_set_auto_transfer(pco, 1);

    guint16 camera_type, camera_subtype;
    pco_get_camera_type(priv->pco, &camera_type, &camera_subtype);
    pco_cl_map_entry *map_entry = get_pco_cl_map_entry(camera_type);
    priv->camera_description = map_entry;

    if (map_entry == NULL) {
        g_set_error(error, UCA_PCO_CAMERA_ERROR, UCA_PCO_CAMERA_ERROR_UNSUPPORTED,
                "Camera type is not supported");
        g_object_unref(camera);
        return NULL;
    }

    priv->fg_port = PORT_A;
    priv->fg = Fg_Init(map_entry->so_file, priv->fg_port);

    if (priv->fg == NULL) {
        g_set_error(error, UCA_PCO_CAMERA_ERROR, UCA_PCO_CAMERA_ERROR_FG_INIT,
                "%s", Fg_getLastErrorDescription(priv->fg));
        g_object_unref(camera);
        return NULL;
    }

    const guint32 fg_width = camera_type == CAMERATYPE_PCO_EDGE ? priv->width * 2 : priv->width;

    FG_TRY_PARAM(priv->fg, camera, FG_CAMERA_LINK_CAMTYP, &map_entry->cl_type, priv->fg_port);
    FG_TRY_PARAM(priv->fg, camera, FG_FORMAT, &map_entry->cl_format, priv->fg_port);
    FG_TRY_PARAM(priv->fg, camera, FG_WIDTH, &fg_width, priv->fg_port);
    FG_TRY_PARAM(priv->fg, camera, FG_HEIGHT, &priv->height, priv->fg_port);

    int val = FREE_RUN;
    FG_TRY_PARAM(priv->fg, camera, FG_TRIGGERMODE, &val, priv->fg_port);

    fill_binnings(priv);

    /*
     * Here we override the temperature property range because this was not
     * possible at property installation time.
     */
    override_temperature_range(priv);

    return camera;
}

static void uca_pco_camera_start_recording(UcaCamera *camera, GError **error)
{
    g_return_if_fail(UCA_IS_PCO_CAMERA(camera));
    guint err = PCO_NOERROR;

    UcaPcoCameraPrivate *priv = UCA_PCO_CAMERA_GET_PRIVATE(camera);

    /*
     * Get the actual size of the frame that we are going to grab based on the
     * currently selected region of interest. This size is fixed until recording
     * has stopped.
     */
    guint16 roi[4] = {0};
    err = pco_get_roi(priv->pco, roi);
    priv->frame_width = roi[2] - roi[0] + 1;
    priv->frame_height = roi[3] - roi[1] + 1;
    priv->num_bytes = 2;

    Fg_setParameter(priv->fg, FG_WIDTH, &priv->frame_width, priv->fg_port);
    Fg_setParameter(priv->fg, FG_HEIGHT, &priv->frame_height, priv->fg_port);

    if (priv->fg_mem)
        Fg_FreeMemEx(priv->fg, priv->fg_mem);

    const guint num_buffers = 2;
    priv->fg_mem = Fg_AllocMemEx(priv->fg, 
            num_buffers * priv->frame_width * priv->frame_height * sizeof(uint16_t), num_buffers);

    if (priv->fg_mem == NULL) {
        g_set_error(error, UCA_PCO_CAMERA_ERROR, UCA_PCO_CAMERA_ERROR_FG_INIT,
                "%s", Fg_getLastErrorDescription(priv->fg));
        g_object_unref(camera);
        return;
    }

    if ((priv->camera_description->camera_type == CAMERATYPE_PCO_DIMAX_STD) ||
        (priv->camera_description->camera_type == CAMERATYPE_PCO4000))
        pco_clear_active_segment(priv->pco);

    err = pco_arm_camera(priv->pco);
    HANDLE_PCO_ERROR(err);

    err = pco_start_recording(priv->pco);
    HANDLE_PCO_ERROR(err);

    err = Fg_AcquireEx(priv->fg, priv->fg_port, GRAB_INFINITE, ACQ_STANDARD, priv->fg_mem);
    FG_SET_ERROR(err, priv->fg, UCA_PCO_CAMERA_ERROR_FG_ACQUISITION);
}

static void uca_pco_camera_stop_recording(UcaCamera *camera, GError **error)
{
    g_return_if_fail(UCA_IS_PCO_CAMERA(camera));
    UcaPcoCameraPrivate *priv = UCA_PCO_CAMERA_GET_PRIVATE(camera);
    pco_stop_recording(priv->pco);
    guint err = Fg_stopAcquireEx(priv->fg, priv->fg_port, priv->fg_mem, STOP_SYNC);

    FG_SET_ERROR(err, priv->fg, UCA_PCO_CAMERA_ERROR_FG_ACQUISITION);
}

static void uca_pco_camera_start_readout(UcaCamera *camera, GError **error)
{
    g_return_if_fail(UCA_IS_PCO_CAMERA(camera));
    UcaPcoCameraPrivate *priv = UCA_PCO_CAMERA_GET_PRIVATE(camera);

    /* 
     * TODO: Check if readout mode is possible. This is not the case for the
     * edge. 
     */

    guint err = pco_get_active_segment(priv->pco, &priv->active_segment);
    HANDLE_PCO_ERROR(err);

    err = pco_get_num_images(priv->pco, priv->active_segment, &priv->num_recorded_images);
    HANDLE_PCO_ERROR(err);

    priv->current_image = 1;
}

static void uca_pco_camera_grab(UcaCamera *camera, gpointer *data, GError **error)
{
    g_return_if_fail(UCA_IS_PCO_CAMERA(camera));
    static frameindex_t last_frame = 0;
    UcaPcoCameraPrivate *priv = UCA_PCO_CAMERA_GET_PRIVATE(camera);

    gboolean is_readout = FALSE;
    g_object_get(G_OBJECT(camera), "is-readout", &is_readout, NULL);

    if (is_readout) {
        if (priv->current_image == priv->num_recorded_images) {
            *data = NULL;
            return;
        }

        /*
         * No joke, the pco firmware allows to read a range of images but
         * implements only reading single images ...
         */
        pco_read_images(priv->pco, priv->active_segment, 
                priv->current_image, priv->current_image);
        priv->current_image++;
    }

    pco_request_image(priv->pco);
    last_frame = Fg_getLastPicNumberBlockingEx(priv->fg, last_frame+1, priv->fg_port, 5, priv->fg_mem);
    
    if (last_frame <= 0) {
        guint err = FG_OK + 1;
        FG_SET_ERROR(err, priv->fg, UCA_PCO_CAMERA_ERROR_FG_GENERAL);
    }

    guint16 *frame = Fg_getImagePtrEx(priv->fg, last_frame, priv->fg_port, priv->fg_mem);

    if (*data == NULL)
        *data = g_malloc0(priv->frame_width * priv->frame_height * priv->num_bytes); 

    if (priv->camera_description->camera_type == CAMERATYPE_PCO_EDGE)
        pco_get_reorder_func(priv->pco)((guint16 *) *data, frame, priv->frame_width, priv->frame_height);
    else
        memcpy((gchar *) *data, (gchar *) frame, priv->frame_width * priv->frame_height * priv->num_bytes);
}

static void uca_pco_camera_set_property(GObject *object, guint property_id, const GValue *value, GParamSpec *pspec)
{
    UcaPcoCameraPrivate *priv = UCA_PCO_CAMERA_GET_PRIVATE(object);

    switch (property_id) {
        case PROP_COOLING_POINT:
            {
                int16_t temperature = (int16_t) g_value_get_int(value); 
                pco_set_cooling_temperature(priv->pco, temperature);
            }
            break;
        default:
            G_OBJECT_WARN_INVALID_PROPERTY_ID(object, property_id, pspec);
            return;
    }
}

static void uca_pco_camera_get_property(GObject *object, guint property_id, GValue *value, GParamSpec *pspec)
{
    UcaPcoCameraPrivate *priv = UCA_PCO_CAMERA_GET_PRIVATE(object);

    switch (property_id) {
        case PROP_SENSOR_WIDTH: 
            g_value_set_uint(value, priv->width);
            break;

        case PROP_SENSOR_HEIGHT: 
            g_value_set_uint(value, priv->height);
            break;

        case PROP_SENSOR_HORIZONTAL_BINNING:
            {
                uint16_t h, v;
                /* TODO: check error */
                pco_get_binning(priv->pco, &h, &v);
                g_value_set_uint(value, h);
            }
            break;

        case PROP_SENSOR_HORIZONTAL_BINNINGS:
            g_value_set_boxed(value, priv->horizontal_binnings);
            break;

        case PROP_SENSOR_VERTICAL_BINNING:
            {
                uint16_t h, v;
                /* TODO: check error */
                pco_get_binning(priv->pco, &h, &v);
                g_value_set_uint(value, v);
            }
            break;

        case PROP_SENSOR_VERTICAL_BINNINGS:
            g_value_set_boxed(value, priv->vertical_binnings);
            break;

        case PROP_SENSOR_MAX_FRAME_RATE:
            g_value_set_float(value, priv->camera_description->max_frame_rate);
            break;

        case PROP_SENSOR_BITDEPTH:
            g_value_set_uint(value, 16);
            break;

        case PROP_HAS_STREAMING:
            g_value_set_boolean(value, TRUE);
            break;

        case PROP_HAS_CAMRAM_RECORDING:
            g_value_set_boolean(value, priv->camera_description->has_camram);
            break;

        case PROP_ROI_X:
            {
                guint16 roi[4] = {0};
                pco_get_roi(priv->pco, roi);
                g_value_set_uint(value, roi[0]);
            }
            break;

        case PROP_ROI_Y:
            {
                guint16 roi[4] = {0};
                pco_get_roi(priv->pco, roi);
                g_value_set_uint(value, roi[1]);
            }
            break;

        case PROP_ROI_WIDTH:
            {
                guint16 roi[4] = {0};
                pco_get_roi(priv->pco, roi);
                g_value_set_uint(value, (roi[2] - roi[0]));
            }
            break;

        case PROP_ROI_HEIGHT:
            {
                guint16 roi[4] = {0};
                pco_get_roi(priv->pco, roi);
                g_value_set_uint(value, (roi[3] - roi[1]));
            }
            break;

        case PROP_NAME: 
            {
                gchar *name = NULL;
                pco_get_name(priv->pco, &name);
                g_value_set_string(value, name);
                g_free(name);
            }
            break;

        case PROP_COOLING_POINT:
            {
                int16_t temperature; 
                pco_get_cooling_temperature(priv->pco, &temperature);
                g_value_set_int(value, temperature);
            }
            break;

        default:
            G_OBJECT_WARN_INVALID_PROPERTY_ID(object, property_id, pspec);
            break;
    }
}

static void uca_pco_camera_finalize(GObject *object)
{
    UcaPcoCameraPrivate *priv = UCA_PCO_CAMERA_GET_PRIVATE(object);

    if (priv->horizontal_binnings)
        g_value_array_free(priv->horizontal_binnings);

    if (priv->vertical_binnings)
        g_value_array_free(priv->vertical_binnings);

    if (priv->fg) {
        if (priv->fg_mem)
            Fg_FreeMemEx(priv->fg, priv->fg_mem);

        Fg_FreeGrabber(priv->fg);
    }

    if (priv->pco)
        pco_destroy(priv->pco);

    G_OBJECT_CLASS(uca_pco_camera_parent_class)->finalize(object);
}

static void uca_pco_camera_class_init(UcaPcoCameraClass *klass)
{
    GObjectClass *gobject_class = G_OBJECT_CLASS(klass);
    gobject_class->set_property = uca_pco_camera_set_property;
    gobject_class->get_property = uca_pco_camera_get_property;
    gobject_class->finalize = uca_pco_camera_finalize;

    UcaCameraClass *camera_class = UCA_CAMERA_CLASS(klass);
    camera_class->start_recording = uca_pco_camera_start_recording;
    camera_class->stop_recording = uca_pco_camera_stop_recording;
    camera_class->start_readout = uca_pco_camera_start_readout;
    camera_class->grab = uca_pco_camera_grab;

    for (guint id = PROP_0 + 1; id < N_INTERFACE_PROPERTIES; id++)
        g_object_class_override_property(gobject_class, id, base_overrideables[id-1]);

    pco_properties[PROP_NAME] = 
        g_param_spec_string("name",
            "Name of the camera",
            "Name of the camera",
            "", G_PARAM_READABLE);

    /*
     * The default values here are set arbitrarily, because we are not yet
     * connected to the camera and just don't know the cooling range. We
     * override these values in uca_pco_camera_new().
     */
    pco_properties[PROP_COOLING_POINT] = 
        g_param_spec_int("cooling-point",
            "Cooling point of the camera",
            "Cooling point of the camera",
            0, 10, 5, G_PARAM_READWRITE);

    for (guint id = N_INTERFACE_PROPERTIES + 1; id < N_PROPERTIES; id++)
        g_object_class_install_property(gobject_class, id, pco_properties[id]);

    g_type_class_add_private(klass, sizeof(UcaPcoCameraPrivate));
}

static void uca_pco_camera_init(UcaPcoCamera *self)
{
    self->priv = UCA_PCO_CAMERA_GET_PRIVATE(self);
    self->priv->fg = NULL;
    self->priv->fg_mem = NULL;
    self->priv->pco = NULL;
    self->priv->horizontal_binnings = NULL;
    self->priv->vertical_binnings = NULL;
    self->priv->camera_description = NULL;
}
