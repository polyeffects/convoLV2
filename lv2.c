/* convoLV2 -- LV2 convolution plugin
 *
 * Copyright (C) 2012 Robin Gareus <robin@gareus.org>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2, or (at your option)
 * any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software Foundation,
 * Inc., 59 Temple Place - Suite 330, Boston, MA 02111-1307, USA.  
 */

#include <stdio.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>
#include "convolution.h"

#include "lv2/lv2plug.in/ns/lv2core/lv2.h"
#include "lv2/lv2plug.in/ns/ext/worker/worker.h"

#include "./uris.h"

typedef enum {
  P_INPUT      = 0,
  P_OUTPUT     = 1,
  P_CONTROL    = 2,
  P_NOTIFY     = 3,
} PortIndex;

typedef struct {
  LV2_URID_Map*        map;
  LV2_Worker_Schedule *schedule;

  LV2_Atom_Forge forge;

  float* input;
  float* output;
  const LV2_Atom_Sequence* control_port;
  LV2_Atom_Sequence*       notify_port;

  LV2_Atom_Forge_Frame notify_frame;

  ConvoLV2URIs uris;

  LV2convolv *clv_online; ///< currently active engine
  LV2convolv *clv_offline; ///< inactive engine being configured
  int bufsize;
  int rate;
  int reinit_in_progress;

} convoLV2;

static LV2_Handle
instantiate(const LV2_Descriptor*     descriptor,
            double                    rate,
            const char*               bundle_path,
            const LV2_Feature* const* features)
{
  int i;
  convoLV2* clv = (convoLV2*)calloc(1, sizeof(convoLV2));
  if(!clv) { return NULL ;}

  for (i = 0; features[i]; ++i) {
    if (!strcmp(features[i]->URI, LV2_URID__map)) {
      clv->map = (LV2_URID_Map*)features[i]->data;
    } else if (!strcmp(features[i]->URI, LV2_WORKER__schedule)) {
      clv->schedule = (LV2_Worker_Schedule*)features[i]->data;
    }
  }

  if (!clv->map) {
    fprintf(stderr, "Missing feature uri:map.\n");
    free(clv);
    return NULL;
  }

  if (!clv->schedule) {
    fprintf(stderr, "Missing feature work:schedule.\n");
    free(clv);
    return NULL;
  }

  /* Map URIs and initialise forge */
  map_convolv2_uris(clv->map, &clv->uris);
  lv2_atom_forge_init(&clv->forge, clv->map);

  clv->bufsize = 1024;
  clv->rate = rate;
  clv->reinit_in_progress = 0;
  clv->clv_online = NULL;
  clv->clv_offline = NULL;

  return (LV2_Handle)clv;
}


static LV2_Worker_Status
work(LV2_Handle                  instance,
     LV2_Worker_Respond_Function respond,
     LV2_Worker_Respond_Handle   handle,
     uint32_t                    size,
     const void*                 data)
{
  convoLV2* clv = (convoLV2*)instance;
  int apply = 0;

  /* prepare new engine instance */
  if (!clv->clv_offline) {
    fprintf(stderr, "allocate offline instance\n");
    clv->clv_offline = allocConvolution();

    if (!clv->clv_offline) {
      clv->reinit_in_progress = 0;
      return LV2_WORKER_ERR_NO_SPACE; // OOM
    }
    cloneConvolutionParams(clv->clv_offline, clv->clv_online);
  }

  if (size == 0) {
    /* simple swap instances with newly created one
     * this is used to simply update buffersize
     */
    apply = 1;

  } else {
    /* handle message described in Atom */
    const LV2_Atom_Object* obj = (const LV2_Atom_Object*) data;
    ConvoLV2URIs* uris = &clv->uris;

    if (obj->body.otype == uris->irfile_load) {
    fprintf(stderr, "DEBUG LOAD\n");
#if 1
      const LV2_Atom* file_path = read_set_file(uris, obj);
      if (file_path) {
	const char *fn = (char*)(file_path+1);
	fprintf(stderr, "load %s\n", fn);
	configConvolution(clv->clv_offline, "convolution.ir.file", fn);
	apply = 1;
      }
#endif
    } else {
      fprintf(stderr, "Unknown message/object type %d\n", obj->body.otype);
    }
  }

  if (apply) {
    if (initConvolution(clv->clv_offline, clv->rate,
	  /*num in channels*/ 1,
	  /*num out channels*/ 1,
	  /*64 <= buffer-size <=4096*/ clv->bufsize));
    //respond(handle, sizeof(clv->clv_offline), &clv->clv_offline);
    respond(handle, 0, NULL);
  }
  return LV2_WORKER_SUCCESS;
}

static LV2_Worker_Status
work_response(LV2_Handle  instance,
              uint32_t    size,
              const void* data)
{
  // swap engine instances
  convoLV2* clv = (convoLV2*)instance;
  LV2convolv *old  = clv->clv_online;
  clv->clv_online  = clv->clv_offline;
  clv->clv_offline = old;

  // message to UI
  char fn[1024];
  if (queryConvolution(clv->clv_online, "convolution.ir.file", fn, 1024) > 0) {
    lv2_atom_forge_frame_time(&clv->forge, 0);
    write_set_file(&clv->forge, &clv->uris, fn);
  }
#if 0 // DEBUG
  char *cfg = dumpCfgConvolution(clv->clv_online);
  if (cfg) {
    lv2_atom_forge_frame_time(&clv->forge, 0);
    write_set_file(&clv->forge, &clv->uris, cfg);
    free(cfg);
  }
#endif

  clv->reinit_in_progress = 0;
  return LV2_WORKER_SUCCESS;
}


static void
connect_port(LV2_Handle instance,
             uint32_t   port,
             void*      data)
{
  convoLV2* clv = (convoLV2*)instance;

  switch ((PortIndex)port) {
    case P_INPUT:
      clv->input = (float*)data;
      break;
    case P_OUTPUT:
      clv->output = (float*)data;
      break;
    case P_CONTROL:
      clv->control_port = (const LV2_Atom_Sequence*)data;
      break;
    case P_NOTIFY:
      clv->notify_port = (LV2_Atom_Sequence*)data;
      break;
  }
}

static void
run(LV2_Handle instance, uint32_t n_samples)
{
  convoLV2* clv = (convoLV2*)instance;

  const float *input[MAX_OUTPUT_CHANNELS];
  float *output[MAX_OUTPUT_CHANNELS];
  // TODO -- assign channels depending on variant.
  input[0] = clv->input;
  output[0] = clv->output;

  /* Set up forge to write directly to notify output port. */
  const uint32_t notify_capacity = clv->notify_port->atom.size;
  lv2_atom_forge_set_buffer(&clv->forge,
			    (uint8_t*)clv->notify_port,
			    notify_capacity);

  /* Start a sequence in the notify output port. */
  lv2_atom_forge_sequence_head(&clv->forge, &clv->notify_frame, 0);

  /* Read incoming events */
  LV2_ATOM_SEQUENCE_FOREACH(clv->control_port, ev) {
    clv->schedule->schedule_work(clv->schedule->handle, lv2_atom_total_size(&ev->body), &ev->body);
  }

  if (clv->bufsize != n_samples) {
    // re-initialize convolver with new buffersize
    if (n_samples < 64 || n_samples > 4096 ||
	/* not power of two */ (n_samples & (n_samples - 1))
	) {
      // silence output port ?
      // TODO: notify user (once for each change)
      return;
    }
    if (!clv->reinit_in_progress) {
      clv->reinit_in_progress = 1;
      clv->bufsize = n_samples;
      clv->schedule->schedule_work(clv->schedule->handle, 0, NULL);
      //clv = (convoLV2*)instance;
    }
  }

  convolve(clv->clv_online, input, output, /*num channels*/1, n_samples);
}

static void
cleanup(LV2_Handle instance)
{
  convoLV2* clv = (convoLV2*)instance;
  freeConvolution(clv->clv_online);
  freeConvolution(clv->clv_offline);
  free(instance);
}

const void*
extension_data(const char* uri)
{
  static const LV2_Worker_Interface worker = { work, work_response, NULL };
  if (!strcmp(uri, LV2_WORKER__interface)) {
    return &worker;
  }
  return NULL;
}

static const LV2_Descriptor descriptor = {
  CONVOLV2_URI,
  instantiate,
  connect_port,
  NULL, // activate,
  run,
  NULL, // deactivate,
  cleanup,
  extension_data
};

LV2_SYMBOL_EXPORT
const LV2_Descriptor*
lv2_descriptor(uint32_t index)
{
  switch (index) {
  case 0:
    return &descriptor;
  default:
    return NULL;
  }
}
/* vi:set ts=8 sts=2 sw=2: */
