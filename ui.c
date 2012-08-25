#include <stdio.h>
#include <stdlib.h>

#include <gtk/gtk.h>

#include "lv2/lv2plug.in/ns/extensions/ui/ui.h"
#include "./uris.h"

typedef struct {
	LV2_Atom_Forge forge;

	LV2_URID_Map* map;
	ConvoLV2URIs   uris;

	LV2UI_Write_Function write;
	LV2UI_Controller     controller;

	GtkWidget* box;
	GtkWidget* btn_load;

	GtkWidget* label;
} ConvoLV2UI;

/******************************************************************************
 * GUI callbacks
 */

static void
on_load_clicked(GtkWidget* widget,
                void*      handle)
{
	ConvoLV2UI* ui = (ConvoLV2UI*)handle;

	/* Create a dialog to select a IR file. */
	GtkWidget* dialog = gtk_file_chooser_dialog_new(
		"Load IR",
		NULL,
		GTK_FILE_CHOOSER_ACTION_OPEN,
		GTK_STOCK_CANCEL, GTK_RESPONSE_CANCEL,
		GTK_STOCK_OPEN, GTK_RESPONSE_ACCEPT,
		NULL);

	/* Run the dialog, and return if it is cancelled. */
	if (gtk_dialog_run(GTK_DIALOG(dialog)) != GTK_RESPONSE_ACCEPT) {
		gtk_widget_destroy(dialog);
		return;
	}

	/* Get the file path from the dialog. */
	char* filename = gtk_file_chooser_get_filename(GTK_FILE_CHOOSER(dialog));

	/* Got what we need, destroy the dialog. */
	gtk_widget_destroy(dialog);

#define OBJ_BUF_SIZE 1024
	uint8_t obj_buf[OBJ_BUF_SIZE];
	lv2_atom_forge_set_buffer(&ui->forge, obj_buf, OBJ_BUF_SIZE);

	LV2_Atom* msg = write_set_file(&ui->forge, &ui->uris, filename);

	ui->write(ui->controller, 2, lv2_atom_total_size(msg),
	          ui->uris.atom_eventTransfer,
	          msg);

	g_free(filename);
}

/******************************************************************************
 * GUI
 */

static void clv_gui_setup(ConvoLV2UI* ui) {
	ui->box = gtk_vbox_new(FALSE, 4);

	ui->label = gtk_label_new("?");
	ui->btn_load = gtk_button_new_with_label("Load IR");

	gtk_box_pack_start(GTK_BOX(ui->box), ui->label, TRUE, TRUE, 4);
	gtk_box_pack_start(GTK_BOX(ui->box), ui->btn_load, FALSE, FALSE, 4);

	g_signal_connect(ui->btn_load, "clicked",
	                 G_CALLBACK(on_load_clicked),
	                 ui);
}

/******************************************************************************
 * LV2 callbacks
 */

static LV2UI_Handle
instantiate(const LV2UI_Descriptor*   descriptor,
            const char*               plugin_uri,
            const char*               bundle_path,
            LV2UI_Write_Function      write_function,
            LV2UI_Controller          controller,
            LV2UI_Widget*             widget,
            const LV2_Feature* const* features)
{
	ConvoLV2UI* ui = (ConvoLV2UI*)malloc(sizeof(ConvoLV2UI));
	ui->map        = NULL;
	ui->write      = write_function;
	ui->controller = controller;
	ui->box        = NULL;
	ui->btn_load   = NULL;
	ui->label      = NULL;

	*widget = NULL;

	for (int i = 0; features[i]; ++i) {
		if (!strcmp(features[i]->URI, LV2_URID_URI "#map")) {
			ui->map = (LV2_URID_Map*)features[i]->data;
		}
	}

	if (!ui->map) {
		fprintf(stderr, "UI: Host does not support urid:map\n");
		free(ui);
		return NULL;
	}

	map_convolv2_uris(ui->map, &ui->uris);

	lv2_atom_forge_init(&ui->forge, ui->map);

	clv_gui_setup(ui);

	*widget = ui->box;

	return ui;
}

static void
cleanup(LV2UI_Handle handle)
{
	ConvoLV2UI* ui = (ConvoLV2UI*)handle;
	gtk_widget_destroy(ui->btn_load);
	free(ui);
}

static void
port_event(LV2UI_Handle handle,
           uint32_t     port_index,
           uint32_t     buffer_size,
           uint32_t     format,
           const void*  buffer)
{
	ConvoLV2UI* ui = (ConvoLV2UI*)handle;
	if (format == ui->uris.atom_eventTransfer) {
		LV2_Atom* atom = (LV2_Atom*)buffer;

		if (atom->type == ui->uris.atom_Blank) {
			LV2_Atom_Object* obj      = (LV2_Atom_Object*)atom;
			const LV2_Atom*  file_uri = read_set_file(&ui->uris, obj);
			if (!file_uri) {
				fprintf(stderr, "UI: Unknown message received from UI.\n");
				return;
			}

			const char* uri = (const char*)LV2_ATOM_BODY(file_uri);
			gtk_label_set_text(GTK_LABEL(ui->label), uri);
		} else {
			fprintf(stderr, "UI: Unknown message type.\n");
		}
	} else {
		fprintf(stderr, "UI: Unknown format.\n");
	}
}

/******************************************************************************
 * LV2 setup
 */
static const void*
extension_data(const char* uri)
{
	return NULL;
}

static const LV2UI_Descriptor descriptor = {
	CONVOLV2_URI "#ui",
	instantiate,
	cleanup,
	port_event,
	extension_data
};

LV2_SYMBOL_EXPORT
const LV2UI_Descriptor*
lv2ui_descriptor(uint32_t index)
{
	switch (index) {
	case 0:
		return &descriptor;
	default:
		return NULL;
	}
}