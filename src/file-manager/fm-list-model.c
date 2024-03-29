/* -*- Mode: C; indent-tabs-mode: t; c-basic-offset: 8; tab-width: 8 -*- */

/* fm-list-model.h - a GtkTreeModel for file lists. 

   Copyright (C) 2001, 2002 Anders Carlsson
   Copyright (C) 2003, Soeren Sandmann
   Copyright (C) 2004, Novell, Inc.

   The Gnome Library is free software; you can redistribute it and/or
   modify it under the terms of the GNU Library General Public License as
   published by the Free Software Foundation; either version 2 of the
   License, or (at your option) any later version.

   The Gnome Library is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
   Library General Public License for more details.

   You should have received a copy of the GNU Library General Public
   License along with the Gnome Library; see the file COPYING.LIB.  If not,
   write to the Free Software Foundation, Inc., 59 Temple Place - Suite 330,
   Boston, MA 02111-1307, USA.

   Authors: Anders Carlsson <andersca@gnu.org>, Soeren Sandmann (sandmann@daimi.au.dk), Dave Camp <dave@ximian.com>
*/

#include <config.h>
#include "fm-list-model.h"
#include <libegg/eggtreemultidnd.h>

#include <string.h>
#include <eel/eel-gtk-macros.h>
#include <eel/eel-glib-extensions.h>
#include <eel/eel-gdk-pixbuf-extensions.h>
#include <gtk/gtktreednd.h>
#include <gtk/gtktreesortable.h>
#include <glib/gi18n.h>
#include <libnautilus-private/nautilus-dnd.h>
#include <glib/gsequence.h>

enum {
	SUBDIRECTORY_UNLOADED,
	LAST_SIGNAL
};

static GQuark attribute_name_q,
	attribute_modification_date_q,
	attribute_date_modified_q;

/* msec delay after Loading... dummy row turns into (empty) */
#define LOADING_TO_EMPTY_DELAY 100

static guint list_model_signals[LAST_SIGNAL] = { 0 };

static int fm_list_model_file_entry_compare_func (gconstpointer a,
						  gconstpointer b,
						  gpointer      user_data);

static GObjectClass *parent_class;

struct FMListModelDetails {
	GSequence *files;
	GHashTable *directory_reverse_map; /* map from directory to GSequenceIter's */
	GHashTable *top_reverse_map;	   /* map from files in top dir to GSequenceIter's */

	int stamp;

	GQuark sort_attribute;
	GtkSortType order;

	gboolean sort_directories_first;

	GtkTreeView *drag_view;
	int drag_begin_x;
	int drag_begin_y;

	GPtrArray *columns;
};

typedef struct {
	FMListModel *model;
	
	GList *path_list;
} DragDataGetInfo;

typedef struct FileEntry FileEntry;

struct FileEntry {
	NautilusFile *file;
	GHashTable *reverse_map;	/* map from files to GSequenceIter's */
	NautilusDirectory *subdirectory;
	FileEntry *parent;
	GSequence *files;
	GSequenceIter *ptr;
	guint loaded : 1;
};

static const GtkTargetEntry drag_types [] = {
	{ NAUTILUS_ICON_DND_GNOME_ICON_LIST_TYPE, 0, NAUTILUS_ICON_DND_GNOME_ICON_LIST },
	{ NAUTILUS_ICON_DND_URI_LIST_TYPE, 0, NAUTILUS_ICON_DND_URI_LIST },
};

static GtkTargetList *drag_target_list = NULL;

static void
file_entry_free (FileEntry *file_entry)
{
	nautilus_file_unref (file_entry->file);
	if (file_entry->reverse_map) {
		g_hash_table_destroy (file_entry->reverse_map);
		file_entry->reverse_map = NULL;
	}
	if (file_entry->subdirectory != NULL) {
		nautilus_directory_unref (file_entry->subdirectory);
	}
	if (file_entry->files != NULL) {
		g_sequence_free (file_entry->files);
	}
	g_free (file_entry);
}

static GtkTreeModelFlags
fm_list_model_get_flags (GtkTreeModel *tree_model)
{
	return GTK_TREE_MODEL_ITERS_PERSIST;
}

static int
fm_list_model_get_n_columns (GtkTreeModel *tree_model)
{
	return FM_LIST_MODEL_NUM_COLUMNS + FM_LIST_MODEL (tree_model)->details->columns->len;
}

static GType
fm_list_model_get_column_type (GtkTreeModel *tree_model, int index)
{
	switch (index) {
	case FM_LIST_MODEL_FILE_COLUMN:
		return NAUTILUS_TYPE_FILE;
	case FM_LIST_MODEL_SUBDIRECTORY_COLUMN:
		return NAUTILUS_TYPE_DIRECTORY;
	case FM_LIST_MODEL_SMALLEST_ICON_COLUMN:
	case FM_LIST_MODEL_SMALLER_ICON_COLUMN:
	case FM_LIST_MODEL_SMALL_ICON_COLUMN:
	case FM_LIST_MODEL_STANDARD_ICON_COLUMN:
	case FM_LIST_MODEL_LARGE_ICON_COLUMN:
	case FM_LIST_MODEL_LARGER_ICON_COLUMN:
	case FM_LIST_MODEL_LARGEST_ICON_COLUMN:
	case FM_LIST_MODEL_SMALLEST_EMBLEM_COLUMN:
	case FM_LIST_MODEL_SMALLER_EMBLEM_COLUMN:
	case FM_LIST_MODEL_SMALL_EMBLEM_COLUMN:
	case FM_LIST_MODEL_STANDARD_EMBLEM_COLUMN:
	case FM_LIST_MODEL_LARGE_EMBLEM_COLUMN:
	case FM_LIST_MODEL_LARGER_EMBLEM_COLUMN:
	case FM_LIST_MODEL_LARGEST_EMBLEM_COLUMN:
		return GDK_TYPE_PIXBUF;
	case FM_LIST_MODEL_FILE_NAME_IS_EDITABLE_COLUMN:
		return G_TYPE_BOOLEAN;
	default:
		if (index < FM_LIST_MODEL_NUM_COLUMNS + FM_LIST_MODEL (tree_model)->details->columns->len) {
			return G_TYPE_STRING;
		} else {
			return G_TYPE_INVALID;
		}
	}
}

static void
fm_list_model_ptr_to_iter (FMListModel *model, GSequenceIter *ptr, GtkTreeIter *iter)
{
	g_assert (!g_sequence_iter_is_end (ptr));
	if (iter != NULL) {
		iter->stamp = model->details->stamp;
		iter->user_data = ptr;
	}
}

static gboolean
fm_list_model_get_iter (GtkTreeModel *tree_model, GtkTreeIter *iter, GtkTreePath *path)
{
	FMListModel *model;
	GSequence *files;
	GSequenceIter *ptr;
	FileEntry *file_entry;
	int i, d;
	
	model = (FMListModel *)tree_model;
	ptr = NULL;
	
	files = model->details->files;
	for (d = 0; d < gtk_tree_path_get_depth (path); d++) {
		i = gtk_tree_path_get_indices (path)[d];

		if (files == NULL || i >= g_sequence_get_length (files)) {
			return FALSE;
		}

		ptr = g_sequence_get_iter_at_pos (files, i);
		file_entry = g_sequence_get (ptr);
		files = file_entry->files;
	}

	fm_list_model_ptr_to_iter (model, ptr, iter);
	
	return TRUE;
}

static GtkTreePath *
fm_list_model_get_path (GtkTreeModel *tree_model, GtkTreeIter *iter)
{
	GtkTreePath *path;
	FMListModel *model;
	GSequenceIter *ptr;
	FileEntry *file_entry;


	model = (FMListModel *)tree_model;
	
	g_return_val_if_fail (iter->stamp == model->details->stamp, NULL);

	if (g_sequence_iter_is_end (iter->user_data)) {
		/* FIXME is this right? */
		return NULL;
	}
	
	path = gtk_tree_path_new ();
	ptr = iter->user_data;
	while (ptr != NULL) {
		gtk_tree_path_prepend_index (path, g_sequence_iter_get_position (ptr));
		file_entry = g_sequence_get (ptr);
		if (file_entry->parent != NULL) {
			ptr = file_entry->parent->ptr;
		} else {
			ptr = NULL;
		}
	}

	return path;
}

static void
fm_list_model_get_value (GtkTreeModel *tree_model, GtkTreeIter *iter, int column, GValue *value)
{
	FMListModel *model;
	FileEntry *file_entry;
	NautilusFile *file;
	char *str;
	GdkPixbuf *icon;
	int icon_size;
	guint emblem_size;
	NautilusZoomLevel zoom_level;
	GList *emblem_pixbufs;
	NautilusFile *parent_file;
	char *emblems_to_ignore[3];
	int i;
	NautilusFileIconFlags flags;
	
	model = (FMListModel *)tree_model;

	g_return_if_fail (model->details->stamp == iter->stamp);
	g_return_if_fail (!g_sequence_iter_is_end (iter->user_data));

	file_entry = g_sequence_get (iter->user_data);
	file = file_entry->file;
	
	switch (column) {
	case FM_LIST_MODEL_FILE_COLUMN:
		g_value_init (value, NAUTILUS_TYPE_FILE);

		g_value_set_object (value, file);
		break;
	case FM_LIST_MODEL_SUBDIRECTORY_COLUMN:
		g_value_init (value, NAUTILUS_TYPE_DIRECTORY);

		g_value_set_object (value, file_entry->subdirectory);
		break;
	case FM_LIST_MODEL_SMALLEST_ICON_COLUMN:
	case FM_LIST_MODEL_SMALLER_ICON_COLUMN:
	case FM_LIST_MODEL_SMALL_ICON_COLUMN:
	case FM_LIST_MODEL_STANDARD_ICON_COLUMN:
	case FM_LIST_MODEL_LARGE_ICON_COLUMN:
	case FM_LIST_MODEL_LARGER_ICON_COLUMN:
	case FM_LIST_MODEL_LARGEST_ICON_COLUMN:
		g_value_init (value, GDK_TYPE_PIXBUF);

		if (file != NULL) {
			zoom_level = fm_list_model_get_zoom_level_from_column_id (column);
			icon_size = nautilus_get_icon_size_for_zoom_level (zoom_level);

			flags = NAUTILUS_FILE_ICON_FLAGS_USE_THUMBNAILS;
			if (model->details->drag_view != NULL) {
				GtkTreePath *path_a, *path_b;
				
				gtk_tree_view_get_drag_dest_row (model->details->drag_view,
								 &path_a,
								 NULL);
				if (path_a != NULL) {
					path_b = gtk_tree_model_get_path (tree_model, iter);

					if (gtk_tree_path_compare (path_a, path_b) == 0) {
						flags |= NAUTILUS_FILE_ICON_FLAGS_FOR_DRAG_ACCEPT;
					}
						
					gtk_tree_path_free (path_a);
					gtk_tree_path_free (path_b);
				}
			}

			icon = nautilus_file_get_icon_pixbuf (file, icon_size, TRUE, flags);
  
			g_value_set_object (value, icon);
			g_object_unref (icon);
		}
		break;
	case FM_LIST_MODEL_SMALLEST_EMBLEM_COLUMN:
	case FM_LIST_MODEL_SMALLER_EMBLEM_COLUMN:
	case FM_LIST_MODEL_SMALL_EMBLEM_COLUMN:
	case FM_LIST_MODEL_STANDARD_EMBLEM_COLUMN:
	case FM_LIST_MODEL_LARGE_EMBLEM_COLUMN:
	case FM_LIST_MODEL_LARGER_EMBLEM_COLUMN:
	case FM_LIST_MODEL_LARGEST_EMBLEM_COLUMN:
		g_value_init (value, GDK_TYPE_PIXBUF);

		if (file != NULL) {
			parent_file = nautilus_file_get_parent (file);
			i = 0;
			emblems_to_ignore[i++] = NAUTILUS_FILE_EMBLEM_NAME_TRASH;
			if (parent_file) {
				if (!nautilus_file_can_write (parent_file)) {
					emblems_to_ignore[i++] = NAUTILUS_FILE_EMBLEM_NAME_CANT_WRITE;
				}
				nautilus_file_unref (parent_file);
			}
			emblems_to_ignore[i++] = NULL;
			
			zoom_level = fm_list_model_get_zoom_level_from_emblem_column_id (column);
			icon_size = nautilus_get_icon_size_for_zoom_level (zoom_level);
			emblem_size = nautilus_icon_get_emblem_size_for_icon_size (icon_size);
			/* Special case default icon size here. This works semi-ok, since we
			   only show one emblem for the list view anyway */
			if (emblem_size == 0 && icon_size >= 24) {
				emblem_size = 16;
			}
			if (emblem_size != 0) {
				emblem_pixbufs = nautilus_file_get_emblem_pixbufs (file,
										   emblem_size,
										   TRUE,
										   emblems_to_ignore);
				if (emblem_pixbufs != NULL) {
					icon = emblem_pixbufs->data;
					g_value_set_object (value, icon);
				}
				eel_gdk_pixbuf_list_free (emblem_pixbufs);
			}
		}
		break;
	case FM_LIST_MODEL_FILE_NAME_IS_EDITABLE_COLUMN:
		g_value_init (value, G_TYPE_BOOLEAN);
		
                g_value_set_boolean (value, file != NULL && nautilus_file_can_rename (file));
                break;
 	default:
 		if (column >= FM_LIST_MODEL_NUM_COLUMNS || column < FM_LIST_MODEL_NUM_COLUMNS + model->details->columns->len) {
			NautilusColumn *nautilus_column;
			GQuark attribute;
			nautilus_column = model->details->columns->pdata[column - FM_LIST_MODEL_NUM_COLUMNS];
			
			g_value_init (value, G_TYPE_STRING);
			g_object_get (nautilus_column, 
				      "attribute_q", &attribute, 
				      NULL);
			if (file != NULL) {
				str = nautilus_file_get_string_attribute_with_default_q (file, 
											 attribute);
				g_value_take_string (value, str);
			} else if (attribute == attribute_name_q) {
				if (file_entry->parent->loaded) {
					g_value_set_string (value, _("(Empty)"));
				} else {
					g_value_set_string (value, _("Loading..."));
				}
			}
		} else {
			g_assert_not_reached ();
		}
	}
}

static gboolean
fm_list_model_iter_next (GtkTreeModel *tree_model, GtkTreeIter *iter)
{
	FMListModel *model;

	model = (FMListModel *)tree_model;

	g_return_val_if_fail (model->details->stamp == iter->stamp, FALSE);

	iter->user_data = g_sequence_iter_next (iter->user_data);

	return !g_sequence_iter_is_end (iter->user_data);
}

static gboolean
fm_list_model_iter_children (GtkTreeModel *tree_model, GtkTreeIter *iter, GtkTreeIter *parent)
{
	FMListModel *model;
	GSequence *files;
	FileEntry *file_entry;

	model = (FMListModel *)tree_model;
	
	if (parent == NULL) {
		files = model->details->files;
	} else {
		file_entry = g_sequence_get (parent->user_data);
		files = file_entry->files;
	}

	if (files == NULL || g_sequence_get_length (files) == 0) {
		return FALSE;
	}
	
	iter->stamp = model->details->stamp;
	iter->user_data = g_sequence_get_begin_iter (files);

	return TRUE;
}

static gboolean
fm_list_model_iter_has_child (GtkTreeModel *tree_model, GtkTreeIter *iter)
{
	FileEntry *file_entry;
	
	if (iter == NULL) {
		return !fm_list_model_is_empty (FM_LIST_MODEL (tree_model));
	}

	file_entry = g_sequence_get (iter->user_data);

	return (file_entry->files != NULL && g_sequence_get_length (file_entry->files) > 0);
}

static int
fm_list_model_iter_n_children (GtkTreeModel *tree_model, GtkTreeIter *iter)
{
	FMListModel *model;
	GSequence *files;
	FileEntry *file_entry;

	model = (FMListModel *)tree_model;

	if (iter == NULL) {
		files = model->details->files;
	} else {
		file_entry = g_sequence_get (iter->user_data);
		files = file_entry->files;
	}

	return g_sequence_get_length (files);
}

static gboolean
fm_list_model_iter_nth_child (GtkTreeModel *tree_model, GtkTreeIter *iter, GtkTreeIter *parent, int n)
{
	FMListModel *model;
	GSequenceIter *child;
	GSequence *files;
	FileEntry *file_entry;

	model = (FMListModel *)tree_model;
	
	if (parent != NULL) {
		file_entry = g_sequence_get (parent->user_data);
		files = file_entry->files;
	} else {
		files = model->details->files;
	}

	child = g_sequence_get_iter_at_pos (files, n);

	if (g_sequence_iter_is_end (child)) {
		return FALSE;
	}

	iter->stamp = model->details->stamp;
	iter->user_data = child;

	return TRUE;
}

static gboolean
fm_list_model_iter_parent (GtkTreeModel *tree_model, GtkTreeIter *iter, GtkTreeIter *child)
{
	FMListModel *model;
	FileEntry *file_entry;
	
	model = (FMListModel *)tree_model;
	
	file_entry = g_sequence_get (child->user_data);
	
	if (file_entry->parent == NULL) {
		return FALSE;
	}

	iter->stamp = model->details->stamp;
	iter->user_data = file_entry->parent->ptr;
	
	return TRUE;
}

static GSequenceIter *
lookup_file (FMListModel *model, NautilusFile *file,
	     NautilusDirectory *directory)
{
	FileEntry *file_entry;
	GSequenceIter *ptr, *parent_ptr;

	parent_ptr = NULL;
	if (directory) {
		parent_ptr = g_hash_table_lookup (model->details->directory_reverse_map,
						  directory);
	}
	
	if (parent_ptr) {
		file_entry = g_sequence_get (parent_ptr);
		ptr = g_hash_table_lookup (file_entry->reverse_map, file);
	} else {
		ptr = g_hash_table_lookup (model->details->top_reverse_map, file);
	}

	if (ptr) {
		g_assert (((FileEntry *)g_sequence_get (ptr))->file == file);
	}
	
	return ptr;
}


struct GetIters {
	FMListModel *model;
	NautilusFile *file;
	GList *iters;
};

static void
dir_to_iters (struct GetIters *data,
	      GHashTable *reverse_map)
{
	GSequenceIter *ptr;
	
	ptr = g_hash_table_lookup (reverse_map, data->file);
	if (ptr) {
		GtkTreeIter *iter;
		iter = g_new0 (GtkTreeIter, 1);
		fm_list_model_ptr_to_iter (data->model, ptr, iter);
		data->iters = g_list_prepend (data->iters, iter);
	}
}

static void
file_to_iter_cb (gpointer  key,
		 gpointer  value,
		 gpointer  user_data)
{
	struct GetIters *data;
	FileEntry *dir_file_entry;

	data = user_data;
	dir_file_entry = g_sequence_get ((GSequenceIter *)value);
	dir_to_iters (data, dir_file_entry->reverse_map);
}

GList *
fm_list_model_get_all_iters_for_file (FMListModel *model, NautilusFile *file)
{
	struct GetIters data;

	data.file = file;
	data.model = model;
	data.iters = NULL;
	
	dir_to_iters (&data, model->details->top_reverse_map);
	g_hash_table_foreach (model->details->directory_reverse_map,
			      file_to_iter_cb, &data);

	return g_list_reverse (data.iters);
}

gboolean
fm_list_model_get_first_iter_for_file (FMListModel          *model,
				       NautilusFile         *file,
				       GtkTreeIter          *iter)
{
	GList *list;
	gboolean res;

	res = FALSE;
	
	list = fm_list_model_get_all_iters_for_file (model, file);
	if (list != NULL) {
		res = TRUE;
		*iter = *(GtkTreeIter *)list->data;
	}
	eel_g_list_free_deep (list);
	
	return res;
}


gboolean
fm_list_model_get_tree_iter_from_file (FMListModel *model, NautilusFile *file,
				       NautilusDirectory *directory,
				       GtkTreeIter *iter)
{
	GSequenceIter *ptr;

	ptr = lookup_file (model, file, directory);
	if (!ptr) {
		return FALSE;
	}

	fm_list_model_ptr_to_iter (model, ptr, iter);
	
	return TRUE;
}

static int
fm_list_model_file_entry_compare_func (gconstpointer a,
				       gconstpointer b,
				       gpointer      user_data)
{
	FileEntry *file_entry1;
	FileEntry *file_entry2;
	FMListModel *model;
	int result;

	model = (FMListModel *)user_data;

	file_entry1 = (FileEntry *)a;
	file_entry2 = (FileEntry *)b;
	
	if (file_entry1->file != NULL && file_entry2->file != NULL) {
		result = nautilus_file_compare_for_sort_by_attribute_q (file_entry1->file, file_entry2->file,
									model->details->sort_attribute,
									model->details->sort_directories_first,
									(model->details->order == GTK_SORT_DESCENDING));
	} else if (file_entry1->file == NULL) {
		return -1;
	} else {
		return 1;
	}

	return result;
}

int
fm_list_model_compare_func (FMListModel *model,
			    NautilusFile *file1,
			    NautilusFile *file2)
{
	int result;

	result = nautilus_file_compare_for_sort_by_attribute_q (file1, file2,
								model->details->sort_attribute,
								model->details->sort_directories_first,
								(model->details->order == GTK_SORT_DESCENDING));

	return result;
}

static void
fm_list_model_sort_file_entries (FMListModel *model, GSequence *files, GtkTreePath *path)
{
	GSequenceIter **old_order;
	GtkTreeIter iter;
	int *new_order;
	int length;
	int i;
	FileEntry *file_entry;
	gboolean has_iter;

	length = g_sequence_get_length (files);

	if (length <= 1) {
		return;
	}
	
	/* generate old order of GSequenceIter's */
	old_order = g_new (GSequenceIter *, length);
	for (i = 0; i < length; ++i) {
		GSequenceIter *ptr = g_sequence_get_iter_at_pos (files, i);
		
		file_entry = g_sequence_get (ptr);
		if (file_entry->files != NULL) {
			gtk_tree_path_append_index (path, i);
			fm_list_model_sort_file_entries (model, file_entry->files, path);
			gtk_tree_path_up (path);
		}

		old_order[i] = ptr;
	}

	/* sort */
	g_sequence_sort (files, fm_list_model_file_entry_compare_func, model);

	/* generate new order */
	new_order = g_new (int, length);
	/* Note: new_order[newpos] = oldpos */
	for (i = 0; i < length; ++i) {
		new_order[g_sequence_iter_get_position (old_order[i])] = i;
	}

	/* Let the world know about our new order */

	g_assert (new_order != NULL);

	has_iter = FALSE;
	if (gtk_tree_path_get_depth (path) != 0) {
		gboolean get_iter_result;
		has_iter = TRUE;
		get_iter_result = gtk_tree_model_get_iter (GTK_TREE_MODEL (model), &iter, path);
		g_assert (get_iter_result);
	}

	gtk_tree_model_rows_reordered (GTK_TREE_MODEL (model),
				       path, has_iter ? &iter : NULL, new_order);

	g_free (old_order);
	g_free (new_order);
}

static void
fm_list_model_sort (FMListModel *model)
{
	GtkTreePath *path;

	path = gtk_tree_path_new ();

	fm_list_model_sort_file_entries (model, model->details->files, path);

	gtk_tree_path_free (path);
}

static gboolean
fm_list_model_get_sort_column_id (GtkTreeSortable *sortable,
				  gint            *sort_column_id,
				  GtkSortType     *order)
{
	FMListModel *model;
	int id;
	
	model = (FMListModel *)sortable;
	
	id = fm_list_model_get_sort_column_id_from_attribute 
		(model, model->details->sort_attribute);
	
	if (id == -1) {
		return FALSE;
	}
	
	if (sort_column_id != NULL) {
		*sort_column_id = id;
	}

	if (order != NULL) {
		*order = model->details->order;
	}

	return TRUE;
}

static void
fm_list_model_set_sort_column_id (GtkTreeSortable *sortable, gint sort_column_id, GtkSortType order)
{
	FMListModel *model;

	model = (FMListModel *)sortable;

	model->details->sort_attribute = fm_list_model_get_attribute_from_sort_column_id (model, sort_column_id);

	model->details->order = order;

	fm_list_model_sort (model);
	gtk_tree_sortable_sort_column_changed (sortable);
}

static gboolean
fm_list_model_has_default_sort_func (GtkTreeSortable *sortable)
{
	return FALSE;
}

static gboolean
fm_list_model_multi_row_draggable (EggTreeMultiDragSource *drag_source, GList *path_list)
{
	return TRUE;
}

static void
each_path_get_data_binder (NautilusDragEachSelectedItemDataGet data_get,
			   gpointer context,
			   gpointer data)
{
	DragDataGetInfo *info;
	GList *l;
	NautilusFile *file;
	GtkTreeRowReference *row;
	GtkTreePath *path;
	char *uri;
	GdkRectangle cell_area;
	GtkTreeViewColumn *column;

	info = context;

	g_return_if_fail (info->model->details->drag_view);

	column = gtk_tree_view_get_column (info->model->details->drag_view, 0);

	for (l = info->path_list; l != NULL; l = l->next) {
		row = l->data;

		path = gtk_tree_row_reference_get_path (row);
		file = fm_list_model_file_for_path (info->model, path);
		if (file) {
			gtk_tree_view_get_cell_area
				(info->model->details->drag_view,
				 path, 
				 column,
				 &cell_area);
				
			uri = nautilus_file_get_uri (file);
				
			(*data_get) (uri, 
				     0,
				     cell_area.y - info->model->details->drag_begin_y,
				     cell_area.width, cell_area.height, 
				     data);
				
			g_free (uri);
			
			nautilus_file_unref (file);
		}
		
		gtk_tree_path_free (path);
	}
}

static gboolean
fm_list_model_multi_drag_data_get (EggTreeMultiDragSource *drag_source, 
				   GList *path_list, 
				   GtkSelectionData *selection_data)
{
	FMListModel *model;
	DragDataGetInfo context;
	guint target_info;
	
	model = FM_LIST_MODEL (drag_source);

	context.model = model;
	context.path_list = path_list;

	if (!drag_target_list) {
		drag_target_list = gtk_target_list_new 
			(drag_types, G_N_ELEMENTS (drag_types));
		gtk_target_list_add_text_targets (
			drag_target_list, NAUTILUS_ICON_DND_TEXT);
	}

	if (gtk_target_list_find (drag_target_list,
				  selection_data->target,
				  &target_info)) {
		nautilus_drag_drag_data_get (NULL,
					     NULL,
					     selection_data,
					     target_info,
					     GDK_CURRENT_TIME,
					     &context,
					     each_path_get_data_binder);
		return TRUE;
	} else {
		return FALSE;
	}
}

static gboolean
fm_list_model_multi_drag_data_delete (EggTreeMultiDragSource *drag_source, GList *path_list)
{
	return TRUE;
}

static void
add_dummy_row (FMListModel *model, FileEntry *parent_entry)
{
	FileEntry *dummy_file_entry;
	GtkTreeIter iter;
	GtkTreePath *path;
	
	dummy_file_entry = g_new0 (FileEntry, 1);
	dummy_file_entry->parent = parent_entry;
	dummy_file_entry->ptr = g_sequence_insert_sorted (parent_entry->files, dummy_file_entry,
							  fm_list_model_file_entry_compare_func, model);
	iter.stamp = model->details->stamp;
	iter.user_data = dummy_file_entry->ptr;
	
	path = gtk_tree_model_get_path (GTK_TREE_MODEL (model), &iter);
	gtk_tree_model_row_inserted (GTK_TREE_MODEL (model), path, &iter);
	gtk_tree_path_free (path);
}

gboolean
fm_list_model_add_file (FMListModel *model, NautilusFile *file,
			NautilusDirectory *directory)
{
	GtkTreeIter iter;
	GtkTreePath *path;
	FileEntry *file_entry;
	GSequenceIter *ptr, *parent_ptr;
	GSequence *files;
	gboolean replace_dummy;
	GHashTable *parent_hash;

	parent_ptr = g_hash_table_lookup (model->details->directory_reverse_map,
					  directory);
	if (parent_ptr) {
		file_entry = g_sequence_get (parent_ptr);
		ptr = g_hash_table_lookup (file_entry->reverse_map, file);
	} else {
		file_entry = NULL;
		ptr = g_hash_table_lookup (model->details->top_reverse_map, file);
	}

	if (ptr != NULL) {
		g_warning ("file already in tree (parent_ptr: %p)!!!\n", parent_ptr);
		return FALSE;
	}
	
	file_entry = g_new0 (FileEntry, 1);
	file_entry->file = nautilus_file_ref (file);
	file_entry->parent = NULL;
	file_entry->subdirectory = NULL;
	file_entry->files = NULL;
	
	files = model->details->files;
	parent_hash = model->details->top_reverse_map;
	
	replace_dummy = FALSE;

	if (parent_ptr != NULL) {
		file_entry->parent = g_sequence_get (parent_ptr);
		/* At this point we set loaded. Either we saw
		 * "done" and ignored it waiting for this, or we do this
		 * earlier, but then we replace the dummy row anyway,
		 * so it doesn't matter */
		file_entry->parent->loaded = 1;
		parent_hash = file_entry->parent->reverse_map;
		files = file_entry->parent->files;
		if (g_sequence_get_length (files) == 1) {
			GSequenceIter *dummy_ptr = g_sequence_get_iter_at_pos (files, 0);
			FileEntry *dummy_entry = g_sequence_get (dummy_ptr);
			if (dummy_entry->file == NULL) {
				/* replace the dummy loading entry */
				model->details->stamp++;
				g_sequence_remove (dummy_ptr);
				
				replace_dummy = TRUE;
			}
		}
	}

	
	file_entry->ptr = g_sequence_insert_sorted (files, file_entry,
					    fm_list_model_file_entry_compare_func, model);

	g_hash_table_insert (parent_hash, file, file_entry->ptr);
	
	iter.stamp = model->details->stamp;
	iter.user_data = file_entry->ptr;

	path = gtk_tree_model_get_path (GTK_TREE_MODEL (model), &iter);
	if (replace_dummy) {
		gtk_tree_model_row_changed (GTK_TREE_MODEL (model), path, &iter);
	} else {
		gtk_tree_model_row_inserted (GTK_TREE_MODEL (model), path, &iter);
	}

	if (nautilus_file_is_directory (file)) {
		file_entry->files = g_sequence_new ((GDestroyNotify)file_entry_free);

		add_dummy_row (model, file_entry);

		gtk_tree_model_row_has_child_toggled (GTK_TREE_MODEL (model),
						      path, &iter);
	}
	gtk_tree_path_free (path);
	
	return TRUE;
}

void
fm_list_model_file_changed (FMListModel *model, NautilusFile *file,
			    NautilusDirectory *directory)
{
	FileEntry *parent_file_entry;
	GtkTreeIter iter;
	GtkTreePath *path, *parent_path;
	GSequenceIter *ptr;
	int pos_before, pos_after, length, i, old;
	int *new_order;
	gboolean has_iter;
	GSequence *files;

	ptr = lookup_file (model, file, directory);
	if (!ptr) {
		return;
	}

	
	pos_before = g_sequence_iter_get_position (ptr);
		
	g_sequence_sort_changed (ptr, fm_list_model_file_entry_compare_func, model);

	pos_after = g_sequence_iter_get_position (ptr);

	if (pos_before != pos_after) {
		/* The file moved, we need to send rows_reordered */
		
		parent_file_entry = ((FileEntry *)g_sequence_get (ptr))->parent;

		if (parent_file_entry == NULL) {
			has_iter = FALSE;
			parent_path = gtk_tree_path_new ();
			files = model->details->files;
		} else {
			has_iter = TRUE;
			fm_list_model_ptr_to_iter (model, parent_file_entry->ptr, &iter);
			parent_path = gtk_tree_model_get_path (GTK_TREE_MODEL (model), &iter);
			files = parent_file_entry->files;
		}

		length = g_sequence_get_length (files);
		new_order = g_new (int, length);
		/* Note: new_order[newpos] = oldpos */
		for (i = 0, old = 0; i < length; ++i) {
			if (i == pos_after) {
				new_order[i] = pos_before;
			} else {
				if (old == pos_before)
					old++;
				new_order[i] = old++;
			}
		}

		gtk_tree_model_rows_reordered (GTK_TREE_MODEL (model),
					       parent_path, has_iter ? &iter : NULL, new_order);

		gtk_tree_path_free (parent_path);
		g_free (new_order);
	}
	
	fm_list_model_ptr_to_iter (model, ptr, &iter);
	path = gtk_tree_model_get_path (GTK_TREE_MODEL (model), &iter);
	gtk_tree_model_row_changed (GTK_TREE_MODEL (model), path, &iter);
	gtk_tree_path_free (path);
}

gboolean
fm_list_model_is_empty (FMListModel *model)
{
	return (g_sequence_get_length (model->details->files) == 0);
}

guint
fm_list_model_get_length (FMListModel *model)
{
	return g_sequence_get_length (model->details->files);
}

static void
fm_list_model_remove (FMListModel *model, GtkTreeIter *iter)
{
	GSequenceIter *ptr, *child_ptr;
	FileEntry *file_entry, *child_file_entry, *parent_file_entry;
	GtkTreePath *path;
	GtkTreeIter parent_iter;

	ptr = iter->user_data;
	file_entry = g_sequence_get (ptr);
	
	if (file_entry->files != NULL) {
		while (g_sequence_get_length (file_entry->files) > 0) {
			child_ptr = g_sequence_get_begin_iter (file_entry->files);
			child_file_entry = g_sequence_get (child_ptr);
			if (child_file_entry->file != NULL) {
				fm_list_model_remove_file (model,
							   child_file_entry->file,
							   file_entry->subdirectory);
			} else {
				path = gtk_tree_model_get_path (GTK_TREE_MODEL (model), iter);
				gtk_tree_path_append_index (path, 0);
				model->details->stamp++;
				g_sequence_remove (child_ptr);
				gtk_tree_model_row_deleted (GTK_TREE_MODEL (model), path);
				gtk_tree_path_free (path);
			}
			
			/* the parent iter didn't actually change */
			iter->stamp = model->details->stamp;
		}
			
	}

	if (file_entry->file != NULL) { /* Don't try to remove dummy row */
		if (file_entry->parent != NULL) {
			g_hash_table_remove (file_entry->parent->reverse_map, file_entry->file);
		} else {
			g_hash_table_remove (model->details->top_reverse_map, file_entry->file);
		}
	}

	parent_file_entry = file_entry->parent;
	if (parent_file_entry && g_sequence_get_length (parent_file_entry->files) == 1 &&
	    file_entry->file != NULL) {
		/* this is the last non-dummy child, add a dummy node */
		/* We need to do this before removing the last file to avoid
		 * collapsing the row.
		 */
		add_dummy_row (model, parent_file_entry);
	}

	if (file_entry->subdirectory != NULL) {
		g_signal_emit (model,
			       list_model_signals[SUBDIRECTORY_UNLOADED], 0,
			       file_entry->subdirectory);
		g_hash_table_remove (model->details->directory_reverse_map,
				     file_entry->subdirectory);
	}
	
	path = gtk_tree_model_get_path (GTK_TREE_MODEL (model), iter);
	
	g_sequence_remove (ptr);
	model->details->stamp++;
	gtk_tree_model_row_deleted (GTK_TREE_MODEL (model), path);
	
	gtk_tree_path_free (path);

	if (parent_file_entry && g_sequence_get_length (parent_file_entry->files) == 0) {
		parent_iter.stamp = model->details->stamp;
		parent_iter.user_data = parent_file_entry->ptr;
		path = gtk_tree_model_get_path (GTK_TREE_MODEL (model), &parent_iter);
		gtk_tree_model_row_has_child_toggled (GTK_TREE_MODEL (model),
						      path, &parent_iter);
		gtk_tree_path_free (path);
	}
}

void
fm_list_model_remove_file (FMListModel *model, NautilusFile *file,
			   NautilusDirectory *directory)
{
	GtkTreeIter iter;

	if (fm_list_model_get_tree_iter_from_file (model, file, directory, &iter)) {
		fm_list_model_remove (model, &iter);
	}
}

static void
fm_list_model_clear_directory (FMListModel *model, GSequence *files)
{
	GtkTreeIter iter;
	FileEntry *file_entry;

	while (g_sequence_get_length (files) > 0) {
		iter.user_data = g_sequence_get_begin_iter (files);

		file_entry = g_sequence_get (iter.user_data);
		if (file_entry->files != NULL) {
			fm_list_model_clear_directory (model, file_entry->files);
		}
		
		iter.stamp = model->details->stamp;
		fm_list_model_remove (model, &iter);
	}
}

void
fm_list_model_clear (FMListModel *model)
{
	g_return_if_fail (model != NULL);

	fm_list_model_clear_directory (model, model->details->files);
}

NautilusFile *
fm_list_model_file_for_path (FMListModel *model, GtkTreePath *path)
{
	NautilusFile *file;
	GtkTreeIter iter;

	file = NULL;
	if (gtk_tree_model_get_iter (GTK_TREE_MODEL (model), 
				     &iter, path)) {
		gtk_tree_model_get (GTK_TREE_MODEL (model), 
				    &iter, 
				    FM_LIST_MODEL_FILE_COLUMN, &file,
				    -1);
	}
	return file;
}

gboolean
fm_list_model_load_subdirectory (FMListModel *model, GtkTreePath *path, NautilusDirectory **directory)
{
	GtkTreeIter iter;
	FileEntry *file_entry;
	NautilusDirectory *subdirectory;
	
	if (!gtk_tree_model_get_iter (GTK_TREE_MODEL (model), &iter, path)) {
		return FALSE;
	}

	file_entry = g_sequence_get (iter.user_data);
	if (file_entry->file == NULL ||
	    file_entry->subdirectory != NULL) {
		return FALSE;
	}

	subdirectory = nautilus_directory_get_for_file (file_entry->file);

	if (g_hash_table_lookup (model->details->directory_reverse_map,
				 subdirectory) != NULL) {
		nautilus_directory_unref (subdirectory);
		g_warning ("Already in directory_reverse_map, failing\n");
		return FALSE;
	}
	
	file_entry->subdirectory = subdirectory,
	g_hash_table_insert (model->details->directory_reverse_map,
			     subdirectory, file_entry->ptr);
	file_entry->reverse_map = g_hash_table_new (g_direct_hash, g_direct_equal);

	/* Return a ref too */
	nautilus_directory_ref (subdirectory);
	*directory = subdirectory;
	
	return TRUE;
}

/* removes all children of the subfolder and unloads the subdirectory */
void
fm_list_model_unload_subdirectory (FMListModel *model, GtkTreeIter *iter)
{
	GSequenceIter *child_ptr;
	FileEntry *file_entry, *child_file_entry;
	GtkTreeIter child_iter;

	file_entry = g_sequence_get (iter->user_data);
	if (file_entry->file == NULL ||
	    file_entry->subdirectory == NULL) {
		return;
	}

	file_entry->loaded = 0;
	
	/* Remove all children */
	while (g_sequence_get_length (file_entry->files) > 0) {
		child_ptr = g_sequence_get_begin_iter (file_entry->files);
		child_file_entry = g_sequence_get (child_ptr);
		if (child_file_entry->file == NULL) {
			/* Don't delete the dummy node */
			break;
		} else {
			fm_list_model_ptr_to_iter (model, child_ptr, &child_iter);
			fm_list_model_remove (model, &child_iter);
		}
	}

	/* Emit unload signal */
	g_signal_emit (model,
		       list_model_signals[SUBDIRECTORY_UNLOADED], 0,
		       file_entry->subdirectory);

	/* actually unload */
	g_hash_table_remove (model->details->directory_reverse_map,
			     file_entry->subdirectory);
	nautilus_directory_unref (file_entry->subdirectory);
	file_entry->subdirectory = NULL;

	g_assert (g_hash_table_size (file_entry->reverse_map) == 0);
	g_hash_table_destroy (file_entry->reverse_map);
	file_entry->reverse_map = NULL;
}



void
fm_list_model_set_should_sort_directories_first (FMListModel *model, gboolean sort_directories_first)
{
	if (model->details->sort_directories_first == sort_directories_first) {
		return;
	}

	model->details->sort_directories_first = sort_directories_first;
	fm_list_model_sort (model);
}

int
fm_list_model_get_sort_column_id_from_attribute (FMListModel *model,
						 GQuark attribute)
{
	guint i;

	if (attribute == 0) {
		return -1;
	}

	/* Hack - the preferences dialog sets modification_date for some 
	 * rather than date_modified for some reason.  Make sure that 
	 * works. */
	if (attribute == attribute_modification_date_q) {
		attribute = attribute_date_modified_q;
	}

	for (i = 0; i < model->details->columns->len; i++) {
		NautilusColumn *column;
		GQuark column_attribute;
		
		column = 
			NAUTILUS_COLUMN (model->details->columns->pdata[i]);
		g_object_get (G_OBJECT (column), 
			      "attribute_q", &column_attribute, 
			      NULL);
		if (column_attribute == attribute) {
			return FM_LIST_MODEL_NUM_COLUMNS + i;
		}
	}
	
	return -1;
}

GQuark
fm_list_model_get_attribute_from_sort_column_id (FMListModel *model,
						 int sort_column_id)
{
	NautilusColumn *column;
	int index;
	GQuark attribute;
	
	index = sort_column_id - FM_LIST_MODEL_NUM_COLUMNS;

	if (index < 0 || index >= model->details->columns->len) {
		g_warning ("unknown sort column id: %d", sort_column_id);
		return 0;
	}

	column = NAUTILUS_COLUMN (model->details->columns->pdata[index]);
	g_object_get (G_OBJECT (column), "attribute_q", &attribute, NULL);

	return attribute;
}

NautilusZoomLevel
fm_list_model_get_zoom_level_from_column_id (int column)
{
	switch (column) {
	case FM_LIST_MODEL_SMALLEST_ICON_COLUMN:
		return NAUTILUS_ZOOM_LEVEL_SMALLEST;
	case FM_LIST_MODEL_SMALLER_ICON_COLUMN:
		return NAUTILUS_ZOOM_LEVEL_SMALLER;
	case FM_LIST_MODEL_SMALL_ICON_COLUMN:
		return NAUTILUS_ZOOM_LEVEL_SMALL;
	case FM_LIST_MODEL_STANDARD_ICON_COLUMN:
		return NAUTILUS_ZOOM_LEVEL_STANDARD;
	case FM_LIST_MODEL_LARGE_ICON_COLUMN:
		return NAUTILUS_ZOOM_LEVEL_LARGE;
	case FM_LIST_MODEL_LARGER_ICON_COLUMN:
		return NAUTILUS_ZOOM_LEVEL_LARGER;
	case FM_LIST_MODEL_LARGEST_ICON_COLUMN:
		return NAUTILUS_ZOOM_LEVEL_LARGEST;
	}

	g_return_val_if_reached (NAUTILUS_ZOOM_LEVEL_STANDARD);
}

int
fm_list_model_get_column_id_from_zoom_level (NautilusZoomLevel zoom_level)
{
	switch (zoom_level) {
	case NAUTILUS_ZOOM_LEVEL_SMALLEST:
		return FM_LIST_MODEL_SMALLEST_ICON_COLUMN;
	case NAUTILUS_ZOOM_LEVEL_SMALLER:
		return FM_LIST_MODEL_SMALLER_ICON_COLUMN;
	case NAUTILUS_ZOOM_LEVEL_SMALL:
		return FM_LIST_MODEL_SMALL_ICON_COLUMN;
	case NAUTILUS_ZOOM_LEVEL_STANDARD:
		return FM_LIST_MODEL_STANDARD_ICON_COLUMN;
	case NAUTILUS_ZOOM_LEVEL_LARGE:
		return FM_LIST_MODEL_LARGE_ICON_COLUMN;
	case NAUTILUS_ZOOM_LEVEL_LARGER:
		return FM_LIST_MODEL_LARGER_ICON_COLUMN;
	case NAUTILUS_ZOOM_LEVEL_LARGEST:
		return FM_LIST_MODEL_LARGEST_ICON_COLUMN;
	}

	g_return_val_if_reached (FM_LIST_MODEL_STANDARD_ICON_COLUMN);
}

NautilusZoomLevel
fm_list_model_get_zoom_level_from_emblem_column_id (int column)
{
	switch (column) {
	case FM_LIST_MODEL_SMALLEST_EMBLEM_COLUMN:
		return NAUTILUS_ZOOM_LEVEL_SMALLEST;
	case FM_LIST_MODEL_SMALLER_EMBLEM_COLUMN:
		return NAUTILUS_ZOOM_LEVEL_SMALLER;
	case FM_LIST_MODEL_SMALL_EMBLEM_COLUMN:
		return NAUTILUS_ZOOM_LEVEL_SMALL;
	case FM_LIST_MODEL_STANDARD_EMBLEM_COLUMN:
		return NAUTILUS_ZOOM_LEVEL_STANDARD;
	case FM_LIST_MODEL_LARGE_EMBLEM_COLUMN:
		return NAUTILUS_ZOOM_LEVEL_LARGE;
	case FM_LIST_MODEL_LARGER_EMBLEM_COLUMN:
		return NAUTILUS_ZOOM_LEVEL_LARGER;
	case FM_LIST_MODEL_LARGEST_EMBLEM_COLUMN:
		return NAUTILUS_ZOOM_LEVEL_LARGEST;
	}

	g_return_val_if_reached (NAUTILUS_ZOOM_LEVEL_STANDARD);
}

int
fm_list_model_get_emblem_column_id_from_zoom_level (NautilusZoomLevel zoom_level)
{
	switch (zoom_level) {
	case NAUTILUS_ZOOM_LEVEL_SMALLEST:
		return FM_LIST_MODEL_SMALLEST_EMBLEM_COLUMN;
	case NAUTILUS_ZOOM_LEVEL_SMALLER:
		return FM_LIST_MODEL_SMALLER_EMBLEM_COLUMN;
	case NAUTILUS_ZOOM_LEVEL_SMALL:
		return FM_LIST_MODEL_SMALL_EMBLEM_COLUMN;
	case NAUTILUS_ZOOM_LEVEL_STANDARD:
		return FM_LIST_MODEL_STANDARD_EMBLEM_COLUMN;
	case NAUTILUS_ZOOM_LEVEL_LARGE:
		return FM_LIST_MODEL_LARGE_EMBLEM_COLUMN;
	case NAUTILUS_ZOOM_LEVEL_LARGER:
		return FM_LIST_MODEL_LARGER_EMBLEM_COLUMN;
	case NAUTILUS_ZOOM_LEVEL_LARGEST:
		return FM_LIST_MODEL_LARGEST_EMBLEM_COLUMN;
	}

	g_return_val_if_reached (FM_LIST_MODEL_STANDARD_EMBLEM_COLUMN);
}

void
fm_list_model_set_drag_view (FMListModel *model,
			     GtkTreeView *view,
			     int drag_begin_x,
			     int drag_begin_y)
{
	g_return_if_fail (model != NULL);
	g_return_if_fail (FM_IS_LIST_MODEL (model));
	g_return_if_fail (!view || GTK_IS_TREE_VIEW (view));
	
	model->details->drag_view = view;
	model->details->drag_begin_x = drag_begin_x;
	model->details->drag_begin_y = drag_begin_y;
}

void
fm_list_model_get_drag_types (const GtkTargetEntry **entries,
			      int *num_entries)
{
	*entries = drag_types;
	*num_entries = G_N_ELEMENTS (drag_types);
}

int               
fm_list_model_add_column (FMListModel *model,
			  NautilusColumn *column)
{
	g_ptr_array_add (model->details->columns, column);
	g_object_ref (column);

	return FM_LIST_MODEL_NUM_COLUMNS + (model->details->columns->len - 1);
}

int
fm_list_model_get_column_number (FMListModel *model,
				 const char *column_name)
{
	int i;

	for (i = 0; i < model->details->columns->len; i++) {
		NautilusColumn *column;
		char *name;
		
		column = model->details->columns->pdata[i];

		g_object_get (G_OBJECT (column), "name", &name, NULL);

		if (!strcmp (name, column_name)) {
			g_free (name);
			return FM_LIST_MODEL_NUM_COLUMNS + i;
		}
		g_free (name);
	}

	return -1;
}

static void
fm_list_model_dispose (GObject *object)
{
	FMListModel *model;
	int i;

	model = FM_LIST_MODEL (object);

	if (model->details->columns) {
		for (i = 0; i < model->details->columns->len; i++) {
			g_object_unref (model->details->columns->pdata[i]);
		}
		g_ptr_array_free (model->details->columns, TRUE);
		model->details->columns = NULL;
	}

	if (model->details->files) {
		g_sequence_free (model->details->files);
		model->details->files = NULL;
	}
	
	if (model->details->top_reverse_map) {
		g_hash_table_destroy (model->details->top_reverse_map);
		model->details->top_reverse_map = NULL;
	}
	if (model->details->directory_reverse_map) {
		g_hash_table_destroy (model->details->directory_reverse_map);
		model->details->directory_reverse_map = NULL;
	}
	
	EEL_CALL_PARENT (G_OBJECT_CLASS, dispose, (object));
}

static void
fm_list_model_finalize (GObject *object)
{
	FMListModel *model;

	model = FM_LIST_MODEL (object);

	g_free (model->details);
	
	EEL_CALL_PARENT (G_OBJECT_CLASS, finalize, (object));
}

static void
fm_list_model_init (FMListModel *model)
{
	model->details = g_new0 (FMListModelDetails, 1);
	model->details->files = g_sequence_new ((GDestroyNotify)file_entry_free);
	model->details->top_reverse_map = g_hash_table_new (g_direct_hash, g_direct_equal);
	model->details->directory_reverse_map = g_hash_table_new (g_direct_hash, g_direct_equal);
	model->details->stamp = g_random_int ();
	model->details->sort_attribute = 0;
	model->details->columns = g_ptr_array_new ();
}

static void
fm_list_model_class_init (FMListModelClass *klass)
{
	GObjectClass *object_class;

	attribute_name_q = g_quark_from_static_string ("name");
	attribute_modification_date_q = g_quark_from_static_string ("modification_date");
	attribute_date_modified_q = g_quark_from_static_string ("date_modified");
	
	object_class = (GObjectClass *)klass;
	parent_class = g_type_class_peek_parent (klass);

	object_class->finalize = fm_list_model_finalize;
	object_class->dispose = fm_list_model_dispose;

      list_model_signals[SUBDIRECTORY_UNLOADED] =
        g_signal_new ("subdirectory_unloaded",
                      FM_TYPE_LIST_MODEL,
                      G_SIGNAL_RUN_FIRST,
                      G_STRUCT_OFFSET (FMListModelClass, subdirectory_unloaded),
                      NULL, NULL,
                      g_cclosure_marshal_VOID__OBJECT,
                      G_TYPE_NONE, 1,
                      NAUTILUS_TYPE_DIRECTORY);
}

static void
fm_list_model_tree_model_init (GtkTreeModelIface *iface)
{
	iface->get_flags = fm_list_model_get_flags;
	iface->get_n_columns = fm_list_model_get_n_columns;
	iface->get_column_type = fm_list_model_get_column_type;
	iface->get_iter = fm_list_model_get_iter;
	iface->get_path = fm_list_model_get_path;
	iface->get_value = fm_list_model_get_value;
	iface->iter_next = fm_list_model_iter_next;
	iface->iter_children = fm_list_model_iter_children;
	iface->iter_has_child = fm_list_model_iter_has_child;
	iface->iter_n_children = fm_list_model_iter_n_children;
	iface->iter_nth_child = fm_list_model_iter_nth_child;
	iface->iter_parent = fm_list_model_iter_parent;
}

static void
fm_list_model_sortable_init (GtkTreeSortableIface *iface)
{
	iface->get_sort_column_id = fm_list_model_get_sort_column_id;
	iface->set_sort_column_id = fm_list_model_set_sort_column_id;
	iface->has_default_sort_func = fm_list_model_has_default_sort_func;
}

static void
fm_list_model_multi_drag_source_init (EggTreeMultiDragSourceIface *iface)
{
	iface->row_draggable = fm_list_model_multi_row_draggable;
	iface->drag_data_get = fm_list_model_multi_drag_data_get;
	iface->drag_data_delete = fm_list_model_multi_drag_data_delete;
}

GType
fm_list_model_get_type (void)
{
	static GType object_type = 0;

	if (object_type == 0) {
		const GTypeInfo object_info = {
			sizeof (FMListModelClass),
			NULL,		/* base_init */
			NULL,		/* base_finalize */
			(GClassInitFunc) fm_list_model_class_init,
			NULL,		/* class_finalize */
			NULL,		/* class_data */
			sizeof (FMListModel),
			0,
			(GInstanceInitFunc) fm_list_model_init,
		};

		const GInterfaceInfo tree_model_info = {
			(GInterfaceInitFunc) fm_list_model_tree_model_init,
			NULL,
			NULL
		};

		const GInterfaceInfo sortable_info = {
			(GInterfaceInitFunc) fm_list_model_sortable_init,
			NULL,
			NULL
		};

		const GInterfaceInfo multi_drag_source_info = {
			(GInterfaceInitFunc) fm_list_model_multi_drag_source_init,
			NULL,
			NULL
		};
		
		object_type = g_type_register_static (G_TYPE_OBJECT, "FMListModel", &object_info, 0);
		g_type_add_interface_static (object_type,
					     GTK_TYPE_TREE_MODEL,
					     &tree_model_info);
		g_type_add_interface_static (object_type,
					     GTK_TYPE_TREE_SORTABLE,
					     &sortable_info);
		g_type_add_interface_static (object_type,
					     EGG_TYPE_TREE_MULTI_DRAG_SOURCE,
					     &multi_drag_source_info);
	}
	
	return object_type;
}

void
fm_list_model_subdirectory_done_loading (FMListModel *model, NautilusDirectory *directory)
{
	GtkTreeIter iter;
	GtkTreePath *path;
	FileEntry *file_entry, *dummy_entry;
	GSequenceIter *parent_ptr, *dummy_ptr;
	GSequence *files;
	
	if (model == NULL || model->details->directory_reverse_map == NULL) {
		return;
	}
	parent_ptr = g_hash_table_lookup (model->details->directory_reverse_map,
					  directory);
	if (parent_ptr == NULL) {
		return;
	}
	
	file_entry = g_sequence_get (parent_ptr);
	files = file_entry->files;

	/* Only swap loading -> empty if we saw no files yet at "done",
	 * otherwise, toggle loading at first added file to the model.
	 */
	if (!nautilus_directory_is_not_empty (directory) &&
	    g_sequence_get_length (files) == 1) {
		dummy_ptr = g_sequence_get_iter_at_pos (file_entry->files, 0);
		dummy_entry = g_sequence_get (dummy_ptr);
		if (dummy_entry->file == NULL) {
			/* was the dummy file */
			file_entry->loaded = 1;
			
			iter.stamp = model->details->stamp;
			iter.user_data = dummy_ptr;
			
			path = gtk_tree_model_get_path (GTK_TREE_MODEL (model), &iter);
			gtk_tree_model_row_changed (GTK_TREE_MODEL (model), path, &iter);
			gtk_tree_path_free (path);
		}
	}
}
