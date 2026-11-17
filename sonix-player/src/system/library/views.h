#ifndef VIEWS_H
#define VIEWS_H

#include <stdbool.h>

// The library menus of this fork: a tree the computer works out from the beets
// library and writes to the card whole, as <card>/.local/views.db (see
// views/build_views.py beside the fork). Nothing in here decides what goes
// where -- which records are a label's, what order a catalogue runs in: the
// player only reads the tree back, a window of rows at a time, so changing
// the menus never needs a new firmware.
//
// A node is a menu (its children are nodes), an album (the tracks of one
// record) or a track list (tracks picked out of several records: one artist's
// part of a compilation). Every node carries its place in the tree as a
// sortpath -- the positions from the root down, five digits each -- so
// everything under a node, in the order the menus show it, is one range.

typedef enum {
	VIEWS_MENU = 0,
	VIEWS_ALBUM = 1,
	VIEWS_TRACKS = 2,
} views_kind_t;

typedef struct {
	int id;
	views_kind_t kind;
	int pos; // its place among its siblings, from 0
	char title[256];
	char subtitle[160];
	char cover[1024]; // full path of the track whose art the row shows; "" for none
} views_node_t;

// Opens <card_root>/.local/views.db read-only, or opens it again when the file
// is no longer the one open: the computer replaces it whole, and the card may
// have been out in between. False, with every call below answering nothing,
// when there is no such file.
bool views_open(const char *card_root);
void views_close(void);

// Bumped every time a file is opened, the first time or after it was
// replaced: node ids from before mean nothing in the new file.
unsigned views_generation(void);

// When the file was written, as the computer put it ("2026-11-17T14:52:00+01:00");
// "" when unknown.
const char *views_generated(void);

// The root's id, 0 when no file is open.
int views_root(void);
bool views_node(int id, views_node_t *out);
int views_parent(int id); // 0 for the root and for an unknown id
int views_child_count(int parent);

// `count` of `parent`'s children starting at position `offset`, in order.
// Returns how many were written.
int views_children(int parent, int offset, int count, views_node_t *out);

// What playing `id` plays -- an album's tracks, a track list's, all of a
// menu's -- in the order the menus give them, as full paths. A menu may name
// another node to play instead (a label plays its Catalogue, not the same
// records once per submenu), and a file is in the list once however many menus
// below hold it. With `from_pos` >= 0, `id` is a menu and the list is what
// playing each of its children from that position on would play, one after
// the other: "play from here". `titles` and `artists` may be NULL. Returns the
// count; free each array with views_free_list().
int views_tracks(int id, int from_pos, char ***paths, char ***titles, char ***artists);
void views_free_list(char **list, int count);

// How many files views_tracks(id, -1, ...) would give, without reading them.
int views_track_count(int id);

// The position among `parent`'s children of the one that has the file at
// `path` somewhere under it, -1 when none has. What marks the row of the
// record that is playing.
int views_child_holding(int parent, const char *path);

#endif // VIEWS_H
