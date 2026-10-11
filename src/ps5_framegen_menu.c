/* Frame generation in RetroArch's menus (patches/series, 0120).
 *
 * One list of options, shown in three places: the XMB's leftmost tab, a submenu opened from the
 * first entry of RGUI's main menu, and (ES-DE's own menu writes the same file) the shared options
 * file. Entries are plain actions the menu code binds by label: "ps5_fg_<n>" cycles with OK, left
 * and right; "ps5_fgt_<n>" (the XMB tab, where left and right move between tabs) cycles with OK;
 * "ps5_framegen" opens the submenu. The values live in RetroArch's settings (the driver reads them
 * each frame) and every change is written to /app0/config/framegen.cfg, which the title appends
 * when it starts RetroArch.
 *
 * This file is compiled into the frontend archive by tools/build-retroarch.sh, against the
 * configured RetroArch tree.
 *
 * Copyright (C) 2026 Mihawk
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#include <stdio.h>
#include <string.h>
#include <time.h>

#include <lists/file_list.h>
#include <retro_miscellaneous.h>
#include <string/stdstring.h>

#include "command.h"
#include "configuration.h"
#include "msg_hash.h"
#include "runloop.h"
#include "verbosity.h"
#include "menu/menu_defines.h"
#include "menu/menu_driver.h"
#include "menu/menu_entries.h"

#include "ps5_framegen_config.h"

#define PS5_FG_LABEL_FOLDER "ps5_framegen"

/* The loaded core's frame rate, kept by the video setup (patches/series, 0121) for the driver's cadence check */
float ps5_fg_core_fps;

/* "ps5_fg_3" -> 3; the tab's "ps5_fgt_3" -> 3; anything else -> -1 */
static int ps5_fg_item_of(const char *label, bool *tab)
{
   const char *digits;
   if (!label)
      return -1;
   if (!strncmp(label, "ps5_fgt_", 8))
   {
      digits = label + 8;
      if (tab)
         *tab = true;
   }
   else if (!strncmp(label, "ps5_fg_", 7))
   {
      digits = label + 7;
      if (tab)
         *tab = false;
   }
   else
      return -1;
   if (digits[0] < '0' || digits[0] > '9' || digits[1])
      return -1;
   return digits[0] - '0' < PS5_FG_ITEM_COUNT ? digits[0] - '0' : -1;
}

bool ps5_fg_menu_is_folder(const char *label)
{
   return label && !strcmp(label, PS5_FG_LABEL_FOLDER);
}

bool ps5_fg_menu_is_label(const char *label)
{
   return ps5_fg_menu_is_folder(label) || ps5_fg_item_of(label, NULL) >= 0;
}

/* Left and right change the value, except in the XMB tab where they move between tabs */
bool ps5_fg_menu_has_values(const char *label)
{
   bool tab = false;
   return ps5_fg_item_of(label, &tab) >= 0 && !tab;
}

static void ps5_fg_current(struct ps5_fg_options *o)
{
   settings_t *settings = config_get_ptr();
   o->enable            = settings->bools.video_framegen ? 1 : 0;
   o->subframes         = settings->bools.video_framegen_auto ? 0 : settings->uints.video_shader_subframes;
   if (o->subframes > 4)
      o->subframes = 1;
   if (!settings->bools.video_framegen_auto && o->subframes < 1)
      o->subframes = 1;
   o->method  = settings->uints.video_framegen_method % 3;
   o->cadence = settings->uints.video_framegen_cadence % 5;
   o->size    = settings->uints.video_framegen_size % 4;
   o->osd     = settings->bools.video_framegen_osd ? 1 : 0;
}

void ps5_fg_menu_fill(file_list_t *list, bool tab)
{
   unsigned i;
   char label[16];
   for (i = 0; i < PS5_FG_ITEM_COUNT; i++)
   {
      snprintf(label, sizeof(label), tab ? "ps5_fgt_%u" : "ps5_fg_%u", i);
      menu_entries_append(list, ps5_fg_item_name(i), label,
            MSG_UNKNOWN, MENU_SETTING_ACTION, 0, 0, NULL);
   }
}

void ps5_fg_menu_step(const char *label, int direction)
{
   struct ps5_fg_options now, before;
   settings_t *settings = config_get_ptr();
   int item             = ps5_fg_item_of(label, NULL);
   if (item < 0)
      return;
   ps5_fg_current(&now);
   before = now;
   ps5_fg_step(&now, (unsigned)item, direction);
   settings->bools.video_framegen         = now.enable != 0;
   settings->bools.video_framegen_auto    = now.subframes == 0;
   if (now.subframes)
      settings->uints.video_shader_subframes = now.subframes;
   settings->uints.video_framegen_method  = now.method;
   settings->uints.video_framegen_cadence = now.cadence;
   settings->uints.video_framegen_size    = now.size;
   settings->bools.video_framegen_osd     = now.osd != 0;
   ps5_fg_save(&now);
   /* The rate is read when the video output starts: its timing follows it */
   if (now.subframes != before.subframes)
      command_event(CMD_EVENT_REINIT, NULL);   /* (Auto picks its rate as the video output starts) */
}

/* The entry's name (s2) and its value (s), as the menu's value callback wants them */
size_t ps5_fg_menu_text(const char *label, char *value, size_t value_len, char *name, size_t name_len)
{
   struct ps5_fg_options now;
   int item;
   if (ps5_fg_menu_is_folder(label))
   {
      strlcpy(name, "Frame Generation", name_len);
      return strlcpy(value, "", value_len);
   }
   item = ps5_fg_item_of(label, NULL);
   if (item < 0)
      return 0;
   strlcpy(name, ps5_fg_item_name((unsigned)item), name_len);
   if (item == PS5_FG_RESET)
      return strlcpy(value, "", value_len);
   ps5_fg_current(&now);
   {
      static unsigned logged;
      if (logged < 24)
      {
         logged++;
         RARCH_LOG("[FrameGen] menu entry %s: \"%s\" = \"%s\"\n", label, name,
               ps5_fg_choice_name((unsigned)item, ps5_fg_get(&now, (unsigned)item)));
      }
   }
   return strlcpy(value, ps5_fg_choice_name((unsigned)item, ps5_fg_get(&now, (unsigned)item)), value_len);
}

void ps5_fg_menu_help(const char *label, char *s, size_t len)
{
   int item = ps5_fg_item_of(label, NULL);
   if (item >= 0)
   {
      static unsigned logged;
      strlcpy(s, ps5_fg_item_help((unsigned)item), len);
      if (logged++ < 3)
         RARCH_LOG("[FrameGen] menu description %s: \"%s\"\n", label, s);
   }
   else if (ps5_fg_menu_is_folder(label))
      strlcpy(s, "Choose how frames are generated between a game's frames, and test the result.", len);
}

/* The driver calls this for every present; the message is refreshed about twice a second */
void ps5_fg_osd(bool plan, unsigned subframes, unsigned period, unsigned width, unsigned height,
      unsigned long generated)
{
   static unsigned calls;
   static unsigned long presents, last_generated;
   static unsigned long gap_n[2], gap_off[2];
   static double gap_sum[2], gap_max[2];
   static struct timespec last;
   struct ps5_fg_options now;
   char text[200];
   settings_t *settings = config_get_ptr();
   struct timespec t;
   if (!settings->bools.video_framegen_osd)
      return;
   presents++;
   clock_gettime(CLOCK_MONOTONIC, &t);
   if (last.tv_sec == 0)
      last = t;
   {
      /* Present spacing, apart for presents that showed a generated frame and those that did not (the log line below) */
      static struct timespec prev;
      static unsigned long prev_generated;
      if (prev.tv_sec)
      {
         const double ms = (double)(t.tv_sec - prev.tv_sec) * 1e3 + (double)(t.tv_nsec - prev.tv_nsec) / 1e6;
         const unsigned cls = generated != prev_generated;   /* 1: this present showed a generated frame */
         gap_n[cls]++;
         gap_sum[cls] += ms;
         if (ms > gap_max[cls])
            gap_max[cls] = ms;
         if (ms > 10.5 || ms < 6.0)
            gap_off[cls]++;
      }
      prev           = t;
      prev_generated = generated;
   }
   if ((t.tv_sec - last.tv_sec) * 1000000000L + (t.tv_nsec - last.tv_nsec) >= 2000000000L)
   {
      double seconds = (double)(t.tv_sec - last.tv_sec) + (double)(t.tv_nsec - last.tv_nsec) / 1e9;
      RARCH_LOG("[FrameGen] pace: %.1f presents/s, %.1f generated/s\n", (double)presents / seconds,
            (double)(generated - last_generated) / seconds);
      RARCH_LOG("[FrameGen] spacing (ms): real n %lu mean %.2f max %.2f off %lu | generated n %lu mean %.2f max %.2f off %lu\n",
            gap_n[0], gap_n[0] ? gap_sum[0] / (double)gap_n[0] : 0.0, gap_max[0], gap_off[0],
            gap_n[1], gap_n[1] ? gap_sum[1] / (double)gap_n[1] : 0.0, gap_max[1], gap_off[1]);
      memset(gap_n, 0, sizeof(gap_n)); memset(gap_sum, 0, sizeof(gap_sum));
      memset(gap_max, 0, sizeof(gap_max)); memset(gap_off, 0, sizeof(gap_off));
      presents       = 0;
      last_generated = generated;
      last           = t;
   }
   if (++calls % 120)
      return;
   ps5_fg_current(&now);
   if (!now.enable)
      snprintf(text, sizeof(text), "FrameGen: off");
   else if (subframes < 2)
      snprintf(text, sizeof(text), "FrameGen: waiting - the output rate is not a multiple of the game's (Output Rate %s)",
            ps5_fg_choice_name(PS5_FG_SUBFRAMES, now.subframes));
   else if (!plan)
      snprintf(text, sizeof(text), "FrameGen: paused (menu, fast-forward or pause)");
   else
      snprintf(text, sizeof(text), "FrameGen: %s  %ux  cadence %u%s  %ux%u  generated %lu",
            ps5_fg_choice_name(PS5_FG_METHOD, now.method), subframes, period ? period : 1,
            now.cadence ? " (forced)" : "", width, height, generated);
   runloop_msg_queue_push(text, strlen(text), 1, 150, false, NULL,
         MESSAGE_QUEUE_ICON_DEFAULT, MESSAGE_QUEUE_CATEGORY_INFO);
}

/* The driver's working-size limit for a size choice */
unsigned long ps5_fg_size_pixels_of(unsigned size)
{
   return ps5_fg_size_pixels(size);
}
