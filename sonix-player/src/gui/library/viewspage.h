#ifndef VIEWSPAGE_H
#define VIEWSPAGE_H

#include "src/gui/shell/gui.h"

#include "lvgl/lvgl.h"

// The Library page: the menus the computer writes to the card (views.h),
// one level at a time on one screen, the way the file browser walks folders.
// A menu row opens the level below it; an album row opens the record's own
// page; the chevron and the back swipe climb back up, and leave the page only
// from the top.
//
// Play all and Shuffle in the corner play everything under the level on
// screen, in the order the menus give it: a label's whole catalogue, say. A
// long press on a row offers the same for that row, and "Play from here" for
// that row and every one after it.
extern lv_obj_t *viewspage_screen;

void viewspage_init(gui_config_t *cfg);

// Moves the now-playing mark to the row that holds the track playing. Called
// from the player at every track change.
void viewspage_notify_now_playing(void);

#endif // VIEWSPAGE_H
