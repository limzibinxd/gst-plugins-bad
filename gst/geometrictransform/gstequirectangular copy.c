/*
 * GStreamer
 * Copyright (C) 2010 Filippo Argiolas <filippo.argiolas@gmail.com>
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the "Software"),
 * to deal in the Software without restriction, including without limitation
 * the rights to use, copy, modify, merge, publish, distribute, sublicense,
 * and/or sell copies of the Software, and to permit persons to whom the
 * Software is furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
 * FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER
 * DEALINGS IN THE SOFTWARE.
 *
 * Alternatively, the contents of this file may be used under the
 * GNU Lesser General Public License Version 2.1 (the "LGPL"), in
 * which case the following provisions apply instead of the ones
 * mentioned above:
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Library General Public
 * License as published by the Free Software Foundation; either
 * version 2 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Library General Public License for more details.
 *
 * You should have received a copy of the GNU Library General Public
 * License along with this library; if not, write to the
 * Free Software Foundation, Inc., 51 Franklin St, Fifth Floor,
 * Boston, MA 02110-1301, USA.
 */

/**
 * SECTION:element-equirectangular
 * @title: equirectangular
 * @see_also: geometrictransform
 *
 * Equirectangular is a geometric image transform element. It transform a MOIL
 * fisheye to an equirectangular image .
 *
 * ## Example launch line
 * |[
 * gst-launch-1.0 -v videotestsrc ! equirectangular ! videoconvert !
 * autovideosink
 * ]|
 *
 */
#include <stdio.h>
#include <stdlib.h>

#ifdef HAVE_CONFIG_H
#include <config.h>
#endif

#include <gst/gst.h>
#include <math.h>

#include "gstequirectangular.h"

GST_DEBUG_CATEGORY_STATIC(gst_equirectangular_debug);
#define GST_CAT_DEFAULT gst_equirectangular_debug

#define gst_equirectangular_parent_class parent_class
G_DEFINE_TYPE(GstEquirectangular, gst_equirectangular,
              GST_TYPE_GEOMETRIC_TRANSFORM);

#ifndef GST_RENESAS
// 1.19.2
GST_ELEMENT_REGISTER_DEFINE_WITH_CODE(
    equirectangular, "equirectangular", GST_RANK_NONE, GST_TYPE_EQUIRECTANGULAR,
    GST_DEBUG_CATEGORY_INIT(gst_equirectangular_debug, "equirectangular", 0,
                            "equirectangular"));
#endif

static gboolean equirectangular_map(GstGeometricTransform *gt, gint x, gint y,
                                    gdouble *in_x, gdouble *in_y) {
#ifndef GST_DISABLE_GST_DEBUG
    GstEquirectangular *equi = GST_EQUIRECTANGULAR_CAST(gt);
#endif    
    gdouble norm_x;
    gdouble norm_y;
    gdouble r;

    gdouble width = gt->width;
    gdouble height = gt->height;

    gint displacement = ((y - 1) * (int)width + x);
    displacement = CLAMP (displacement, 0, BUF_SZ - 1);        
    if (( gt->width == equi->mat_width ) && ( gt->height == equi->mat_height )){
    *in_x = (gdouble)equi->buf_x[displacement];
    *in_y = (gdouble)equi->buf_y[displacement];
    }
    else { // missing maps
    }
    return TRUE;
}

static void gst_equirectangular_class_init(GstEquirectangularClass *klass) {

    GObjectClass *gobject_class;
    GstElementClass *gstelement_class;
    GstGeometricTransformClass *gstgt_class;

    gobject_class = (GObjectClass *) klass;
    gstelement_class = (GstElementClass *)klass;
    gstgt_class = (GstGeometricTransformClass *)klass;

    gst_element_class_set_static_metadata(
        gstelement_class, "equirectangular", "Transform/Effect/Video",
        "Simulate a equirectangular image",
        "Filippo Argiolas <filippo.argiolas@gmail.com>");

    gobject_class->finalize = gst_equirectangular_finalize;
    gstgt_class->prepare_func = equirectangular_prepare;
    gstgt_class->map_func = equirectangular_map;

}

static void gst_equirectangular_init(GstEquirectangular *filter) {
    GstGeometricTransform *gt = GST_GEOMETRIC_TRANSFORM(filter);

    gt->off_edge_pixels = GST_GT_OFF_EDGES_PIXELS_CLAMP;
}

gboolean gst_equirectangular_plugin_init(GstPlugin *plugin) {
    GST_DEBUG_CATEGORY_INIT(gst_equirectangular_debug, "equirectangular", 0,
                            "equirectangular");

    return gst_element_register(plugin, "equirectangular", GST_RANK_NONE,
                                GST_TYPE_EQUIRECTANGULAR);
}

/* Clean up */
static void
gst_equirectangular_finalize (GObject * obj)
{
    fprintf(stdout, "Finalize! \n");
    GstEquirectangular *equi = GST_EQUIRECTANGULAR_CAST (obj);

    g_free (equi->buf_x);
    g_free (equi->buf_y);

  G_OBJECT_CLASS (parent_class)->finalize (obj);
}


static gboolean
equirectangular_prepare(GstGeometricTransform * trans) {
  GstEquirectangular *equi = GST_EQUIRECTANGULAR_CAST (trans);
    equi->fptr_x = fopen("EquimatX", "rb");
    equi->fptr_y = fopen("EquimatY", "rb");
   
    if ((equi->fptr_x != NULL) && (equi->fptr_y != NULL)) {
        int Buf_Size = BUF_SZ;
        equi->buf_x = malloc(sizeof(float) * Buf_Size);
        equi->buf_y = malloc(sizeof(float) * Buf_Size);
        int rows, cols, type, channels;
        fread(&equi->mat_height, 1, sizeof(int), equi->fptr_x);
        fread(&equi->mat_width, 1, sizeof(int), equi->fptr_x);
        fread(&type, 1, sizeof(int), equi->fptr_x);
        fread(&channels, 1, sizeof(int), equi->fptr_x);
        fread(equi->buf_x, Buf_Size, sizeof(float), equi->fptr_x);
        fread(&rows, 1, sizeof(int), equi->fptr_y);
        fread(&cols, 1, sizeof(int), equi->fptr_y);
        fread(&type, 1, sizeof(int), equi->fptr_y);
        fread(&channels, 1, sizeof(int), equi->fptr_y);
        fread(equi->buf_y, Buf_Size, sizeof(float), equi->fptr_y);
        
        if (( equi->mat_height == rows ) && ( equi->mat_width == cols )){
        fprintf(stdout, "( rows, cols ) = ( %d, %d ) \n", rows, cols);
        fprintf(stdout, "X, Y Mats Loaded! \n");
    } else {
        if (errno != 0)
            fprintf(stderr, "Could not open mat file, for this reason: %s\n!",
                    strerror(errno));
        else
            fprintf(stderr, "Unknown errors.\n");
    }   
    fclose(equi->fptr_x);
    fclose(equi->fptr_y);
    }
    return TRUE;
}
