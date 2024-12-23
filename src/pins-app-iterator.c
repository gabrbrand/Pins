/* pins-app-iterator.c
 *
 * Copyright 2024 Fabrizio
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <https://www.gnu.org/licenses/>.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "pins-app-iterator.h"

#include "pins-desktop-file.h"
#include "pins-directories.h"
#include "pins-locale-utils-private.h"

#define DIR_LIST_FILE_ATTRIBUTES                                              \
    g_strjoin (",", G_FILE_ATTRIBUTE_STANDARD_CONTENT_TYPE,                   \
               G_FILE_ATTRIBUTE_STANDARD_DISPLAY_NAME,                        \
               G_FILE_ATTRIBUTE_STANDARD_EDIT_NAME, NULL)

struct _PinsAppIterator
{
    GObject parent_instance;

    gchar **duplicates;
    gchar **unique_filenames;
    gboolean just_created_file;
    GListModel *model;
};

static void list_model_iface_init (GListModelInterface *iface);

G_DEFINE_TYPE_WITH_CODE (PinsAppIterator, pins_app_iterator, G_TYPE_OBJECT,
                         G_IMPLEMENT_INTERFACE (G_TYPE_LIST_MODEL,
                                                list_model_iface_init))

enum
{
    LOADING,
    FILE_CREATED,
    N_SIGNALS
};

static guint signals[N_SIGNALS];

PinsAppIterator *
pins_app_iterator_new (void)
{
    return g_object_new (PINS_TYPE_APP_ITERATOR, NULL);
}

void
pins_app_iterator_create_user_file (PinsAppIterator *self, gchar *basename,
                                    gchar *suffix, GError **error)
{
    gchar increment[8] = "";
    gchar *filename;
    GFile *file;
    GError *err = NULL;

    if (self->just_created_file)
        return;

    for (int i = 0; i < 999999; i++)
        {
            if (i > 0)
                sprintf (increment, "-%d", i);

            filename = g_strconcat (basename, increment, suffix, NULL);
            if (!g_strv_contains ((const gchar **)self->unique_filenames,
                                  filename))
                {
                    break;
                }
        }

    file = g_file_new_for_path (
        g_strconcat (pins_user_app_path (), "/", filename, NULL));
    g_file_create (file, G_FILE_CREATE_NONE, NULL, &err);
    if (err != NULL)
        {
            g_warning ("Could not create new file `%s`",
                       g_file_get_path (file));
            g_propagate_error (error, err);
            return;
        }

    self->just_created_file = TRUE;
}

static void
pins_app_iterator_update_duplicates (GListModel *model, guint position,
                                     guint removed, guint added,
                                     gpointer user_data)
{
    PinsAppIterator *self = PINS_APP_ITERATOR (user_data);
    GStrvBuilder *strv_builder = g_strv_builder_new ();

    gchar *unique_filenames[g_list_model_get_n_items (model) + 1] = {};
    gsize n_unique_filenames = 0;

    for (int i = 0; i < g_list_model_get_n_items (model); i++)
        {
            GFileInfo *file_info
                = G_FILE_INFO (g_list_model_get_item (model, i));
            GFile *file = G_FILE (g_file_info_get_attribute_object (
                file_info, "standard::file"));

            if (g_strv_contains ((const gchar *const *)unique_filenames,
                                 g_file_info_get_display_name (file_info)))
                {
                    g_strv_builder_add (strv_builder, g_file_get_path (file));
                }
            else
                {
                    unique_filenames[n_unique_filenames]
                        = (gchar *)g_file_info_get_display_name (file_info);
                    n_unique_filenames++;
                }
        }

    self->duplicates = g_strv_builder_end (strv_builder);
    self->unique_filenames = g_strdupv (unique_filenames);
}

gboolean
pins_app_iterator_filter_match_func (gpointer file_info, gpointer user_data)
{
    gboolean is_desktop_file, is_duplicate = FALSE;
    PinsAppIterator *self = PINS_APP_ITERATOR (user_data);
    GFile *file = G_FILE (
        g_file_info_get_attribute_object (file_info, "standard::file"));
    gchar **split_path = g_strsplit (g_file_get_path (file), ".", -1);

    is_desktop_file
        = g_strcmp0 (g_strconcat (".",
                                  split_path[g_strv_length (split_path) - 1],
                                  NULL),
                     DESKTOP_FILE_SUFFIX)
          == 0;
    is_duplicate = g_strv_contains ((const gchar *const *)self->duplicates,
                                    g_file_get_path (file));

    return is_desktop_file && !is_duplicate;
}

void
desktop_file_key_set_cb (PinsDesktopFile *desktop_file, gchar *key,
                         GtkSorter *sorter)
{
    if (g_strcmp0 (key, G_KEY_FILE_DESKTOP_KEY_NAME) == 0)
        {
            gtk_sorter_changed (sorter, GTK_SORTER_CHANGE_DIFFERENT);
        }
}

gpointer
pins_app_iterator_map_func (gpointer file_info, gpointer sorter)
{
    PinsDesktopFile *desktop_file;
    GError *err = NULL;
    GFile *file;

    g_assert (G_IS_FILE_INFO (file_info));

    file = G_FILE (
        g_file_info_get_attribute_object (file_info, "standard::file"));

    desktop_file = pins_desktop_file_new_from_file (file, &err);
    if (err != NULL)
        {
            g_warning ("Could not load desktop file at `%s`",
                       g_file_get_path (file));
        }

    g_signal_connect_object (desktop_file, "key-set",
                             G_CALLBACK (desktop_file_key_set_cb), sorter, 0);

    return desktop_file;
}

int
pins_app_iterator_sort_compare_func (gconstpointer a, gconstpointer b,
                                     gpointer user_data)
{
    PinsDesktopFile *first = PINS_DESKTOP_FILE ((gpointer)a);
    PinsDesktopFile *second = PINS_DESKTOP_FILE ((gpointer)b);
    const gchar *first_key, *second_key, *first_name, *second_name;

    g_assert (PINS_IS_DESKTOP_FILE (first));
    g_assert (PINS_IS_DESKTOP_FILE (second));

    first_key = _pins_join_key_locale (
        G_KEY_FILE_DESKTOP_KEY_NAME, pins_desktop_file_get_locale_for_key (
                                         first, G_KEY_FILE_DESKTOP_KEY_NAME));
    first_name = pins_desktop_file_get_string (first, first_key, NULL);

    second_key = _pins_join_key_locale (
        G_KEY_FILE_DESKTOP_KEY_NAME, pins_desktop_file_get_locale_for_key (
                                         second, G_KEY_FILE_DESKTOP_KEY_NAME));
    second_name = pins_desktop_file_get_string (second, second_key, NULL);

    return g_strcmp0 (first_name, second_name);
}

void
pins_app_iterator_filter_pending_changed_cb (GtkFilterListModel *model,
                                             guint amount,
                                             PinsAppIterator *self)
{
    g_assert (GTK_IS_FILTER_LIST_MODEL (model));
    g_assert (PINS_IS_APP_ITERATOR (self));

    g_signal_emit (self, signals[LOADING], 0,
                   gtk_filter_list_model_get_pending (model) != 0);
}

void
desktop_file_model_items_changed_cb (GListModel *model, guint position,
                                     guint removed, guint added,
                                     PinsAppIterator *self)
{
    if (added > 0 && self->just_created_file)
        {
            PinsDesktopFile *desktop_file
                = PINS_DESKTOP_FILE (g_list_model_get_item (model, position));

            pins_desktop_file_set_default (desktop_file);
            g_signal_emit (self, signals[FILE_CREATED], 0, desktop_file);
            self->just_created_file = FALSE;
        }

    g_list_model_items_changed (G_LIST_MODEL (self), position, removed, added);
}

void
pins_app_iterator_set_directory_list (PinsAppIterator *self,
                                      GListModel *dir_list)
{
    GtkFilterListModel *filter_model;
    GtkMapListModel *map_model;
    GtkSorter *sorter = GTK_SORTER (gtk_custom_sorter_new (
        pins_app_iterator_sort_compare_func, NULL, NULL));
    GtkSortListModel *sort_model;

    /// TODO: Deleting newly created files does not update list
    g_signal_connect_object (G_LIST_MODEL (dir_list), "items-changed",
                             G_CALLBACK (pins_app_iterator_update_duplicates),
                             self, 0);

    filter_model = gtk_filter_list_model_new (
        G_LIST_MODEL (dir_list),
        GTK_FILTER (gtk_custom_filter_new (
            &pins_app_iterator_filter_match_func, self, NULL)));
    gtk_filter_list_model_set_incremental (filter_model, TRUE);

    g_signal_connect_object (
        filter_model, "notify::pending",
        G_CALLBACK (pins_app_iterator_filter_pending_changed_cb), self, 0);

    map_model
        = gtk_map_list_model_new (G_LIST_MODEL (filter_model),
                                  &pins_app_iterator_map_func, sorter, NULL);

    /// TODO: call gtk_sorter_changed() when filenames change
    sort_model = gtk_sort_list_model_new (G_LIST_MODEL (map_model), sorter);

    g_signal_connect_object (G_LIST_MODEL (sort_model), "items-changed",
                             G_CALLBACK (desktop_file_model_items_changed_cb),
                             self, 0);

    self->model = G_LIST_MODEL (sort_model);
}

void
pins_app_iterator_set_paths (PinsAppIterator *self, gchar **paths)
{
    GListStore *dir_list_store;
    GtkFlattenListModel *flattened_dir_list;

    dir_list_store = g_list_store_new (GTK_TYPE_DIRECTORY_LIST);

    for (int i = 0; i < g_strv_length (paths); i++)
        {
            GFile *file = g_file_new_for_path (paths[i]);
            GtkDirectoryList *dir_list
                = gtk_directory_list_new (DIR_LIST_FILE_ATTRIBUTES, file);

            g_list_store_append (dir_list_store, dir_list);
        }

    flattened_dir_list
        = gtk_flatten_list_model_new (G_LIST_MODEL (dir_list_store));

    pins_app_iterator_set_directory_list (self,
                                          G_LIST_MODEL (flattened_dir_list));
}

static void
pins_app_iterator_class_init (PinsAppIteratorClass *klass)
{
    signals[LOADING] = g_signal_new ("loading", G_TYPE_FROM_CLASS (klass),
                                     G_SIGNAL_RUN_FIRST, 0, NULL, NULL, NULL,
                                     G_TYPE_NONE, 1, G_TYPE_BOOLEAN);

    signals[FILE_CREATED] = g_signal_new (
        "file-created", G_TYPE_FROM_CLASS (klass), G_SIGNAL_RUN_FIRST, 0, NULL,
        NULL, NULL, G_TYPE_NONE, 1, G_TYPE_OBJECT);
}

static void
pins_app_iterator_init (PinsAppIterator *self)
{
    self->just_created_file = FALSE;
}

gpointer
pins_app_iterator_get_item (GListModel *list, guint position)
{
    PinsAppIterator *self = PINS_APP_ITERATOR (list);

    return g_list_model_get_item (self->model, position);
}

GType
pins_app_iterator_get_item_type (GListModel *list)
{
    return PINS_TYPE_DESKTOP_FILE;
}

guint
pins_app_iterator_get_n_items (GListModel *list)
{
    PinsAppIterator *self = PINS_APP_ITERATOR (list);

    return g_list_model_get_n_items (self->model);
}

static void
list_model_iface_init (GListModelInterface *iface)
{
    iface->get_item = pins_app_iterator_get_item;
    iface->get_item_type = pins_app_iterator_get_item_type;
    iface->get_n_items = pins_app_iterator_get_n_items;
}
