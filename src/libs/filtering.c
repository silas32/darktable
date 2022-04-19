/*
    This file is part of darktable,
    Copyright (C) 2022 darktable developers.

    darktable is free software: you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation, either version 3 of the License, or
    (at your option) any later version.

    darktable is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.

    You should have received a copy of the GNU General Public License
    along with darktable.  If not, see <http://www.gnu.org/licenses/>.
*/

#include "bauhaus/bauhaus.h"
#include "common/collection.h"
#include "common/darktable.h"
#include "common/datetime.h"
#include "common/debug.h"
#include "common/film.h"
#include "common/history.h"
#include "common/map_locations.h"
#include "common/metadata.h"
#include "common/utility.h"
#include "control/conf.h"
#include "control/control.h"
#include "control/jobs.h"
#include "dtgtk/button.h"
#include "dtgtk/range.h"
#include "gui/accelerators.h"
#include "gui/gtk.h"
#include "gui/preferences_dialogs.h"
#include "libs/collect.h"
#include "libs/lib.h"
#include "libs/lib_api.h"
#include "views/view.h"
#ifndef _WIN32
#include <gio/gunixmounts.h>
#endif
#ifdef GDK_WINDOWING_QUARTZ
#include "osx/osx.h"
#endif

#include <locale.h>

DT_MODULE(1)

#define PARAM_STRING_SIZE 256 // FIXME: is this enough !?

typedef enum _preset_save_type_t
{
  _PRESET_NONE = 0,
  _PRESET_FILTERS = 1 << 0,
  _PRESET_SORT = 1 << 1,
  _PRESET_ERASE_TOPBAR = 1 << 2,
  _PRESET_ALL = _PRESET_FILTERS | _PRESET_SORT | _PRESET_ERASE_TOPBAR
} _preset_save_type_t;

typedef struct _widgets_sort_t
{
  dt_collection_sort_t sortid;
  GtkWidget *box;
  GtkWidget *sort;
  GtkWidget *direction;
  GtkWidget *close;

  int num;
  gboolean top;
  struct dt_lib_filtering_t *lib;
} _widgets_sort_t;

typedef struct dt_lib_filtering_rule_t
{
  int num;

  dt_collection_properties_t prop;

  GtkWidget *w_main;
  GtkWidget *w_operator;
  GtkWidget *w_prop;
  GtkWidget *w_btn_box;
  GtkWidget *w_close;
  GtkWidget *w_off;

  GtkWidget *w_widget_box;
  char raw_text[PARAM_STRING_SIZE];
  GtkWidget *w_special_box;
  void *w_specific;      // structure which contains all the widgets specific to the rule type
  // and we have the same for the top bar duplicate widgets
  GtkWidget *w_special_box_top;
  void *w_specific_top;  // structure which contains all the widgets specific to the rule type
  int manual_widget_set; // when we update manually the widget, we don't want events to be handled
  gboolean cleaning;     // if we have started a gui_cleanup (we don't want certain event to occurs)

  gboolean topbar;

  struct dt_lib_filtering_t *lib;
} dt_lib_filtering_rule_t;

typedef struct dt_lib_filtering_t
{
  dt_lib_filtering_rule_t rule[DT_COLLECTION_MAX_RULES];
  int nb_rules;

  GtkWidget *rules_box;
  GtkWidget *rules_sw;

  _widgets_sort_t sort[DT_COLLECTION_MAX_RULES];
  int nb_sort;
  _widgets_sort_t sorttop;
  GtkWidget *sort_box;
  gboolean manual_sort_set;

  struct dt_lib_filtering_params_t *params;

  gchar *last_where_ext;
} dt_lib_filtering_t;

typedef struct dt_lib_filtering_params_rule_t
{
  uint32_t item : 16;
  uint32_t mode : 16;
  uint32_t off : 16;
  uint32_t topbar : 16;
  char string[PARAM_STRING_SIZE];
} dt_lib_filtering_params_rule_t;

typedef struct dt_lib_filtering_params_sort_t
{
  uint32_t item : 16;
  uint32_t order : 16;
} dt_lib_filtering_params_sort_t;

typedef struct dt_lib_filtering_params_t
{
  uint32_t rules;
  dt_lib_filtering_params_rule_t rule[DT_COLLECTION_MAX_RULES];
  uint32_t sorts;
  dt_lib_filtering_params_sort_t sort[DT_COLLECTION_MAX_RULES];
  uint32_t preset_type;
} dt_lib_filtering_params_t;

typedef struct _widgets_range_t
{
  dt_lib_filtering_rule_t *rule;

  GtkWidget *range_select;
} _widgets_range_t;

typedef enum _tree_cols_t
{
  TREE_COL_TEXT = 0,
  TREE_COL_TOOLTIP,
  TREE_COL_PATH,
  TREE_COL_COUNT,
  TREE_NUM_COLS
} _tree_cols_t;

static void _filters_gui_update(dt_lib_module_t *self);
static void _sort_gui_update(dt_lib_module_t *self);
static void _filtering_gui_update(dt_lib_module_t *self);
static void _dt_collection_updated(gpointer instance, dt_collection_change_t query_change,
                                   dt_collection_properties_t changed_property, gpointer imgs, int next,
                                   gpointer self);
static void _rule_set_raw_text(dt_lib_filtering_rule_t *rule, const gchar *text, const gboolean signal);

static void _range_changed(GtkWidget *widget, gpointer user_data);
static void _range_widget_add_to_rule(dt_lib_filtering_rule_t *rule, _widgets_range_t *special, const gboolean top);
static void _sort_append_sort(GtkWidget *widget, dt_lib_module_t *self);

typedef void (*_widget_init_func)(dt_lib_filtering_rule_t *rule, const dt_collection_properties_t prop,
                                  const gchar *text, dt_lib_module_t *self, gboolean top);

typedef gboolean (*_widget_update_func)(dt_lib_filtering_rule_t *rule);
typedef struct _filter_t
{
  dt_collection_properties_t prop;
  _widget_init_func widget_init;
  _widget_update_func update;
} _filter_t;


// filters definitions
#include "libs/filters/aperture.c"
#include "libs/filters/colors.c"
#include "libs/filters/date.c"
#include "libs/filters/exposure.c"
#include "libs/filters/filename.c"
#include "libs/filters/focal.c"
#include "libs/filters/iso.c"
#include "libs/filters/rating.c"
#include "libs/filters/ratio.c"
#include "libs/filters/search.c"

static _filter_t filters[] = { { DT_COLLECTION_PROP_COLORLABEL, _colors_widget_init, _colors_update },
                               { DT_COLLECTION_PROP_FILENAME, _filename_widget_init, _filename_update },
                               { DT_COLLECTION_PROP_TEXTSEARCH, _search_widget_init, _search_update },
                               { DT_COLLECTION_PROP_TIME, _date_widget_init, _date_update },
                               { DT_COLLECTION_PROP_CHANGE_TIMESTAMP, _date_widget_init, _date_update },
                               { DT_COLLECTION_PROP_EXPORT_TIMESTAMP, _date_widget_init, _date_update },
                               { DT_COLLECTION_PROP_IMPORT_TIMESTAMP, _date_widget_init, _date_update },
                               { DT_COLLECTION_PROP_PRINT_TIMESTAMP, _date_widget_init, _date_update },
                               { DT_COLLECTION_PROP_ASPECT_RATIO, _ratio_widget_init, _ratio_update },
                               { DT_COLLECTION_PROP_RATING, _rating_widget_init, _rating_update },
                               { DT_COLLECTION_PROP_APERTURE, _aperture_widget_init, _aperture_update },
                               { DT_COLLECTION_PROP_FOCAL_LENGTH, _focal_widget_init, _focal_update },
                               { DT_COLLECTION_PROP_ISO, _iso_widget_init, _iso_update },
                               { DT_COLLECTION_PROP_EXPOSURE, _exposure_widget_init, _exposure_update } };

static _filter_t *_filters_get(const dt_collection_properties_t prop)
{
  const int nb = sizeof(filters) / sizeof(_filter_t);
  for(int i = 0; i < nb; i++)
  {
    if(filters[i].prop == prop) return &filters[i];
  }
  return NULL;
}

const char *name(dt_lib_module_t *self)
{
  return _("collection filters");
}

void init_presets(dt_lib_module_t *self)
{
  dt_lib_filtering_params_t params;

#define CLEAR_PARAMS(t, r, s)                                                                                     \
  {                                                                                                               \
    memset(&params, 0, sizeof(params));                                                                           \
    params.preset_type = t;                                                                                       \
    params.rules = 1;                                                                                             \
    params.rule[0].mode = 0;                                                                                      \
    params.rule[0].off = 0;                                                                                       \
    params.rule[0].topbar = 0;                                                                                    \
    params.rule[0].item = r;                                                                                      \
    params.sorts = 1;                                                                                             \
    params.sort[0].item = s;                                                                                      \
    params.sort[0].order = 0;                                                                                     \
  }

  // initial preset
  CLEAR_PARAMS(_PRESET_ALL, DT_COLLECTION_PROP_RATING, DT_COLLECTION_SORT_DATETIME);
  params.rules = 3;
  params.rule[0].topbar = 1;
  params.rule[1].item = DT_COLLECTION_PROP_COLORLABEL;
  params.rule[1].mode = 0;
  params.rule[1].off = 0;
  params.rule[1].topbar = 1;
  params.rule[2].item = DT_COLLECTION_PROP_TEXTSEARCH;
  params.rule[2].mode = 0;
  params.rule[2].off = 0;
  params.rule[2].topbar = 1;
  dt_lib_presets_add(_("initial setting"), self->plugin_name, self->version(), &params, sizeof(params), TRUE);

  // based on aspect-ratio
  CLEAR_PARAMS(_PRESET_FILTERS, DT_COLLECTION_PROP_ASPECT_RATIO, DT_COLLECTION_SORT_DATETIME);
  g_strlcpy(params.rule[0].string, "[1;1]", PARAM_STRING_SIZE);
  dt_lib_presets_add(_("square"), self->plugin_name, self->version(), &params, sizeof(params), TRUE);

  CLEAR_PARAMS(_PRESET_FILTERS, DT_COLLECTION_PROP_ASPECT_RATIO, DT_COLLECTION_SORT_DATETIME);
  g_strlcpy(params.rule[0].string, ">=1.01", PARAM_STRING_SIZE);
  dt_lib_presets_add(_("landscape"), self->plugin_name, self->version(), &params, sizeof(params), TRUE);

  CLEAR_PARAMS(_PRESET_FILTERS, DT_COLLECTION_PROP_ASPECT_RATIO, DT_COLLECTION_SORT_DATETIME);
  g_strlcpy(params.rule[0].string, "<=0.99", PARAM_STRING_SIZE);
  dt_lib_presets_add(_("portrait"), self->plugin_name, self->version(), &params, sizeof(params), TRUE);

  // presets based on import
  CLEAR_PARAMS(_PRESET_FILTERS | _PRESET_SORT, DT_COLLECTION_PROP_IMPORT_TIMESTAMP,
               DT_COLLECTION_SORT_IMPORT_TIMESTAMP);
  g_strlcpy(params.rule[0].string, "[-0000:00:01 00:00:00;now]", PARAM_STRING_SIZE);
  dt_lib_presets_add(_("imported: last 24h"), self->plugin_name, self->version(), &params, sizeof(params), TRUE);

  CLEAR_PARAMS(_PRESET_FILTERS | _PRESET_SORT, DT_COLLECTION_PROP_IMPORT_TIMESTAMP,
               DT_COLLECTION_SORT_IMPORT_TIMESTAMP);
  g_strlcpy(params.rule[0].string, "[-0000:00:30 00:00:00;now]", PARAM_STRING_SIZE);
  dt_lib_presets_add(_("imported: last 30 days"), self->plugin_name, self->version(), &params, sizeof(params),
                     TRUE);

  // presets based on image metadata (image taken)
  CLEAR_PARAMS(_PRESET_FILTERS | _PRESET_SORT, DT_COLLECTION_PROP_TIME, DT_COLLECTION_SORT_DATETIME);
  g_strlcpy(params.rule[0].string, "[-0000:00:01 00:00:00;now]", PARAM_STRING_SIZE);
  dt_lib_presets_add(_("taken: last 24h"), self->plugin_name, self->version(), &params, sizeof(params), TRUE);

  CLEAR_PARAMS(_PRESET_FILTERS | _PRESET_SORT, DT_COLLECTION_PROP_TIME, DT_COLLECTION_SORT_DATETIME);
  g_strlcpy(params.rule[0].string, "[-0000:00:30 00:00:00;now]", PARAM_STRING_SIZE);
  dt_lib_presets_add(_("taken: last 30 days"), self->plugin_name, self->version(), &params, sizeof(params), TRUE);

#undef CLEAR_PARAMS
}

static void _filtering_reset(const _preset_save_type_t reset)
{
  if((reset & _PRESET_FILTERS) && (reset & _PRESET_ERASE_TOPBAR))
  {
    // easy case : we remove all rules
    dt_conf_set_int("plugins/lighttable/filtering/num_rules", 0);
  }
  else if(reset & _PRESET_FILTERS)
  {
    // for the filtering rules, we
    // - remove the unpinned ones
    // - reset the pinned ones
    const int nb_rules
        = CLAMP(dt_conf_get_int("plugins/lighttable/filtering/num_rules"), 0, DT_COLLECTION_MAX_RULES);
    int nb_removed = 0;
    for(int i = 0; i < nb_rules; i++)
    {
      char confname[200] = { 0 };
      // read the topbar state
      snprintf(confname, sizeof(confname), "plugins/lighttable/filtering/top%1d", i - nb_removed);
      if(dt_conf_get_int(confname))
      {
        // we "just" reset the filter
        snprintf(confname, sizeof(confname), "plugins/lighttable/filtering/mode%1d", i - nb_removed);
        dt_conf_set_int(confname, 0);
        snprintf(confname, sizeof(confname), "plugins/lighttable/filtering/string%1d", i - nb_removed);
        dt_conf_set_string(confname, "");
      }
      else
      {
        // we remove the filter and move up the next ones
        for(int j = i + 1; j < nb_rules; j++)
        {
          snprintf(confname, sizeof(confname), "plugins/lighttable/filtering/mode%1d", j - nb_removed);
          const int mode = dt_conf_get_int(confname);
          snprintf(confname, sizeof(confname), "plugins/lighttable/filtering/item%1d", j - nb_removed);
          const int item = dt_conf_get_int(confname);
          snprintf(confname, sizeof(confname), "plugins/lighttable/filtering/off%1d", j - nb_removed);
          const int off = dt_conf_get_int(confname);
          snprintf(confname, sizeof(confname), "plugins/lighttable/filtering/top%1d", j - nb_removed);
          const int top = dt_conf_get_int(confname);
          snprintf(confname, sizeof(confname), "plugins/lighttable/filtering/string%1d", j - nb_removed);
          gchar *string = dt_conf_get_string(confname);
          if(string)
          {
            snprintf(confname, sizeof(confname), "plugins/lighttable/filtering/mode%1d", j - nb_removed - 1);
            dt_conf_set_int(confname, mode);
            snprintf(confname, sizeof(confname), "plugins/lighttable/filtering/item%1d", j - nb_removed - 1);
            dt_conf_set_int(confname, item);
            snprintf(confname, sizeof(confname), "plugins/lighttable/filtering/off%1d", j - nb_removed - 1);
            dt_conf_set_int(confname, off);
            snprintf(confname, sizeof(confname), "plugins/lighttable/filtering/top%1d", j - nb_removed - 1);
            dt_conf_set_int(confname, top);
            snprintf(confname, sizeof(confname), "plugins/lighttable/filtering/string%1d", j - nb_removed - 1);
            dt_conf_set_string(confname, string);
            g_free(string);
          }
        }
        nb_removed++;
      }
    }
    dt_conf_set_int("plugins/lighttable/filtering/num_rules", nb_rules - nb_removed);
  }

  if(reset & _PRESET_SORT)
  {
    // we reset the sorting orders
    dt_conf_set_int("plugins/lighttable/filtering/num_sort", 1);
    dt_conf_set_int("plugins/lighttable/filtering/sort0", 0);
    dt_conf_set_int("plugins/lighttable/filtering/sortorder0", 0);
  }
}

/* Update the params struct with active ruleset */
static void _filters_update_params(dt_lib_filtering_t *d)
{
  /* reset params */
  dt_lib_filtering_params_t *p = d->params;
  memset(p, 0, sizeof(dt_lib_filtering_params_t));
  p->preset_type = _PRESET_ALL;

  /* for each active rule set update params */
  p->rules = CLAMP(dt_conf_get_int("plugins/lighttable/filtering/num_rules"), 0, DT_COLLECTION_MAX_RULES);
  char confname[200] = { 0 };
  for(int i = 0; i < p->rules; i++)
  {
    /* get item */
    snprintf(confname, sizeof(confname), "plugins/lighttable/filtering/item%1d", i);
    p->rule[i].item = dt_conf_get_int(confname);

    /* get mode */
    snprintf(confname, sizeof(confname), "plugins/lighttable/filtering/mode%1d", i);
    p->rule[i].mode = dt_conf_get_int(confname);

    /* get on-off */
    snprintf(confname, sizeof(confname), "plugins/lighttable/filtering/off%1d", i);
    p->rule[i].off = dt_conf_get_int(confname);

    /* get topbar */
    snprintf(confname, sizeof(confname), "plugins/lighttable/filtering/top%1d", i);
    p->rule[i].topbar = dt_conf_get_int(confname);

    /* get string */
    snprintf(confname, sizeof(confname), "plugins/lighttable/filtering/string%1d", i);
    const char *string = dt_conf_get_string_const(confname);
    if(string != NULL)
    {
      g_strlcpy(p->rule[i].string, string, PARAM_STRING_SIZE);
    }
  }

  /* for each sort orders set update params */
  p->sorts = CLAMP(dt_conf_get_int("plugins/lighttable/filtering/num_sort"), 0, DT_COLLECTION_MAX_RULES);
  for(int i = 0; i < p->sorts; i++)
  {
    /* get item */
    snprintf(confname, sizeof(confname), "plugins/lighttable/filtering/sort%1d", i);
    p->sort[i].item = dt_conf_get_int(confname);

    /* get sort order */
    snprintf(confname, sizeof(confname), "plugins/lighttable/filtering/sortorder%1d", i);
    p->sort[i].order = dt_conf_get_int(confname);
  }
}

static void _history_save(dt_lib_filtering_t *d, const gboolean sort)
{
  // get the string of the rules
  char buf[4096] = { 0 };
  if(sort)
    dt_collection_sort_serialize(buf, sizeof(buf));
  else
    dt_collection_serialize(buf, sizeof(buf), TRUE);

  // compare to last saved history
  char confname[200] = { 0 };
  snprintf(confname, sizeof(confname), "plugins/lighttable/filtering/%shistory0", (sort) ? "sort_" : "");
  gchar *str = dt_conf_get_string(confname);
  if(!g_strcmp0(str, buf))
  {
    g_free(str);
    return;
  }
  g_free(str);

  // remove all subsequent history that have the same values
  snprintf(confname, sizeof(confname), "plugins/lighttable/filtering/%shistory_max", (sort) ? "sort_" : "");
  const int nbmax = dt_conf_get_int(confname);
  int move = 0;
  for(int i = 1; i < nbmax; i++)
  {
    snprintf(confname, sizeof(confname), "plugins/lighttable/filtering/%shistory%1d", (sort) ? "sort_" : "", i);
    gchar *string = dt_conf_get_string(confname);

    if(!g_strcmp0(string, buf))
    {
      move++;
      dt_conf_set_string(confname, "");
    }
    else if(move > 0)
    {
      dt_conf_set_string(confname, "");
      snprintf(confname, sizeof(confname), "plugins/lighttable/filtering/%shistory%1d", (sort) ? "sort_" : "",
               i - move);
      dt_conf_set_string(confname, string);
    }
    g_free(string);
  }

  // move all history entries +1 (and delete the last one)
  for(int i = nbmax - 2; i >= 0; i--)
  {
    snprintf(confname, sizeof(confname), "plugins/lighttable/filtering/%shistory%1d", (sort) ? "sort_" : "", i);
    gchar *string = dt_conf_get_string(confname);

    snprintf(confname, sizeof(confname), "plugins/lighttable/filtering/%shistory%1d", (sort) ? "sort_" : "", i + 1);
    dt_conf_set_string(confname, string);
    g_free(string);
  }

  // save current history
  snprintf(confname, sizeof(confname), "plugins/lighttable/filtering/%shistory0", (sort) ? "sort_" : "");
  dt_conf_set_string(confname, buf);
}

void *get_params(dt_lib_module_t *self, int *size)
{
  _filters_update_params(self->data);

  /* allocate a copy of params to return, freed by caller */
  *size = sizeof(dt_lib_filtering_params_t);
  void *p = malloc(*size);
  memcpy(p, ((dt_lib_filtering_t *)self->data)->params, *size);
  return p;
}

int set_params(dt_lib_module_t *self, const void *params, int size)
{
  /* update conf settings from params */
  dt_lib_filtering_params_t *p = (dt_lib_filtering_params_t *)params;

  // reset conf value
  _filtering_reset(p->preset_type);

  char confname[200] = { 0 };
  const int nb_rules_ini = dt_conf_get_int("plugins/lighttable/filtering/num_rules");
  int nb_rules_skipped = 0;

  for(uint32_t i = 0; i < p->rules; i++)
  {
    // if we don't have erased the topbar, be sure that the rule don't already exist in topbar
    int pos = i + nb_rules_ini - nb_rules_skipped;
    for(int j = 0; j < nb_rules_ini; j++)
    {
      snprintf(confname, sizeof(confname), "plugins/lighttable/filtering/item%1d", j);
      if(p->rule[i].item == dt_conf_get_int(confname))
      {
        pos = j;
        nb_rules_skipped++;
        // force params value to be ok for topbar
        p->rule[i].topbar = TRUE;
        p->rule[i].mode = 0;
        p->rule[i].off = FALSE;
      }
    }
    /* set item */
    snprintf(confname, sizeof(confname), "plugins/lighttable/filtering/item%1d", pos);
    dt_conf_set_int(confname, p->rule[i].item);

    /* set mode */
    snprintf(confname, sizeof(confname), "plugins/lighttable/filtering/mode%1u", pos);
    dt_conf_set_int(confname, p->rule[i].mode);

    /* set on-off */
    snprintf(confname, sizeof(confname), "plugins/lighttable/filtering/off%1u", pos);
    dt_conf_set_int(confname, p->rule[i].off);

    /* set topbar */
    snprintf(confname, sizeof(confname), "plugins/lighttable/filtering/top%1u", pos);
    dt_conf_set_int(confname, p->rule[i].topbar);

    /* set string */
    snprintf(confname, sizeof(confname), "plugins/lighttable/filtering/string%1u", pos);
    dt_conf_set_string(confname, p->rule[i].string);
  }

  /* set number of rules */
  g_strlcpy(confname, "plugins/lighttable/filtering/num_rules", sizeof(confname));
  dt_conf_set_int(confname, p->rules + nb_rules_ini - nb_rules_skipped);

  if(p->preset_type & _PRESET_SORT)
  {
    for(uint32_t i = 0; i < p->sorts; i++)
    {
      /* set item */
      snprintf(confname, sizeof(confname), "plugins/lighttable/filtering/sort%1d", i);
      dt_conf_set_int(confname, p->sort[i].item);

      /* set order */
      snprintf(confname, sizeof(confname), "plugins/lighttable/filtering/sortorder%1u", i);
      dt_conf_set_int(confname, p->sort[i].order);
    }

    /* set number of sorts */
    g_strlcpy(confname, "plugins/lighttable/filtering/num_sort", sizeof(confname));
    dt_conf_set_int(confname, p->sorts);
  }

  /* update internal params */
  _filters_update_params(self->data);
  _history_save(self->data, TRUE);
  _history_save(self->data, FALSE);

  /* update ui */
  _filters_gui_update(self);
  _sort_gui_update(self);

  /* update view */
  dt_collection_update_query(darktable.collection, DT_COLLECTION_CHANGE_RELOAD, DT_COLLECTION_PROP_UNDEF, NULL);
  return 0;
}


const char **views(dt_lib_module_t *self)
{
  static const char *v[] = { "lighttable", "map", "print", NULL };
  return v;
}

uint32_t container(dt_lib_module_t *self)
{
  return DT_UI_CONTAINER_PANEL_LEFT_CENTER;
}

static void _conf_update_rule(dt_lib_filtering_rule_t *rule)
{
  const dt_lib_collect_mode_t mode = MAX(0, dt_bauhaus_combobox_get(rule->w_operator));
  const gboolean off = !gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(rule->w_off));

  char confname[200] = { 0 };
  snprintf(confname, sizeof(confname), "plugins/lighttable/filtering/string%1d", rule->num);
  dt_conf_set_string(confname, rule->raw_text);
  snprintf(confname, sizeof(confname), "plugins/lighttable/filtering/item%1d", rule->num);
  dt_conf_set_int(confname, rule->prop);
  snprintf(confname, sizeof(confname), "plugins/lighttable/filtering/mode%1d", rule->num);
  dt_conf_set_int(confname, mode);
  snprintf(confname, sizeof(confname), "plugins/lighttable/filtering/off%1d", rule->num);
  dt_conf_set_int(confname, off);
  snprintf(confname, sizeof(confname), "plugins/lighttable/filtering/top%1d", rule->num);
  dt_conf_set_int(confname, rule->topbar);

  _history_save(rule->lib, FALSE);
}

static void _event_rule_changed(GtkWidget *entry, dt_lib_filtering_rule_t *rule)
{
  if(rule->manual_widget_set) return;

  // force the on-off to on if topbar
  if(rule->topbar && !gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(rule->w_off)))
  {
    rule->manual_widget_set++;
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(rule->w_off), TRUE);
    rule->manual_widget_set--;
  }
  // update the config files
  _conf_update_rule(rule);

  // update the query without throwing signal everywhere
  dt_control_signal_block_by_func(darktable.signals, G_CALLBACK(_dt_collection_updated),
                                  darktable.view_manager->proxy.module_collect.module);
  dt_collection_update_query(darktable.collection, DT_COLLECTION_CHANGE_RELOAD, rule->prop, NULL);
  dt_control_signal_unblock_by_func(darktable.signals, G_CALLBACK(_dt_collection_updated),
                                    darktable.view_manager->proxy.module_collect.module);
}

static void _rule_set_raw_text(dt_lib_filtering_rule_t *rule, const gchar *text, const gboolean signal)
{
  snprintf(rule->raw_text, sizeof(rule->raw_text), "%s", (text == NULL) ? "" : text);
  if(signal) _event_rule_changed(NULL, rule);
}

static void _range_set_tooltip(_widgets_range_t *special)
{
  // we recreate the tooltip
  gchar *val = dtgtk_range_select_get_bounds_pretty(DTGTK_RANGE_SELECT(special->range_select));
  gchar *txt = g_strdup_printf("<b>%s</b>\n%s\n%s\n%s",
                               dt_collection_name(special->rule->prop),
                               _("right-click to set specific values"),
                               _("actual selection:"),
                               val);
  gtk_widget_set_tooltip_markup(special->range_select, txt);
  g_free(txt);
  g_free(val);
}

static void _range_changed(GtkWidget *widget, gpointer user_data)
{
  _widgets_range_t *special = (_widgets_range_t *)user_data;
  if(special->rule->manual_widget_set) return;

  // we recreate the right raw text and put it in the raw entry
  gchar *txt = dtgtk_range_select_get_raw_text(DTGTK_RANGE_SELECT(special->range_select));
  _rule_set_raw_text(special->rule, txt, TRUE);
  g_free(txt);

  _range_set_tooltip(special);

  // synchronize the other widget if any
  _widgets_range_t *dest = NULL;
  if(special == special->rule->w_specific_top)
    dest = special->rule->w_specific;
  else
    dest = special->rule->w_specific_top;

  if(dest)
  {
    special->rule->manual_widget_set++;
    dtgtk_range_select_set_selection_from_raw_text(DTGTK_RANGE_SELECT(dest->range_select), special->rule->raw_text,
                                                   FALSE);
    special->rule->manual_widget_set--;
  }
}

static void _range_widget_add_to_rule(dt_lib_filtering_rule_t *rule, _widgets_range_t *special, const gboolean top)
{
  special->rule = rule;

  _range_set_tooltip(special);

  gtk_box_pack_start(GTK_BOX((top) ? rule->w_special_box_top : rule->w_special_box), special->range_select, TRUE,
                     TRUE, 0);
  g_signal_connect(G_OBJECT(special->range_select), "value-changed", G_CALLBACK(_range_changed), special);
  if(top)
  {
    dt_gui_add_class(gtk_bin_get_child(GTK_BIN(special->range_select)), "quick_filter_box");
  }

  if(top)
    rule->w_specific_top = special;
  else
    rule->w_specific = special;
}

static gboolean _widget_update(dt_lib_filtering_rule_t *rule)
{
  _filter_t *f = _filters_get(rule->prop);
  if(f) return f->update(rule);
  return FALSE;
}

static gboolean _widget_init_special(dt_lib_filtering_rule_t *rule, const gchar *text, dt_lib_module_t *self,
                                     gboolean top)
{
  // remove eventual existing box
  if(!top && rule->w_special_box)
    gtk_widget_destroy(rule->w_special_box);
  else if(rule->w_special_box_top)
    gtk_widget_destroy(rule->w_special_box_top);

  // recreate the box
  if(!top)
    rule->w_special_box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
  else
    rule->w_special_box_top = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
  GtkWidget *special_box = (top) ? rule->w_special_box_top : rule->w_special_box;
  if(!top) gtk_widget_set_name(special_box, "collect-rule-special");
  if(!top)
    gtk_box_pack_start(GTK_BOX(rule->w_widget_box), rule->w_special_box, TRUE, TRUE, 0);
  else
    g_object_ref(G_OBJECT(rule->w_special_box_top));

  _filter_t *f = _filters_get(rule->prop);
  if(f)
    f->widget_init(rule, rule->prop, text, self, top);
  else
    return FALSE;

  gtk_widget_show_all(special_box);
  return TRUE;
}

static void _event_rule_change_type(GtkWidget *widget, dt_lib_module_t *self)
{
  const int mode = GPOINTER_TO_INT(dt_bauhaus_combobox_get_data(widget));
  dt_lib_filtering_rule_t *rule = (dt_lib_filtering_rule_t *)g_object_get_data(G_OBJECT(widget), "rule");

  if(mode == rule->prop) return;

  dt_collection_properties_t old = rule->prop;
  rule->prop = mode;

  // re-init the special widgets
  _widget_init_special(rule, "", self, FALSE);
  _widget_update(rule);

  // reset the raw entry
  _rule_set_raw_text(rule, "", FALSE);

  // update the config files
  _conf_update_rule(rule);

  // update the query without throwing signal everywhere
  dt_control_signal_block_by_func(darktable.signals, G_CALLBACK(_dt_collection_updated),
                                  darktable.view_manager->proxy.module_collect.module);
  dt_collection_update_query(darktable.collection, DT_COLLECTION_CHANGE_RELOAD, old, NULL);
  dt_control_signal_unblock_by_func(darktable.signals, G_CALLBACK(_dt_collection_updated),
                                    darktable.view_manager->proxy.module_collect.module);
}

static void _event_append_rule(GtkWidget *widget, dt_lib_module_t *self)
{
  // add new rule
  dt_lib_filtering_t *d = (dt_lib_filtering_t *)self->data;
  const int mode = GPOINTER_TO_INT(g_object_get_data(G_OBJECT(widget), "collect_id"));
  char confname[200] = { 0 };

  if(mode >= 0)
  {
    // add an empty rule
    if(d->nb_rules >= DT_COLLECTION_MAX_RULES)
    {
      dt_control_log("You can't have more than %d rules", DT_COLLECTION_MAX_RULES);
      return;
    }
    snprintf(confname, sizeof(confname), "plugins/lighttable/filtering/item%1d", d->nb_rules);
    dt_conf_set_int(confname, mode);
    snprintf(confname, sizeof(confname), "plugins/lighttable/filtering/mode%1d", d->nb_rules);
    dt_conf_set_int(confname, DT_LIB_COLLECT_MODE_AND);
    snprintf(confname, sizeof(confname), "plugins/lighttable/filtering/off%1d", d->nb_rules);
    dt_conf_set_int(confname, 0);
    snprintf(confname, sizeof(confname), "plugins/lighttable/filtering/top%1d", d->nb_rules);
    dt_conf_set_int(confname, 0);
    snprintf(confname, sizeof(confname), "plugins/lighttable/filtering/string%1d", d->nb_rules);
    dt_conf_set_string(confname, "");
    d->nb_rules++;
    dt_conf_set_int("plugins/lighttable/filtering/num_rules", d->nb_rules);

    _filters_gui_update(self);
    dt_collection_update_query(darktable.collection, DT_COLLECTION_CHANGE_RELOAD, mode, NULL);
  }
}

static void _popup_add_item(GtkMenuShell *pop, const gchar *name, const int id, const gboolean title,
                            GCallback callback, gpointer data, dt_lib_module_t *self, const float xalign)
{
  // we first verify that the filter is defined
  if(callback != G_CALLBACK(_sort_append_sort) && !title && !_filters_get(id)) return;

  GtkWidget *smt = gtk_menu_item_new_with_label(name);
  if(title)
  {
    gtk_widget_set_name(smt, "collect-popup-title");
    GtkWidget *child = gtk_bin_get_child(GTK_BIN(smt));
    gtk_label_set_xalign(GTK_LABEL(child), xalign);
    gtk_widget_set_sensitive(smt, FALSE);
  }
  else
  {
    gtk_widget_set_name(smt, "collect-popup-item");
    GtkWidget *child = gtk_bin_get_child(GTK_BIN(smt));
    gtk_label_set_xalign(GTK_LABEL(child), xalign);
    g_object_set_data(G_OBJECT(smt), "collect_id", GINT_TO_POINTER(id));
    if(data) g_object_set_data(G_OBJECT(smt), "collect_data", data);
    g_signal_connect(G_OBJECT(smt), "activate", callback, self);
  }
  gtk_menu_shell_append(pop, smt);
}

static gboolean _rule_show_popup(GtkWidget *widget, dt_lib_filtering_rule_t *rule, dt_lib_module_t *self)
{
#define ADD_COLLECT_ENTRY(menu, value)                                                                            \
  _popup_add_item(menu, dt_collection_name(value), value, FALSE, G_CALLBACK(_event_append_rule), rule, self, 0.5);

  // we show a popup with all the possible rules
  // note that only rules with defined filters will be shown
  GtkMenuShell *spop = GTK_MENU_SHELL(gtk_menu_new());
  gtk_widget_set_name(GTK_WIDGET(spop), "collect-popup");
  gtk_widget_set_size_request(GTK_WIDGET(spop), 200, -1);

  // the differents categories
  _popup_add_item(spop, _("files"), 0, TRUE, NULL, NULL, self, 0.0);
  ADD_COLLECT_ENTRY(spop, DT_COLLECTION_PROP_FILMROLL);
  ADD_COLLECT_ENTRY(spop, DT_COLLECTION_PROP_FOLDERS);
  ADD_COLLECT_ENTRY(spop, DT_COLLECTION_PROP_FILENAME);

  _popup_add_item(spop, _("metadata"), 0, TRUE, NULL, NULL, self, 0.0);
  ADD_COLLECT_ENTRY(spop, DT_COLLECTION_PROP_TAG);
  for(unsigned int i = 0; i < DT_METADATA_NUMBER; i++)
  {
    const uint32_t keyid = dt_metadata_get_keyid_by_display_order(i);
    const gchar *name = dt_metadata_get_name(keyid);
    gchar *setting = g_strdup_printf("plugins/lighttable/metadata/%s_flag", name);
    const gboolean hidden = dt_conf_get_int(setting) & DT_METADATA_FLAG_HIDDEN;
    g_free(setting);
    const int meta_type = dt_metadata_get_type(keyid);
    if(meta_type != DT_METADATA_TYPE_INTERNAL && !hidden)
    {
      ADD_COLLECT_ENTRY(spop, DT_COLLECTION_PROP_METADATA + i);
    }
  }
  ADD_COLLECT_ENTRY(spop, DT_COLLECTION_PROP_RATING);
  ADD_COLLECT_ENTRY(spop, DT_COLLECTION_PROP_COLORLABEL);
  ADD_COLLECT_ENTRY(spop, DT_COLLECTION_PROP_TEXTSEARCH);
  ADD_COLLECT_ENTRY(spop, DT_COLLECTION_PROP_GEOTAGGING);

  _popup_add_item(spop, _("times"), 0, TRUE, NULL, NULL, self, 0.0);
  ADD_COLLECT_ENTRY(spop, DT_COLLECTION_PROP_DAY);
  ADD_COLLECT_ENTRY(spop, DT_COLLECTION_PROP_TIME);
  ADD_COLLECT_ENTRY(spop, DT_COLLECTION_PROP_IMPORT_TIMESTAMP);
  ADD_COLLECT_ENTRY(spop, DT_COLLECTION_PROP_CHANGE_TIMESTAMP);
  ADD_COLLECT_ENTRY(spop, DT_COLLECTION_PROP_EXPORT_TIMESTAMP);
  ADD_COLLECT_ENTRY(spop, DT_COLLECTION_PROP_PRINT_TIMESTAMP);

  _popup_add_item(spop, _("capture details"), 0, TRUE, NULL, NULL, self, 0.0);
  ADD_COLLECT_ENTRY(spop, DT_COLLECTION_PROP_CAMERA);
  ADD_COLLECT_ENTRY(spop, DT_COLLECTION_PROP_LENS);
  ADD_COLLECT_ENTRY(spop, DT_COLLECTION_PROP_APERTURE);
  ADD_COLLECT_ENTRY(spop, DT_COLLECTION_PROP_EXPOSURE);
  ADD_COLLECT_ENTRY(spop, DT_COLLECTION_PROP_FOCAL_LENGTH);
  ADD_COLLECT_ENTRY(spop, DT_COLLECTION_PROP_ISO);
  ADD_COLLECT_ENTRY(spop, DT_COLLECTION_PROP_ASPECT_RATIO);

  /* TO BE restored once the filters will be implemented
  _popup_add_item(spop, _("darktable"), 0, TRUE, NULL, NULL, self, 0.0);
  ADD_COLLECT_ENTRY(spop, DT_COLLECTION_PROP_GROUPING);
  ADD_COLLECT_ENTRY(spop, DT_COLLECTION_PROP_LOCAL_COPY);
  ADD_COLLECT_ENTRY(spop, DT_COLLECTION_PROP_HISTORY);
  ADD_COLLECT_ENTRY(spop, DT_COLLECTION_PROP_MODULE);
  ADD_COLLECT_ENTRY(spop, DT_COLLECTION_PROP_ORDER);*/

  dt_gui_menu_popup(GTK_MENU(spop), widget, GDK_GRAVITY_SOUTH, GDK_GRAVITY_NORTH);
  return TRUE;
#undef ADD_COLLECT_ENTRY
}

static void _rule_populate_prop_combo_add(GtkWidget *w, const dt_collection_properties_t prop)
{
  if(!_filters_get(prop)) return;
  dt_bauhaus_combobox_add_full(w, dt_collection_name(prop), DT_BAUHAUS_COMBOBOX_ALIGN_MIDDLE,
                               GUINT_TO_POINTER(prop), NULL, TRUE);
}
static void _rule_populate_prop_combo(dt_lib_filtering_rule_t *rule)
{
  GtkWidget *w = rule->w_prop;
  // first we cleanup existing entries
  dt_bauhaus_combobox_clear(w);
#define ADD_COLLECT_ENTRY(value) _rule_populate_prop_combo_add(w, value);

  // in the case of a pinned rule, we only add the selected entry
  if(rule->topbar)
  {
    ADD_COLLECT_ENTRY(rule->prop);
    gtk_widget_set_tooltip_text(w,
                                _("rule property\nthis can't be changed as the rule is pinned into the toolbar"));
    rule->manual_widget_set++;
    dt_bauhaus_combobox_set_from_value(rule->w_prop, rule->prop);
    rule->manual_widget_set--;
    return;
  }
  // otherwise we add all implemented rules
  gtk_widget_set_tooltip_text(w, _("rule property"));

  dt_bauhaus_combobox_add_section(w, _("files"));
  ADD_COLLECT_ENTRY(DT_COLLECTION_PROP_FILMROLL);
  ADD_COLLECT_ENTRY(DT_COLLECTION_PROP_FOLDERS);
  ADD_COLLECT_ENTRY(DT_COLLECTION_PROP_FILENAME);

  dt_bauhaus_combobox_add_section(w, _("metadata"));
  ADD_COLLECT_ENTRY(DT_COLLECTION_PROP_TAG);
  for(unsigned int i = 0; i < DT_METADATA_NUMBER; i++)
  {
    const uint32_t keyid = dt_metadata_get_keyid_by_display_order(i);
    const gchar *name = dt_metadata_get_name(keyid);
    gchar *setting = g_strdup_printf("plugins/lighttable/metadata/%s_flag", name);
    const gboolean hidden = dt_conf_get_int(setting) & DT_METADATA_FLAG_HIDDEN;
    g_free(setting);
    const int meta_type = dt_metadata_get_type(keyid);
    if(meta_type != DT_METADATA_TYPE_INTERNAL && !hidden)
    {
      ADD_COLLECT_ENTRY(DT_COLLECTION_PROP_METADATA + i);
    }
  }
  ADD_COLLECT_ENTRY(DT_COLLECTION_PROP_RATING);
  ADD_COLLECT_ENTRY(DT_COLLECTION_PROP_COLORLABEL);
  ADD_COLLECT_ENTRY(DT_COLLECTION_PROP_TEXTSEARCH);
  ADD_COLLECT_ENTRY(DT_COLLECTION_PROP_GEOTAGGING);

  dt_bauhaus_combobox_add_section(w, _("times"));
  ADD_COLLECT_ENTRY(DT_COLLECTION_PROP_DAY);
  ADD_COLLECT_ENTRY(DT_COLLECTION_PROP_TIME);
  ADD_COLLECT_ENTRY(DT_COLLECTION_PROP_IMPORT_TIMESTAMP);
  ADD_COLLECT_ENTRY(DT_COLLECTION_PROP_CHANGE_TIMESTAMP);
  ADD_COLLECT_ENTRY(DT_COLLECTION_PROP_EXPORT_TIMESTAMP);
  ADD_COLLECT_ENTRY(DT_COLLECTION_PROP_PRINT_TIMESTAMP);

  dt_bauhaus_combobox_add_section(w, _("capture details"));
  ADD_COLLECT_ENTRY(DT_COLLECTION_PROP_CAMERA);
  ADD_COLLECT_ENTRY(DT_COLLECTION_PROP_LENS);
  ADD_COLLECT_ENTRY(DT_COLLECTION_PROP_APERTURE);
  ADD_COLLECT_ENTRY(DT_COLLECTION_PROP_EXPOSURE);
  ADD_COLLECT_ENTRY(DT_COLLECTION_PROP_FOCAL_LENGTH);
  ADD_COLLECT_ENTRY(DT_COLLECTION_PROP_ISO);
  ADD_COLLECT_ENTRY(DT_COLLECTION_PROP_ASPECT_RATIO);

  /* TO BE restored once the filters will be implemented
  dt_bauhaus_combobox_add_section(w, _("darktable"));
  ADD_COLLECT_ENTRY(DT_COLLECTION_PROP_GROUPING);
  ADD_COLLECT_ENTRY(DT_COLLECTION_PROP_LOCAL_COPY);
  ADD_COLLECT_ENTRY(DT_COLLECTION_PROP_HISTORY);
  ADD_COLLECT_ENTRY(DT_COLLECTION_PROP_MODULE);
  ADD_COLLECT_ENTRY(DT_COLLECTION_PROP_ORDER);*/

#undef ADD_COLLECT_ENTRY

  rule->manual_widget_set++;
  dt_bauhaus_combobox_set_from_value(rule->w_prop, rule->prop);
  rule->manual_widget_set--;
}

static void _event_rule_append(GtkWidget *widget, gpointer user_data)
{
  _rule_show_popup(widget, NULL, (dt_lib_module_t *)user_data);
}

static void _topbar_update(dt_lib_module_t *self)
{
  dt_lib_filtering_t *d = (dt_lib_filtering_t *)self->data;

  // first, we cleanup the filter box
  GtkWidget *fbox = dt_view_filter_get_filters_box(darktable.view_manager);
  GList *childrens = gtk_container_get_children(GTK_CONTAINER(fbox));
  for(GList *l = childrens; l; l = g_list_next(l))
  {
    g_object_ref(G_OBJECT(l->data));
    gtk_container_remove(GTK_CONTAINER(fbox), GTK_WIDGET(l->data));
  }
  g_list_free(childrens);

  // and we add all the special widgets with a top structure
  int nb = 0;
  for(int i = 0; i < d->nb_rules; i++)
  {
    if(d->rule[i].topbar)
    {
      // we create the widget if needed
      if(!d->rule[i].w_special_box_top)
      {
        _widget_init_special(&d->rule[i], d->rule[i].raw_text, self, TRUE);
        _widget_update(&d->rule[i]);
      }
      // we add the filter label if it's the first filter
      if(nb == 0)
      {
        GtkWidget *label = gtk_label_new(C_("quickfilter", "filter"));
        gtk_box_pack_start(GTK_BOX(fbox), label, TRUE, TRUE, 0);
        gtk_widget_show(label);
      }
      gtk_box_pack_start(GTK_BOX(fbox), d->rule[i].w_special_box_top, FALSE, TRUE, 0);
      gtk_widget_show_all(d->rule[i].w_special_box_top);
      nb++;
    }
    else if(d->rule[i].w_special_box_top)
    {
      // we destroy the widget if needed
      gtk_widget_destroy(d->rule[i].w_special_box_top);
      d->rule[i].w_special_box_top = NULL;
      g_free(d->rule[i].w_specific_top);
      d->rule[i].w_specific_top = NULL;
    }
  }
}

static void _widget_header_update(dt_lib_filtering_rule_t *rule)
{
  gtk_widget_set_visible(rule->w_close, !rule->topbar);
  dtgtk_togglebutton_set_paint(DTGTK_TOGGLEBUTTON(rule->w_off),
                               (rule->topbar) ? dtgtk_cairo_paint_pin : dtgtk_cairo_paint_switch, 0, NULL);
  if(rule->topbar)
  {
    gtk_widget_set_tooltip_text(rule->w_off, _("this rule is pinned into the top toolbar\nctrl-click to un-pin"));
  }
  else
  {
    gtk_widget_set_tooltip_text(rule->w_off,
                                _("disable this collect rule\nctrl-click to pin into the top toolbar"));
  }
  _rule_populate_prop_combo(rule);
}

static void _rule_topbar_toggle(dt_lib_filtering_rule_t *rule, dt_lib_module_t *self)
{
  rule->topbar = !rule->topbar;
  // activate the rule if needed
  if(rule->topbar && !gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(rule->w_off)))
  {
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(rule->w_off), TRUE);
  }
  _conf_update_rule(rule);
  _topbar_update(self);

  // update the rule header
  _widget_header_update(rule);
}

static gboolean _event_rule_close(GtkWidget *widget, GdkEventButton *event, dt_lib_module_t *self)
{
  dt_lib_filtering_rule_t *rule = (dt_lib_filtering_rule_t *)g_object_get_data(G_OBJECT(widget), "rule");
  if(rule->manual_widget_set) return TRUE;

  if(dt_modifier_is(event->state, GDK_CONTROL_MASK))
  {
    _rule_topbar_toggle(rule, self);
  }
  else if(!rule->topbar)
  {
    // decrease the nb of active rules
    dt_lib_filtering_t *d = rule->lib;
    if(d->nb_rules <= 0) return FALSE;
    d->nb_rules--;
    dt_conf_set_int("plugins/lighttable/filtering/num_rules", d->nb_rules);

    // move up all still active rules by one.
    for(int i = rule->num; i < DT_COLLECTION_MAX_RULES - 1; i++)
    {
      char confname[200] = { 0 };
      snprintf(confname, sizeof(confname), "plugins/lighttable/filtering/mode%1d", i + 1);
      const int mode = dt_conf_get_int(confname);
      snprintf(confname, sizeof(confname), "plugins/lighttable/filtering/item%1d", i + 1);
      const int item = dt_conf_get_int(confname);
      snprintf(confname, sizeof(confname), "plugins/lighttable/filtering/off%1d", i + 1);
      const int off = dt_conf_get_int(confname);
      snprintf(confname, sizeof(confname), "plugins/lighttable/filtering/top%1d", i + 1);
      const int top = dt_conf_get_int(confname);
      snprintf(confname, sizeof(confname), "plugins/lighttable/filtering/string%1d", i + 1);
      gchar *string = dt_conf_get_string(confname);
      if(string)
      {
        snprintf(confname, sizeof(confname), "plugins/lighttable/filtering/mode%1d", i);
        dt_conf_set_int(confname, mode);
        snprintf(confname, sizeof(confname), "plugins/lighttable/filtering/item%1d", i);
        dt_conf_set_int(confname, item);
        snprintf(confname, sizeof(confname), "plugins/lighttable/filtering/off%1d", i);
        dt_conf_set_int(confname, off);
        snprintf(confname, sizeof(confname), "plugins/lighttable/filtering/top%1d", i);
        dt_conf_set_int(confname, top);
        snprintf(confname, sizeof(confname), "plugins/lighttable/filtering/string%1d", i);
        dt_conf_set_string(confname, string);
        g_free(string);
      }
    }

    _filters_gui_update(self);
    dt_collection_update_query(darktable.collection, DT_COLLECTION_CHANGE_RELOAD, rule->prop, NULL);
  }
  else
    return FALSE;

  return TRUE;
}

static gboolean _event_rule_change_popup(GtkWidget *widget, GdkEventButton *event, dt_lib_module_t *self)
{
  dt_lib_filtering_rule_t *rule = (dt_lib_filtering_rule_t *)g_object_get_data(G_OBJECT(widget), "rule");
  if(dt_modifier_is(event->state, GDK_CONTROL_MASK) || (rule->topbar && widget != rule->w_prop))
  {
    _rule_topbar_toggle(rule, self);
  }
  else if(!rule->topbar && widget == rule->w_prop)
  {
    _rule_show_popup(rule->w_prop, rule, self);
  }
  else
    return FALSE;
  return TRUE;
}

static void _event_header_resized(GtkWidget *widget, GtkAllocation *allocation, dt_lib_filtering_rule_t *rule)
{
  // we want to ensure that the combobox and the buttons box are the same size
  // so the property widget is centered
  gtk_widget_set_size_request(rule->w_btn_box, allocation->width, -1);
}

// initialise or update a rule widget. Return if the a new widget has been created
static gboolean _widget_init(dt_lib_filtering_rule_t *rule, const dt_collection_properties_t prop,
                             const gchar *text, const dt_lib_collect_mode_t mode, gboolean off, gboolean top,
                             const int pos, dt_lib_module_t *self)
{
  rule->manual_widget_set++;

  const gboolean newmain = (rule->w_main == NULL);
  const gboolean newprop = (prop != rule->prop);
  GtkWidget *hbox = NULL;

  rule->prop = prop;
  rule->topbar = top;

  if(newmain)
  {
    // the main box
    rule->w_main = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    gtk_widget_set_name(rule->w_main, "collect-rule-widget");

    // the first line
    hbox = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
    gtk_box_pack_start(GTK_BOX(rule->w_main), hbox, TRUE, TRUE, 0);
    gtk_widget_set_name(hbox, "collect-header-box");

    // operator type
    rule->w_operator = dt_bauhaus_combobox_new(NULL);
    DT_BAUHAUS_WIDGET(rule->w_operator)->show_quad = FALSE;
    dt_bauhaus_combobox_add_aligned(rule->w_operator, _("and"), DT_BAUHAUS_COMBOBOX_ALIGN_LEFT);
    dt_bauhaus_combobox_add_aligned(rule->w_operator, _("or"), DT_BAUHAUS_COMBOBOX_ALIGN_LEFT);
    dt_bauhaus_combobox_add_aligned(rule->w_operator, _("and not"), DT_BAUHAUS_COMBOBOX_ALIGN_LEFT);
    dt_bauhaus_combobox_set_selected_text_align(rule->w_operator, DT_BAUHAUS_COMBOBOX_ALIGN_LEFT);
    gtk_widget_set_tooltip_text(rule->w_operator, _("define how this rule should interact with the previous one"));
    gtk_box_pack_start(GTK_BOX(hbox), rule->w_operator, FALSE, FALSE, 0);
    g_signal_connect(G_OBJECT(rule->w_operator), "value-changed", G_CALLBACK(_event_rule_changed), rule);
  }

  dt_bauhaus_combobox_set(rule->w_operator, mode);
  gtk_widget_set_sensitive(rule->w_operator, pos > 0);

  // property
  if(newmain)
  {
    rule->w_prop = dt_bauhaus_combobox_new(NULL);
    dt_bauhaus_combobox_set_selected_text_align(rule->w_prop, DT_BAUHAUS_COMBOBOX_ALIGN_MIDDLE);
    DT_BAUHAUS_WIDGET(rule->w_prop)->show_quad = FALSE;
    _rule_populate_prop_combo(rule);
    g_object_set_data(G_OBJECT(rule->w_prop), "rule", rule);
    dt_bauhaus_combobox_set_from_value(rule->w_prop, prop);
    g_signal_connect(G_OBJECT(rule->w_prop), "value-changed", G_CALLBACK(_event_rule_change_type), self);
    gtk_box_pack_start(GTK_BOX(hbox), rule->w_prop, TRUE, TRUE, 0);
  }
  else if(newprop)
  {
    _rule_populate_prop_combo(rule);
    dt_bauhaus_combobox_set_from_value(rule->w_prop, prop);
  }

  if(newmain)
  {
    rule->w_btn_box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
    gtk_box_pack_start(GTK_BOX(hbox), rule->w_btn_box, FALSE, FALSE, 0);

    // on-off button
    rule->w_off = dtgtk_togglebutton_new(dtgtk_cairo_paint_switch, 0, NULL);
    dt_gui_add_class(rule->w_off, "dt_transparent_background");
    g_object_set_data(G_OBJECT(rule->w_off), "rule", rule);
    g_signal_connect(G_OBJECT(rule->w_off), "button-press-event", G_CALLBACK(_event_rule_change_popup), self);
    g_signal_connect(G_OBJECT(rule->w_off), "toggled", G_CALLBACK(_event_rule_changed), rule);
    gtk_box_pack_end(GTK_BOX(rule->w_btn_box), rule->w_off, FALSE, FALSE, 0);

    // remove button
    rule->w_close = dtgtk_button_new(dtgtk_cairo_paint_cancel, 0, NULL);
    gtk_widget_set_no_show_all(rule->w_close, TRUE);
    g_object_set_data(G_OBJECT(rule->w_close), "rule", rule);
    gtk_widget_set_tooltip_text(rule->w_close,
                                _("remove this collect rule\nctrl-click to pin into the top toolbar"));
    g_signal_connect(G_OBJECT(rule->w_close), "button-press-event", G_CALLBACK(_event_rule_close), self);
    gtk_box_pack_end(GTK_BOX(rule->w_btn_box), rule->w_close, FALSE, FALSE, 0);

    g_signal_connect(G_OBJECT(rule->w_operator), "size-allocate", G_CALLBACK(_event_header_resized), rule);
  }

  gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(rule->w_off), !off);

  _widget_header_update(rule);

  if(newmain)
  {
    // the second line
    rule->w_widget_box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
    gtk_box_pack_start(GTK_BOX(rule->w_main), rule->w_widget_box, TRUE, TRUE, 0);
    gtk_widget_set_name(rule->w_widget_box, "collect-module-hbox");
  }

  _rule_set_raw_text(rule, text, FALSE);

  // initialize the specific entries if any
  _widget_init_special(rule, text, self, FALSE);

  rule->manual_widget_set--;
  return newmain;
}

static void _widget_special_destroy(dt_lib_filtering_rule_t *rule)
{
  if(rule->w_special_box)
  {
    gtk_widget_destroy(rule->w_special_box);
    rule->w_special_box = NULL;
    g_free(rule->w_specific);
    rule->w_specific = NULL;
  }
  if(rule->w_special_box_top)
  {
    gtk_widget_destroy(rule->w_special_box_top);
    rule->w_special_box_top = NULL;
    g_free(rule->w_specific_top);
    rule->w_specific_top = NULL;
  }
}

static void _filters_gui_update(dt_lib_module_t *self)
{
  dt_lib_filtering_t *d = (dt_lib_filtering_t *)self->data;

  ++darktable.gui->reset;
  d->nb_rules = CLAMP(dt_conf_get_int("plugins/lighttable/filtering/num_rules"), 0, DT_COLLECTION_MAX_RULES);
  char confname[200] = { 0 };

  // create or update defined rules
  for(int i = 0; i < d->nb_rules; i++)
  {
    snprintf(confname, sizeof(confname), "plugins/lighttable/filtering/item%1d", i);
    const dt_collection_properties_t prop = dt_conf_get_int(confname);
    snprintf(confname, sizeof(confname), "plugins/lighttable/filtering/string%1d", i);
    const gchar *txt = dt_conf_get_string_const(confname);
    snprintf(confname, sizeof(confname), "plugins/lighttable/filtering/mode%1d", i);
    const dt_lib_collect_mode_t rmode = dt_conf_get_int(confname);
    snprintf(confname, sizeof(confname), "plugins/lighttable/filtering/top%1d", i);
    const int top = dt_conf_get_int(confname);
    snprintf(confname, sizeof(confname), "plugins/lighttable/filtering/off%1d", i);
    const int off = (top) ? FALSE : dt_conf_get_int(confname);
    // cleanup previous special widgets
    _widget_special_destroy(&d->rule[i]);
    // recreate main widget
    if(_widget_init(&d->rule[i], prop, txt, rmode, off, top, i, self))
      gtk_box_pack_start(GTK_BOX(d->rules_box), d->rule[i].w_main, FALSE, TRUE, 0);
    gtk_widget_show_all(d->rule[i].w_main);

    // if needed, we also load the duplicate widget for the topbar
    if(top)
    {
      _widget_init_special(&d->rule[i], txt, self, TRUE);
    }
    _widget_update(&d->rule[i]);
  }

  // remove all remaining rules
  for(int i = d->nb_rules; i < DT_COLLECTION_MAX_RULES; i++)
  {
    d->rule[i].prop = 0;
    if(d->rule[i].w_main)
    {
      gtk_widget_destroy(d->rule[i].w_main);
      d->rule[i].w_main = NULL;
      d->rule[i].w_special_box = NULL;
    }
  }

  // update topbar
  _topbar_update(self);

  --darktable.gui->reset;
}

static void _filtering_gui_update(dt_lib_module_t *self)
{
  _filters_gui_update(self);
  _sort_gui_update(self);
}

void gui_reset(dt_lib_module_t *self)
{
  GdkKeymap *kmap = gdk_keymap_get_for_display(gdk_display_get_default());
  guint state = gdk_keymap_get_modifier_state(kmap);
  if(state & GDK_CONTROL_MASK)
  {
    // we remove all rules
    _filtering_reset(_PRESET_ALL);
  }
  else
  {
    _filtering_reset(_PRESET_FILTERS | _PRESET_SORT);
  }

  _filters_gui_update(self);
  _sort_gui_update(self);

  dt_collection_set_query_flags(darktable.collection, COLLECTION_QUERY_FULL);
  dt_collection_update_query(darktable.collection, DT_COLLECTION_CHANGE_RELOAD, DT_COLLECTION_PROP_UNDEF, NULL);
}

int position()
{
  return 380;
}

static void _dt_collection_updated(gpointer instance, dt_collection_change_t query_change,
                                   dt_collection_properties_t changed_property, gpointer imgs, int next,
                                   gpointer self)
{
  dt_lib_module_t *dm = (dt_lib_module_t *)self;
  dt_lib_filtering_t *d = (dt_lib_filtering_t *)dm->data;

  gchar *where_ext = dt_collection_get_extended_where(darktable.collection, 99999);
  if(g_strcmp0(where_ext, d->last_where_ext))
  {
    g_free(d->last_where_ext);
    d->last_where_ext = g_strdup(where_ext);
    for(int i = 0; i <= d->nb_rules; i++)
    {
      _widget_update(&d->rule[i]);
    }
  }
}

static void _history_pretty_print(const char *buf, char *out, size_t outsize)
{
  memset(out, 0, outsize);

  if(!buf || buf[0] == '\0') return;

  int num_rules = 0;
  char str[400] = { 0 };
  int mode, item, off, top;
  int c;
  sscanf(buf, "%d", &num_rules);
  while(buf[0] != '\0' && buf[0] != ':') buf++;
  if(buf[0] == ':') buf++;

  for(int k = 0; k < num_rules; k++)
  {
    const int n = sscanf(buf, "%d:%d:%d:%d:%399[^$]", &mode, &item, &off, &top, str);

    if(n == 5)
    {
      if(k > 0)
      {
        switch(mode)
        {
          case DT_LIB_COLLECT_MODE_AND:
            c = g_strlcpy(out, _(" and "), outsize);
            out += c;
            outsize -= c;
            break;
          case DT_LIB_COLLECT_MODE_OR:
            c = g_strlcpy(out, _(" or "), outsize);
            out += c;
            outsize -= c;
            break;
          default: // case DT_LIB_COLLECT_MODE_AND_NOT:
            c = g_strlcpy(out, _(" but not "), outsize);
            out += c;
            outsize -= c;
            break;
        }
      }
      int i = 0;
      while(str[i] != '\0' && str[i] != '$') i++;
      if(str[i] == '$') str[i] = '\0';

      gchar *pretty = NULL;
      if(item == DT_COLLECTION_PROP_COLORLABEL)
        pretty = _colors_pretty_print(str);
      else if(!g_strcmp0(str, "%"))
        pretty = g_strdup(_("all"));
      else
        pretty = g_markup_escape_text(str, -1);

      if(off)
      {
        c = snprintf(out, outsize, "%s%s %s", item < DT_COLLECTION_PROP_LAST ? dt_collection_name(item) : "???",
                     _(" (off)"), pretty);
      }
      else
      {
        c = snprintf(out, outsize, "%s %s", item < DT_COLLECTION_PROP_LAST ? dt_collection_name(item) : "???",
                     pretty);
      }

      g_free(pretty);
      out += c;
      outsize -= c;
    }
    while(buf[0] != '$' && buf[0] != '\0') buf++;
    if(buf[0] == '$') buf++;
  }
}

static void _event_history_apply(GtkWidget *widget, dt_lib_module_t *self)
{
  const int hid = GPOINTER_TO_INT(g_object_get_data(G_OBJECT(widget), "history"));
  if(hid < 0 || hid >= dt_conf_get_int("plugins/lighttable/filtering/history_max")) return;

  char confname[200];
  snprintf(confname, sizeof(confname), "plugins/lighttable/filtering/history%1d", hid);
  const char *line = dt_conf_get_string_const(confname);
  if(line && line[0] != '\0')
  {
    dt_collection_deserialize(line, TRUE);
    _filters_gui_update(self);
  }
}

static void _event_history_show(GtkWidget *widget, gpointer user_data)
{
  dt_lib_module_t *self = (dt_lib_module_t *)user_data;
  // we show a popup with all the history entries
  GtkMenuShell *pop = GTK_MENU_SHELL(gtk_menu_new());
  gtk_widget_set_name(GTK_WIDGET(pop), "collect-popup");
  gtk_widget_set_size_request(GTK_WIDGET(pop), 200, -1);

  const int maxitems = dt_conf_get_int("plugins/lighttable/filtering/history_max");

  for(int i = 0; i < maxitems; i++)
  {
    char confname[200];
    snprintf(confname, sizeof(confname), "plugins/lighttable/filtering/history%1d", i);
    const char *line = dt_conf_get_string_const(confname);
    if(line && line[0] != '\0')
    {
      char str[2048] = { 0 };
      _history_pretty_print(line, str, sizeof(str));
      GtkWidget *smt = gtk_menu_item_new_with_label(str);
      gtk_widget_set_name(smt, "collect-popup-item");
      gtk_widget_set_tooltip_text(smt, str);
      GtkWidget *child = gtk_bin_get_child(GTK_BIN(smt));
      gtk_label_set_use_markup(GTK_LABEL(child), TRUE);
      g_object_set_data(G_OBJECT(smt), "history", GINT_TO_POINTER(i));
      g_signal_connect(G_OBJECT(smt), "activate", G_CALLBACK(_event_history_apply), self);
      gtk_menu_shell_append(pop, smt);
    }
    else
      break;
  }

  dt_gui_menu_popup(GTK_MENU(pop), widget, GDK_GRAVITY_SOUTH, GDK_GRAVITY_NORTH);
}

// save a sort rule inside the conf
static void _conf_update_sort(_widgets_sort_t *sort)
{
  const gboolean order = gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(sort->direction));
  const int sortid = GPOINTER_TO_UINT(dt_bauhaus_combobox_get_data(sort->sort));

  char confname[200] = { 0 };
  // if it's the last sort order, remember previous value for last order
  if(sort->num == sort->lib->nb_sort - 1)
  {
    snprintf(confname, sizeof(confname), "plugins/lighttable/filtering/sort%1d", sort->num);
    const dt_collection_sort_t lastsort = dt_conf_get_int(confname);
    snprintf(confname, sizeof(confname), "plugins/lighttable/filtering/sortorder%1d", sort->num);
    const int lastsortorder = dt_conf_get_int(confname);
    if(lastsort != sortid)
    {
      dt_conf_set_int("plugins/lighttable/filtering/lastsort", lastsort);
      dt_conf_set_int("plugins/lighttable/filtering/lastsortorder", lastsortorder);
    }
  }

  snprintf(confname, sizeof(confname), "plugins/lighttable/filtering/sort%1d", sort->num);
  dt_conf_set_int(confname, sortid);
  snprintf(confname, sizeof(confname), "plugins/lighttable/filtering/sortorder%1d", sort->num);
  dt_conf_set_int(confname, order);

  _history_save(sort->lib, TRUE);
}

// update the sort asc/desc arrow
static void _sort_update_arrow(GtkWidget *widget)
{
  const gboolean reverse = gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(widget));
  if(reverse)
    dtgtk_togglebutton_set_paint(DTGTK_TOGGLEBUTTON(widget), dtgtk_cairo_paint_sortby, CPF_DIRECTION_DOWN, NULL);
  else
    dtgtk_togglebutton_set_paint(DTGTK_TOGGLEBUTTON(widget), dtgtk_cairo_paint_sortby, CPF_DIRECTION_UP, NULL);
  gtk_widget_queue_draw(widget);
}

// set the sort order to the collection and update the query
static void _sort_update_query(_widgets_sort_t *sort)
{
  // if needed, we sync the filter bar
  if(sort->num == 0)
  {
    _widgets_sort_t *dest = (sort->top) ? &sort->lib->sort[0] : &sort->lib->sorttop;
    sort->lib->manual_sort_set++;
    const gboolean active = gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(sort->direction));
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(dest->direction), active);
    _sort_update_arrow(dest->direction);
    const int val = GPOINTER_TO_UINT(dt_bauhaus_combobox_get_data(sort->sort));
    dt_bauhaus_combobox_set_from_value(dest->sort, val);
    sort->lib->manual_sort_set--;
  }

  // we save the sort in conf
  _conf_update_sort(sort);

  /* sometimes changes */
  dt_collection_set_query_flags(darktable.collection, COLLECTION_QUERY_FULL);

  /* updates query */
  dt_collection_update_query(darktable.collection, DT_COLLECTION_CHANGE_RELOAD, DT_COLLECTION_PROP_SORT, NULL);
}

static void _sort_reverse_changed(GtkDarktableToggleButton *widget, _widgets_sort_t *sort)
{
  if(sort->lib->manual_sort_set) return;

  _sort_update_arrow(GTK_WIDGET(widget));
  _sort_update_query(sort);
}

static void _sort_combobox_changed(GtkWidget *widget, gpointer user_data)
{
  _widgets_sort_t *sort = (_widgets_sort_t *)user_data;
  if(sort->lib->manual_sort_set) return;

  _sort_update_query(sort);
}

// this proxy function is primary called when the sort part of the filter bar is changed
static void _proxy_reset_filter(dt_lib_module_t *self, gboolean smart_filter)
{
  dt_lib_filtering_t *d = (dt_lib_filtering_t *)self->data;

  // reset each rule. we only throw the signal for the last one
  for(int i = 0; i < d->nb_rules; i++)
  {
    _rule_set_raw_text(&d->rule[i], "", (i == d->nb_rules - 1));
    _widget_update(&d->rule[i]);
    _conf_update_rule(&d->rule[i]);
  }
}

static gboolean _sort_close(GtkWidget *widget, GdkEventButton *event, dt_lib_module_t *self)
{
  _widgets_sort_t *sort = (_widgets_sort_t *)g_object_get_data(G_OBJECT(widget), "sort");
  if(sort->lib->manual_sort_set) return TRUE;

  // decrease the nb of active rules
  dt_lib_filtering_t *d = sort->lib;
  if(d->nb_sort <= 1) return FALSE;
  d->nb_sort--;
  dt_conf_set_int("plugins/lighttable/filtering/num_sort", d->nb_sort);

  // move up all still active rules by one.
  for(int i = sort->num; i < DT_COLLECTION_MAX_RULES - 1; i++)
  {
    char confname[200] = { 0 };
    snprintf(confname, sizeof(confname), "plugins/lighttable/filtering/sort%1d", i + 1);
    const int sortid = dt_conf_get_int(confname);
    snprintf(confname, sizeof(confname), "plugins/lighttable/filtering/sortorder%1d", i + 1);
    const int sortorder = dt_conf_get_int(confname);

    snprintf(confname, sizeof(confname), "plugins/lighttable/filtering/sort%1d", i);
    dt_conf_set_int(confname, sortid);
    snprintf(confname, sizeof(confname), "plugins/lighttable/filtering/sortorder%1d", i);
    dt_conf_set_int(confname, sortorder);
  }

  _history_save(d, TRUE);
  _sort_gui_update(self);
  dt_collection_update_query(darktable.collection, DT_COLLECTION_CHANGE_RELOAD, DT_COLLECTION_PROP_SORT, NULL);

  return TRUE;
}

static gboolean _sort_init(_widgets_sort_t *sort, const dt_collection_sort_t sortid, const int sortorder,
                           const int num, dt_lib_module_t *self)
{
  dt_lib_filtering_t *d = (dt_lib_filtering_t *)self->data;
  d->manual_sort_set++;
  sort->num = num;
  sort->sortid = sortid;

  const gboolean top = (sort == &d->sorttop);

  const gboolean ret = (!sort->box);

  if(!sort->box)
  {
    sort->lib = d;
    sort->box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
    gtk_widget_set_hexpand(sort->box, TRUE);
    gtk_widget_set_name(sort->box, "collect-sort-widget");
    sort->sort = dt_bauhaus_combobox_new(NULL);
    gtk_widget_set_tooltip_text(sort->sort, _("determine the sort order of shown images"));
    g_signal_connect(G_OBJECT(sort->sort), "value-changed", G_CALLBACK(_sort_combobox_changed), sort);

#define ADD_SORT_ENTRY(value)                                                                                     \
  dt_bauhaus_combobox_add_full(sort->sort, dt_collection_sort_name(value), DT_BAUHAUS_COMBOBOX_ALIGN_RIGHT,       \
                               GUINT_TO_POINTER(value), NULL, TRUE)

    ADD_SORT_ENTRY(DT_COLLECTION_SORT_FILENAME);
    ADD_SORT_ENTRY(DT_COLLECTION_SORT_DATETIME);
    ADD_SORT_ENTRY(DT_COLLECTION_SORT_IMPORT_TIMESTAMP);
    ADD_SORT_ENTRY(DT_COLLECTION_SORT_CHANGE_TIMESTAMP);
    ADD_SORT_ENTRY(DT_COLLECTION_SORT_EXPORT_TIMESTAMP);
    ADD_SORT_ENTRY(DT_COLLECTION_SORT_PRINT_TIMESTAMP);
    ADD_SORT_ENTRY(DT_COLLECTION_SORT_RATING);
    ADD_SORT_ENTRY(DT_COLLECTION_SORT_ID);
    ADD_SORT_ENTRY(DT_COLLECTION_SORT_COLOR);
    ADD_SORT_ENTRY(DT_COLLECTION_SORT_GROUP);
    ADD_SORT_ENTRY(DT_COLLECTION_SORT_PATH);
    ADD_SORT_ENTRY(DT_COLLECTION_SORT_CUSTOM_ORDER);
    ADD_SORT_ENTRY(DT_COLLECTION_SORT_TITLE);
    ADD_SORT_ENTRY(DT_COLLECTION_SORT_DESCRIPTION);
    ADD_SORT_ENTRY(DT_COLLECTION_SORT_ASPECT_RATIO);
    ADD_SORT_ENTRY(DT_COLLECTION_SORT_SHUFFLE);

#undef ADD_SORT_ENTRY

    gtk_box_pack_start(GTK_BOX(sort->box), sort->sort, TRUE, TRUE, 0);

    /* reverse order checkbutton */
    sort->direction = dtgtk_togglebutton_new(dtgtk_cairo_paint_sortby, CPF_DIRECTION_UP, NULL);
    gtk_widget_set_halign(sort->direction, GTK_ALIGN_START);
    gtk_box_pack_start(GTK_BOX(sort->box), sort->direction, FALSE, TRUE, 0);
    g_signal_connect(G_OBJECT(sort->direction), "toggled", G_CALLBACK(_sort_reverse_changed), sort);
    dt_gui_add_class(sort->direction, "dt_ignore_fg_state");

    sort->close = dtgtk_button_new(dtgtk_cairo_paint_cancel, 0, NULL);
    gtk_widget_set_no_show_all(sort->close, TRUE);
    g_object_set_data(G_OBJECT(sort->close), "sort", sort);
    gtk_widget_set_tooltip_text(sort->close, _("remove this sort order"));
    g_signal_connect(G_OBJECT(sort->close), "button-press-event", G_CALLBACK(_sort_close), self);
    gtk_box_pack_start(GTK_BOX(sort->box), sort->close, FALSE, FALSE, 0);
  }

  dt_bauhaus_combobox_set_from_value(sort->sort, sortid);
  gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(sort->direction), sortorder);
  gtk_widget_set_visible(sort->close, (sort->lib->nb_sort > 1) && !top);
  _sort_update_arrow(sort->direction);

  gtk_widget_show_all(sort->box);

  d->manual_sort_set--;
  return ret;
}

static void _sort_gui_update(dt_lib_module_t *self)
{
  dt_lib_filtering_t *d = (dt_lib_filtering_t *)self->data;

  ++darktable.gui->reset;
  d->nb_sort = CLAMP(dt_conf_get_int("plugins/lighttable/filtering/num_sort"), 0, DT_COLLECTION_MAX_RULES);
  char confname[200] = { 0 };

  // handle the case where no sort item is already defined
  if(d->nb_sort == 0)
  {
    // set conf values
    dt_conf_set_int("plugins/lighttable/filtering/num_sort", 1);
    dt_conf_set_int("plugins/lighttable/filtering/sort0", DT_COLLECTION_SORT_FILENAME);
    dt_conf_set_int("plugins/lighttable/filtering/sortorder0", 0);
    d->nb_sort = 1;
  }

  // create or update defined rules
  for(int i = 0; i < d->nb_sort; i++)
  {
    snprintf(confname, sizeof(confname), "plugins/lighttable/filtering/sort%1d", i);
    const dt_collection_sort_t sort = dt_conf_get_int(confname);
    snprintf(confname, sizeof(confname), "plugins/lighttable/filtering/sortorder%1d", i);
    const int sortorder = dt_conf_get_int(confname);

    // recreate main widget
    if(_sort_init(&d->sort[i], sort, sortorder, i, self))
      gtk_grid_attach(GTK_GRID(d->sort_box), d->sort[i].box, 1, i, 1, 1);

    // we also put the first sort item into the topbar
    if(i == 0)
    {
      d->sorttop.top = TRUE;
      GtkWidget *sort_topbox = dt_view_filter_get_sort_box(darktable.view_manager);
      if(sort_topbox && _sort_init(&d->sorttop, sort, sortorder, i, self))
      {
        gtk_box_pack_start(GTK_BOX(sort_topbox), d->sorttop.box, FALSE, TRUE, 0);
      }
    }
  }

  // remove all remaining rules
  for(int i = d->nb_sort; i < DT_COLLECTION_MAX_RULES; i++)
  {
    d->sort[i].sortid = 0;
    if(d->sort[i].box)
    {
      gtk_widget_destroy(d->sort[i].box);
      d->sort[i].box = NULL;
    }
  }

  --darktable.gui->reset;
}

static void _sort_append_sort(GtkWidget *widget, dt_lib_module_t *self)
{
  // add new rule
  dt_lib_filtering_t *d = (dt_lib_filtering_t *)self->data;
  const int sortid = GPOINTER_TO_INT(g_object_get_data(G_OBJECT(widget), "collect_id"));
  char confname[200] = { 0 };

  if(sortid >= 0)
  {
    // add an empty rule
    if(d->nb_sort >= DT_COLLECTION_MAX_RULES)
    {
      dt_control_log("You can't have more than %d sort orders", DT_COLLECTION_MAX_RULES);
      return;
    }
    snprintf(confname, sizeof(confname), "plugins/lighttable/filtering/sort%1d", d->nb_sort);
    dt_conf_set_int(confname, sortid);
    snprintf(confname, sizeof(confname), "plugins/lighttable/filtering/sortorder%1d", d->nb_sort);
    dt_conf_set_int(confname, 0);
    d->nb_sort++;
    dt_conf_set_int("plugins/lighttable/filtering/num_sort", d->nb_sort);

    _history_save(d, TRUE);
    _sort_gui_update(self);
    dt_collection_update_query(darktable.collection, DT_COLLECTION_CHANGE_RELOAD, DT_COLLECTION_PROP_SORT, NULL);
  }
}

static void _sort_show_add_popup(GtkWidget *widget, gpointer user_data)
{
  dt_lib_module_t *self = (dt_lib_module_t *)user_data;
  // we show a popup with all the possible sort
  GtkMenuShell *spop = GTK_MENU_SHELL(gtk_menu_new());
  gtk_widget_set_name(GTK_WIDGET(spop), "collect-popup");
  gtk_widget_set_size_request(GTK_WIDGET(spop), 200, -1);

#define ADD_SORT_ENTRY(value)                                                                                     \
  _popup_add_item(spop, dt_collection_sort_name(value), value, FALSE, G_CALLBACK(_sort_append_sort), NULL, self,  \
                  0.0);

  ADD_SORT_ENTRY(DT_COLLECTION_SORT_FILENAME);
  ADD_SORT_ENTRY(DT_COLLECTION_SORT_DATETIME);
  ADD_SORT_ENTRY(DT_COLLECTION_SORT_IMPORT_TIMESTAMP);
  ADD_SORT_ENTRY(DT_COLLECTION_SORT_CHANGE_TIMESTAMP);
  ADD_SORT_ENTRY(DT_COLLECTION_SORT_EXPORT_TIMESTAMP);
  ADD_SORT_ENTRY(DT_COLLECTION_SORT_PRINT_TIMESTAMP);
  ADD_SORT_ENTRY(DT_COLLECTION_SORT_RATING);
  ADD_SORT_ENTRY(DT_COLLECTION_SORT_ID);
  ADD_SORT_ENTRY(DT_COLLECTION_SORT_COLOR);
  ADD_SORT_ENTRY(DT_COLLECTION_SORT_GROUP);
  ADD_SORT_ENTRY(DT_COLLECTION_SORT_PATH);
  ADD_SORT_ENTRY(DT_COLLECTION_SORT_CUSTOM_ORDER);
  ADD_SORT_ENTRY(DT_COLLECTION_SORT_TITLE);
  ADD_SORT_ENTRY(DT_COLLECTION_SORT_DESCRIPTION);
  ADD_SORT_ENTRY(DT_COLLECTION_SORT_ASPECT_RATIO);
  ADD_SORT_ENTRY(DT_COLLECTION_SORT_SHUFFLE);

  dt_gui_menu_popup(GTK_MENU(spop), widget, GDK_GRAVITY_SOUTH, GDK_GRAVITY_NORTH);
#undef ADD_SORT_ENTRY

}

static void _sort_history_pretty_print(const char *buf, char *out, size_t outsize)
{
  memset(out, 0, outsize);

  if(!buf || buf[0] == '\0') return;

  int num_rules = 0;
  int sortid, sortorder;
  int c;
  sscanf(buf, "%d", &num_rules);
  while(buf[0] != '\0' && buf[0] != ':') buf++;
  if(buf[0] == ':') buf++;

  for(int k = 0; k < num_rules; k++)
  {
    const int n = sscanf(buf, "%d:%d", &sortid, &sortorder);

    if(n == 2)
    {
      c = snprintf(out, outsize, "%s%s (%s)", (k > 0) ? " - " : "", dt_collection_sort_name(sortid),
                   (sortorder) ? _("DESC") : _("ASC"));
      out += c;
      outsize -= c;
    }
    while(buf[0] != '$' && buf[0] != '\0') buf++;
    if(buf[0] == '$') buf++;
  }
}

static void _sort_history_apply(GtkWidget *widget, dt_lib_module_t *self)
{
  const int hid = GPOINTER_TO_INT(g_object_get_data(G_OBJECT(widget), "history"));
  if(hid < 0 || hid >= dt_conf_get_int("plugins/lighttable/filtering/sort_history_max")) return;

  char confname[200];
  snprintf(confname, sizeof(confname), "plugins/lighttable/filtering/sort_history%1d", hid);
  const char *line = dt_conf_get_string_const(confname);
  if(line && line[0] != '\0')
  {
    dt_collection_sort_deserialize(line);
    _sort_gui_update(self);
  }
}

static void _dt_images_order_change(gpointer instance, gpointer order, gpointer self)
{
  gchar *txt = (gchar *)order;
  if(txt)
  {
    dt_collection_sort_deserialize(txt);
    _sort_gui_update(self);
  }
}

static void _sort_history_show(GtkWidget *widget, gpointer user_data)
{
  dt_lib_module_t *self = (dt_lib_module_t *)user_data;
  // we show a popup with all the history entries
  GtkMenuShell *pop = GTK_MENU_SHELL(gtk_menu_new());
  gtk_widget_set_name(GTK_WIDGET(pop), "collect-popup");
  gtk_widget_set_size_request(GTK_WIDGET(pop), 200, -1);

  const int maxitems = dt_conf_get_int("plugins/lighttable/filtering/sort_history_max");

  for(int i = 0; i < maxitems; i++)
  {
    char confname[200];
    snprintf(confname, sizeof(confname), "plugins/lighttable/filtering/sort_history%1d", i);
    const char *line = dt_conf_get_string_const(confname);
    if(line && line[0] != '\0')
    {
      char str[2048] = { 0 };
      _sort_history_pretty_print(line, str, sizeof(str));
      GtkWidget *smt = gtk_menu_item_new_with_label(str);
      gtk_widget_set_name(smt, "collect-popup-item");
      gtk_widget_set_tooltip_text(smt, str);
      g_object_set_data(G_OBJECT(smt), "history", GINT_TO_POINTER(i));
      g_signal_connect(G_OBJECT(smt), "activate", G_CALLBACK(_sort_history_apply), self);
      gtk_menu_shell_append(pop, smt);
    }
    else
      break;
  }

  dt_gui_menu_popup(GTK_MENU(pop), widget, GDK_GRAVITY_SOUTH, GDK_GRAVITY_NORTH);
}

void gui_init(dt_lib_module_t *self)
{
  dt_lib_filtering_t *d = (dt_lib_filtering_t *)calloc(1, sizeof(dt_lib_filtering_t));

  self->data = (void *)d;
  self->widget = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
  gtk_widget_set_name(self->widget, "module_filtering");
  dt_gui_add_help_link(self->widget, dt_get_help_url(self->plugin_name));

  d->nb_rules = 0;
  d->params = (dt_lib_filtering_params_t *)g_malloc0(sizeof(dt_lib_filtering_params_t));

  for(int i = 0; i < DT_COLLECTION_MAX_RULES; i++)
  {
    d->rule[i].num = i;
    d->rule[i].lib = d;
  }

  // the box to insert the collect rules
  d->rules_box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
  gtk_box_pack_start(GTK_BOX(self->widget), d->rules_box, FALSE, TRUE, 0);

  // the botton buttons for the rules
  GtkWidget *bhbox = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
  gtk_widget_set_name(bhbox, "collect-actions-widget");
  gtk_box_set_homogeneous(GTK_BOX(bhbox), TRUE);
  gtk_box_pack_start(GTK_BOX(self->widget), bhbox, TRUE, TRUE, 0);
  GtkWidget *btn = dt_action_button_new(self, _("new rule"), G_CALLBACK(_event_rule_append), self,
                                        _("append new rule to collect images"), 0, 0);
  gtk_box_pack_start(GTK_BOX(bhbox), btn, TRUE, TRUE, 0);
  btn = dt_action_button_new(self, _("history"), G_CALLBACK(_event_history_show), self,
                             _("revert to a previous set of rules"), 0, 0);
  gtk_box_pack_start(GTK_BOX(bhbox), btn, TRUE, TRUE, 0);
  gtk_widget_show_all(bhbox);

  // the sorting part
  GtkWidget *spacer = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
  gtk_widget_set_name(spacer, "collect-spacer2");
  gtk_box_pack_start(GTK_BOX(self->widget), spacer, TRUE, TRUE, 0);
  d->sort_box = gtk_grid_new();
  gtk_grid_attach(GTK_GRID(d->sort_box), gtk_label_new(_("sort by")), 0, 0, 1, 1);
  gtk_widget_set_name(d->sort_box, "filter_sort_box");
  gtk_box_pack_start(GTK_BOX(self->widget), d->sort_box, TRUE, TRUE, 0);

  // the botton buttons for the sort
  bhbox = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
  gtk_widget_set_name(bhbox, "collect-actions-widget");
  gtk_box_set_homogeneous(GTK_BOX(bhbox), TRUE);
  gtk_box_pack_start(GTK_BOX(self->widget), bhbox, TRUE, TRUE, 0);
  btn = dt_action_button_new(self, _("new sort"), G_CALLBACK(_sort_show_add_popup), self,
                             _("append new sort to order images"), 0, 0);
  gtk_box_pack_start(GTK_BOX(bhbox), btn, TRUE, TRUE, 0);
  btn = dt_action_button_new(self, _("history"), G_CALLBACK(_sort_history_show), self,
                             _("revert to a previous set of sort orders"), 0, 0);
  gtk_box_pack_start(GTK_BOX(bhbox), btn, TRUE, TRUE, 0);
  gtk_widget_show_all(bhbox);

  /* setup proxy */
  darktable.view_manager->proxy.module_filtering.module = self;
  darktable.view_manager->proxy.module_filtering.update = _filtering_gui_update;
  darktable.view_manager->proxy.module_filtering.reset_filter = _proxy_reset_filter;

  d->last_where_ext = dt_collection_get_extended_where(darktable.collection, 99999);

  // test if the filter toolbar module is already loaded and update the gui in this case
  // otherwise, the filter toolbar module will do it in it's gui_init()
  if(darktable.view_manager->proxy.filter.module) _filtering_gui_update(self);

  DT_DEBUG_CONTROL_SIGNAL_CONNECT(darktable.signals, DT_SIGNAL_COLLECTION_CHANGED,
                                  G_CALLBACK(_dt_collection_updated), self);
  DT_DEBUG_CONTROL_SIGNAL_CONNECT(darktable.signals, DT_SIGNAL_IMAGES_ORDER_CHANGE,
                                  G_CALLBACK(_dt_images_order_change), self);
}

void gui_cleanup(dt_lib_module_t *self)
{
  dt_lib_filtering_t *d = (dt_lib_filtering_t *)self->data;

  for(int i = 0; i < DT_COLLECTION_MAX_RULES; i++)
  {
    d->rule[i].cleaning = TRUE;
  }

  DT_DEBUG_CONTROL_SIGNAL_DISCONNECT(darktable.signals, G_CALLBACK(_dt_collection_updated), self);
  darktable.view_manager->proxy.module_filtering.module = NULL;
  free(d->params);

  /* TODO: Make sure we are cleaning up all allocations */

  free(self->data);
  self->data = NULL;
}

void view_enter(struct dt_lib_module_t *self, struct dt_view_t *old_view, struct dt_view_t *new_view)
{
  // if we enter lighttable view, then we need to populate the filter topbar
  // we do it here because we are sure that both libs are loaded at this point
  _topbar_update(self);

  // we change the tooltip of the reset button here, as we are sure the header is defined now
  gtk_widget_set_tooltip_text(self->reset_button, "reset\nctrl-click to remove pinned rules too");
}

// clang-format off
// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.py
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
// clang-format on