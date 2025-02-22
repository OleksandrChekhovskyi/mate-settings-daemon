/* -*- Mode: C; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 8 -*-
 *
 * Copyright (C) 2007 William Jon McCann <mccann@jhu.edu>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1301, USA.
 *
 */

#include "config.h"

#include <sys/types.h>
#include <sys/wait.h>
#include <stdlib.h>
#include <stdio.h>
#include <unistd.h>
#include <string.h>
#include <errno.h>
#include <signal.h>

#include <locale.h>

#include <glib.h>
#include <glib/gi18n.h>
#include <gdk/gdk.h>
#include <gdk/gdkx.h>
#include <gtk/gtk.h>
#include <gio/gio.h>

#include "mate-settings-profile.h"
#include "msd-typing-break-manager.h"

#define MATE_BREAK_SCHEMA "org.mate.typing-break"

struct _MsdTypingBreakManager
{
        GObject    parent;

        GPid       typing_monitor_pid;
        guint      typing_monitor_idle_id;
        guint      child_watch_id;
        guint      setup_id;
        GSettings *settings;
};

static void msd_typing_break_manager_finalize (GObject *object);

G_DEFINE_TYPE (MsdTypingBreakManager, msd_typing_break_manager, G_TYPE_OBJECT)

static gpointer manager_object = NULL;

static gboolean
typing_break_timeout (MsdTypingBreakManager *manager)
{
        if (manager->typing_monitor_pid > 0) {
                kill (manager->typing_monitor_pid, SIGKILL);
        }

        manager->typing_monitor_idle_id = 0;

        return FALSE;
}

static void
child_watch (GPid                   pid,
             int                    status,
             MsdTypingBreakManager *manager)
{
        if (pid == manager->typing_monitor_pid) {
                manager->typing_monitor_pid = 0;
                g_spawn_close_pid (pid);
        }
}

static void
setup_typing_break (MsdTypingBreakManager *manager,
                    gboolean               enabled)
{
        mate_settings_profile_start (NULL);

        if (! enabled) {
                if (manager->typing_monitor_pid != 0) {
                        manager->typing_monitor_idle_id = g_timeout_add_seconds (3, (GSourceFunc) typing_break_timeout, manager);
                }
                return;
        }

        if (manager->typing_monitor_idle_id != 0) {
                g_source_remove (manager->typing_monitor_idle_id);
                manager->typing_monitor_idle_id = 0;
        }

        if (manager->typing_monitor_pid == 0) {
                GError  *error;
                char    *argv[] = { "mate-typing-monitor", "-n", NULL };
                gboolean res;

                error = NULL;
                res = g_spawn_async ("/",
                                     argv,
                                     NULL,
                                     G_SPAWN_STDOUT_TO_DEV_NULL
                                     | G_SPAWN_STDERR_TO_DEV_NULL
                                     | G_SPAWN_SEARCH_PATH
                                     | G_SPAWN_DO_NOT_REAP_CHILD,
                                     NULL,
                                     NULL,
                                     &manager->typing_monitor_pid,
                                     &error);
                if (! res) {
                        /* FIXME: put up a warning */
                        g_warning ("failed: %s\n", error->message);
                        g_error_free (error);
                        manager->typing_monitor_pid = 0;
                        return;
                }

                manager->child_watch_id = g_child_watch_add (manager->typing_monitor_pid,
                                                             (GChildWatchFunc)child_watch,
                                                             manager);
        }

        mate_settings_profile_end (NULL);
}

static void
typing_break_enabled_callback (GSettings             *settings,
                               gchar                 *key,
                               MsdTypingBreakManager *manager)
{
        setup_typing_break (manager, g_settings_get_boolean (settings, key));
}

static gboolean
really_setup_typing_break (MsdTypingBreakManager *manager)
{
        setup_typing_break (manager, TRUE);
        manager->setup_id = 0;
        return FALSE;
}

gboolean
msd_typing_break_manager_start (MsdTypingBreakManager *manager,
                                GError               **error)
{
        gboolean     enabled;

        g_debug ("Starting typing_break manager");
        mate_settings_profile_start (NULL);

        manager->settings = g_settings_new (MATE_BREAK_SCHEMA);

        g_signal_connect (manager->settings,
                          "changed::enabled",
                          G_CALLBACK (typing_break_enabled_callback),
                          manager);

        enabled = g_settings_get_boolean (manager->settings, "enabled");

        if (enabled) {
                manager->setup_id =
                        g_timeout_add_seconds (3,
                                               (GSourceFunc) really_setup_typing_break,
                                               manager);
        }

        mate_settings_profile_end (NULL);

        return TRUE;
}

void
msd_typing_break_manager_stop (MsdTypingBreakManager *manager)
{
        g_debug ("Stopping typing_break manager");

        if (manager->setup_id != 0) {
                g_source_remove (manager->setup_id);
                manager->setup_id = 0;
        }

        if (manager->child_watch_id != 0) {
                g_source_remove (manager->child_watch_id);
                manager->child_watch_id = 0;
        }

        if (manager->typing_monitor_idle_id != 0) {
                g_source_remove (manager->typing_monitor_idle_id);
                manager->typing_monitor_idle_id = 0;
        }

        if (manager->typing_monitor_pid > 0) {
                kill (manager->typing_monitor_pid, SIGKILL);
                g_spawn_close_pid (manager->typing_monitor_pid);
                manager->typing_monitor_pid = 0;
        }

        if (manager->settings != NULL) {
                g_object_unref (manager->settings);
        }
}

static void
msd_typing_break_manager_class_init (MsdTypingBreakManagerClass *klass)
{
        GObjectClass   *object_class = G_OBJECT_CLASS (klass);

        object_class->finalize = msd_typing_break_manager_finalize;
}

static void
msd_typing_break_manager_init (MsdTypingBreakManager *manager)
{
}

static void
msd_typing_break_manager_finalize (GObject *object)
{
        g_return_if_fail (object != NULL);
        g_return_if_fail (MSD_IS_TYPING_BREAK_MANAGER (object));

        G_OBJECT_CLASS (msd_typing_break_manager_parent_class)->finalize (object);
}

MsdTypingBreakManager *
msd_typing_break_manager_new (void)
{
        if (manager_object != NULL) {
                g_object_ref (manager_object);
        } else {
                manager_object = g_object_new (MSD_TYPE_TYPING_BREAK_MANAGER, NULL);
                g_object_add_weak_pointer (manager_object,
                                           (gpointer *) &manager_object);
        }

        return MSD_TYPING_BREAK_MANAGER (manager_object);
}
