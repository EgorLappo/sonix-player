#include "views.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#include "src/system/db/sqlite3.h"

// Where the computer leaves the tree, beside the player's own index.
#define VIEWS_FILE ".local/views.db"

// The layout build_views.py writes; a file of another version is not read.
#define VIEWS_VERSION 1

static sqlite3 *db;
static char card[512];		// the card root the paths in the file are relative to
static char file_path[600]; // the file that is open
static struct stat opened;	// and what it looked like when it was opened
static char generated[64];
static unsigned generation;

static sqlite3_stmt *st_node;
static sqlite3_stmt *st_parent;
static sqlite3_stmt *st_count;
static sqlite3_stmt *st_children;
static sqlite3_stmt *st_target;
static sqlite3_stmt *st_sortpath;
static sqlite3_stmt *st_siblings;
static sqlite3_stmt *st_range;
static sqlite3_stmt *st_range_count;
static sqlite3_stmt *st_holding;

static sqlite3_stmt **const STATEMENTS[] = {
	&st_node,	  &st_parent, &st_count,	   &st_children, &st_target,
	&st_sortpath, &st_siblings, &st_range, &st_range_count, &st_holding,
};

static const char *const SQL[] = {
	"SELECT id, kind, pos, title, ifnull(subtitle, ''), ifnull(cover, '') FROM node WHERE id = ?1",
	"SELECT parent FROM node WHERE id = ?1",
	"SELECT count(*) FROM node WHERE parent = ?1",
	// Positions run from 0 without gaps, so a window is a range of them: no
	// OFFSET, which would step over every row before it.
	"SELECT id, kind, pos, title, ifnull(subtitle, ''), ifnull(cover, '') FROM node"
	" WHERE parent = ?1 AND pos >= ?2 ORDER BY pos LIMIT ?3",
	// What playing a node plays: another node (a label's Catalogue), or itself.
	"SELECT ifnull(play, id) FROM node WHERE id = ?1",
	"SELECT sortpath FROM node WHERE id = ?1",
	"SELECT ifnull(play, id) FROM node WHERE parent = ?1 AND pos >= ?2 ORDER BY pos",
	// Everything in one subtree. '/' is the character after '.', so "00002/"
	// is past every "00002.xxxxx" and before "00003".
	"SELECT t.path, t.title, ifnull(t.artist, '') FROM node n JOIN track t ON t.node = n.id"
	" WHERE n.sortpath >= ?1 AND n.sortpath < ?2 ORDER BY n.sortpath, t.pos",
	"SELECT count(DISTINCT t.path) FROM node n JOIN track t ON t.node = n.id"
	" WHERE n.sortpath >= ?1 AND n.sortpath < ?2",
	// The child of ?2 above a track: the prefix of the track's node's sortpath
	// that is one level below ?2 (whose own sortpath is ?3 characters long).
	"SELECT c.pos FROM track t JOIN node n ON n.id = t.node"
	" JOIN node c ON c.sortpath = substr(n.sortpath, 1, ?3) WHERE t.path = ?1 AND c.parent = ?2 LIMIT 1",
};

static void copy_column(char *dst, size_t size, sqlite3_stmt *st, int column) {
	const unsigned char *text = sqlite3_column_text(st, column);
	snprintf(dst, size, "%s", text ? (const char *)text : "");
}

void views_close(void) {
	for (size_t i = 0; i < sizeof(STATEMENTS) / sizeof(STATEMENTS[0]); i++) {
		if (*STATEMENTS[i]) {
			sqlite3_finalize(*STATEMENTS[i]);
			*STATEMENTS[i] = NULL;
		}
	}
	if (db) {
		sqlite3_close(db);
		db = NULL;
	}
	file_path[0] = '\0';
	generated[0] = '\0';
}

static bool same_file(const struct stat *a, const struct stat *b) {
	return a->st_dev == b->st_dev && a->st_ino == b->st_ino && a->st_size == b->st_size &&
		   a->st_mtime == b->st_mtime;
}

bool views_open(const char *card_root) {
	if (!card_root) {
		views_close();
		return false;
	}
	char path[sizeof(file_path)];
	snprintf(path, sizeof(path), "%s/%s", card_root, VIEWS_FILE);

	struct stat st;
	if (stat(path, &st) != 0) {
		views_close();
		return false;
	}
	if (db && strcmp(path, file_path) == 0 && same_file(&st, &opened)) {
		return true;
	}

	views_close();
	if (sqlite3_open_v2(path, &db, SQLITE_OPEN_READONLY | SQLITE_OPEN_NOMUTEX, NULL) != SQLITE_OK) {
		fprintf(stderr, "views: cannot open %s: %s\n", path, db ? sqlite3_errmsg(db) : "out of memory");
		views_close();
		return false;
	}

	sqlite3_stmt *version = NULL;
	int found = -1;
	if (sqlite3_prepare_v2(db, "PRAGMA user_version", -1, &version, NULL) == SQLITE_OK &&
		sqlite3_step(version) == SQLITE_ROW) {
		found = sqlite3_column_int(version, 0);
	}
	sqlite3_finalize(version);
	if (found != VIEWS_VERSION) {
		fprintf(stderr, "views: %s is version %d, this player reads %d\n", path, found, VIEWS_VERSION);
		views_close();
		return false;
	}

	for (size_t i = 0; i < sizeof(STATEMENTS) / sizeof(STATEMENTS[0]); i++) {
		if (sqlite3_prepare_v2(db, SQL[i], -1, STATEMENTS[i], NULL) != SQLITE_OK) {
			fprintf(stderr, "views: %s: %s\n", path, sqlite3_errmsg(db));
			views_close();
			return false;
		}
	}

	sqlite3_stmt *meta = NULL;
	if (sqlite3_prepare_v2(db, "SELECT value FROM meta WHERE key = 'generated'", -1, &meta, NULL) == SQLITE_OK &&
		sqlite3_step(meta) == SQLITE_ROW) {
		copy_column(generated, sizeof(generated), meta, 0);
	}
	sqlite3_finalize(meta);

	snprintf(card, sizeof(card), "%s", card_root);
	snprintf(file_path, sizeof(file_path), "%s", path);
	opened = st;
	generation++;
	printf("views: %s, written %s\n", path, generated[0] ? generated : "(no date)");
	return true;
}

unsigned views_generation(void) { return generation; }

const char *views_generated(void) { return generated; }

int views_root(void) { return db ? 1 : 0; }

// A node row as the two statements above read it, cover made a full path.
static void read_node(sqlite3_stmt *st, views_node_t *out) {
	out->id = sqlite3_column_int(st, 0);
	out->kind = (views_kind_t)sqlite3_column_int(st, 1);
	out->pos = sqlite3_column_int(st, 2);
	copy_column(out->title, sizeof(out->title), st, 3);
	copy_column(out->subtitle, sizeof(out->subtitle), st, 4);
	const unsigned char *cover = sqlite3_column_text(st, 5);
	if (cover && cover[0]) {
		snprintf(out->cover, sizeof(out->cover), "%s/%s", card, (const char *)cover);
	} else {
		out->cover[0] = '\0';
	}
}

bool views_node(int id, views_node_t *out) {
	if (!db) {
		return false;
	}
	sqlite3_reset(st_node);
	sqlite3_bind_int(st_node, 1, id);
	bool found = sqlite3_step(st_node) == SQLITE_ROW;
	if (found) {
		read_node(st_node, out);
	}
	sqlite3_reset(st_node);
	return found;
}

int views_parent(int id) {
	if (!db) {
		return 0;
	}
	sqlite3_reset(st_parent);
	sqlite3_bind_int(st_parent, 1, id);
	int parent = sqlite3_step(st_parent) == SQLITE_ROW ? sqlite3_column_int(st_parent, 0) : 0;
	sqlite3_reset(st_parent);
	return parent;
}

int views_child_count(int parent) {
	if (!db) {
		return 0;
	}
	sqlite3_reset(st_count);
	sqlite3_bind_int(st_count, 1, parent);
	int n = sqlite3_step(st_count) == SQLITE_ROW ? sqlite3_column_int(st_count, 0) : 0;
	sqlite3_reset(st_count);
	return n;
}

int views_children(int parent, int offset, int count, views_node_t *out) {
	if (!db || count <= 0) {
		return 0;
	}
	sqlite3_reset(st_children);
	sqlite3_bind_int(st_children, 1, parent);
	sqlite3_bind_int(st_children, 2, offset);
	sqlite3_bind_int(st_children, 3, count);
	int n = 0;
	while (n < count && sqlite3_step(st_children) == SQLITE_ROW) {
		read_node(st_children, &out[n++]);
	}
	sqlite3_reset(st_children);
	return n;
}

// The node playing `id` plays; `id` itself unless the file names another.
static int play_target(int id) {
	sqlite3_reset(st_target);
	sqlite3_bind_int(st_target, 1, id);
	int target = sqlite3_step(st_target) == SQLITE_ROW ? sqlite3_column_int(st_target, 0) : 0;
	sqlite3_reset(st_target);
	return target;
}

// `id`'s subtree as a range of sortpaths, [lo, hi).
static bool subtree_range(int id, char *lo, size_t lo_size, char *hi, size_t hi_size) {
	sqlite3_reset(st_sortpath);
	sqlite3_bind_int(st_sortpath, 1, id);
	bool found = sqlite3_step(st_sortpath) == SQLITE_ROW;
	if (found) {
		copy_column(lo, lo_size, st_sortpath, 0);
	}
	sqlite3_reset(st_sortpath);
	if (!found || !lo[0]) {
		return false;
	}
	snprintf(hi, hi_size, "%s/", lo);
	return true;
}

void views_free_list(char **list, int count) {
	if (!list) {
		return;
	}
	for (int i = 0; i < count; i++) {
		free(list[i]);
	}
	free(list);
}

// ---------------------------------------------------------------------------
// gathering a track list
//
// One file goes into a list once, however many of the menus under the node
// hold it: a label's records sit under its series, its Catalogue, its Artists
// and its years, and "play from here" across several labels would otherwise
// play each of them again and again. The files already taken are remembered by
// a 64-bit hash of their path, in an open-addressed table.
// ---------------------------------------------------------------------------

typedef struct {
	bool want[3];	 // paths, titles, artists: which of the three are gathered
	char **lists[3];
	int n;
	int capacity;
	uint64_t *seen;
	size_t seen_cap; // a power of two
	size_t seen_used;
	bool failed;
} gather_t;

static uint64_t path_hash(const char *s) {
	uint64_t h = 1469598103934665603ULL; // FNV-1a
	for (; *s; s++) {
		h ^= (unsigned char)*s;
		h *= 1099511628211ULL;
	}
	return h ? h : 1; // 0 marks an empty slot
}

// True when `h` was not there yet (and is now).
static bool seen_add(gather_t *g, uint64_t h) {
	if ((g->seen_used + 1) * 2 > g->seen_cap) {
		size_t cap = g->seen_cap ? g->seen_cap * 2 : 1024;
		uint64_t *table = calloc(cap, sizeof(uint64_t));
		if (!table) {
			g->failed = true;
			return false;
		}
		for (size_t i = 0; i < g->seen_cap; i++) {
			if (g->seen[i]) {
				size_t j = (size_t)g->seen[i] & (cap - 1);
				while (table[j]) {
					j = (j + 1) & (cap - 1);
				}
				table[j] = g->seen[i];
			}
		}
		free(g->seen);
		g->seen = table;
		g->seen_cap = cap;
	}
	size_t j = (size_t)h & (g->seen_cap - 1);
	while (g->seen[j]) {
		if (g->seen[j] == h) {
			return false;
		}
		j = (j + 1) & (g->seen_cap - 1);
	}
	g->seen[j] = h;
	g->seen_used++;
	return true;
}

static bool gather_push(gather_t *g, const char *values[3]) {
	if (g->n == g->capacity) {
		int grown = g->capacity ? g->capacity * 2 : 64;
		for (int k = 0; k < 3; k++) {
			if (!g->want[k]) {
				continue;
			}
			char **bigger = realloc(g->lists[k], (size_t)grown * sizeof(char *));
			if (!bigger) {
				return false;
			}
			g->lists[k] = bigger;
		}
		g->capacity = grown;
	}
	for (int k = 0; k < 3; k++) {
		if (!g->want[k]) {
			continue;
		}
		g->lists[k][g->n] = strdup(values[k]);
		if (!g->lists[k][g->n]) {
			for (int j = 0; j < k; j++) {
				if (g->want[j]) {
					free(g->lists[j][g->n]);
				}
			}
			return false;
		}
	}
	g->n++;
	return true;
}

// Every track of `id`'s subtree not taken yet, in order, onto the list.
static void gather_subtree(gather_t *g, int id) {
	char lo[256], hi[260];
	if (g->failed || !subtree_range(id, lo, sizeof(lo), hi, sizeof(hi))) {
		return;
	}
	sqlite3_reset(st_range);
	sqlite3_bind_text(st_range, 1, lo, -1, SQLITE_TRANSIENT);
	sqlite3_bind_text(st_range, 2, hi, -1, SQLITE_TRANSIENT);
	while (sqlite3_step(st_range) == SQLITE_ROW) {
		const unsigned char *rel = sqlite3_column_text(st_range, 0);
		if (!rel || !seen_add(g, path_hash((const char *)rel))) {
			if (g->failed) {
				break;
			}
			continue;
		}
		char full[1024];
		snprintf(full, sizeof(full), "%s/%s", card, (const char *)rel);
		const unsigned char *title = sqlite3_column_text(st_range, 1);
		const unsigned char *artist = sqlite3_column_text(st_range, 2);
		const char *values[3] = {full, title ? (const char *)title : "", artist ? (const char *)artist : ""};
		if (!gather_push(g, values)) {
			g->failed = true; // out of memory: what was gathered so far is still a list
			break;
		}
	}
	sqlite3_reset(st_range);
}

int views_tracks(int id, int from_pos, char ***paths, char ***titles, char ***artists) {
	*paths = NULL;
	if (titles) {
		*titles = NULL;
	}
	if (artists) {
		*artists = NULL;
	}
	if (!db) {
		return 0;
	}

	gather_t g = {0};
	g.want[0] = true;
	g.want[1] = titles != NULL;
	g.want[2] = artists != NULL;

	if (from_pos < 0) {
		gather_subtree(&g, play_target(id));
	} else {
		// The children from `from_pos` on, each as playing it would play it.
		int targets_cap = 64, targets_n = 0;
		int *targets = malloc((size_t)targets_cap * sizeof(int));
		sqlite3_reset(st_siblings);
		sqlite3_bind_int(st_siblings, 1, id);
		sqlite3_bind_int(st_siblings, 2, from_pos);
		while (targets && sqlite3_step(st_siblings) == SQLITE_ROW) {
			if (targets_n == targets_cap) {
				int *bigger = realloc(targets, (size_t)(targets_cap *= 2) * sizeof(int));
				if (!bigger) {
					break;
				}
				targets = bigger;
			}
			targets[targets_n++] = sqlite3_column_int(st_siblings, 0);
		}
		sqlite3_reset(st_siblings);
		for (int i = 0; i < targets_n && !g.failed; i++) {
			gather_subtree(&g, targets[i]);
		}
		free(targets);
	}
	free(g.seen);

	if (g.n == 0) {
		for (int k = 0; k < 3; k++) {
			free(g.lists[k]);
		}
		return 0;
	}
	*paths = g.lists[0];
	if (titles) {
		*titles = g.lists[1];
	}
	if (artists) {
		*artists = g.lists[2];
	}
	return g.n;
}

int views_track_count(int id) {
	if (!db) {
		return 0;
	}
	char lo[256], hi[260];
	if (!subtree_range(play_target(id), lo, sizeof(lo), hi, sizeof(hi))) {
		return 0;
	}
	sqlite3_reset(st_range_count);
	sqlite3_bind_text(st_range_count, 1, lo, -1, SQLITE_TRANSIENT);
	sqlite3_bind_text(st_range_count, 2, hi, -1, SQLITE_TRANSIENT);
	int n = sqlite3_step(st_range_count) == SQLITE_ROW ? sqlite3_column_int(st_range_count, 0) : 0;
	sqlite3_reset(st_range_count);
	return n;
}

int views_child_holding(int parent, const char *path) {
	if (!db || !path || !path[0]) {
		return -1;
	}
	size_t root_len = strlen(card);
	if (strncmp(path, card, root_len) != 0 || path[root_len] != '/') {
		return -1; // not on the card
	}

	char own[256], hi[260];
	if (!subtree_range(parent, own, sizeof(own), hi, sizeof(hi))) {
		return -1;
	}

	sqlite3_reset(st_holding);
	sqlite3_bind_text(st_holding, 1, path + root_len + 1, -1, SQLITE_TRANSIENT);
	sqlite3_bind_int(st_holding, 2, parent);
	sqlite3_bind_int(st_holding, 3, (int)strlen(own) + 6); // ".NNNNN"
	int pos = sqlite3_step(st_holding) == SQLITE_ROW ? sqlite3_column_int(st_holding, 0) : -1;
	sqlite3_reset(st_holding);
	return pos;
}
