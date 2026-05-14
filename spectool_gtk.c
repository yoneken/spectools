/* Metageek WiSPY interface 
 * Mike Kershaw/Dragorn <dragorn@kismetwireless.net>
 *
 * This code is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This code is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * Extra thanks to Ryan Woodings @ Metageek for interface documentation
 */

#include <stdio.h>
#include <usb.h>

#include <sys/types.h>
#include <sys/socket.h>
#include <signal.h>
#include <errno.h>
#include <string.h>
#include <locale.h>
#include <stdlib.h>
#include <time.h>

#include "config.h"

#include "spectool_gtk.h"
#include "spectool_container.h"
#include "spectool_gtk_planar.h"
#include "spectool_gtk_spectral.h"
#include "spectool_gtk_topo.h"
#include "spectool_gtk_channel.h"

#define GETTEXT_PACKAGE	"spectool_gtk"
#define LOCALEDIR		"/usr/share/locale/spectool_gtk"
#define SPECTOOL_CSV_MAGIC	"# spectool_csv_v1"

typedef struct _playback_sweep {
	spectool_sample_sweep *sweep;
} playback_sweep;

typedef struct _nb_aux nb_aux;

void Spectool_Alert_Dialog(char *text) {
	GtkWidget *dialog, *okbutton, *label;

	label = gtk_label_new(text);
	dialog = gtk_dialog_new_with_buttons ("SpecTool", NULL,
										  GTK_DIALOG_MODAL, NULL);
	gtk_window_set_default_size (GTK_WINDOW (dialog), 300, 100);
	okbutton = gtk_dialog_add_button (GTK_DIALOG (dialog), 
									  GTK_STOCK_OK, GTK_RESPONSE_NONE);
	g_signal_connect_swapped (GTK_OBJECT (dialog), 
							  "response", G_CALLBACK (gtk_widget_destroy), 
							  GTK_OBJECT (dialog));
	gtk_label_set_line_wrap(GTK_LABEL(label), TRUE);
	gtk_widget_grab_focus(okbutton);
	gtk_container_add(GTK_CONTAINER(GTK_DIALOG(dialog)->vbox), label);
	gtk_widget_show_all(dialog);
}

void Spectool_Help_Dialog(char *title, char *text) {
	GtkWidget *dialog, *scroll, *okbutton, *label;

	label = gtk_label_new(NULL);
	gtk_label_set_markup(GTK_LABEL(label), text);
	dialog = gtk_dialog_new_with_buttons (title, NULL,
										  GTK_DIALOG_MODAL, NULL);
	gtk_window_set_default_size (GTK_WINDOW (dialog), 300, 100);
	okbutton = gtk_dialog_add_button (GTK_DIALOG (dialog), 
									  GTK_STOCK_OK, GTK_RESPONSE_NONE);
	g_signal_connect_swapped (GTK_OBJECT (dialog), 
							  "response", G_CALLBACK (gtk_widget_destroy), 
							  GTK_OBJECT (dialog));
	gtk_label_set_line_wrap(GTK_LABEL(label), TRUE);
	gtk_widget_grab_focus(okbutton);
	gtk_container_add(GTK_CONTAINER(GTK_DIALOG(dialog)->vbox), label);
	gtk_widget_show_all(dialog);
}

typedef struct _wg_aux {
	spectool_device_registry *wdr;
	GtkWidget *main_vbox;
	GtkWidget *main_window;
	GtkWidget *notebook;
	GtkWidget *record_toggle_button;
	GtkWidget *playback_first_button;
	GtkWidget *playback_play_button;
	GtkWidget *playback_seek;
	GtkWidget *footer_label;
	GList *tabs;
	GPtrArray *playback_sweeps;
	GPtrArray *recent_sweeps;
	guint playback_timeout;
	int playback_index;
	int playback_playing;
	int playback_loaded;
	FILE *record_file;
	nb_aux *record_tab;
	int num_tabs;
} wg_aux;

struct _nb_aux {
	GtkWidget *nbvbox, *nodev_vbox;
	GtkWidget *nblabel;
	wg_aux *auxptr;

	GtkWidget *planar, *spectral, *topo, *channel;
	SpectoolWidgetController *p_con, *s_con, *t_con;

	gint pagenum;

	SpectoolChannelOpts *chanopts;

	spectool_phy *phydev;
	int wdr_slot;
	GList *wdr_menu;
};

/* fwd defs */
static nb_aux *build_nb_page(GtkWidget *notebook, wg_aux *auxptr);
static nb_aux *main_get_current_tab(wg_aux *auxptr);

static void playback_sweep_free(gpointer data) {
	playback_sweep *ps = (playback_sweep *) data;

	if (ps == NULL)
		return;

	if (ps->sweep != NULL)
		free(ps->sweep);
	free(ps);
}

static void format_sweep_time(spectool_sample_sweep *sweep, char *timestr, size_t len) {
	struct tm *tmnow;
	time_t sec;

	if (sweep == NULL || len == 0)
		return;

	sec = sweep->tm_start.tv_sec;
	tmnow = localtime(&sec);
	if (tmnow == NULL)
		return;

	strftime(timestr, len, "%Y-%m-%d %H:%M:%S", tmnow);
}

static void update_footer_display(wg_aux *auxptr) {
	char timestr[64] = "";

	g_return_if_fail(auxptr != NULL);

	if (auxptr->playback_loaded && auxptr->playback_sweeps != NULL &&
		auxptr->playback_index >= 0 &&
		auxptr->playback_index < (int) auxptr->playback_sweeps->len) {
		playback_sweep *ps =
			(playback_sweep *) g_ptr_array_index(auxptr->playback_sweeps,
												 auxptr->playback_index);
		format_sweep_time(ps->sweep, timestr, sizeof(timestr));
	} else {
		time_t now;
		struct tm *tmnow;

		now = time(NULL);
		tmnow = localtime(&now);
		if (tmnow != NULL)
			strftime(timestr, sizeof(timestr), "%Y-%m-%d %H:%M:%S", tmnow);
	}

	if (timestr[0] != '\0')
		gtk_label_set_text(GTK_LABEL(auxptr->footer_label), timestr);
}

static gboolean update_footer_time(gpointer data) {
	wg_aux *auxptr = (wg_aux *) data;

	update_footer_display(auxptr);

	return TRUE;
}

static void csv_write_sweep(FILE *file, spectool_sample_sweep *sweep) {
	unsigned int x;

	if (file == NULL || sweep == NULL)
		return;

	fprintf(file, "%ld,%ld,%ld,%ld,%u,%u,%u,%d,%d,%u,%u,%u",
			(long) sweep->tm_start.tv_sec,
			(long) sweep->tm_start.tv_usec,
			(long) sweep->tm_end.tv_sec,
			(long) sweep->tm_end.tv_usec,
			sweep->start_khz,
			sweep->end_khz,
			sweep->res_hz,
			sweep->amp_offset_mdbm,
			sweep->amp_res_mdbm,
			sweep->rssi_max,
			sweep->min_rssi_seen,
			sweep->num_samples);

	for (x = 0; x < sweep->num_samples; x++)
		fprintf(file, ",%u", sweep->sample_data[x]);

	fprintf(file, "\n");
	fflush(file);
}

static void csv_write_header(FILE *file) {
	if (file == NULL)
		return;

	fprintf(file, "%s\n", SPECTOOL_CSV_MAGIC);
	fprintf(file,
			"# start_sec,start_usec,end_sec,end_usec,start_khz,end_khz,res_hz,"
			"amp_offset_mdbm,amp_res_mdbm,rssi_max,min_rssi_seen,num_samples,samples...\n");
	fflush(file);
}

static double sweep_start_seconds(spectool_sample_sweep *sweep) {
	if (sweep == NULL)
		return 0;

	return (double) sweep->tm_start.tv_sec + ((double) sweep->tm_start.tv_usec / 1000000.0);
}

static void recent_sweeps_trim(wg_aux *auxptr, double newest_time) {
	double cutoff = newest_time - 4.0;

	if (auxptr == NULL || auxptr->recent_sweeps == NULL)
		return;

	while (auxptr->recent_sweeps->len > 0) {
		playback_sweep *ps =
			(playback_sweep *) g_ptr_array_index(auxptr->recent_sweeps, 0);

		if (ps == NULL || ps->sweep == NULL ||
			sweep_start_seconds(ps->sweep) >= cutoff)
			break;

		g_ptr_array_remove_index(auxptr->recent_sweeps, 0);
	}
}

static void recent_sweeps_add(wg_aux *auxptr, spectool_sample_sweep *sweep) {
	playback_sweep *ps;

	if (auxptr == NULL || auxptr->recent_sweeps == NULL || sweep == NULL)
		return;

	ps = (playback_sweep *) malloc(sizeof(playback_sweep));
	if (ps == NULL)
		return;

	ps->sweep = (spectool_sample_sweep *) malloc(SPECTOOL_SWEEP_SIZE(sweep->num_samples));
	if (ps->sweep == NULL) {
		free(ps);
		return;
	}

	memcpy(ps->sweep, sweep, SPECTOOL_SWEEP_SIZE(sweep->num_samples));
	g_ptr_array_add(auxptr->recent_sweeps, ps);
	recent_sweeps_trim(auxptr, sweep_start_seconds(sweep));
}

static void main_recent_sweep_cb(int slot, int mode, spectool_sample_sweep *sweep,
								 void *aux) {
	nb_aux *nbaux = (nb_aux *) aux;

	if (nbaux == NULL)
		return;

	recent_sweeps_add(nbaux->auxptr, sweep);
}

static void main_record_sweep_cb(int slot, int mode, spectool_sample_sweep *sweep,
								 void *aux) {
	wg_aux *auxptr = (wg_aux *) aux;

	if (auxptr == NULL || auxptr->record_file == NULL || sweep == NULL)
		return;

	csv_write_sweep(auxptr->record_file, sweep);
}

static void main_devopen(int slot, void *aux) {
	nb_aux *nbaux = (nb_aux *) aux;

	g_return_if_fail(aux != NULL);

	if (nbaux->wdr_slot >= 0)
		wdr_del_sweepcb(nbaux->auxptr->wdr, nbaux->wdr_slot,
						main_recent_sweep_cb, nbaux);

	spectool_widget_bind_dev(nbaux->planar, nbaux->auxptr->wdr, slot);
	spectool_widget_bind_dev(nbaux->topo, nbaux->auxptr->wdr, slot);
	spectool_widget_bind_dev(nbaux->spectral, nbaux->auxptr->wdr, slot);
	spectool_widget_bind_dev(nbaux->channel, nbaux->auxptr->wdr, slot);

	nbaux->phydev = wdr_get_phy(nbaux->auxptr->wdr, slot);
	nbaux->wdr_slot = slot;
	wdr_add_sweepcb(nbaux->auxptr->wdr, nbaux->wdr_slot,
					main_recent_sweep_cb, 0, nbaux);

	gtk_label_set_text(GTK_LABEL(nbaux->nblabel), spectool_phy_getname(nbaux->phydev));

	gtk_widget_hide(nbaux->nodev_vbox);

	gtk_widget_show(nbaux->planar);
	gtk_widget_show(nbaux->topo);
	gtk_widget_show(nbaux->spectral);
	gtk_widget_show(nbaux->channel);
	gtk_widget_show(nbaux->p_con->evbox);
	gtk_widget_show(nbaux->t_con->evbox);
	gtk_widget_show(nbaux->s_con->evbox);
}

/* Picks an existing device from the dynamic WDR list in the 
 * popup menu */
static void main_menu_devpicker(gpointer *aux) {
	wdr_menu_rec *r;
	nb_aux *nbaux;

	g_return_if_fail(aux != NULL);

	/* We're opening a known device, call it directly */
	r = (wdr_menu_rec *) aux;
	nbaux = (nb_aux *) r->aux;

	if (r->slot < 0) {
		wdr_devpicker_spawn(r->wdr, main_devopen, nbaux);
		return;
	}

	main_devopen(r->slot, nbaux);
}

/* Spawns the device picker window from the popup menu */
static void main_menu_spawnpicker(gpointer *aux) {
	nb_aux *nbaux = (nb_aux *) aux;

	g_return_if_fail(aux != NULL);

	wdr_devpicker_spawn(nbaux->auxptr->wdr, main_devopen, nbaux);
}

/* Spawns the network picker window from the popup menu */
static void main_menu_spawnnetpicker(gpointer *aux) {
	nb_aux *nbaux = (nb_aux *) aux;

	g_return_if_fail(aux != NULL);

	wdr_netmanager_spawn(nbaux->auxptr->wdr,
						 main_devopen, nbaux);
}

/* Called when the popup button menu goes away, responsible for freeing the
 * dynamic wdr menu items (which in turn decrements the use counter so we can
 * release devices if we need to) */
static void main_menu_destroy(gpointer *aux) {
	nb_aux *nbaux = (nb_aux *) aux;

	g_return_if_fail(aux != NULL);

	wdr_free_menu(nbaux->auxptr->wdr, nbaux->wdr_menu);
	nbaux->wdr_menu = NULL;
}

static gboolean main_nodev_menu_button_press(gpointer *aux,
											 GdkEvent *event) {
	GtkWidget *menu;
	nb_aux *nbaux = (nb_aux *) aux;

	g_return_val_if_fail(aux != NULL, FALSE);

	if (event->type == GDK_BUTTON_PRESS) {
		GdkEventButton *bevent = (GdkEventButton *) event;

		menu = gtk_menu_new();
		gtk_widget_show(menu);

		/* Add the WDR generated menus, if any */
		nbaux->wdr_menu = 
			wdr_populate_menu(nbaux->auxptr->wdr,
							  GTK_WIDGET(menu),
							  0, 1,
							  G_CALLBACK(main_menu_devpicker),
							  nbaux);

		/* Set the cleanup function for the dynamic menu generation */
		g_signal_connect_swapped(G_OBJECT(menu), "selection-done",
								 G_CALLBACK(main_menu_destroy),
								 nbaux);

		gtk_menu_popup(GTK_MENU(menu), NULL, NULL, NULL, NULL, 
					   bevent->button, bevent->time);

		return TRUE;
	}

	return FALSE;
}

static void del_tab(nb_aux *page) {
	wg_aux *auxptr = page->auxptr;

	if (auxptr->record_tab == page) {
		if (page->wdr_slot >= 0)
			wdr_del_sweepcb(auxptr->wdr, page->wdr_slot, main_record_sweep_cb, auxptr);
		if (auxptr->record_file != NULL)
			fclose(auxptr->record_file);
		auxptr->record_file = NULL;
		auxptr->record_tab = NULL;
	}
	if (page->wdr_slot >= 0)
		wdr_del_sweepcb(auxptr->wdr, page->wdr_slot, main_recent_sweep_cb, page);

	/* Tear down all the graphs */
	free(page->p_con);
	free(page->s_con);
	free(page->t_con);

	gtk_widget_destroy(page->planar);
	gtk_widget_destroy(page->spectral);
	gtk_widget_destroy(page->topo);
	gtk_widget_destroy(page->channel);

	/* delete the page */
	gtk_notebook_remove_page(GTK_NOTEBOOK(auxptr->notebook), page->pagenum);
	auxptr->tabs = g_list_remove(auxptr->tabs, page);

	/* Delete the vbox, should cascade down the rest */
	gtk_widget_destroy(page->nbvbox);

	if (--auxptr->num_tabs == 0) {
		build_nb_page(auxptr->notebook, auxptr);
	}

	free(page);
}

static void close_nb_button(GtkWidget *widget, gpointer *aux) {
	nb_aux *nbaux = (nb_aux *) aux;

	del_tab(nbaux);
}

static nb_aux *build_nb_page(GtkWidget *notebook, wg_aux *auxptr) {
	nb_aux *nbaux = (nb_aux *) malloc(sizeof(nb_aux));
	GtkWidget *temp, *hbox, *arrow, *closebutton, *closeicon;

	nbaux->auxptr = auxptr;
	nbaux->wdr_slot = -1;
	nbaux->phydev = NULL;
	nbaux->wdr_menu = NULL;

	/* Default label for the tab, packed into a hbox */
	hbox = gtk_hbox_new(FALSE, 1);
	gtk_widget_show(hbox);
	nbaux->nblabel = gtk_label_new("No device");
	gtk_box_pack_start(GTK_BOX(hbox), nbaux->nblabel, FALSE, FALSE, 0);

	closebutton = gtk_button_new();
	closeicon = gtk_image_new_from_stock(GTK_STOCK_CLOSE, GTK_ICON_SIZE_MENU);
	gtk_container_add(GTK_CONTAINER(closebutton), closeicon);
	gtk_button_set_relief(GTK_BUTTON(closebutton), GTK_RELIEF_NONE);
	gtk_widget_show(closebutton);
	gtk_widget_show(closeicon);
	gtk_box_pack_end(GTK_BOX(hbox), closebutton, FALSE, FALSE, 0);

	gtk_signal_connect(GTK_OBJECT(closebutton), "clicked",
					   GTK_SIGNAL_FUNC(close_nb_button), nbaux);

	nbaux->nbvbox = gtk_vbox_new(FALSE, 1);
	nbaux->pagenum = gtk_notebook_append_page(GTK_NOTEBOOK(notebook), nbaux->nbvbox, 
											  hbox);

	/* Make the device picker buttons and label */
	nbaux->nodev_vbox = gtk_vbox_new(FALSE, 0);
	temp = gtk_label_new("No device selected...");
	gtk_box_pack_start(GTK_BOX(nbaux->nodev_vbox), temp, FALSE, FALSE, 4);
	gtk_widget_show(temp);

	/* Build the arrow for using an open device */
	hbox = gtk_hbox_new(FALSE, 0);
	gtk_box_pack_start(GTK_BOX(nbaux->nodev_vbox), hbox, FALSE, FALSE, 2);

	temp = gtk_button_new_with_label("Open Device");
	g_signal_connect_swapped(G_OBJECT(temp), "clicked",
							 G_CALLBACK(main_menu_spawnpicker), 
							 nbaux);
	gtk_box_pack_start(GTK_BOX(hbox), temp, TRUE, TRUE, 0);
	gtk_widget_show(temp);

	temp = gtk_button_new();
	arrow = gtk_arrow_new(GTK_ARROW_DOWN, GTK_SHADOW_OUT);
	gtk_container_add(GTK_CONTAINER(temp), arrow);
	g_signal_connect_swapped(G_OBJECT(temp), "event", 
							 G_CALLBACK(main_nodev_menu_button_press),
							 nbaux);
	gtk_box_pack_start(GTK_BOX(hbox), temp, FALSE, FALSE, 0);
	gtk_widget_show(temp);
	gtk_widget_show(hbox);
	gtk_widget_show(arrow);

	temp = gtk_button_new_with_label("Open Network Device");
	g_signal_connect_swapped(G_OBJECT(temp), "clicked",
							 G_CALLBACK(main_menu_spawnnetpicker), 
							 nbaux);
	gtk_box_pack_start(GTK_BOX(nbaux->nodev_vbox), temp, FALSE, FALSE, 2);
	gtk_widget_show(temp);

	/*
	temp = gtk_button_new_with_label("Close Tab");
	g_signal_connect_swapped(G_OBJECT(temp), "clicked",
							 G_CALLBACK(gtk_widget_destroy), G_OBJECT());
	gtk_box_pack_start(GTK_BOX(nbaux->nodev_vbox), temp, FALSE, FALSE, 2);
	gtk_widget_show(temp);
	*/

	gtk_box_pack_start(GTK_BOX(nbaux->nbvbox), nbaux->nodev_vbox, FALSE, FALSE, 0);

	gtk_widget_show(nbaux->nodev_vbox);

	gtk_widget_show(nbaux->nblabel);

	/* Make the inactive devices */
	nbaux->chanopts = (SpectoolChannelOpts *) malloc(sizeof(SpectoolChannelOpts));
	spectoolchannelopts_init(nbaux->chanopts);

	nbaux->channel = spectool_channel_new();
	spectool_widget_link_channel(nbaux->channel, nbaux->chanopts);
	gtk_box_pack_end(GTK_BOX(nbaux->nbvbox), nbaux->channel, FALSE, FALSE, 0);

	nbaux->planar = spectool_planar_new();
	spectool_widget_link_channel(nbaux->planar, nbaux->chanopts);
	nbaux->p_con = spectool_widget_buildcontroller(GTK_WIDGET(nbaux->planar));
	gtk_box_pack_end(GTK_BOX(nbaux->nbvbox), nbaux->planar, TRUE, TRUE, 0);
	gtk_box_pack_end(GTK_BOX(nbaux->nbvbox), nbaux->p_con->evbox, FALSE, FALSE, 2);

	nbaux->topo = spectool_topo_new();
	spectool_widget_link_channel(nbaux->topo, nbaux->chanopts);
	nbaux->t_con = spectool_widget_buildcontroller(GTK_WIDGET(nbaux->topo));
	gtk_box_pack_end(GTK_BOX(nbaux->nbvbox), nbaux->topo, TRUE, TRUE, 0);
	gtk_box_pack_end(GTK_BOX(nbaux->nbvbox), nbaux->t_con->evbox, FALSE, FALSE, 2);

	nbaux->spectral = spectool_spectral_new();
	spectool_widget_link_channel(nbaux->spectral, nbaux->chanopts);
	nbaux->s_con = spectool_widget_buildcontroller(GTK_WIDGET(nbaux->spectral));
	gtk_box_pack_end(GTK_BOX(nbaux->nbvbox), nbaux->spectral, TRUE, TRUE, 0);
	gtk_box_pack_end(GTK_BOX(nbaux->nbvbox), nbaux->s_con->evbox, FALSE, FALSE, 2);

	spectool_channel_append_update(nbaux->channel, nbaux->planar);
	spectool_channel_append_update(nbaux->channel, nbaux->topo);
	spectool_channel_append_update(nbaux->channel, nbaux->spectral);

	gtk_widget_show(nbaux->nbvbox);

	auxptr->tabs = g_list_append(auxptr->tabs, nbaux);
	auxptr->num_tabs++;

	return nbaux;
}

static void add_tab(gpointer *data, gpointer *aux) {
	wg_aux *auxptr = (wg_aux *) data;
	nb_aux *nbaux;

	nbaux = build_nb_page(auxptr->notebook, auxptr);
}

static nb_aux *main_get_current_tab(wg_aux *auxptr) {
	GtkWidget *page;
	GList *tab;
	int pagenum;

	g_return_val_if_fail(auxptr != NULL, NULL);

	pagenum = gtk_notebook_get_current_page(GTK_NOTEBOOK(auxptr->notebook));
	if (pagenum < 0)
		return NULL;

	page = gtk_notebook_get_nth_page(GTK_NOTEBOOK(auxptr->notebook), pagenum);
	for (tab = auxptr->tabs; tab != NULL; tab = g_list_next(tab)) {
		nb_aux *nbaux = (nb_aux *) tab->data;
		if (nbaux->nbvbox == page)
			return nbaux;
	}

	return NULL;
}

static void main_menu_reset_planar_peak(gpointer *data, gpointer *aux) {
	wg_aux *auxptr = (wg_aux *) data;
	nb_aux *nbaux;
	SpectoolWidget *wwidget;
	spectool_sweep_cache *cache;

	nbaux = main_get_current_tab(auxptr);
	if (nbaux == NULL)
		return;

	wwidget = SPECTOOL_WIDGET(nbaux->planar);
	cache = wwidget->sweepcache;

	if (cache == NULL || cache->latest == NULL)
		return;

	if (cache->peak != NULL)
		free(cache->peak);
	if (cache->roll_peak != NULL)
		free(cache->roll_peak);

	cache->peak = (spectool_sample_sweep *) malloc(SPECTOOL_SWEEP_SIZE(cache->latest->num_samples));
	cache->roll_peak = (spectool_sample_sweep *) malloc(SPECTOOL_SWEEP_SIZE(cache->latest->num_samples));

	if (cache->peak == NULL || cache->roll_peak == NULL) {
		if (cache->peak != NULL)
			free(cache->peak);
		if (cache->roll_peak != NULL)
			free(cache->roll_peak);
		cache->peak = NULL;
		cache->roll_peak = NULL;
		Spectool_Alert_Dialog("Unable to reset planar peak: out of memory.");
		return;
	}

	memcpy(cache->peak, cache->latest, SPECTOOL_SWEEP_SIZE(cache->latest->num_samples));
	memcpy(cache->roll_peak, cache->latest, SPECTOOL_SWEEP_SIZE(cache->latest->num_samples));

	spectool_widget_graphics_update(wwidget);
	spectool_widget_update(GTK_WIDGET(wwidget));
}

static void capture_recent_csv(wg_aux *auxptr, const char *filename) {
	FILE *file;
	unsigned int x;

	if (auxptr == NULL || filename == NULL)
		return;

	file = fopen(filename, "w");
	if (file == NULL) {
		char errstr[SPECTOOL_ERROR_MAX];
		snprintf(errstr, sizeof(errstr), "Unable to save recent CSV: %s",
				 strerror(errno));
		Spectool_Alert_Dialog(errstr);
		return;
	}

	csv_write_header(file);
	if (auxptr->recent_sweeps != NULL) {
		for (x = 0; x < auxptr->recent_sweeps->len; x++) {
			playback_sweep *ps =
				(playback_sweep *) g_ptr_array_index(auxptr->recent_sweeps, x);
			if (ps != NULL)
				csv_write_sweep(file, ps->sweep);
		}
	}

	fclose(file);
}

static void main_menu_capture_window(gpointer *data, gpointer *aux) {
	wg_aux *auxptr = (wg_aux *) data;
	GdkWindow *window;
	GdkColormap *colormap;
	GdkPixbuf *pixbuf;
	GError *error = NULL;
	time_t now;
	struct tm *tmnow;
	char filename[128];
	char csvfilename[128];
	int width, height;

	g_return_if_fail(auxptr != NULL);

	window = auxptr->main_window->window;
	if (window == NULL)
		return;

	now = time(NULL);
	tmnow = localtime(&now);
	if (tmnow == NULL)
		return;

	if (strftime(filename, sizeof(filename), "spectool_%Y%m%d_%H%M%S.png", tmnow) == 0)
		return;
	if (strftime(csvfilename, sizeof(csvfilename), "spectool_%Y%m%d_%H%M%S.csv", tmnow) == 0)
		return;

	gdk_drawable_get_size(GDK_DRAWABLE(window), &width, &height);
	colormap = gdk_drawable_get_colormap(GDK_DRAWABLE(window));
	pixbuf = gdk_pixbuf_get_from_drawable(NULL, GDK_DRAWABLE(window), colormap,
										  0, 0, 0, 0, width, height);
	if (pixbuf == NULL) {
		Spectool_Alert_Dialog("Unable to capture the application window.");
		return;
	}

	if (!gdk_pixbuf_save(pixbuf, filename, "png", &error, NULL)) {
		char errstr[SPECTOOL_ERROR_MAX];
		snprintf(errstr, sizeof(errstr), "Unable to save screenshot: %s",
				 error != NULL ? error->message : "unknown error");
		Spectool_Alert_Dialog(errstr);
		if (error != NULL)
			g_error_free(error);
		g_object_unref(pixbuf);
		return;
	}

	g_object_unref(pixbuf);
	capture_recent_csv(auxptr, csvfilename);
}

static void main_menu_start_record(gpointer *data, gpointer *aux) {
	wg_aux *auxptr = (wg_aux *) data;
	nb_aux *nbaux;
	time_t now;
	struct tm *tmnow;
	char filename[128];

	nbaux = main_get_current_tab(auxptr);
	if (nbaux == NULL || nbaux->wdr_slot < 0) {
		Spectool_Alert_Dialog("Open a device before starting CSV recording.");
		return;
	}

	if (auxptr->record_file != NULL) {
		Spectool_Alert_Dialog("CSV recording is already running.");
		return;
	}

	now = time(NULL);
	tmnow = localtime(&now);
	if (tmnow == NULL ||
		strftime(filename, sizeof(filename), "spectool_%Y%m%d_%H%M%S.csv", tmnow) == 0) {
		Spectool_Alert_Dialog("Unable to create a CSV filename.");
		return;
	}

	auxptr->record_file = fopen(filename, "w");
	if (auxptr->record_file == NULL) {
		char errstr[SPECTOOL_ERROR_MAX];
		snprintf(errstr, sizeof(errstr), "Unable to start CSV recording: %s",
				 strerror(errno));
		Spectool_Alert_Dialog(errstr);
		return;
	}

	csv_write_header(auxptr->record_file);

	auxptr->record_tab = nbaux;
	wdr_add_sweepcb(auxptr->wdr, nbaux->wdr_slot, main_record_sweep_cb, 0, auxptr);
	if (auxptr->record_toggle_button != NULL)
		gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(auxptr->record_toggle_button), TRUE);
}

static void main_menu_stop_record(gpointer *data, gpointer *aux) {
	wg_aux *auxptr = (wg_aux *) data;

	if (auxptr->record_file == NULL)
		return;

	if (auxptr->record_tab != NULL && auxptr->record_tab->wdr_slot >= 0) {
		wdr_del_sweepcb(auxptr->wdr, auxptr->record_tab->wdr_slot,
						main_record_sweep_cb, auxptr);
	}

	fclose(auxptr->record_file);
	auxptr->record_file = NULL;
	auxptr->record_tab = NULL;
	if (auxptr->record_toggle_button != NULL)
		gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(auxptr->record_toggle_button), FALSE);
}

static void record_toggle_button(GtkWidget *widget, gpointer data) {
	wg_aux *auxptr = (wg_aux *) data;
	gboolean active;

	active = gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(widget));

	if (active && auxptr->record_file == NULL) {
		main_menu_start_record((gpointer *) auxptr, NULL);
	} else if (!active && auxptr->record_file != NULL) {
		main_menu_stop_record((gpointer *) auxptr, NULL);
	}

	if ((auxptr->record_file != NULL) !=
		gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(widget))) {
		gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(widget),
									 auxptr->record_file != NULL);
	}
}

static void main_stop_tab_live_device(wg_aux *auxptr, nb_aux *nbaux) {
	if (nbaux == NULL || nbaux->wdr_slot < 0)
		return;

	if (auxptr->record_tab == nbaux)
		main_menu_stop_record((gpointer *) auxptr, NULL);

	wdr_del_sweepcb(auxptr->wdr, nbaux->wdr_slot, main_recent_sweep_cb, nbaux);

	spectool_widget_unbind_dev(nbaux->planar);
	spectool_widget_unbind_dev(nbaux->topo);
	spectool_widget_unbind_dev(nbaux->spectral);
	spectool_widget_unbind_dev(nbaux->channel);

	nbaux->wdr_slot = -1;
	nbaux->phydev = NULL;
}

static spectool_sample_sweep *csv_parse_sweep_line(char *line) {
	char *tok;
	char *saveptr = NULL;
	long vals[12];
	unsigned int x;
	spectool_sample_sweep *sweep;

	for (x = 0; x < 12; x++) {
		tok = strtok_r(x == 0 ? line : NULL, ",\r\n", &saveptr);
		if (tok == NULL)
			return NULL;
		vals[x] = strtol(tok, NULL, 10);
	}

	if (vals[11] <= 0 || vals[11] > 4096)
		return NULL;

	sweep = (spectool_sample_sweep *) malloc(SPECTOOL_SWEEP_SIZE(vals[11]));
	if (sweep == NULL)
		return NULL;

	memset(sweep, 0, SPECTOOL_SWEEP_SIZE(vals[11]));
	sweep->tm_start.tv_sec = vals[0];
	sweep->tm_start.tv_usec = vals[1];
	sweep->tm_end.tv_sec = vals[2];
	sweep->tm_end.tv_usec = vals[3];
	sweep->start_khz = vals[4];
	sweep->end_khz = vals[5];
	sweep->res_hz = vals[6];
	sweep->amp_offset_mdbm = vals[7];
	sweep->amp_res_mdbm = vals[8];
	sweep->rssi_max = vals[9];
	sweep->min_rssi_seen = vals[10];
	sweep->num_samples = vals[11];

	for (x = 0; x < sweep->num_samples; x++) {
		long sample;

		tok = strtok_r(NULL, ",\r\n", &saveptr);
		if (tok == NULL) {
			free(sweep);
			return NULL;
		}

		sample = strtol(tok, NULL, 10);
		if (sample < 0)
			sample = 0;
		if (sample > 255)
			sample = 255;
		sweep->sample_data[x] = sample;
	}

	return sweep;
}

static void playback_clear(wg_aux *auxptr) {
	if (auxptr->playback_timeout > 0) {
		g_source_remove(auxptr->playback_timeout);
		auxptr->playback_timeout = 0;
	}

	auxptr->playback_playing = 0;
	auxptr->playback_loaded = 0;
	auxptr->playback_index = 0;

	if (auxptr->playback_sweeps != NULL)
		g_ptr_array_set_size(auxptr->playback_sweeps, 0);
	if (auxptr->recent_sweeps != NULL)
		g_ptr_array_set_size(auxptr->recent_sweeps, 0);

	gtk_button_set_label(GTK_BUTTON(auxptr->playback_play_button), "Play");
	gtk_range_set_range(GTK_RANGE(auxptr->playback_seek), 0, 1);
	gtk_range_set_value(GTK_RANGE(auxptr->playback_seek), 0);
}

static void playback_feed_index(wg_aux *auxptr, int index) {
	nb_aux *nbaux;
	playback_sweep *ps;

	if (!auxptr->playback_loaded || auxptr->playback_sweeps == NULL ||
		index < 0 || index >= (int) auxptr->playback_sweeps->len)
		return;

	nbaux = main_get_current_tab(auxptr);
	if (nbaux == NULL)
		return;

	gtk_widget_hide(nbaux->nodev_vbox);
	gtk_widget_show(nbaux->planar);
	gtk_widget_show(nbaux->topo);
	gtk_widget_show(nbaux->spectral);
	gtk_widget_show(nbaux->channel);
	gtk_widget_show(nbaux->p_con->evbox);
	gtk_widget_show(nbaux->t_con->evbox);
	gtk_widget_show(nbaux->s_con->evbox);
	gtk_label_set_text(GTK_LABEL(nbaux->nblabel), "CSV playback");

	ps = (playback_sweep *) g_ptr_array_index(auxptr->playback_sweeps, index);
	auxptr->playback_index = index;
	recent_sweeps_add(auxptr, ps->sweep);

	if (index == 0) {
		spectool_widget_feed_sweep(nbaux->planar, SPECTOOL_POLL_CONFIGURED, ps->sweep);
		spectool_widget_feed_sweep(nbaux->topo, SPECTOOL_POLL_CONFIGURED, ps->sweep);
		spectool_widget_feed_sweep(nbaux->spectral, SPECTOOL_POLL_CONFIGURED, ps->sweep);
		spectool_widget_feed_sweep(nbaux->channel, SPECTOOL_POLL_CONFIGURED, ps->sweep);
	}

	spectool_widget_feed_sweep(nbaux->planar, SPECTOOL_POLL_SWEEPCOMPLETE, ps->sweep);
	spectool_widget_feed_sweep(nbaux->topo, SPECTOOL_POLL_SWEEPCOMPLETE, ps->sweep);
	spectool_widget_feed_sweep(nbaux->spectral, SPECTOOL_POLL_SWEEPCOMPLETE, ps->sweep);
	spectool_widget_feed_sweep(nbaux->channel, SPECTOOL_POLL_SWEEPCOMPLETE, ps->sweep);

	spectool_widget_graphics_update(SPECTOOL_WIDGET(nbaux->planar));
	spectool_widget_graphics_update(SPECTOOL_WIDGET(nbaux->topo));
	spectool_widget_graphics_update(SPECTOOL_WIDGET(nbaux->spectral));
	spectool_widget_graphics_update(SPECTOOL_WIDGET(nbaux->channel));
	spectool_widget_update(nbaux->planar);
	spectool_widget_update(nbaux->topo);
	spectool_widget_update(nbaux->spectral);
	spectool_widget_update(nbaux->channel);

	gtk_range_set_value(GTK_RANGE(auxptr->playback_seek), index);
	update_footer_display(auxptr);
}

static gboolean playback_tick(gpointer data) {
	wg_aux *auxptr = (wg_aux *) data;
	int next;

	if (!auxptr->playback_playing || !auxptr->playback_loaded)
		return FALSE;

	next = auxptr->playback_index + 1;
	if (next >= (int) auxptr->playback_sweeps->len) {
		auxptr->playback_playing = 0;
		auxptr->playback_timeout = 0;
		gtk_button_set_label(GTK_BUTTON(auxptr->playback_play_button), "Play");
		return FALSE;
	}

	playback_feed_index(auxptr, next);

	return TRUE;
}

static void playback_first_button(GtkWidget *widget, gpointer data) {
	wg_aux *auxptr = (wg_aux *) data;

	if (!auxptr->playback_loaded)
		return;

	playback_feed_index(auxptr, 0);
}

static void playback_play_button(GtkWidget *widget, gpointer data) {
	wg_aux *auxptr = (wg_aux *) data;

	if (!auxptr->playback_loaded)
		return;

	if (auxptr->playback_playing) {
		if (auxptr->playback_timeout > 0)
			g_source_remove(auxptr->playback_timeout);
		auxptr->playback_timeout = 0;
		auxptr->playback_playing = 0;
		gtk_button_set_label(GTK_BUTTON(auxptr->playback_play_button), "Play");
		return;
	}

	auxptr->playback_playing = 1;
	gtk_button_set_label(GTK_BUTTON(auxptr->playback_play_button), "Pause");
	auxptr->playback_timeout = g_timeout_add(250, playback_tick, auxptr);
}

static void playback_seek_changed(GtkRange *range, gpointer data) {
	wg_aux *auxptr = (wg_aux *) data;
	int index;

	if (!auxptr->playback_loaded)
		return;

	index = (int) gtk_range_get_value(range);
	if (index != auxptr->playback_index)
		playback_feed_index(auxptr, index);
}

static void main_menu_load_csv(gpointer *data, gpointer *aux) {
	wg_aux *auxptr = (wg_aux *) data;
	nb_aux *nbaux;
	GtkWidget *dialog;
	FILE *file;
	char *filename;
	char *line = NULL;
	size_t linecap = 0;
	ssize_t linelen;
	int loaded = 0;

	dialog = gtk_file_chooser_dialog_new("Load CSV", GTK_WINDOW(auxptr->main_window),
										 GTK_FILE_CHOOSER_ACTION_OPEN,
										 GTK_STOCK_CANCEL, GTK_RESPONSE_CANCEL,
										 GTK_STOCK_OPEN, GTK_RESPONSE_ACCEPT,
										 NULL);
	gtk_window_set_transient_for(GTK_WINDOW(dialog), GTK_WINDOW(auxptr->main_window));
	gtk_window_set_modal(GTK_WINDOW(dialog), TRUE);
	gtk_window_set_position(GTK_WINDOW(dialog), GTK_WIN_POS_CENTER_ON_PARENT);
	gtk_window_present(GTK_WINDOW(dialog));

	if (gtk_dialog_run(GTK_DIALOG(dialog)) != GTK_RESPONSE_ACCEPT) {
		gtk_widget_destroy(dialog);
		return;
	}

	filename = gtk_file_chooser_get_filename(GTK_FILE_CHOOSER(dialog));
	gtk_widget_destroy(dialog);

	file = fopen(filename, "r");
	g_free(filename);

	if (file == NULL) {
		Spectool_Alert_Dialog("Unable to open CSV file.");
		return;
	}

	playback_clear(auxptr);

	while ((linelen = getline(&line, &linecap, file)) >= 0) {
		spectool_sample_sweep *sweep;
		playback_sweep *ps;

		if (linelen == 0 || line[0] == '#')
			continue;

		sweep = csv_parse_sweep_line(line);
		if (sweep == NULL)
			continue;

		ps = (playback_sweep *) malloc(sizeof(playback_sweep));
		if (ps == NULL) {
			free(sweep);
			continue;
		}

		ps->sweep = sweep;
		g_ptr_array_add(auxptr->playback_sweeps, ps);
		loaded++;
	}

	if (line != NULL)
		free(line);
	fclose(file);

	if (loaded == 0) {
		Spectool_Alert_Dialog("No sweep rows were loaded from the CSV file.");
		return;
	}

	nbaux = main_get_current_tab(auxptr);
	main_stop_tab_live_device(auxptr, nbaux);

	auxptr->playback_loaded = 1;
	auxptr->playback_index = 0;
	gtk_range_set_range(GTK_RANGE(auxptr->playback_seek), 0, loaded > 1 ? loaded - 1 : 1);
	gtk_range_set_increments(GTK_RANGE(auxptr->playback_seek), 1, 10);
	playback_feed_index(auxptr, 0);
}

static GtkItemFactoryEntry main_menu_items[] = {
	{ "/_SpecAn",			NULL,			NULL,	0, "<Branch>" },
	{ "/SpecAn/Add Device", "<control>A", add_tab, 0, "<Item>" },
	{ "/SpecAn/Start CSV Recording", NULL, main_menu_start_record, 0, "<Item>" },
	{ "/SpecAn/Stop CSV Recording", NULL, main_menu_stop_record, 0, "<Item>" },
	{ "/SpecAn/Load CSV", NULL, main_menu_load_csv, 0, "<Item>" },
	{ "/SpecAn/Reset Planar Peak", "r", main_menu_reset_planar_peak, 0, "<Item>" },
	{ "/SpecAn/Capture Window", "space", main_menu_capture_window, 0, "<Item>" },
	{ "/SpecAn/_Quit",		"<control>Q",	gtk_main_quit, 0, "<Item>" },
};

static gint nmain_menu_items = 
	sizeof (main_menu_items) / sizeof (main_menu_items[0]);

int main(int argc, char *argv[]) {
	GtkWidget *window, *vbox, *notebook, *footer_hbox, *playback_hbox;
	wg_aux auxptr;
	nb_aux *nbfirst;

#if 0
	GtkWidget *menubar, *menu, *mn_spectool, *menuitem;
#endif

	GtkWidget *menubar;

	GtkItemFactory *item_factory;
	GtkAccelGroup *accel_group;

	spectool_device_registry wdr;

	int x;

	char errstr[SPECTOOL_ERROR_MAX];

	setlocale(LC_ALL, "");
	bindtextdomain(GETTEXT_PACKAGE, LOCALEDIR);
	bind_textdomain_codeset(GETTEXT_PACKAGE, "UTF-8");
	textdomain(GETTEXT_PACKAGE);

	gtk_init(&argc, &argv);

	wdr_init(&wdr);

	/* Turn on broadcast autodetection */
	if (wdr_enable_bcast(&wdr, errstr) < 0) {
		Spectool_Alert_Dialog(errstr);
	}

	window = gtk_window_new(GTK_WINDOW_TOPLEVEL);

	gtk_window_set_title(GTK_WINDOW(window), "WiSPY");

	gtk_window_set_default_size(GTK_WINDOW(window), 750, 650);

	auxptr.wdr = &wdr;
	auxptr.main_vbox = NULL;
	auxptr.main_window = window;
	auxptr.notebook = NULL;
	auxptr.record_toggle_button = NULL;
	auxptr.playback_first_button = NULL;
	auxptr.playback_play_button = NULL;
	auxptr.playback_seek = NULL;
	auxptr.footer_label = NULL;
	auxptr.tabs = NULL;
	auxptr.playback_sweeps = g_ptr_array_new_with_free_func(playback_sweep_free);
	auxptr.recent_sweeps = g_ptr_array_new_with_free_func(playback_sweep_free);
	auxptr.playback_timeout = 0;
	auxptr.playback_index = 0;
	auxptr.playback_playing = 0;
	auxptr.playback_loaded = 0;
	auxptr.record_file = NULL;
	auxptr.record_tab = NULL;
	auxptr.num_tabs = 0;

	g_signal_connect(G_OBJECT (window), "delete_event",
					 G_CALLBACK (gtk_main_quit), NULL);
	g_signal_connect(G_OBJECT (window), "destroy",
					 G_CALLBACK (gtk_main_quit), NULL);

	accel_group = gtk_accel_group_new();

	item_factory = gtk_item_factory_new(GTK_TYPE_MENU_BAR, "<main>",
										accel_group);
	gtk_item_factory_create_items(item_factory, nmain_menu_items,
								  main_menu_items, &auxptr);
	gtk_window_add_accel_group(GTK_WINDOW(window), accel_group);
	menubar = gtk_item_factory_get_widget(item_factory, "<main>");

	vbox = gtk_vbox_new(FALSE, 0);
	gtk_container_add(GTK_CONTAINER(window), vbox); 

	gtk_widget_show(vbox);

	gtk_box_pack_start(GTK_BOX(vbox), menubar, FALSE, FALSE, 0);
	gtk_widget_show(menubar);

	playback_hbox = gtk_hbox_new(FALSE, 4);
	auxptr.record_toggle_button = gtk_toggle_button_new_with_label("Rec");
	auxptr.playback_first_button = gtk_button_new_with_label("|<");
	auxptr.playback_play_button = gtk_button_new_with_label("Play");
	auxptr.playback_seek = gtk_hscale_new_with_range(0, 1, 1);
	gtk_scale_set_draw_value(GTK_SCALE(auxptr.playback_seek), FALSE);
	gtk_widget_set_sensitive(auxptr.record_toggle_button, TRUE);
	gtk_widget_set_sensitive(auxptr.playback_first_button, TRUE);
	gtk_widget_set_sensitive(auxptr.playback_play_button, TRUE);
	gtk_box_pack_start(GTK_BOX(playback_hbox), auxptr.record_toggle_button, FALSE, FALSE, 4);
	gtk_box_pack_start(GTK_BOX(playback_hbox), auxptr.playback_first_button, FALSE, FALSE, 4);
	gtk_box_pack_start(GTK_BOX(playback_hbox), auxptr.playback_play_button, FALSE, FALSE, 0);
	gtk_box_pack_start(GTK_BOX(playback_hbox), auxptr.playback_seek, TRUE, TRUE, 4);
	g_signal_connect(G_OBJECT(auxptr.record_toggle_button), "toggled",
					 G_CALLBACK(record_toggle_button), &auxptr);
	g_signal_connect(G_OBJECT(auxptr.playback_first_button), "clicked",
					 G_CALLBACK(playback_first_button), &auxptr);
	g_signal_connect(G_OBJECT(auxptr.playback_play_button), "clicked",
					 G_CALLBACK(playback_play_button), &auxptr);
	g_signal_connect(G_OBJECT(auxptr.playback_seek), "value-changed",
					 G_CALLBACK(playback_seek_changed), &auxptr);
	gtk_box_pack_start(GTK_BOX(vbox), playback_hbox, FALSE, FALSE, 2);
	gtk_widget_show(auxptr.record_toggle_button);
	gtk_widget_show(auxptr.playback_first_button);
	gtk_widget_show(auxptr.playback_play_button);
	gtk_widget_show(auxptr.playback_seek);
	gtk_widget_show(playback_hbox);

	/* Make a notebook */
	notebook = gtk_notebook_new();
	gtk_box_pack_start(GTK_BOX(vbox), notebook, TRUE, TRUE, 0);

	gtk_widget_show(notebook);

	footer_hbox = gtk_hbox_new(FALSE, 0);
	auxptr.footer_label = gtk_label_new("");
	gtk_misc_set_alignment(GTK_MISC(auxptr.footer_label), 1.0, 0.5);
	gtk_box_pack_end(GTK_BOX(footer_hbox), auxptr.footer_label, FALSE, FALSE, 6);
	gtk_box_pack_start(GTK_BOX(vbox), footer_hbox, FALSE, FALSE, 2);
	update_footer_time(&auxptr);
	g_timeout_add(1000, update_footer_time, &auxptr);
	gtk_widget_show(auxptr.footer_label);
	gtk_widget_show(footer_hbox);

	auxptr.main_vbox = vbox;
	auxptr.notebook = notebook;

	nbfirst = build_nb_page(notebook, &auxptr);

	gtk_widget_show(window);

	gtk_main();

	if (auxptr.record_file != NULL)
		fclose(auxptr.record_file);
	if (auxptr.playback_sweeps != NULL)
		g_ptr_array_free(auxptr.playback_sweeps, TRUE);
	if (auxptr.recent_sweeps != NULL)
		g_ptr_array_free(auxptr.recent_sweeps, TRUE);

	return 0;
}	
