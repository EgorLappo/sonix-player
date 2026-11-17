#include "viewspage.h"

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "lvgl/lvgl.h"

#include "src/gui/fonts/fonts.h"
#include "src/gui/library/medialist.h"
#include "src/gui/nowplaying/cover.h"
#include "src/gui/nowplaying/coverloader.h"
#include "src/gui/nowplaying/player.h"
#include "src/gui/settings/musicsettings.h"
#include "src/gui/shell/gui.h"
#include "src/gui/shell/icons.h"
#include "src/gui/shell/popover.h"
#include "src/gui/shell/settingsrow.h"
#include "src/gui/shell/switcher.h"
#include "src/gui/shell/theme.h"
#include "src/gui/shell/toast.h"
#include "src/system/core/lang.h"
#include "src/system/library/library.h"
#include "src/system/library/views.h"
#include "src/system/playback/device_state.h"
#include "src/system/playback/playlist.h"

lv_obj_t *viewspage_screen;

// Row geometry: the file browser's, so the two lists read as one family.
#define THUMB_SIZE 72
#define ROW_HEIGHT 100
#define LIST_ROW_GAP 8
#define ROW_PITCH (ROW_HEIGHT + LIST_ROW_GAP)
#define LIST_ROW_RADIUS 12
#define LIST_ROW_PAD 14
#define PLAYMARK_WIDTH 6
#define PLAYMARK_HEIGHT 52
#define PLAYMARK_INSET 4

// Rows that exist; the list is windowed over them (see browser.c, which this
// page follows). Each one has a cover slot of its own.
#define ROW_POOL 12
#define THUMB_TICK_PERIOD_MS 40

// How deep the menus go, with room to spare: Library > Labels > FAX >
// Artists > an artist > a record is six.
#define DEPTH 16

// The most a Play all may hand the queue at once. The queue holds every path
// in full, so a whole library's worth is memory this device does not have;
// a label's catalogue (three thousand tracks at most here) is well under it.
#define PLAY_MAX 6000

typedef struct {
	lv_obj_t *button;
	lv_obj_t *icon;
	lv_obj_t *title;
	lv_obj_t *subtitle;
	lv_obj_t *playmark;
	int index; // which child of the level it shows, -1 when parked
	uint32_t generation;
	views_node_t node;
	bool has_thumb;
	bool thumb_requested;
	bool thumb_settled;
	cover_image_t thumb;
} row_t;

static row_t rows[ROW_POOL];
static gui_config_t *config;

static lv_obj_t *title_label;
static lv_obj_t *list;	   // the scrolling viewport
static lv_obj_t *list_body; // as tall as the whole level
static lv_obj_t *message;   // "no menus on the card", in place of the rows
static lv_obj_t *play_button;
static lv_obj_t *shuffle_button;
static lv_obj_t *locate_button;
static lv_timer_t *thumb_timer;

static lv_style_t style_row;

// The walk down: the node of every level, and where each was scrolled to.
static int stack_id[DEPTH];
static int stack_scroll[DEPTH];
static int depth;
static unsigned file_generation; // views_generation() the stack belongs to

static views_node_t here; // the level on screen
static int count;		  // its children
static int here_tracks;	  // tracks under it
static uint32_t generation = 1;

static char np_path[1024];
static int playing_pos = -1; // the child holding the track playing, or -1

static int row_slot(const row_t *row) { return COVERLOADER_VIEWS_BASE + (int)(row - rows); }

// ---------------------------------------------------------------------------
// rows
// ---------------------------------------------------------------------------

static void row_show_glyph(row_t *row) {
	const lv_image_dsc_t *glyph = row->node.kind == VIEWS_MENU ? &icon_folder : &icon_album;
	lv_image_set_src(row->icon, glyph);
	lv_obj_add_style(row->icon, &theme_style_icon, 0);
	lv_obj_set_style_image_recolor_opa(row->icon, LV_OPA_COVER, 0);
}

static void row_drop_thumb(row_t *row) {
	coverloader_release(row_slot(row));
	if (row->has_thumb) {
		row_show_glyph(row); // off the pixels before they go
		cover_free(&row->thumb);
		row->has_thumb = false;
	}
	row->thumb_requested = false;
	row->thumb_settled = false;
}

static void row_update_playmark(row_t *row) {
	lv_obj_set_hidden(row->playmark, !(row->index >= 0 && row->index == playing_pos));
}

static void row_bind(row_t *row, int index) {
	if (row->index == index && row->generation == generation) {
		return;
	}
	row_drop_thumb(row);
	row->index = index;
	row->generation = generation;

	if (index < 0 || index >= count || views_children(here.id, index, 1, &row->node) != 1) {
		row->index = -1;
		lv_obj_set_hidden(row->button, true);
		lv_obj_set_hidden(row->playmark, true);
		return;
	}

	lv_obj_set_hidden(row->button, false);
	lv_obj_set_y(row->button, index * ROW_PITCH);
	lv_label_set_text(row->title, row->node.title);
	bool sub = row->node.subtitle[0] != '\0';
	lv_obj_set_hidden(row->subtitle, !sub);
	if (sub) {
		lv_label_set_text(row->subtitle, row->node.subtitle);
	}
	// A name alone may take two lines; with a second line under it, one.
	int lines = sub ? 1 : 2;
	lv_obj_set_style_max_height(row->title,
								lines * lv_font_get_line_height(&font_ui_24) +
									(lines - 1) * lv_obj_get_style_text_line_space(row->title, LV_PART_MAIN),
								0);
	row_show_glyph(row);
	row_update_playmark(row);
}

static void window_update(void) {
	int scroll = lv_obj_get_scroll_y(list);
	if (scroll < 0) {
		scroll = 0;
	}
	int first = scroll / ROW_PITCH - 1;
	if (first + ROW_POOL > count) {
		first = count - ROW_POOL;
	}
	if (first < 0) {
		first = 0;
	}
	// Entry i always lives in row i % ROW_POOL, so a row that stays on screen
	// while the list scrolls keeps its cover (browser.c explains).
	for (int i = 0; i < ROW_POOL; i++) {
		int index = first + i;
		row_bind(&rows[index % ROW_POOL], index < count ? index : -1);
	}
}

static void thumbs_update(void) {
	bool busy = false;
	for (int i = 0; i < ROW_POOL; i++) {
		row_t *row = &rows[i];
		if (row->index < 0 || row->thumb_settled) {
			continue;
		}
		if (!row->node.cover[0]) {
			row->thumb_settled = true;
			continue;
		}
		if (!row->thumb_requested) {
			coverloader_request(row_slot(row), row->node.cover, THUMB_SIZE);
			row->thumb_requested = true;
			busy = true;
			continue;
		}
		bool finished = false;
		cover_image_t image;
		bool found = coverloader_take(row_slot(row), &image, &finished);
		if (!finished) {
			busy = true;
			continue;
		}
		row->thumb_settled = true;
		if (found) {
			row->thumb = image;
			row->has_thumb = true;
			lv_image_set_src(row->icon, &row->thumb.dsc);
			lv_obj_set_style_image_recolor_opa(row->icon, LV_OPA_TRANSP, 0);
		}
	}
	if (!busy) {
		lv_timer_pause(thumb_timer);
	}
}

static void thumb_timer_cb(lv_timer_t *timer) {
	(void)timer;
	thumbs_update();
}

static void list_scroll_cb(lv_event_t *e) {
	(void)e;
	window_update();
	lv_timer_resume(thumb_timer);
}

// ---------------------------------------------------------------------------
// the level on screen
// ---------------------------------------------------------------------------

static void update_corner_buttons(void) {
	// Not at the top: "everything" there is the whole library several times
	// over, one copy per menu it appears in.
	bool playable = depth > 1 && here_tracks > 0 && here_tracks <= PLAY_MAX;
	bool locatable = playing_pos >= 0;
	lv_obj_set_hidden(play_button, !playable);
	lv_obj_set_hidden(shuffle_button, !playable);
	lv_obj_set_hidden(locate_button, !locatable);
	// The locate button goes in the first free slot from the right, and the
	// title keeps all the width the buttons leave it: a label's name is often
	// the whole point of the page.
	int used = playable ? 2 : 0;
	lv_obj_align(locate_button, LV_ALIGN_TOP_RIGHT, -config->padding - used * 62,
				 config->padding + config->top_bar_height);
	settingsrow_title_corner_slots(title_label, config, used + (locatable ? 1 : 0));
}

static void update_playing(void) {
	device_state_t state;
	device_state_get(&state);
	snprintf(np_path, sizeof(np_path), "%s", state.current_file);
	playing_pos = count > 0 ? views_child_holding(here.id, np_path) : -1;
	for (int i = 0; i < ROW_POOL; i++) {
		row_update_playmark(&rows[i]);
	}
}

static void show_message(const char *text) {
	lv_label_set_text(message, text);
	lv_obj_set_hidden(message, false);
}

// Puts the level at the top of the stack on screen, scrolled to `scroll`.
static void show_level(int scroll) {
	if (!views_node(stack_id[depth - 1], &here)) {
		depth = 1;
		stack_id[0] = views_root();
		if (!views_node(stack_id[0], &here)) {
			count = 0;
			return;
		}
	}
	count = views_child_count(here.id);
	here_tracks = depth > 1 ? views_track_count(here.id) : 0;
	lv_label_set_text(title_label, depth == 1 ? tr("views_library") : here.title);
	lv_obj_set_hidden(message, true);

	generation++;
	lv_obj_set_height(list_body, count > 0 ? count * ROW_PITCH : ROW_PITCH);
	lv_obj_update_layout(list);
	lv_obj_scroll_to_y(list, scroll, LV_ANIM_OFF);
	window_update();
	update_playing();
	update_corner_buttons();
	lv_timer_resume(thumb_timer);
}

static void show_nothing(void) {
	count = 0;
	here_tracks = 0;
	playing_pos = -1;
	generation++;
	window_update();
	lv_label_set_text(title_label, tr("views_library"));
	show_message(tr("views_missing"));
	update_corner_buttons();
}

// Opens (or reopens) the file and puts the right level on screen: where the
// walk was left, unless the file is new, in which case its top.
static void refresh(void) {
	if (!views_open(config->sd_root_path)) {
		depth = 0;
		show_nothing();
		return;
	}
	int scroll = depth > 0 ? lv_obj_get_scroll_y(list) : 0;
	if (depth == 0 || file_generation != views_generation()) {
		file_generation = views_generation();
		depth = 1;
		stack_id[0] = views_root();
		scroll = 0;
	}
	show_level(scroll);
}

static void push(int id) {
	if (depth >= DEPTH) {
		return;
	}
	stack_scroll[depth - 1] = lv_obj_get_scroll_y(list);
	stack_id[depth++] = id;
	show_level(0);
}

static bool go_up(void) {
	if (depth <= 1) {
		return false; // the top: the chevron leaves the page
	}
	depth--;
	show_level(stack_scroll[depth - 1]);
	return true;
}

static bool go_up_peek(void) { return depth > 1; }

// ---------------------------------------------------------------------------
// playing
// ---------------------------------------------------------------------------

// Everything under `id` (from its child at `from_pos` on, when >= 0) into the
// queue, in order or shuffled, and the player up.
static void play(int id, int from_pos, bool shuffled) {
	char **paths = NULL;
	int n = views_tracks(id, from_pos, &paths, NULL, NULL);
	if (n <= 0) {
		toast_plain(tr("views_nothing_to_play"));
		return;
	}
	if (n > PLAY_MAX) {
		views_free_list(paths, n);
		toast_plain(tr("views_too_many"));
		return;
	}
	// Mode first, as the library lists do: with shuffle on, the queue deals
	// its order as it loads.
	playback_mode_t shuffle_mode =
		musicsettings_endless_shuffle() ? PLAYBACK_MODE_SHUFFLE_REPEAT : PLAYBACK_MODE_SHUFFLE;
	playlist_set_mode(shuffled ? shuffle_mode : PLAYBACK_MODE_NORMAL);
	int start = shuffled && n > 1 ? (int)lv_rand(0, (uint32_t)(n - 1)) : 0;
	device_state_play_list((const char *const *)paths, n, start);
	views_free_list(paths, n);

	player_refresh_now_playing();
	switch_screen(player_screen);
}

// An album row opens the record's own page when the library index has it --
// cover, play and shuffle, the track menus -- and a plain list of its tracks
// when it does not (not scanned yet). A track list is always the plain list:
// it is part of a record, and the record's page would show all of it.
static void open_album(const views_node_t *node) {
	char **paths = NULL, **titles = NULL, **artists = NULL;
	int n = views_tracks(node->id, -1, &paths, &titles, &artists);
	if (n <= 0) {
		return;
	}
	char value[512];
	if (node->kind == VIEWS_ALBUM && library_track_album_value(paths[0], value, sizeof(value))) {
		medialist_open(node->title, LIBRARY_LIST_TRACKS, LIBRARY_FILTER_ALBUM, value);
	} else {
		medialist_open_paths(node->title, (const char *const *)paths, (const char *const *)titles,
							 (const char *const *)artists, n, NULL);
	}
	views_free_list(paths, n);
	views_free_list(titles, n);
	views_free_list(artists, n);
}

static row_t *row_of_button(lv_obj_t *button) {
	for (int i = 0; i < ROW_POOL; i++) {
		if (rows[i].button == button) {
			return &rows[i];
		}
	}
	return NULL;
}

static void row_clicked_cb(lv_event_t *e) {
	if (player_sheet_drag_active() || switcher_back_drag_active()) {
		return; // a swipe across the row, not a tap on it
	}
	row_t *row = row_of_button(lv_event_get_current_target(e));
	if (!row || row->index < 0) {
		return;
	}
	if (row->node.kind == VIEWS_MENU) {
		push(row->node.id);
	} else {
		open_album(&row->node);
	}
}

// What a long press is about, for the popover's actions.
static int pressed_id;
static int pressed_pos;

static void act_play(void *user) {
	(void)user;
	play(pressed_id, -1, false);
}

static void act_from_here(void *user) {
	(void)user;
	play(here.id, pressed_pos, false);
}

static void act_shuffle(void *user) {
	(void)user;
	play(pressed_id, -1, true);
}

static void row_long_pressed_cb(lv_event_t *e) {
	if (player_sheet_drag_active() || switcher_back_drag_active()) {
		return;
	}
	row_t *row = row_of_button(lv_event_get_current_target(e));
	if (!row || row->index < 0) {
		return;
	}
	// The lift that ends this press would otherwise arrive as a tap too, and
	// open the row under the menu (browser.c does the same).
	lv_indev_t *indev = lv_indev_active();
	if (indev) {
		lv_indev_wait_release(indev);
	}

	pressed_id = row->node.id;
	pressed_pos = row->index;

	static popover_item_t items[3];
	int n = 0;
	items[n++] = (popover_item_t){tr("views_play"), act_play, NULL, false};
	if (row->index < count - 1) {
		// this row and every one after it: a catalogue from one number on
		items[n++] = (popover_item_t){tr("views_play_from_here"), act_from_here, NULL, false};
	}
	items[n++] = (popover_item_t){tr("views_shuffle"), act_shuffle, NULL, false};
	popover_show(row->button, items, n);
}

static void play_button_cb(lv_event_t *e) {
	(void)e;
	play(here.id, -1, false);
}

static void shuffle_button_cb(lv_event_t *e) {
	(void)e;
	play(here.id, -1, true);
}

// To the row of the record that is playing, wherever it is in a long list.
static void locate_button_cb(lv_event_t *e) {
	(void)e;
	update_playing();
	if (playing_pos < 0) {
		return;
	}
	int target = playing_pos * ROW_PITCH - (lv_obj_get_height(list) - ROW_HEIGHT) / 2;
	lv_obj_scroll_to_y(list, target > 0 ? target : 0, LV_ANIM_ON);
}

void viewspage_notify_now_playing(void) {
	if (!rows[0].button || depth == 0) {
		return; // before the page exists, or with nothing on it
	}
	update_playing();
	update_corner_buttons();
}

static void screen_load_cb(lv_event_t *e) {
	(void)e;
	refresh();
}

// ---------------------------------------------------------------------------
// building
// ---------------------------------------------------------------------------

static lv_obj_t *corner_button(gui_config_t *cfg, int slot, const lv_image_dsc_t *glyph, lv_event_cb_t cb) {
	lv_obj_t *btn = lv_btn_create(viewspage_screen);
	lv_obj_set_size(btn, 56, 56);
	lv_obj_set_style_bg_opa(btn, LV_OPA_TRANSP, 0);
	lv_obj_set_style_border_width(btn, 0, 0);
	lv_obj_set_style_shadow_width(btn, 0, 0);
	lv_obj_set_style_pad_all(btn, 0, 0);
	lv_obj_align(btn, LV_ALIGN_TOP_RIGHT, -cfg->padding - slot * 62, cfg->padding + cfg->top_bar_height);
	lv_obj_add_event_cb(btn, cb, LV_EVENT_CLICKED, NULL);
	lv_obj_set_hidden(btn, true);

	lv_obj_t *image = lv_image_create(btn);
	lv_image_set_src(image, glyph);
	lv_obj_add_style(image, &theme_style_icon, 0);
	lv_obj_center(image);
	return btn;
}

static void build_rows(int width) {
	lv_style_init(&style_row);
	lv_style_set_border_width(&style_row, 0);
	lv_style_set_radius(&style_row, LIST_ROW_RADIUS);
	lv_style_set_shadow_width(&style_row, 0);
	lv_style_set_pad_all(&style_row, LIST_ROW_PAD);
	lv_style_set_pad_column(&style_row, 14);

	for (int i = 0; i < ROW_POOL; i++) {
		row_t *row = &rows[i];

		row->button = lv_btn_create(list_body);
		lv_obj_set_size(row->button, width, ROW_HEIGHT);
		lv_obj_set_x(row->button, 0);
		lv_obj_add_style(row->button, &theme_style_card, 0);
		lv_obj_add_style(row->button, &style_row, 0);
		lv_obj_add_style(row->button, &theme_style_card_pressed, LV_STATE_PRESSED);
		lv_obj_set_hidden(row->button, true);
		lv_obj_set_event_bubble(row->button, true); // so a swipe can start on a row
		lv_obj_add_event_cb(row->button, row_clicked_cb, LV_EVENT_CLICKED, NULL);
		lv_obj_add_event_cb(row->button, row_long_pressed_cb, LV_EVENT_LONG_PRESSED, NULL);
		lv_obj_set_flex_flow(row->button, LV_FLEX_FLOW_ROW);
		lv_obj_set_flex_align(row->button, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

		row->icon = lv_image_create(row->button);
		lv_obj_set_size(row->icon, THUMB_SIZE, THUMB_SIZE);
		lv_image_set_inner_align(row->icon, LV_IMAGE_ALIGN_CENTER);

		lv_obj_t *text = lv_obj_create(row->button);
		lv_obj_set_flex_grow(text, 1);
		lv_obj_set_height(text, LV_SIZE_CONTENT);
		lv_obj_set_style_bg_opa(text, LV_OPA_TRANSP, 0);
		lv_obj_set_style_border_width(text, 0, 0);
		lv_obj_set_style_pad_all(text, 0, 0);
		lv_obj_set_style_pad_row(text, 2, 0);
		lv_obj_set_flex_flow(text, LV_FLEX_FLOW_COLUMN);
		lv_obj_set_clickable(text, false);
		lv_obj_set_scrollable(text, false);
		lv_obj_set_event_bubble(text, true);

		row->title = lv_label_create(text);
		lv_obj_set_width(row->title, lv_pct(100));
		lv_label_set_long_mode(row->title, LV_LABEL_LONG_DOT);
		lv_obj_add_style(row->title, &theme_style_text, 0);
		lv_obj_set_style_text_font(row->title, &font_ui_24, 0);

		row->subtitle = lv_label_create(text);
		lv_obj_set_width(row->subtitle, lv_pct(100));
		lv_label_set_long_mode(row->subtitle, LV_LABEL_LONG_DOT);
		lv_obj_add_style(row->subtitle, &theme_style_text_dim, 0);
		lv_obj_set_style_text_font(row->subtitle, &font_ui_20, 0);
		lv_obj_set_style_max_height(row->subtitle, lv_font_get_line_height(&font_ui_20), 0);

		// In the row's own left padding, outside the layout: it marks the row
		// without moving anything on it.
		row->playmark = lv_obj_create(row->button);
		lv_obj_set_ignore_layout(row->playmark, true);
		lv_obj_set_size(row->playmark, PLAYMARK_WIDTH, PLAYMARK_HEIGHT);
		lv_obj_align(row->playmark, LV_ALIGN_LEFT_MID, PLAYMARK_INSET - LIST_ROW_PAD, 0);
		lv_obj_add_style(row->playmark, &theme_style_accent_bg, 0);
		lv_obj_set_style_radius(row->playmark, LV_RADIUS_CIRCLE, 0);
		lv_obj_set_style_border_width(row->playmark, 0, 0);
		lv_obj_set_style_shadow_width(row->playmark, 0, 0);
		lv_obj_set_style_pad_all(row->playmark, 0, 0);
		lv_obj_set_scrollable(row->playmark, false);
		lv_obj_set_clickable(row->playmark, false);
		lv_obj_set_hidden(row->playmark, true);

		row->index = -1;
	}
}

void viewspage_init(gui_config_t *cfg) {
	config = cfg;
	lv_obj_add_style(viewspage_screen, &theme_style_screen, 0);

	title_label = settingsrow_title(viewspage_screen, cfg, "views_library");
	settingsrow_title_corner_slots(title_label, cfg, 0); // update_corner_buttons() sets it
	play_button = corner_button(cfg, 0, &icon_play, play_button_cb);
	shuffle_button = corner_button(cfg, 1, &icon_shuffle, shuffle_button_cb);
	locate_button = corner_button(cfg, 2, &icon_now_playing, locate_button_cb);

	int content_top = settingsrow_content_top(cfg);

	list = lv_obj_create(viewspage_screen);
	lv_obj_set_size(list, lv_pct(100), cfg->screen_height - content_top);
	lv_obj_align(list, LV_ALIGN_TOP_LEFT, 0, content_top);
	lv_obj_set_style_bg_opa(list, 0, 0);
	lv_obj_set_style_border_width(list, 0, 0);
	lv_obj_set_style_radius(list, 0, 0);
	lv_obj_set_style_pad_hor(list, cfg->padding, 0);
	lv_obj_set_style_pad_ver(list, 0, 0);
	lv_obj_set_scroll_dir(list, LV_DIR_VER);
	lv_obj_add_event_cb(list, list_scroll_cb, LV_EVENT_SCROLL, NULL);

	list_body = lv_obj_create(list);
	lv_obj_set_width(list_body, lv_pct(100));
	lv_obj_set_height(list_body, ROW_PITCH);
	lv_obj_set_style_bg_opa(list_body, 0, 0);
	lv_obj_set_style_border_width(list_body, 0, 0);
	lv_obj_set_style_pad_all(list_body, 0, 0);
	lv_obj_set_scrollable(list_body, false);
	lv_obj_set_event_bubble(list_body, true);

	build_rows(cfg->screen_width - 2 * cfg->padding);

	message = lv_label_create(list);
	lv_obj_set_width(message, lv_pct(100));
	lv_label_set_long_mode(message, LV_LABEL_LONG_WRAP);
	lv_obj_set_style_text_align(message, LV_TEXT_ALIGN_CENTER, 0);
	lv_obj_add_style(message, &theme_style_text_dim, 0);
	lv_obj_set_style_text_font(message, &font_ui_22, 0);
	lv_obj_align(message, LV_ALIGN_TOP_MID, 0, 40);
	lv_obj_set_hidden(message, true);

	player_sheet_attach_drag(list, true);
	switcher_attach_back_gesture(list);
	switcher_set_back_guard(viewspage_screen, go_up);
	switcher_set_back_guard_peek(viewspage_screen, go_up_peek);
	lv_obj_add_event_cb(viewspage_screen, screen_load_cb, LV_EVENT_SCREEN_LOAD_START, NULL);

	coverloader_start();
	thumb_timer = lv_timer_create(thumb_timer_cb, THUMB_TICK_PERIOD_MS, NULL);
	lv_timer_pause(thumb_timer);
}
