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

/* Presents of each picture (the display's rate over the target's), set with the rate (patches/series, 0121) */
unsigned ps5_fg_dup = 1;
/* The rate of the pictures the display gets: 119.88 or 59.94 (the Target Framerate), set with the rate */
float ps5_fg_target_fps = 119.88f;
/* The instant of the picture the last present showed (1: a real frame), set by the driver */
float ps5_fg_dbg_t = 1.0f;
/* 1: the core cannot keep its own rate, so frames are shown as it makes them (the driver reads it) */
int ps5_fg_floor;

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
   o->target            = settings->uints.video_framegen_target == 60 ? 1 : 0;
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
   settings->bools.video_framegen_auto    = true;
   settings->uints.video_framegen_target  = now.target ? 60 : 120;
   settings->uints.video_framegen_method  = now.method;
   settings->uints.video_framegen_cadence = now.cadence;
   settings->uints.video_framegen_size    = now.size;
   settings->bools.video_framegen_osd     = now.osd != 0;
   ps5_fg_save(&now);
   /* The rate is read when the video output starts: its timing follows it */
   if (now.target != before.target)
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
      unsigned long generated, unsigned long unique, unsigned level, double gen_ms, unsigned long calls)
{
   static unsigned calls;
   const unsigned pictures = subframes / (ps5_fg_dup ? ps5_fg_dup : 1);
   static unsigned long presents, last_generated;
   static unsigned long gap_n[2], gap_off[2], last_unique, last_calls;
   static double new_per_s, calls_per_s;
   static unsigned slow_windows;
   static time_t floor_until;
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
   {
      /* the last presents: instant shown and ms since the one before (a line with the pace) */
      static float seq_t[24];
      static float seq_ms[24];
      static unsigned seq_n;
      static struct timespec seq_prev;
      if (seq_prev.tv_sec)
         seq_ms[seq_n % 24] = (float)((t.tv_sec - seq_prev.tv_sec) * 1e3 + (t.tv_nsec - seq_prev.tv_nsec) / 1e6);
      seq_t[seq_n % 24] = ps5_fg_dbg_t;
      seq_prev          = t;
      seq_n++;
      if (seq_n % 240 == 0)
      {
         char line[400];
         unsigned q, o = 0;
         for (q = 0; q < 24 && o < sizeof(line) - 24; q++)
            o += (unsigned)snprintf(line + o, sizeof(line) - o, " %.2f@%.1f", seq_t[(seq_n + q) % 24], seq_ms[(seq_n + q) % 24]);
         RARCH_LOG("[FrameGen] seq:%s\n", line);
      }
   }
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
      new_per_s   = (double)(unique - last_unique) / seconds;
      calls_per_s = (double)(calls - last_calls) / seconds;
      last_unique = unique;
      last_calls  = calls;
      RARCH_LOG("[FrameGen] pace: %.1f presents/s, %.1f generated/s, %.1f new frames/s, core %.1f calls/s, governor level %u (%.2f ms a picture)\n",
            (double)presents / seconds, (double)(generated - last_generated) / seconds, new_per_s, calls_per_s, level, gen_ms);
      /* The floor: a core that cannot keep its own rate gains nothing from frames made between its frames, and the work
       * only slows it more. It is shown as it makes its frames; every ten seconds the generator is tried again. */
      if (ps5_fg_floor)
      {
         if (t.tv_sec >= floor_until)
         {
            ps5_fg_floor = 0;
            slow_windows = 0;
         }
      }
      else if (plan && ps5_fg_core_fps > 20.0f && calls_per_s < 0.85 * (double)ps5_fg_core_fps)
      {
         if (++slow_windows >= 2)
         {
            ps5_fg_floor = 1;
            floor_until  = t.tv_sec + 10;
            RARCH_LOG("[FrameGen] floor: the core runs at %.1f of %.1f frames/s; frames are shown as it makes them for 10 s\n",
                  calls_per_s, (double)ps5_fg_core_fps);
         }
      }
      else
         slow_windows = 0;
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
   else if (pictures < 2)
      snprintf(text, sizeof(text), "FrameGen: nothing to make - the game already runs at the Target Framerate (%s)",
            ps5_fg_choice_name(PS5_FG_TARGET, now.target));
   else if (ps5_fg_floor)
      snprintf(text, sizeof(text), "FrameGen: core at %.0f fps (needs %.0f), too slow: frames shown as the core makes them",
            calls_per_s, (double)ps5_fg_core_fps);
   else if (!plan)
      snprintf(text, sizeof(text), "FrameGen: paused (menu, fast-forward or pause)");
   else
      snprintf(text, sizeof(text), "FrameGen: %s  game %.1f fps (%.1f new/s) -> %.0f fps  cadence %u%s  %ux%u  generated %lu  cost level %u (%.1f ms)",
            ps5_fg_choice_name(PS5_FG_METHOD, now.method), (double)ps5_fg_core_fps, new_per_s, (double)ps5_fg_target_fps,
            period ? period : 1, now.cadence ? " (forced)" : "", width, height, generated, level, gen_ms);
   runloop_msg_queue_push(text, strlen(text), 1, 150, false, NULL,
         MESSAGE_QUEUE_ICON_DEFAULT, MESSAGE_QUEUE_CATEGORY_INFO);
}

/* The driver's working-size limit for a size choice */
unsigned long ps5_fg_size_pixels_of(unsigned size)
{
   return ps5_fg_size_pixels(size);
}
