/* -*- c-basic-offset: 4; -*- */
// Original code taken from the example webkit-gtk+ application. see notice below.
// Modified code is licensed under the GPL 3.  See LICENSE file.


/*
 * Copyright (C) 2006, 2007 Apple Inc.
 * Copyright (C) 2007 Alp Toker <alp@atoker.com>
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY APPLE COMPUTER, INC. ``AS IS'' AND ANY
 * EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR
 * PURPOSE ARE DISCLAIMED.  IN NO EVENT SHALL APPLE COMPUTER, INC. OR
 * CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL,
 * EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO,
 * PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR
 * PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY
 * OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
 * OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */


#define LENGTH(x) (sizeof x / sizeof x[0])
#define MAX_BINDINGS 256

#include <gtk/gtk.h>
#include <gdk/gdkx.h>
#include <gdk/gdkkeysyms.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/un.h>
#include <sys/utsname.h>
#include <webkit/webkit.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <stdlib.h>
#include <errno.h>
#include <string.h>
#include <fcntl.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <libsoup/soup.h>
#include <signal.h>
#include "uzbl.h"


static Uzbl uzbl;


/* commandline arguments (set initial values for the state variables) */
static GOptionEntry entries[] =
{
    { "uri",     'u', 0, G_OPTION_ARG_STRING, &uzbl.state.uri,           "Uri to load", "URI" },
    { "name",    'n', 0, G_OPTION_ARG_STRING, &uzbl.state.instance_name, "Name of the current instance", "NAME" },
    { "config",  'c', 0, G_OPTION_ARG_STRING, &uzbl.state.config_file,   "Config file", "FILE" },
    { NULL,      0, 0, 0, NULL, NULL, NULL }
};

typedef void (*Command)(WebKitWebView*, const char *);

/* XDG stuff */
static char *XDG_CONFIG_HOME_default[256];
static char *XDG_CONFIG_DIRS_default = "/etc/xdg";


/* --- UTILITY FUNCTIONS --- */

char *
itos(int val) {
    char tmp[20];

    snprintf(tmp, sizeof(tmp), "%i", val);
    return g_strdup(tmp);
}

static char *
str_replace (const char* search, const char* replace, const char* string) {
    return g_strjoinv (replace, g_strsplit(string, search, -1));
}

static sigfunc*
setup_signal(int signr, sigfunc *shandler) {
    struct sigaction nh, oh;

    nh.sa_handler = shandler;
    sigemptyset(&nh.sa_mask);
    nh.sa_flags = 0;

    if(sigaction(signr, &nh, &oh) < 0)
        return SIG_ERR;

    return NULL;
}

static void
clean_up(void) {
    if (uzbl.behave.fifo_dir)
        unlink (uzbl.comm.fifo_path);
    if (uzbl.behave.socket_dir)
        unlink (uzbl.comm.socket_path);

    g_string_free(uzbl.state.keycmd, TRUE);
    g_hash_table_destroy(uzbl.bindings);
    g_hash_table_destroy(uzbl.behave.commands);
}


/* --- SIGNAL HANDLER --- */

static void
catch_sigterm(int s) {
    (void) s;
    clean_up();
}

/* --- CALLBACKS --- */

static gboolean
new_window_cb (WebKitWebView *web_view, WebKitWebFrame *frame, WebKitNetworkRequest *request, WebKitWebNavigationAction *navigation_action, WebKitWebPolicyDecision *policy_decision, gpointer user_data) {
    (void) web_view;
    (void) frame;
    (void) navigation_action;
    (void) policy_decision;
    (void) user_data;
    const gchar* uri = webkit_network_request_get_uri (request);
    printf("New window requested -> %s \n", uri);
    new_window_load_uri(uri);
    return (FALSE);
}

WebKitWebView*
create_web_view_cb (WebKitWebView  *web_view, WebKitWebFrame *frame, gpointer user_data) {
    (void) web_view;
    (void) frame;
    (void) user_data;
    if (uzbl.state.selected_url[0]!=0) {
        printf("\nNew web view -> %s\n",uzbl.state.selected_url);
        new_window_load_uri(uzbl.state.selected_url);
    } else {
        printf("New web view -> %s\n","Nothing to open, exiting");
    }
    return (NULL);
}

static gboolean
download_cb (WebKitWebView *web_view, GObject *download, gpointer user_data) {
    (void) web_view;
    (void) user_data;
    if (uzbl.behave.download_handler) {
        const gchar* uri = webkit_download_get_uri ((WebKitDownload*)download);
        printf("Download -> %s\n",uri);
        run_command_async(uzbl.behave.download_handler, uri);
    }
    return (FALSE);
}

/* scroll a bar in a given direction */
static void
scroll (GtkAdjustment* bar, const char *param) {
    gdouble amount;
    gchar *end;

    amount = g_ascii_strtod(param, &end);

    if (*end)
        fprintf(stderr, "found something after double: %s\n", end);

    gtk_adjustment_set_value (bar, gtk_adjustment_get_value(bar)+amount);
}

static void scroll_vert(WebKitWebView* page, const char *param) {
    (void) page;

    scroll(uzbl.gui.bar_v, param);
}

static void scroll_horz(WebKitWebView* page, const char *param) {
    (void) page;

    scroll(uzbl.gui.bar_h, param);
}

static void
toggle_status_cb (WebKitWebView* page, const char *param) {
    (void)page;
    (void)param;

    if (uzbl.behave.show_status) {
        gtk_widget_hide(uzbl.gui.mainbar);
    } else {
        gtk_widget_show(uzbl.gui.mainbar);
    }
    uzbl.behave.show_status = !uzbl.behave.show_status;
    update_title();
}

static void
link_hover_cb (WebKitWebView* page, const gchar* title, const gchar* link, gpointer data) {
    (void) page;
    (void) title;
    (void) data;    
    //ADD HOVER URL TO WINDOW TITLE
    uzbl.state.selected_url[0] = '\0';
    if (link) {
        strcpy (uzbl.state.selected_url, link);
    }
    update_title();
}

static void
title_change_cb (WebKitWebView* web_view, WebKitWebFrame* web_frame, const gchar* title, gpointer data) {
    (void) web_view;
    (void) web_frame;
    (void) data;
    if (uzbl.gui.main_title)
        g_free (uzbl.gui.main_title);
    uzbl.gui.main_title = g_strdup (title);
    update_title();
}

static void
progress_change_cb (WebKitWebView* page, gint progress, gpointer data) {
    (void) page;
    (void) data;
    uzbl.gui.sbar.load_progress = progress;
    update_title();
}

static void
load_commit_cb (WebKitWebView* page, WebKitWebFrame* frame, gpointer data) {
    (void) page;
    (void) data;
    free (uzbl.state.uri);
    GString* newuri = g_string_new (webkit_web_frame_get_uri (frame));
    uzbl.state.uri = g_string_free (newuri, FALSE);
}

static void
destroy_cb (GtkWidget* widget, gpointer data) {
    (void) widget;
    (void) data;
    gtk_main_quit ();
}

static void
log_history_cb () {
   if (uzbl.behave.history_handler) {
       time_t rawtime;
       struct tm * timeinfo;
       char date [80];
       time ( &rawtime );
       timeinfo = localtime ( &rawtime );
       strftime (date, 80, "%Y-%m-%d %H:%M:%S", timeinfo);
       GString* args = g_string_new ("");
       g_string_printf (args, "'%s'", date);
       run_command_async(uzbl.behave.history_handler, args->str);
       g_string_free (args, TRUE);
   }
}


/* VIEW funcs (little webkit wrappers) */
#define VIEWFUNC(name) static void view_##name(WebKitWebView *page, const char *param){(void)param; webkit_web_view_##name(page);}
VIEWFUNC(reload)
VIEWFUNC(reload_bypass_cache)
VIEWFUNC(stop_loading)
VIEWFUNC(zoom_in)
VIEWFUNC(zoom_out)
VIEWFUNC(go_back)
VIEWFUNC(go_forward)
#undef VIEWFUNC

/* -- command to callback/function map for things we cannot attach to any signals */
// TODO: reload

static struct {char *name; Command command;} cmdlist[] =
{
    { "back",             view_go_back            },
    { "forward",          view_go_forward         },
    { "scroll_vert",      scroll_vert             },
    { "scroll_horz",      scroll_horz             },
    { "reload",           view_reload,            }, 
    { "reload_ign_cache", view_reload_bypass_cache},
    { "stop",             view_stop_loading,      },
    { "zoom_in",          view_zoom_in,           }, //Can crash (when max zoom reached?).
    { "zoom_out",         view_zoom_out,          },
    { "uri",              load_uri                },
    { "script",           run_js                  },
    { "toggle_status",    toggle_status_cb        },
    { "spawn",            spawn                   },
    { "exit",             close_uzbl              },
    { "search",           search_text             },
    { "insert_mode",      set_insert_mode         }
};

static void
commands_hash(void)
{
    unsigned int i;
    uzbl.behave.commands = g_hash_table_new(g_str_hash, g_str_equal);

    for (i = 0; i < LENGTH(cmdlist); i++)
        g_hash_table_insert(uzbl.behave.commands, cmdlist[i].name, cmdlist[i].command);
}

/* -- CORE FUNCTIONS -- */

void
free_action(gpointer act) {
    Action *action = (Action*)act;
    g_free(action->name);
    if (action->param)
        g_free(action->param);
    g_free(action);
}

Action*
new_action(const gchar *name, const gchar *param) {
    Action *action = g_new(Action, 1);

    action->name = g_strdup(name);
    if (param)
        action->param = g_strdup(param);
    else
        action->param = NULL;

    return action;
}

static bool
file_exists (const char * filename) {
    FILE *file = fopen (filename, "r");
    if (file) {
        fclose (file);
        return true;
    }
    return false;
}

void
set_insert_mode(WebKitWebView *page, const gchar *param) {
    (void)page;
    (void)param;

    uzbl.behave.insert_mode = TRUE;
    update_title();
}

static void
load_uri (WebKitWebView * web_view, const gchar *param) {
    if (param) {
        GString* newuri = g_string_new (param);
        if (g_strrstr (param, "://") == NULL)
            g_string_prepend (newuri, "http://");
		/* if we do handle cookies, ask our handler for them */
        webkit_web_view_load_uri (web_view, newuri->str);
        g_string_free (newuri, TRUE);
    }
}

static void
run_js (WebKitWebView * web_view, const gchar *param) {
    if (param)
        webkit_web_view_execute_script (web_view, param);
}

static void
search_text (WebKitWebView *page, const char *param) {
    if ((param) && (param[0] != '\0')) {
        strcpy(uzbl.state.searchtx, param);
    }
    if (uzbl.state.searchtx[0] != '\0') {
        printf ("Searching: %s\n", uzbl.state.searchtx);
        webkit_web_view_unmark_text_matches (page);
        webkit_web_view_mark_text_matches (page, uzbl.state.searchtx, FALSE, 0);
        webkit_web_view_set_highlight_text_matches (page, TRUE);
        webkit_web_view_search_text (page, uzbl.state.searchtx, FALSE, TRUE, TRUE);
    }
}

static void
new_window_load_uri (const gchar * uri) {
    GString* to_execute = g_string_new ("");
    g_string_append_printf (to_execute, "%s --uri '%s'", uzbl.state.executable_path, uri);
    int i;
    for (i = 0; entries[i].long_name != NULL; i++) {
        if ((entries[i].arg == G_OPTION_ARG_STRING) && (strcmp(entries[i].long_name,"uri")!=0)) {
            gchar** str = (gchar**)entries[i].arg_data;
            if (*str!=NULL) {
                g_string_append_printf (to_execute, " --%s '%s'", entries[i].long_name, *str);
            }
        }
    }
    printf("\n%s\n", to_execute->str);
    g_spawn_command_line_async (to_execute->str, NULL);
    g_string_free (to_execute, TRUE);
}

static void
close_uzbl (WebKitWebView *page, const char *param) {
    (void)page;
    (void)param;
    gtk_main_quit ();
}

/* --Statusbar functions-- */
static char*
build_progressbar_ascii(int percent) {
   int width=10;
   int i;
   double l;
   GString *bar = g_string_new("");

   l = (double)percent*((double)width/100.);
   l = (int)(l+.5)>=(int)l ? l+.5 : l;

   g_string_append(bar, "[");
   for(i=0; i<(int)l; i++)
       g_string_append(bar, "=");
          
   for(; i<width; i++)
       g_string_append(bar, "·");
   g_string_append(bar, "]");

   return g_string_free(bar, FALSE);
}

static void
setup_scanner() {
     const GScannerConfig scan_config = {
             (
              "\t\r\n"
             )            /* cset_skip_characters */,
             (
              G_CSET_a_2_z
              "_#"
              G_CSET_A_2_Z
             )            /* cset_identifier_first */,
             (
              G_CSET_a_2_z
              "_0123456789"
              G_CSET_A_2_Z
              G_CSET_LATINS
              G_CSET_LATINC
             )            /* cset_identifier_nth */,
             ( "" )    /* cpair_comment_single */,

             TRUE         /* case_sensitive */,

             FALSE        /* skip_comment_multi */,
             FALSE        /* skip_comment_single */,
             FALSE        /* scan_comment_multi */,
             TRUE         /* scan_identifier */,
             TRUE         /* scan_identifier_1char */,
             FALSE        /* scan_identifier_NULL */,
             TRUE         /* scan_symbols */,
             FALSE        /* scan_binary */,
             FALSE        /* scan_octal */,
             FALSE        /* scan_float */,
             FALSE        /* scan_hex */,
             FALSE        /* scan_hex_dollar */,
             FALSE        /* scan_string_sq */,
             FALSE        /* scan_string_dq */,
             TRUE         /* numbers_2_int */,
             FALSE        /* int_2_float */,
             FALSE        /* identifier_2_string */,
             FALSE        /* char_2_token */,
             FALSE        /* symbol_2_token */,
             TRUE         /* scope_0_fallback */,
             FALSE,
             TRUE
     };

     uzbl.scan = g_scanner_new(&scan_config);
     while(symp->symbol_name) {
         g_scanner_scope_add_symbol(uzbl.scan, 0,
                         symp->symbol_name,
                         GINT_TO_POINTER(symp->symbol_token));
         symp++;
     }
}

static gchar *
parse_status_template(const char *template) {
     GTokenType token = G_TOKEN_NONE;
     GString *ret = g_string_new("");
     gchar *buf=NULL;
     int sym;

     if(!template)
         return NULL;

     g_scanner_input_text(uzbl.scan, template, strlen(template));
     while(!g_scanner_eof(uzbl.scan) && token != G_TOKEN_LAST) {
         token = g_scanner_get_next_token(uzbl.scan);

         if(token == G_TOKEN_SYMBOL) {
             sym = (int)g_scanner_cur_value(uzbl.scan).v_symbol;
             switch(sym) {
                 case SYM_URI:
                     g_string_append(ret, 
                         uzbl.state.uri?
                         g_markup_printf_escaped("%s", uzbl.state.uri):"");
                     break;
                 case SYM_LOADPRGS:
                     g_string_append(ret, itos(uzbl.gui.sbar.load_progress));
                     break;
                 case SYM_LOADPRGSBAR:
                     buf = build_progressbar_ascii(uzbl.gui.sbar.load_progress);
                     g_string_append(ret, buf);
                     g_free(buf);
                     break;
                 case SYM_TITLE:
                     g_string_append(ret,
                         uzbl.gui.main_title?
                         g_markup_printf_escaped("%s", uzbl.gui.main_title):"");
                     break;
                 case SYM_NAME:
                     g_string_append(ret, 
                         uzbl.state.instance_name?uzbl.state.instance_name:itos(uzbl.xwin));
                     break;
                 case SYM_KEYCMD:
                     g_string_append(ret, 
                         uzbl.state.keycmd->str ?
                         g_markup_printf_escaped("%s", uzbl.state.keycmd->str):"");
                     break;
                 case SYM_MODE:
                     g_string_append(ret, 
                         uzbl.behave.insert_mode?"[I]":"[C]");
                     break;
                 default:
                     break;
             }
         }
         else if(token == G_TOKEN_INT) {
             g_string_append(ret, itos(g_scanner_cur_value(uzbl.scan).v_int));
         }
         else if(token == G_TOKEN_IDENTIFIER) {
             g_string_append(ret, (gchar *)g_scanner_cur_value(uzbl.scan).v_identifier);
         }
         else if(token == G_TOKEN_CHAR) {
             g_string_append_c(ret, (gchar)g_scanner_cur_value(uzbl.scan).v_char);
         }
     }

     return g_string_free(ret, FALSE);
}
/* --End Statusbar functions-- */


// make sure to put '' around args, so that if there is whitespace we can still keep arguments together.
static gboolean
run_command_async(const char *command, const char *args) {
   //command <uzbl conf> <uzbl pid> <uzbl win id> <uzbl fifo file> <uzbl socket file> [args]
    GString* to_execute = g_string_new ("");
    gboolean result;
    g_string_printf (to_execute, "%s '%s' '%i' '%i' '%s' '%s'", 
                    command, uzbl.state.config_file, (int) getpid() ,
                    (int) uzbl.xwin, uzbl.comm.fifo_path, uzbl.comm.socket_path);
    g_string_append_printf (to_execute, " '%s' '%s'", 
                    uzbl.state.uri, "TODO title here");
    if(args) {
        g_string_append_printf (to_execute, " %s", args);
    }
    result = g_spawn_command_line_async (to_execute->str, NULL);
    printf("Called %s.  Result: %s\n", to_execute->str, (result ? "TRUE" : "FALSE" ));
    g_string_free (to_execute, TRUE);
    return result;
}

static gboolean
run_command_sync(const char *command, const char *args, char **stdout) {
	//command <uzbl conf> <uzbl pid> <uzbl win id> <uzbl fifo file> <uzbl socket file> [args]
    GString* to_execute = g_string_new ("");
    gboolean result;
    g_string_printf (to_execute, "%s '%s' '%i' '%i' '%s' '%s'", command, uzbl.state.config_file, (int) getpid() , (int) uzbl.xwin, uzbl.comm.fifo_path, uzbl.comm.socket_path);
    g_string_append_printf (to_execute, " '%s' '%s'", uzbl.state.uri, "TODO title here");
    if(args) {
        g_string_append_printf (to_execute, " %s", args);
    }
    result = g_spawn_command_line_sync (to_execute->str, stdout, NULL, NULL, NULL);
    printf("Called %s.  Result: %s\n", to_execute->str, (result ? "TRUE" : "FALSE" ));
    g_string_free (to_execute, TRUE);
    return result;
}

static void
spawn(WebKitWebView *web_view, const char *param) {
    (void)web_view;
    run_command_async(param, NULL);
}

static void
parse_command(const char *cmd, const char *param) {
    Command c;

    if ((c = g_hash_table_lookup(uzbl.behave.commands, cmd)))
        c(uzbl.gui.web_view, param);
    else
        fprintf (stderr, "command \"%s\" not understood. ignoring.\n", cmd);
}

static void
parse_line(char *line) {
    gchar **parts;

    g_strstrip(line);

    parts = g_strsplit(line, " ", 2);

    if (!parts)
        return;

    parse_command(parts[0], parts[1]);

    g_strfreev(parts);
}

void
build_stream_name(int type) {
    char *xwin_str;
    State *s = &uzbl.state;
    Behaviour *b = &uzbl.behave;

    xwin_str = itos((int)uzbl.xwin);
    switch(type) {
        case FIFO:
            if (b->fifo_dir) {
                sprintf (uzbl.comm.fifo_path, "%s/uzbl_fifo_%s", 
                            b->fifo_dir,
                            s->instance_name ? s->instance_name : xwin_str);
            } else {
                sprintf (uzbl.comm.fifo_path, "/tmp/uzbl_fifo_%s",
                            s->instance_name ? s->instance_name : xwin_str);
            }
            break;

        case SOCKET:
            if (b->socket_dir) {
                sprintf (uzbl.comm.socket_path, "%s/uzbl_socket_%s",
                            b->socket_dir,
                            s->instance_name ? s->instance_name : xwin_str);
            } else {
                sprintf (uzbl.comm.socket_path, "/tmp/uzbl_socket_%s",
                                s->instance_name ? s->instance_name : xwin_str);
            }
            break;
        default:
            break;
    }
    g_free(xwin_str);
}

static void
control_fifo(GIOChannel *gio, GIOCondition condition) {
    printf("triggered\n");
    gchar *ctl_line;
    GIOStatus ret;
    GError *err = NULL;

    if (condition & G_IO_HUP)
        g_error ("Fifo: Read end of pipe died!\n");

    if(!gio)
       g_error ("Fifo: GIOChannel broke\n");

    ret = g_io_channel_read_line(gio, &ctl_line, NULL, NULL, &err);
    if (ret == G_IO_STATUS_ERROR)
        g_error ("Fifo: Error reading: %s\n", err->message);

    parse_line(ctl_line);
    g_free(ctl_line);
    printf("...done\n");
    return;
}

static void
create_fifo() {
    GIOChannel *chan = NULL;
    GError *error = NULL;

    build_stream_name(FIFO);
    if (file_exists(uzbl.comm.fifo_path)) {
        g_error ("Fifo: Error when creating %s: File exists\n", uzbl.comm.fifo_path);
        return;
    }
    if (mkfifo (uzbl.comm.fifo_path, 0666) == -1) {
        g_error ("Fifo: Error when creating %s: %s\n", uzbl.comm.fifo_path, strerror(errno));
    } else {
        // we don't really need to write to the file, but if we open the file as 'r' we will block here, waiting for a writer to open the file.
        chan = g_io_channel_new_file((gchar *) uzbl.comm.fifo_path, "r+", &error);
        if (chan) {
            if (!g_io_add_watch(chan, G_IO_IN|G_IO_HUP, (GIOFunc) control_fifo, NULL)) {
                g_error ("Fifo: could not add watch on %s\n", uzbl.comm.fifo_path);
            } else { 
                printf ("Fifo: created successfully as %s\n", uzbl.comm.fifo_path);
            }
        } else {
            g_error ("Fifo: Error while opening: %s\n", error->message);
        }
    }
    return;
}

static void
control_socket(GIOChannel *chan) {
    struct sockaddr_un remote;
    char buffer[512], *ctl_line;
    char temp[128];
    int sock, clientsock, n, done;
    unsigned int t;

    sock = g_io_channel_unix_get_fd(chan);

    memset (buffer, 0, sizeof (buffer));

    t          = sizeof (remote);
    clientsock = accept (sock, (struct sockaddr *) &remote, &t);

    done = 0;
    do {
        memset (temp, 0, sizeof (temp));
        n = recv (clientsock, temp, 128, 0);
        if (n == 0) {
            buffer[strlen (buffer)] = '\0';
            done = 1;
        }
        if (!done)
            strcat (buffer, temp);
    } while (!done);

    if (strcmp (buffer, "\n") < 0) {
        buffer[strlen (buffer) - 1] = '\0';
    } else {
        buffer[strlen (buffer)] = '\0';
    }
    close (clientsock);
    ctl_line = g_strdup(buffer);
    parse_line (ctl_line);

/*
   TODO: we should be able to do it with this.  but glib errors out with "Invalid argument"
    GError *error = NULL;
    gsize len;
    GIOStatus ret;
    ret = g_io_channel_read_line(chan, &ctl_line, &len, NULL, &error);
    if (ret == G_IO_STATUS_ERROR)
        g_error ("Error reading: %s\n", error->message);

    printf("Got line %s (%u bytes) \n",ctl_line, len);
    if(ctl_line) {
       parse_line(ctl_line);
*/

    g_free(ctl_line);
    return;
} 

static void
create_socket() {
    GIOChannel *chan = NULL;
    int sock, len;
    struct sockaddr_un local;

    build_stream_name(SOCKET);
    sock = socket (AF_UNIX, SOCK_STREAM, 0);

    local.sun_family = AF_UNIX;
    strcpy (local.sun_path, uzbl.comm.socket_path);
    unlink (local.sun_path);

    len = strlen (local.sun_path) + sizeof (local.sun_family);
    bind (sock, (struct sockaddr *) &local, len);

    if (errno == -1) {
        printf ("Socket: Could not open in %s: %s\n", uzbl.comm.socket_path, strerror(errno));
    } else {
        printf ("Socket: Opened in %s\n", uzbl.comm.socket_path);
        listen (sock, 5);

        if( (chan = g_io_channel_unix_new(sock)) )
            g_io_add_watch(chan, G_IO_IN|G_IO_HUP, (GIOFunc) control_socket, chan);
    }
}

static void
update_title (void) {
    GString* string_long = g_string_new ("");
    GString* string_short = g_string_new ("");
    char* iname = NULL;
    gchar *statln;
    int iname_len;
    State *s = &uzbl.state;
    Behaviour *b = &uzbl.behave;

    if(s->instance_name) {
            iname_len = strlen(s->instance_name)+4;
            iname = malloc(iname_len);
            snprintf(iname, iname_len, "<%s> ", s->instance_name);
            
            g_string_prepend(string_long, iname);
            g_string_prepend(string_short, iname);
            free(iname);
    }

    g_string_append_printf(string_long, "%s ", s->keycmd->str);
    if (!b->always_insert_mode)
        g_string_append (string_long, (b->insert_mode ? "[I] " : "[C] "));
    if (uzbl.gui.main_title) {
        g_string_append (string_long, uzbl.gui.main_title);
        g_string_append (string_short, uzbl.gui.main_title);
    }
    g_string_append (string_long, " - Uzbl browser");
    g_string_append (string_short, " - Uzbl browser");
    if (s->selected_url[0]!=0) {
        g_string_append_printf (string_long, " -> (%s)", s->selected_url);
    }

    gchar* title_long = g_string_free (string_long, FALSE);
    gchar* title_short = g_string_free (string_short, FALSE);

    if (b->show_status) {
        gtk_window_set_title (GTK_WINDOW(uzbl.gui.main_window), title_short);
        // TODO: we should probably not do this every time we want to update the title..?
        statln = parse_status_template(uzbl.behave.status_format);
        gtk_label_set_markup(GTK_LABEL(uzbl.gui.mainbar_label), statln);
        if (b->status_background) {
            GdkColor color;
            gdk_color_parse (b->status_background, &color);
            //labels and hboxes do not draw their own background.  applying this on the window is ok as we the statusbar is the only affected widget.  (if not, we could also use GtkEventBox)
            gtk_widget_modify_bg (uzbl.gui.main_window, GTK_STATE_NORMAL, &color);
        }
        g_free(statln);
    } else {
        gtk_window_set_title (GTK_WINDOW(uzbl.gui.main_window), title_long);
    }

    g_free (title_long);
    g_free (title_short);
}

static gboolean
key_press_cb (WebKitWebView* page, GdkEventKey* event)
{
    //TRUE to stop other handlers from being invoked for the event. FALSE to propagate the event further.

    (void) page;
    Action *action;

    if (event->type != GDK_KEY_PRESS || event->keyval == GDK_Page_Up || event->keyval == GDK_Page_Down
        || event->keyval == GDK_Up || event->keyval == GDK_Down || event->keyval == GDK_Left || event->keyval == GDK_Right)
        return FALSE;

    /* turn off insert mode (if always_insert_mode is not used) */
    if (uzbl.behave.insert_mode && (event->keyval == GDK_Escape)) {
        uzbl.behave.insert_mode = uzbl.behave.always_insert_mode;
        update_title();
        return TRUE;
    }

    if (uzbl.behave.insert_mode && ((event->state & uzbl.behave.modmask) != uzbl.behave.modmask))
        return FALSE;

    if (event->keyval == GDK_Escape) {
        g_string_truncate(uzbl.state.keycmd, 0);
        update_title();
        return TRUE;
    }

    //Insert without shift - insert from clipboard; Insert with shift - insert from primary
    if (event->keyval == GDK_Insert) {
        gchar * str;
        if ((event->state & GDK_SHIFT_MASK) == GDK_SHIFT_MASK) {
            str = gtk_clipboard_wait_for_text (gtk_clipboard_get (GDK_SELECTION_PRIMARY));
        } else {
            str = gtk_clipboard_wait_for_text (gtk_clipboard_get (GDK_SELECTION_CLIPBOARD)); 
        }
        if (str) {
            g_string_append_printf (uzbl.state.keycmd, "%s",  str);
            update_title ();
            free (str);
        }
        return TRUE;
    }

    if ((event->keyval == GDK_BackSpace) && (uzbl.state.keycmd->len > 0)) {
        g_string_truncate(uzbl.state.keycmd, uzbl.state.keycmd->len - 1);
        update_title();
        return TRUE;
    }

    if ((event->keyval == GDK_Return) || (event->keyval == GDK_KP_Enter)) {
        GString* short_keys = g_string_new ("");
        unsigned int i;
        for (i=0; i<(uzbl.state.keycmd->len); i++) {
            g_string_append_c(short_keys, uzbl.state.keycmd->str[i]);
            g_string_append_c(short_keys, '_');
            
            //printf("\nTesting string: @%s@\n", short_keys->str);
            if ((action = g_hash_table_lookup(uzbl.bindings, short_keys->str))) {
                GString* parampart = g_string_new (uzbl.state.keycmd->str);
                g_string_erase (parampart, 0, i+1);
                //printf("\nParameter: @%s@\n", parampart->str);
                GString* actionname = g_string_new ("");
                if (action->name)
                    g_string_printf (actionname, action->name, parampart->str);
                GString* actionparam = g_string_new ("");
                if (action->param)
                    g_string_printf (actionparam, action->param, parampart->str);
                parse_command(actionname->str, actionparam->str);
                g_string_free (actionname, TRUE);
                g_string_free (actionparam, TRUE);
                g_string_free (parampart, TRUE);
                g_string_truncate(uzbl.state.keycmd, 0);
                update_title();
            }          

            g_string_truncate(short_keys, short_keys->len - 1);
        }
        g_string_free (short_keys, TRUE);
        return (!uzbl.behave.insert_mode);
    }

    g_string_append(uzbl.state.keycmd, event->string);
    if ((action = g_hash_table_lookup(uzbl.bindings, uzbl.state.keycmd->str))) {
        g_string_truncate(uzbl.state.keycmd, 0);
        parse_command(action->name, action->param);
    }

    update_title();

    return TRUE;
}

static GtkWidget*
create_browser () {
    GUI *g = &uzbl.gui;

    GtkWidget* scrolled_window = gtk_scrolled_window_new (NULL, NULL);
    gtk_scrolled_window_set_policy (GTK_SCROLLED_WINDOW (scrolled_window), GTK_POLICY_NEVER, GTK_POLICY_NEVER); //todo: some sort of display of position/total length. like what emacs does

    g->web_view = WEBKIT_WEB_VIEW (webkit_web_view_new ());
    gtk_container_add (GTK_CONTAINER (scrolled_window), GTK_WIDGET (g->web_view));

    g_signal_connect (G_OBJECT (g->web_view), "title-changed", G_CALLBACK (title_change_cb), g->web_view);
    g_signal_connect (G_OBJECT (g->web_view), "load-progress-changed", G_CALLBACK (progress_change_cb), g->web_view);
    g_signal_connect (G_OBJECT (g->web_view), "load-committed", G_CALLBACK (load_commit_cb), g->web_view);
    g_signal_connect (G_OBJECT (g->web_view), "load-committed", G_CALLBACK (log_history_cb), g->web_view);
    g_signal_connect (G_OBJECT (g->web_view), "hovering-over-link", G_CALLBACK (link_hover_cb), g->web_view);
    g_signal_connect (G_OBJECT (g->web_view), "key-press-event", G_CALLBACK (key_press_cb), g->web_view);
    g_signal_connect (G_OBJECT (g->web_view), "new-window-policy-decision-requested", G_CALLBACK (new_window_cb), g->web_view); 
    g_signal_connect (G_OBJECT (g->web_view), "download-requested", G_CALLBACK (download_cb), g->web_view); 
    g_signal_connect (G_OBJECT (g->web_view), "create-web-view", G_CALLBACK (create_web_view_cb), g->web_view);  

    return scrolled_window;
}

static GtkWidget*
create_mainbar () {
    GUI *g = &uzbl.gui;

    g->mainbar = gtk_hbox_new (FALSE, 0);
    g->mainbar_label = gtk_label_new ("");  
    gtk_label_set_selectable((GtkLabel *)g->mainbar_label, TRUE);
    gtk_label_set_ellipsize(GTK_LABEL(g->mainbar_label), PANGO_ELLIPSIZE_END);
    gtk_misc_set_alignment (GTK_MISC(g->mainbar_label), 0, 0);
    gtk_misc_set_padding (GTK_MISC(g->mainbar_label), 2, 2);
    gtk_box_pack_start (GTK_BOX (g->mainbar), g->mainbar_label, TRUE, TRUE, 0);
    return g->mainbar;
}

static
GtkWidget* create_window () {
    GtkWidget* window = gtk_window_new (GTK_WINDOW_TOPLEVEL);
    gtk_window_set_default_size (GTK_WINDOW (window), 800, 600);
    gtk_widget_set_name (window, "Uzbl browser");
    g_signal_connect (G_OBJECT (window), "destroy", G_CALLBACK (destroy_cb), NULL);

    return window;
}

static void
add_binding (const gchar *key, const gchar *act) {
    char **parts = g_strsplit(act, " ", 2);
    Action *action;

    if (!parts)
        return;

    //Debug:
    printf ("Binding %-10s : %s\n", key, act);
    action = new_action(parts[0], parts[1]);
    g_hash_table_insert(uzbl.bindings, g_strdup(key), action);

    g_strfreev(parts);
}

static void
settings_init () {
    GKeyFile* config = NULL;
    gboolean res  = FALSE;
    char *saveptr;
    gchar** keys = NULL;
    State *s = &uzbl.state;
    Network *n = &uzbl.net;
    Behaviour *b = &uzbl.behave;

    if (!s->config_file) {
        const char* XDG_CONFIG_HOME = getenv ("XDG_CONFIG_HOME");
        if (! XDG_CONFIG_HOME || ! strcmp (XDG_CONFIG_HOME, "")) {
          XDG_CONFIG_HOME = (char*)XDG_CONFIG_HOME_default;
        }
        printf("XDG_CONFIG_HOME: %s\n", XDG_CONFIG_HOME);
    
        strcpy (s->config_file_path, XDG_CONFIG_HOME);
        strcat (s->config_file_path, "/uzbl/config");
        if (file_exists (s->config_file_path)) {
          printf ("Config file %s found.\n", s->config_file_path);
          s->config_file = &s->config_file_path[0];
        } else {
            // Now we check $XDG_CONFIG_DIRS
            char *XDG_CONFIG_DIRS = getenv ("XDG_CONFIG_DIRS");
            if (! XDG_CONFIG_DIRS || ! strcmp (XDG_CONFIG_DIRS, ""))
                XDG_CONFIG_DIRS = XDG_CONFIG_DIRS_default;

            printf("XDG_CONFIG_DIRS: %s\n", XDG_CONFIG_DIRS);

            char buffer[512];
            strcpy (buffer, XDG_CONFIG_DIRS);
            const gchar* dir = (char *) strtok_r (buffer, ":", &saveptr);
            while (dir && ! file_exists (s->config_file_path)) {
                strcpy (s->config_file_path, dir);
                strcat (s->config_file_path, "/uzbl/config_file_pathig");
                if (file_exists (s->config_file_path)) {
                    printf ("Config file %s found.\n", s->config_file_path);
                    s->config_file = &s->config_file_path[0];
                }
                dir = (char * ) strtok_r (NULL, ":", &saveptr);
            }
        }
    }

    if (s->config_file) {
        config = g_key_file_new ();
        res = g_key_file_load_from_file (config, s->config_file, G_KEY_FILE_NONE, NULL);
          if (res) {
            printf ("Config %s loaded\n", s->config_file);
          } else {
            fprintf (stderr, "Config %s loading failed\n", s->config_file);
        }
    } else {
        printf ("No configuration.\n");
    }

    if (res) {
        b->history_handler    = g_key_file_get_value   (config, "behavior", "history_handler",    NULL);
        b->download_handler   = g_key_file_get_value   (config, "behavior", "download_handler",   NULL);
        b->cookie_handler     = g_key_file_get_string  (config, "behavior", "cookie_handler",     NULL);
        b->always_insert_mode = g_key_file_get_boolean (config, "behavior", "always_insert_mode", NULL);
        b->show_status        = g_key_file_get_boolean (config, "behavior", "show_status",        NULL);
        b->modkey             = g_key_file_get_value   (config, "behavior", "modkey",             NULL);
        b->status_top         = g_key_file_get_boolean (config, "behavior", "status_top",         NULL);
        b->status_format      = g_key_file_get_string  (config, "behavior", "status_format",      NULL);
        b->status_background  = g_key_file_get_string  (config, "behavior", "status_background",  NULL);
        if (! b->fifo_dir)
            b->fifo_dir       = g_key_file_get_value   (config, "behavior", "fifo_dir",           NULL);
        if (! b->socket_dir)
            b->socket_dir     = g_key_file_get_value   (config, "behavior", "socket_dir",         NULL);
        keys                  = g_key_file_get_keys    (config, "bindings", NULL,                 NULL);
    }

    printf ("History handler: %s\n",    (b->history_handler    ? b->history_handler  : "disabled"));
    printf ("Download manager: %s\n",   (b->download_handler   ? b->download_handler : "disabled"));
    printf ("Cookie handler: %s\n",     (b->cookie_handler     ? b->cookie_handler   : "disabled"));
    printf ("Fifo directory: %s\n",     (b->fifo_dir           ? b->fifo_dir         : "disabled"));
    printf ("Socket directory: %s\n",   (b->socket_dir         ? b->socket_dir       : "disabled"));
    printf ("Always insert mode: %s\n", (b->always_insert_mode ? "TRUE"              : "FALSE"));
    printf ("Show status: %s\n",        (b->show_status        ? "TRUE"              : "FALSE"));
    printf ("Status top: %s\n",         (b->status_top         ? "TRUE"              : "FALSE"));
    printf ("Modkey: %s\n",             (b->modkey             ? b->modkey           : "disabled"));
    printf ("Status format: %s\n",             (b->status_format            ? b->status_format           : "none"));

    if (!b->modkey)
        b->modkey = "";

    //POSSIBLE MODKEY VALUES (COMBINATIONS CAN BE USED)
    gchar* modkeyup = g_utf8_strup (b->modkey, -1);
    if (g_strrstr (modkeyup,"SHIFT") != NULL)    b->modmask |= GDK_SHIFT_MASK;    //the Shift key.
    if (g_strrstr (modkeyup,"LOCK") != NULL)     b->modmask |= GDK_LOCK_MASK;     //a Lock key (depending on the modifier mapping of the X server this may either be CapsLock or ShiftLock).
    if (g_strrstr (modkeyup,"CONTROL") != NULL)  b->modmask |= GDK_CONTROL_MASK;  //the Control key.
    if (g_strrstr (modkeyup,"MOD1") != NULL)     b->modmask |= GDK_MOD1_MASK;     //the fourth modifier key (it depends on the modifier mapping of the X server which key is interpreted as this modifier, but normally it is the Alt key).
    if (g_strrstr (modkeyup,"MOD2") != NULL)     b->modmask |= GDK_MOD2_MASK;     //the fifth modifier key (it depends on the modifier mapping of the X server which key is interpreted as this modifier).
    if (g_strrstr (modkeyup,"MOD3") != NULL)     b->modmask |= GDK_MOD3_MASK;     //the sixth modifier key (it depends on the modifier mapping of the X server which key is interpreted as this modifier).
    if (g_strrstr (modkeyup,"MOD4") != NULL)     b->modmask |= GDK_MOD4_MASK;     //the seventh modifier key (it depends on the modifier mapping of the X server which key is interpreted as this modifier).
    if (g_strrstr (modkeyup,"MOD5") != NULL)     b->modmask |= GDK_MOD5_MASK;     //the eighth modifier key (it depends on the modifier mapping of the X server which key is interpreted as this modifier).
    if (g_strrstr (modkeyup,"BUTTON1") != NULL)  b->modmask |= GDK_BUTTON1_MASK;  //the first mouse button.
    if (g_strrstr (modkeyup,"BUTTON2") != NULL)  b->modmask |= GDK_BUTTON2_MASK;  //the second mouse button.
    if (g_strrstr (modkeyup,"BUTTON3") != NULL)  b->modmask |= GDK_BUTTON3_MASK;  //the third mouse button.
    if (g_strrstr (modkeyup,"BUTTON4") != NULL)  b->modmask |= GDK_BUTTON4_MASK;  //the fourth mouse button.
    if (g_strrstr (modkeyup,"BUTTON5") != NULL)  b->modmask |= GDK_BUTTON5_MASK;  //the fifth mouse button.
    if (g_strrstr (modkeyup,"SUPER") != NULL)    b->modmask |= GDK_SUPER_MASK;    //the Super modifier. Since 2.10
    if (g_strrstr (modkeyup,"HYPER") != NULL)    b->modmask |= GDK_HYPER_MASK;    //the Hyper modifier. Since 2.10
    if (g_strrstr (modkeyup,"META") != NULL)     b->modmask |= GDK_META_MASK;     //the Meta modifier. Since 2.10  */
    free (modkeyup);

    if (keys) {
        int i;
        for (i = 0; keys[i]; i++) {
            gchar *value = g_key_file_get_string (config, "bindings", keys[i], NULL);
            
            add_binding(g_strstrip(keys[i]), value);
            g_free(value);
        }

        g_strfreev(keys);
    }

    /* networking options */
    if (res) {
        n->proxy_url      = g_key_file_get_value   (config, "network", "proxy_server",       NULL);
        b->http_debug     = g_key_file_get_integer (config, "network", "http_debug",         NULL);
        n->useragent      = g_key_file_get_value   (config, "network", "user-agent",         NULL);
        n->max_conns      = g_key_file_get_integer (config, "network", "max_conns",          NULL);
        n->max_conns_host = g_key_file_get_integer (config, "network", "max_conns_per_host", NULL);
    }

    if(n->proxy_url){
        g_object_set(G_OBJECT(n->soup_session), SOUP_SESSION_PROXY_URI, soup_uri_new(n->proxy_url), NULL);
    }
	
    if(!(b->http_debug <= 3)){
        b->http_debug = 0;
        fprintf(stderr, "Wrong http_debug level, ignoring.\n");
    } else if (b->http_debug > 0) {
        n->soup_logger = soup_logger_new(b->http_debug, -1);
        soup_session_add_feature(n->soup_session, SOUP_SESSION_FEATURE(n->soup_logger));
    }
	
    if(n->useragent){
        char* newagent  = malloc(1024);

        strcpy(newagent, str_replace("%webkit-major%", itos(WEBKIT_MAJOR_VERSION), n->useragent));
        strcpy(newagent, str_replace("%webkit-minor%", itos(WEBKIT_MINOR_VERSION), newagent));
        strcpy(newagent, str_replace("%webkit-micro%", itos(WEBKIT_MICRO_VERSION), newagent));

        if (uname (&s->unameinfo) == -1) {
            printf("Error getting uname info. Not replacing system-related user agent variables.\n");
        } else {
            strcpy(newagent, str_replace("%sysname%",     s->unameinfo.sysname, newagent));
            strcpy(newagent, str_replace("%nodename%",    s->unameinfo.nodename, newagent));
            strcpy(newagent, str_replace("%kernrel%",     s->unameinfo.release, newagent));
            strcpy(newagent, str_replace("%kernver%",     s->unameinfo.version, newagent));
            strcpy(newagent, str_replace("%arch-system%", s->unameinfo.machine, newagent));

            #ifdef _GNU_SOURCE
                strcpy(newagent, str_replace("%domainname%", s->unameinfo.domainname, newagent));
            #endif
        }

        strcpy(newagent, str_replace("%arch-uzbl%",    ARCH,                       newagent));
        strcpy(newagent, str_replace("%commit%",       COMMIT,                     newagent));

        n->useragent = malloc(1024);
        strcpy(n->useragent, newagent);
        g_object_set(G_OBJECT(n->soup_session), SOUP_SESSION_USER_AGENT, n->useragent, NULL);
    }

    if(n->max_conns >= 1){
        g_object_set(G_OBJECT(n->soup_session), SOUP_SESSION_MAX_CONNS, n->max_conns, NULL);
    }

    if(n->max_conns_host >= 1){
        g_object_set(G_OBJECT(n->soup_session), SOUP_SESSION_MAX_CONNS_PER_HOST, n->max_conns_host, NULL);
    }

    printf("Proxy configured: %s\n", n->proxy_url ? n->proxy_url : "none");
    printf("HTTP logging level: %d\n", b->http_debug);
    printf("User-agent: %s\n", n->useragent? n->useragent : "default");
    printf("Maximum connections: %d\n", n->max_conns ? n->max_conns : 0);
    printf("Maximum connections per host: %d\n", n->max_conns_host ? n->max_conns_host: 0);
		


	if(b->cookie_handler){
		/* ck = soup_cookie_jar_new(); */
		/* soup_session_add_feature(soup_session, SOUP_SESSION_FEATURE(ck)); */
		/* g_signal_connect(ck, "changed", G_CALLBACK(cookie_recieved_action), NULL); */
		g_signal_connect(n->soup_session, "request-queued", G_CALLBACK(handle_cookies), NULL);
	}
	
}

static void handle_cookies (SoupSession *session, SoupMessage *msg, gpointer user_data){
    (void) session;
    (void) user_data;
    gchar * stdout = NULL;
    soup_message_add_header_handler(msg, "got-headers", "Set-Cookie", G_CALLBACK(save_cookies), NULL);
    GString* args = g_string_new ("");
    SoupURI * soup_uri = soup_message_get_uri(msg);
    g_string_printf (args, "GET %s %s", soup_uri->host, soup_uri->path);
    run_command_sync(uzbl.behave.cookie_handler, args->str, &stdout);
    if(stdout) {
        soup_message_headers_replace (msg->request_headers, "Cookie", stdout);
    }
    g_string_free(args, TRUE);
}

static void
save_cookies (SoupMessage *msg, gpointer user_data){
    (void) user_data;
    GSList *ck;
    char *cookie;
    for (ck = soup_cookies_from_response(msg); ck; ck = ck->next){
        cookie = soup_cookie_to_set_cookie_header(ck->data);
        GString* args = g_string_new ("");
        SoupURI * soup_uri = soup_message_get_uri(msg);
        g_string_printf (args, "PUT %s %s \"%s\"", soup_uri->host, soup_uri->path, cookie);
        run_command_async(uzbl.behave.cookie_handler, args->str);
        g_string_free(args, TRUE);
        free(cookie);
    }
    g_slist_free(ck);
}

int
main (int argc, char* argv[]) {
    gtk_init (&argc, &argv);
    if (!g_thread_supported ())
        g_thread_init (NULL);

    printf("Uzbl start location: %s\n", argv[0]);
    strcpy(uzbl.state.executable_path,argv[0]);

    strcat ((char *) XDG_CONFIG_HOME_default, getenv ("HOME"));
    strcat ((char *) XDG_CONFIG_HOME_default, "/.config");

    GError *error = NULL;
    GOptionContext* context = g_option_context_new ("- some stuff here maybe someday");
    g_option_context_add_main_entries (context, entries, NULL);
    g_option_context_add_group (context, gtk_get_option_group (TRUE));
    g_option_context_parse (context, &argc, &argv, &error);
    /* initialize hash table */
    uzbl.bindings = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, free_action);
	
	uzbl.net.soup_session = webkit_get_default_session();
    uzbl.state.keycmd = g_string_new("");

    settings_init ();
    commands_hash ();
	
    if (uzbl.behave.always_insert_mode)
        uzbl.behave.insert_mode = TRUE;

    GtkWidget* vbox = gtk_vbox_new (FALSE, 0);
    if (uzbl.behave.status_top)
        gtk_box_pack_start (GTK_BOX (vbox), create_mainbar (), FALSE, TRUE, 0);
    gtk_box_pack_start (GTK_BOX (vbox), create_browser (), TRUE, TRUE, 0);
    if (!uzbl.behave.status_top)
        gtk_box_pack_start (GTK_BOX (vbox), create_mainbar (), FALSE, TRUE, 0);

    uzbl.gui.main_window = create_window ();
    gtk_container_add (GTK_CONTAINER (uzbl.gui.main_window), vbox);

    load_uri (uzbl.gui.web_view, uzbl.state.uri);

    gtk_widget_grab_focus (GTK_WIDGET (uzbl.gui.web_view));
    gtk_widget_show_all (uzbl.gui.main_window);
    uzbl.xwin = GDK_WINDOW_XID (GTK_WIDGET (uzbl.gui.main_window)->window);
    printf("window_id %i\n",(int) uzbl.xwin);
    printf("pid %i\n", getpid ());
    printf("name: %s\n", uzbl.state.instance_name);

    uzbl.gui.scbar_v = (GtkScrollbar*) gtk_vscrollbar_new (NULL);
    uzbl.gui.bar_v = gtk_range_get_adjustment((GtkRange*) uzbl.gui.scbar_v);
    uzbl.gui.scbar_h = (GtkScrollbar*) gtk_hscrollbar_new (NULL);
    uzbl.gui.bar_h = gtk_range_get_adjustment((GtkRange*) uzbl.gui.scbar_h);
    gtk_widget_set_scroll_adjustments ((GtkWidget*) uzbl.gui.web_view, uzbl.gui.bar_h, uzbl.gui.bar_v);


    if(setup_signal(SIGTERM, catch_sigterm) == SIG_ERR)
        fprintf(stderr, "uzbl: error hooking SIGTERM\n");

    setup_scanner();
    if (!uzbl.behave.status_format)
        uzbl.behave.status_format = STATUS_DEFAULT;
    if (!uzbl.behave.show_status)
        gtk_widget_hide(uzbl.gui.mainbar);
    else
        update_title();


    if (uzbl.behave.fifo_dir)
        create_fifo ();
    if (uzbl.behave.socket_dir)
        create_socket();

    gtk_main ();
    clean_up();

    return EXIT_SUCCESS;
}

/* vi: set et ts=4: */
