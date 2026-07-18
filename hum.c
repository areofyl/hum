#include <ncurses.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <sys/stat.h>
#include <signal.h>
#include <fcntl.h>
#include <dirent.h>
#include <time.h>

#include "config.h"

#define MAX_RESULTS  20
#define MAX_TITLE    512
#define MAX_URL      768
#define MAX_QUERY    256
#define MAX_LIB      1000
static char sock_path[128];

typedef struct {
	char title[MAX_TITLE];
	char url[MAX_URL];
	int is_playlist;
} Track;

enum {
	MODE_HOME, MODE_SEARCH, MODE_BROWSE, MODE_QUEUE, MODE_LIBRARY,
	MODE_PLAYLIST, MODE_PLSAVE, MODE_PLRENAME, MODE_PLADD, MODE_HELP,
	MODE_CONFIRM, MODE_DLNAME, MODE_BATCHNAME
};

enum { REP_OFF, REP_ONE, REP_ALL };

static Track results[MAX_RESULTS];
static int nresults;
static Track queue[500];
static int nqueue, qpos = -1;
static Track library[MAX_LIB];
static int nlib;
static int sel, mode = MODE_BROWSE;
static char query[MAX_QUERY];
static int qlen;
static pid_t mpv_pid = -1;
static int paused;
static char nowplaying[MAX_TITLE];
static char libpath[512];
static int qscroll, libscroll, rscroll;
static double cur_pos, cur_dur;
static char plspath[1024];
static char plnames[50][128];
static int nplaylists;
static Track pl_tracks[500];
static int npl_tracks;
static int pl_level;
static int plscroll;
static int pl_cur;
static char input_buf[128];
static int input_len;
static int repeat_mode = REP_OFF;
static int home_sel;

/* recently played */
#define MAX_RECENT 5
static Track recent[MAX_RECENT];
static int nrecent;
static char histpath[1024];
static char queuepath[1024];

/* filter (shared across list modes) */
static char filter_buf[MAX_QUERY];
static int filter_len;
static int filtering;
static int filt_idx[MAX_LIB]; /* MAX_LIB is the largest list */
static int nfilt;

/* visual mode */
static int visual;
static int vsel_start;

/* add-to-playlist state */
static Track pladd_tracks[500];
static int npladd;
static int pladd_ret;
static int pladd_scroll;

/* rename state */
static int plrename_idx;

/* download name state */
static char dl_url[MAX_URL];
static char dl_default[MAX_TITLE];
static int dl_retmode;

/* batch download state */
static Track dl_batch[500];
static char dl_names[500][MAX_TITLE];
static int dl_batch_count;
static int dl_batch_idx;
static char dl_batch_plname[128];

/* status flash message */
static char status_msg[128];
static int status_ticks; /* counts down each 200ms tick */

static void
status_set(const char *msg)
{
	snprintf(status_msg, sizeof(status_msg), "%s", msg);
	status_ticks = 10; /* 2 seconds */
}

/* confirm state */
static char confirm_msg[256];
static int confirm_ret;
static void (*confirm_action)(void);
static int confirm_arg;

/* pending confirm actions - defined after functions they call */
static void do_pl_delete(void);
static void do_lib_delete(void);
static void do_queue_clear(void);

/* ---- path helpers ---- */

static void
resolve_libpath(void)
{
	const char *home;

	if (lib_dir[0] == '~') {
		home = getenv("HOME");
		if (!home) home = "/tmp";
		snprintf(libpath, sizeof(libpath), "%s%s", home, lib_dir + 1);
	} else {
		snprintf(libpath, sizeof(libpath), "%s", lib_dir);
	}
}

static int
filter_match(const char *title)
{
	int ti, fi;
	if (filter_len == 0)
		return 1;
	for (ti = 0; title[ti]; ti++) {
		for (fi = 0; filter_buf[fi]; fi++) {
			char tc = title[ti + fi];
			char fc = filter_buf[fi];
			if (tc >= 'A' && tc <= 'Z') tc += 32;
			if (fc >= 'A' && fc <= 'Z') fc += 32;
			if (tc != fc) break;
		}
		if (!filter_buf[fi]) return 1;
	}
	return 0;
}

static void
resolve_plspath(void)
{
	snprintf(plspath, sizeof(plspath), "%s/playlists", libpath);
}

static void
ensure_dir(const char *path)
{
	struct stat st;
	if (stat(path, &st) < 0)
		mkdir(path, 0755);
}

/* ---- library ---- */

static void
lib_scan(void)
{
	DIR *d;
	struct dirent *e;
	const char *ext;

	nlib = 0;
	d = opendir(libpath);
	if (!d)
		return;
	while ((e = readdir(d)) && nlib < MAX_LIB) {
		if (e->d_name[0] == '.')
			continue;
		ext = strrchr(e->d_name, '.');
		if (!ext)
			continue;
		if (strcmp(ext, ".mp3") && strcmp(ext, ".opus") &&
		    strcmp(ext, ".m4a") && strcmp(ext, ".ogg") &&
		    strcmp(ext, ".flac") && strcmp(ext, ".wav") &&
		    strcmp(ext, ".webm"))
			continue;
		snprintf(library[nlib].title, MAX_TITLE, "%s", e->d_name);
		char *dot = strrchr(library[nlib].title, '.');
		if (dot) *dot = '\0';
		snprintf(library[nlib].url, MAX_URL, "%s/%s", libpath, e->d_name);
		nlib++;
	}
	closedir(d);
	/* sort alphabetically */
	qsort(library, nlib, sizeof(Track),
	    (int (*)(const void *, const void *))strcmp);
}

static int
lib_has(const char *title)
{
	int i;
	for (i = 0; i < nlib; i++) {
		if (strcmp(library[i].title, title) == 0)
			return 1;
	}
	return 0;
}

/* return local path if track is in library, otherwise original url */
static const char *
lib_resolve(const char *title, const char *url)
{
	int i;
	for (i = 0; i < nlib; i++) {
		if (strcmp(library[i].title, title) == 0)
			return library[i].url;
	}
	return url;
}

static void
lib_delete(int idx)
{
	if (idx < 0 || idx >= nlib)
		return;
	unlink(library[idx].url);
	lib_scan();
	if (sel >= nlib) sel = nlib - 1;
	if (sel < 0) sel = 0;
}

/* ---- playlists ---- */

static void
pl_scan(void)
{
	DIR *d;
	struct dirent *e;
	const char *ext;

	nplaylists = 0;
	d = opendir(plspath);
	if (!d)
		return;
	while ((e = readdir(d)) && nplaylists < 50) {
		if (e->d_name[0] == '.')
			continue;
		ext = strrchr(e->d_name, '.');
		if (!ext || strcmp(ext, ".hum"))
			continue;
		snprintf(plnames[nplaylists], 128, "%s", e->d_name);
		char *dot = strrchr(plnames[nplaylists], '.');
		if (dot) *dot = '\0';
		nplaylists++;
	}
	closedir(d);
}

static void
pl_load(int idx)
{
	char path[2048];
	FILE *fp;
	char line[MAX_TITLE + MAX_URL];

	snprintf(path, sizeof(path), "%s/%s.hum", plspath, plnames[idx]);
	fp = fopen(path, "r");
	if (!fp)
		return;
	npl_tracks = 0;
	while (fgets(line, sizeof(line), fp) && npl_tracks < 500) {
		char *tab = strchr(line, '\t');
		if (!tab)
			continue;
		*tab = '\0';
		tab++;
		tab[strcspn(tab, "\n")] = '\0';
		snprintf(pl_tracks[npl_tracks].title, MAX_TITLE, "%s", line);
		snprintf(pl_tracks[npl_tracks].url, MAX_URL, "%s", tab);
		npl_tracks++;
	}
	fclose(fp);
	pl_cur = idx;
}

static void
pl_write(int idx)
{
	char path[2048];
	FILE *fp;
	int i;

	snprintf(path, sizeof(path), "%s/%s.hum", plspath, plnames[idx]);
	fp = fopen(path, "w");
	if (!fp)
		return;
	for (i = 0; i < npl_tracks; i++)
		fprintf(fp, "%s\t%s\n", pl_tracks[i].title, pl_tracks[i].url);
	fclose(fp);
}

static void
pl_save(const char *name)
{
	char path[2048];
	FILE *fp;
	int i;

	if (nqueue == 0)
		return;
	snprintf(path, sizeof(path), "%s/%s.hum", plspath, name);
	fp = fopen(path, "w");
	if (!fp)
		return;
	for (i = 0; i < nqueue; i++)
		fprintf(fp, "%s\t%s\n", queue[i].title, queue[i].url);
	fclose(fp);
}

static void
pl_delete(int idx)
{
	char path[2048];

	if (idx < 0 || idx >= nplaylists)
		return;
	snprintf(path, sizeof(path), "%s/%s.hum", plspath, plnames[idx]);
	unlink(path);
	pl_scan();
	if (sel >= nplaylists) sel = nplaylists - 1;
	if (sel < 0) sel = 0;
}

static void
pl_rename(int idx, const char *newname)
{
	char oldpath[2048], newpath[2048];

	if (idx < 0 || idx >= nplaylists)
		return;
	snprintf(oldpath, sizeof(oldpath), "%s/%s.hum", plspath, plnames[idx]);
	snprintf(newpath, sizeof(newpath), "%s/%s.hum", plspath, newname);
	rename(oldpath, newpath);
	pl_scan();
}

static void
pl_del_track(int tidx)
{
	int i;

	if (tidx < 0 || tidx >= npl_tracks)
		return;
	for (i = tidx; i < npl_tracks - 1; i++)
		pl_tracks[i] = pl_tracks[i + 1];
	npl_tracks--;
	pl_write(pl_cur);
	if (sel >= npl_tracks) sel = npl_tracks - 1;
	if (sel < 0) sel = 0;
}

static void
pl_append_tracks(int idx, Track *tracks, int n)
{
	char path[2048];
	FILE *fp;
	int i;

	if (idx < 0 || idx >= nplaylists)
		return;
	snprintf(path, sizeof(path), "%s/%s.hum", plspath, plnames[idx]);
	fp = fopen(path, "a");
	if (!fp)
		return;
	for (i = 0; i < n; i++)
		fprintf(fp, "%s\t%s\n", tracks[i].title, tracks[i].url);
	fclose(fp);
}

/* ---- history ---- */

static void
history_load(void)
{
	FILE *fp;
	char line[MAX_TITLE + MAX_URL];
	Track tmp[200];
	int ntmp = 0;
	int i, j;

	snprintf(histpath, sizeof(histpath), "%s/.history", libpath);
	fp = fopen(histpath, "r");
	if (!fp)
		return;
	while (fgets(line, sizeof(line), fp) && ntmp < 200) {
		char *tab = strchr(line, '\t');
		if (!tab)
			continue;
		*tab = '\0';
		tab++;
		tab[strcspn(tab, "\n")] = '\0';
		snprintf(tmp[ntmp].title, MAX_TITLE, "%s", line);
		snprintf(tmp[ntmp].url, MAX_URL, "%s", tab);
		ntmp++;
	}
	fclose(fp);

	/* walk backwards, collect unique titles */
	nrecent = 0;
	for (i = ntmp - 1; i >= 0 && nrecent < MAX_RECENT; i--) {
		int dup = 0;
		for (j = 0; j < nrecent; j++) {
			if (strcmp(recent[j].title, tmp[i].title) == 0) {
				dup = 1;
				break;
			}
		}
		if (!dup)
			recent[nrecent++] = tmp[i];
	}
}

static void
history_log(const char *title, const char *url)
{
	FILE *fp;

	fp = fopen(histpath, "a");
	if (!fp)
		return;
	fprintf(fp, "%s\t%s\n", title, url);
	fclose(fp);

	/* update in-memory recent list: push to front, dedup */
	int i, j;
	for (i = 0; i < nrecent; i++) {
		if (strcmp(recent[i].title, title) == 0) {
			/* shift down */
			Track t = recent[i];
			for (j = i; j > 0; j--)
				recent[j] = recent[j - 1];
			recent[0] = t;
			return;
		}
	}
	/* new entry: shift everything down */
	if (nrecent < MAX_RECENT)
		nrecent++;
	for (j = nrecent - 1; j > 0; j--)
		recent[j] = recent[j - 1];
	snprintf(recent[0].title, MAX_TITLE, "%s", title);
	snprintf(recent[0].url, MAX_URL, "%s", url);
}

static void
history_trim(void)
{
	FILE *fp;
	char lines[50][MAX_TITLE + MAX_URL];
	int nlines = 0, total = 0;
	char line[MAX_TITLE + MAX_URL];

	fp = fopen(histpath, "r");
	if (!fp)
		return;
	while (fgets(line, sizeof(line), fp))
		total++;
	fclose(fp);
	if (total <= 50)
		return;

	fp = fopen(histpath, "r");
	if (!fp)
		return;
	int skip = total - 50;
	int i = 0;
	while (fgets(line, sizeof(line), fp)) {
		if (i >= skip && nlines < 50)
			snprintf(lines[nlines++], sizeof(lines[0]), "%s", line);
		i++;
	}
	fclose(fp);

	fp = fopen(histpath, "w");
	if (!fp)
		return;
	for (i = 0; i < nlines; i++)
		fputs(lines[i], fp);
	fclose(fp);
}

/* ---- persistent queue ---- */

static void
queue_save(void)
{
	FILE *fp;
	int i;

	fp = fopen(queuepath, "w");
	if (!fp)
		return;
	fprintf(fp, "%d\n", qpos);
	for (i = 0; i < nqueue; i++)
		fprintf(fp, "%s\t%s\n", queue[i].title, queue[i].url);
	fclose(fp);
}

static void
queue_load(void)
{
	FILE *fp;
	char line[MAX_TITLE + MAX_URL];

	snprintf(queuepath, sizeof(queuepath), "%s/.queue", libpath);
	fp = fopen(queuepath, "r");
	if (!fp)
		return;
	if (fgets(line, sizeof(line), fp))
		qpos = atoi(line);
	while (fgets(line, sizeof(line), fp) && nqueue < 500) {
		char *tab = strchr(line, '\t');
		if (!tab)
			continue;
		*tab = '\0';
		tab++;
		tab[strcspn(tab, "\n")] = '\0';
		snprintf(queue[nqueue].title, MAX_TITLE, "%s", line);
		snprintf(queue[nqueue].url, MAX_URL, "%s", tab);
		nqueue++;
	}
	fclose(fp);
	if (qpos >= nqueue) qpos = nqueue - 1;
}

/* ---- mpv ---- */

static int
mpv_connect(void)
{
	struct sockaddr_un addr = {0};
	int fd;

	fd = socket(AF_UNIX, SOCK_STREAM, 0);
	if (fd < 0)
		return -1;
	addr.sun_family = AF_UNIX;
	snprintf(addr.sun_path, sizeof(addr.sun_path), "%s", sock_path);
	if (connect(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
		close(fd);
		return -1;
	}
	return fd;
}

static void
mpv_cmd(const char *cmd)
{
	int fd = mpv_connect();
	if (fd < 0)
		return;
	if (write(fd, cmd, strlen(cmd)) < 0 || write(fd, "\n", 1) < 0) {
		/* ignore */
	}
	close(fd);
}

static double
mpv_get_prop(const char *prop)
{
	int fd;
	char cmd[128], buf[1024];
	ssize_t n;
	char *dptr;

	fd = mpv_connect();
	if (fd < 0)
		return -1;
	snprintf(cmd, sizeof(cmd),
	    "{\"command\":[\"get_property\",\"%s\"]}\n", prop);
	if (write(fd, cmd, strlen(cmd)) < 0) {
		close(fd);
		return -1;
	}

	n = read(fd, buf, sizeof(buf) - 1);
	close(fd);
	if (n <= 0)
		return -1;
	buf[n] = '\0';

	dptr = strstr(buf, "\"data\":");
	if (!dptr)
		return -1;
	return atof(dptr + 7);
}

static void
mpv_get_progress(void)
{
	if (mpv_pid <= 0)
		return;
	cur_pos = mpv_get_prop("time-pos");
	cur_dur = mpv_get_prop("duration");
	if (cur_pos < 0) cur_pos = 0;
	if (cur_dur < 0) cur_dur = 0;
}

static void
mpv_stop(void)
{
	if (mpv_pid > 0) {
		kill(mpv_pid, SIGTERM);
		waitpid(mpv_pid, NULL, 0);
		mpv_pid = -1;
	}
	unlink(sock_path);
}

static void
mpv_play(const char *url, const char *title)
{
	const char *resolved = lib_resolve(title, url);

	mpv_stop();
	mpv_pid = fork();
	if (mpv_pid == 0) {
		close(STDIN_FILENO);
		close(STDOUT_FILENO);
		close(STDERR_FILENO);
		char ipc_arg[256];
		snprintf(ipc_arg, sizeof(ipc_arg),
		    "--input-ipc-server=%s", sock_path);
		execlp("mpv", "mpv", "--no-video", "--really-quiet",
		    ipc_arg, resolved, NULL);
		_exit(1);
	}
	snprintf(nowplaying, sizeof(nowplaying), "%s", title);
	paused = 0;
	history_log(title, url);
}

static void
lib_download(const char *url, const char *name)
{
	pid_t pid;
	char out[2048];

	if (lib_has(name))
		return;
	snprintf(out, sizeof(out), "%s/%s.%%(ext)s", libpath, name);
	pid = fork();
	if (pid == 0) {
		close(STDIN_FILENO);
		close(STDOUT_FILENO);
		close(STDERR_FILENO);
		execlp("yt-dlp", "yt-dlp", "-x", "--audio-format", "opus",
		    "-o", out, url, NULL);
		_exit(1);
	}
}

/* ---- search (non-blocking, songs + playlists) ---- */

static pid_t search_pid = -1;
static int search_fd = -1;
static char search_buf[4096];
static int search_buf_len;

static pid_t search_pid_pl = -1;
static int search_fd_pl = -1;
static char search_buf_pl[4096];
static int search_buf_pl_len;

static int searching;
static int search_is_url; /* 1 if query was a URL or expanded playlist */
static int npl_results;

static void
search_kill(pid_t *pid, int *fd)
{
	if (*pid > 0) {
		kill(*pid, SIGTERM);
		waitpid(*pid, NULL, 0);
		close(*fd);
		*pid = -1;
		*fd = -1;
	}
}

static void
search_start(const char *q)
{
	int pipefd[2];
	char arg[MAX_QUERY + 128];
	char plend[16], plend_pl[16];

	search_kill(&search_pid, &search_fd);
	search_kill(&search_pid_pl, &search_fd_pl);

	memset(results, 0, sizeof(results));
	nresults = 0;
	npl_results = 0;
	searching = 1;
	search_is_url = (strncmp(q, "http", 4) == 0);

	snprintf(plend, sizeof(plend), "%d", MAX_RESULTS);
	snprintf(plend_pl, sizeof(plend_pl), "%d", search_pl_count);

	/* song search */
	if (pipe(pipefd) < 0)
		return;
	if (search_is_url)
		snprintf(arg, sizeof(arg), "%s", q);
	else
		snprintf(arg, sizeof(arg), "ytsearch%d:%s", search_count, q);

	search_pid = fork();
	if (search_pid == 0) {
		close(pipefd[0]);
		dup2(pipefd[1], STDOUT_FILENO);
		close(pipefd[1]);
		close(STDERR_FILENO);
		setenv("PYTHONUNBUFFERED", "1", 1);
		execlp("yt-dlp", "yt-dlp", "--flat-playlist",
		    "--playlist-end", plend,
		    "--print", "%(title)s\t%(id)s", arg, NULL);
		_exit(1);
	}
	close(pipefd[1]);
	fcntl(pipefd[0], F_SETFL, O_NONBLOCK);
	search_fd = pipefd[0];
	search_buf_len = 0;
	search_buf[0] = '\0';

	/* playlist search (only for text queries) */
	if (!search_is_url) {
		char plurl[MAX_QUERY + 128];
		char encoded[MAX_QUERY * 3];
		int ei = 0;
		const char *p;

		/* url-encode query */
		for (p = q; *p && ei < (int)sizeof(encoded) - 4; p++) {
			if ((*p >= 'a' && *p <= 'z') || (*p >= 'A' && *p <= 'Z') ||
			    (*p >= '0' && *p <= '9') || *p == '-' || *p == '_' || *p == '.') {
				encoded[ei++] = *p;
			} else if (*p == ' ') {
				encoded[ei++] = '+';
			} else {
				snprintf(encoded + ei, 4, "%%%02X", (unsigned char)*p);
				ei += 3;
			}
		}
		encoded[ei] = '\0';
		snprintf(plurl, sizeof(plurl),
		    "https://www.youtube.com/results?search_query=%s&sp=EgIQAw%%3D%%3D",
		    encoded);

		if (pipe(pipefd) < 0)
			return;
		search_pid_pl = fork();
		if (search_pid_pl == 0) {
			close(pipefd[0]);
			dup2(pipefd[1], STDOUT_FILENO);
			close(pipefd[1]);
			close(STDERR_FILENO);
			setenv("PYTHONUNBUFFERED", "1", 1);
			execlp("yt-dlp", "yt-dlp", "--flat-playlist",
			    "--playlist-end", plend_pl,
			    "--print", "%(title)s\t%(id)s", plurl, NULL);
			_exit(1);
		}
		close(pipefd[1]);
		fcntl(pipefd[0], F_SETFL, O_NONBLOCK);
		search_fd_pl = pipefd[0];
		search_buf_pl_len = 0;
		search_buf_pl[0] = '\0';
	}
}

/* returns 1 on EOF (child closed pipe) */
static int
search_read_fd(int fd, char *buf, int *buf_len, int bufsize, int is_pl)
{
	char tmp[1024];
	ssize_t n;
	char *line, *next;
	int got_eof = 0;

	/* drain all available data from non-blocking fd */
	while ((n = read(fd, tmp, sizeof(tmp) - 1)) > 0) {
		if (*buf_len + n >= bufsize)
			n = bufsize - *buf_len - 1;
		if (n <= 0)
			break;
		memcpy(buf + *buf_len, tmp, n);
		*buf_len += n;
		buf[*buf_len] = '\0';
	}
	if (n == 0)
		got_eof = 1;

	line = buf;
	while ((next = strchr(line, '\n')) && nresults < MAX_RESULTS) {
		*next = '\0';
		char *tab = strchr(line, '\t');
		if (tab) {
			if (is_pl && npl_results >= search_pl_count) {
				line = next + 1;
				continue;
			}
			*tab = '\0';
			tab++;
			snprintf(results[nresults].title, MAX_TITLE, "%s", line);
			if (is_pl)
				snprintf(results[nresults].url, MAX_URL,
				    "https://www.youtube.com/playlist?list=%s", tab);
			else
				snprintf(results[nresults].url, MAX_URL,
				    "https://www.youtube.com/watch?v=%s", tab);
			results[nresults].is_playlist = is_pl;
			nresults++;
			if (is_pl) npl_results++;
		}
		line = next + 1;
	}
	if (line != buf) {
		*buf_len = strlen(line);
		memmove(buf, line, *buf_len + 1);
	}
	return got_eof;
}

static void
search_poll(void)
{
	int done = 1;

	if (search_fd >= 0) {
		int eof = search_read_fd(search_fd, search_buf,
		    &search_buf_len, sizeof(search_buf), 0);
		if (eof) {
			close(search_fd);
			search_fd = -1;
			if (search_pid > 0) {
				waitpid(search_pid, NULL, WNOHANG);
				search_pid = -1;
			}
		} else {
			done = 0;
		}
	}

	if (search_fd_pl >= 0) {
		int eof = search_read_fd(search_fd_pl, search_buf_pl,
		    &search_buf_pl_len, sizeof(search_buf_pl), 1);
		if (eof) {
			close(search_fd_pl);
			search_fd_pl = -1;
			if (search_pid_pl > 0) {
				waitpid(search_pid_pl, NULL, WNOHANG);
				search_pid_pl = -1;
			}
		} else {
			done = 0;
		}
	}

	if (done && searching) {
		searching = 0;
		sel = 0;
		rscroll = 0;
		if (nresults == 0)
			status_set("no results");
	}
}

static void
search_cancel(void)
{
	search_kill(&search_pid, &search_fd);
	search_kill(&search_pid_pl, &search_fd_pl);
	searching = 0;
}

/* ---- queue helpers ---- */

static void
queue_add(Track *t)
{
	if (nqueue < 500)
		queue[nqueue++] = *t;
}

static void
queue_del(int idx)
{
	int i;

	if (idx < 0 || idx >= nqueue)
		return;
	if (qpos == idx) {
		/* deleting currently playing track - stop */
		mpv_stop();
		nowplaying[0] = '\0';
		cur_pos = cur_dur = 0;
	}
	for (i = idx; i < nqueue - 1; i++)
		queue[i] = queue[i + 1];
	nqueue--;
	if (qpos > idx) qpos--;
	if (qpos >= nqueue) qpos = nqueue - 1;
	if (sel >= nqueue) sel = nqueue - 1;
	if (sel < 0) sel = 0;
}

static void
queue_clear(void)
{
	mpv_stop();
	nowplaying[0] = '\0';
	cur_pos = cur_dur = 0;
	nqueue = 0;
	qpos = -1;
	sel = 0;
	qscroll = 0;
}

static void do_pl_delete(void) { pl_delete(confirm_arg); status_set("playlist deleted"); }
static void do_lib_delete(void) { lib_delete(confirm_arg); status_set("track deleted"); }
static void do_queue_clear(void) { queue_clear(); status_set("queue cleared"); }

static void
queue_move(int from, int to)
{
	Track tmp;

	if (from < 0 || from >= nqueue || to < 0 || to >= nqueue)
		return;
	tmp = queue[from];
	queue[from] = queue[to];
	queue[to] = tmp;
	if (qpos == from) qpos = to;
	else if (qpos == to) qpos = from;
}

static void
queue_shuffle(void)
{
	int i, j;
	Track tmp;

	if (nqueue < 2)
		return;
	for (i = nqueue - 1; i > 0; i--) {
		j = rand() % (i + 1);
		tmp = queue[i];
		queue[i] = queue[j];
		queue[j] = tmp;
	}
	/* find where current track ended up */
	if (nowplaying[0]) {
		for (i = 0; i < nqueue; i++) {
			if (strcmp(queue[i].title, nowplaying) == 0) {
				qpos = i;
				break;
			}
		}
	}
}

/* ---- playback ---- */

static void
play_next(void)
{
	if (repeat_mode == REP_ONE && qpos >= 0 && qpos < nqueue) {
		mpv_play(queue[qpos].url, queue[qpos].title);
		return;
	}
	if (qpos + 1 < nqueue) {
		qpos++;
		mpv_play(queue[qpos].url, queue[qpos].title);
		lib_download(queue[qpos].url, queue[qpos].title);
	} else if (repeat_mode == REP_ALL && nqueue > 0) {
		qpos = 0;
		mpv_play(queue[qpos].url, queue[qpos].title);
	} else {
		nowplaying[0] = '\0';
		cur_pos = cur_dur = 0;
		paused = 0;
	}
}

static void
play_prev(void)
{
	if (qpos > 0) {
		qpos--;
		mpv_play(queue[qpos].url, queue[qpos].title);
	}
}

static void
play_selected(void)
{
	if (mode == MODE_LIBRARY) {
		if (sel < 0 || sel >= nfilt)
			return;
		int idx = filt_idx[sel];
		queue_add(&library[idx]);
		qpos = nqueue - 1;
		mpv_play(library[idx].url, library[idx].title);
		return;
	}
	if (mode == MODE_QUEUE) {
		if (sel < 0 || sel >= nqueue)
			return;
		qpos = sel;
		mpv_play(queue[sel].url, queue[sel].title);
		lib_download(queue[sel].url, queue[sel].title);
		return;
	}
	if (sel < 0 || sel >= nfilt)
		return;
	{
	int ridx = filt_idx[sel];
	/* if it's a playlist, expand it */
	if (results[ridx].is_playlist) {
		char expand_url[MAX_URL];
		snprintf(expand_url, sizeof(expand_url), "%s", results[ridx].url);
		filter_len = 0;
		filter_buf[0] = '\0';
		search_start(expand_url);
		return;
	}
	queue_add(&results[ridx]);
	if (mpv_pid <= 0 || waitpid(mpv_pid, NULL, WNOHANG) > 0) {
		mpv_pid = -1;
		qpos = nqueue - 1;
		mpv_play(results[ridx].url, results[ridx].title);
	}
	/* prompt for download name */
	snprintf(dl_url, MAX_URL, "%s", results[ridx].url);
	snprintf(dl_default, MAX_TITLE, "%s", results[ridx].title);
	snprintf(input_buf, sizeof(input_buf), "%s", results[ridx].title);
	input_len = strlen(input_buf);
	dl_retmode = MODE_BROWSE;
	mode = MODE_DLNAME;
	}
}

static void
cleanup(void)
{
	queue_save();
	search_cancel();
	mpv_stop();
	endwin();
}

/* ---- visual mode helpers ---- */

static int
vsel_min(void)
{
	return vsel_start < sel ? vsel_start : sel;
}

static int
vsel_max(void)
{
	return vsel_start > sel ? vsel_start : sel;
}

/* ---- list length ---- */

static int
list_len(void)
{
	if (mode == MODE_QUEUE) return nfilt;
	if (mode == MODE_LIBRARY) return nfilt;
	if (mode == MODE_PLAYLIST) return nfilt;
	if (mode == MODE_PLADD) return nplaylists;
	return nfilt + (search_is_url && nfilt > 0 && !searching && filter_len == 0 ? 1 : 0);
}

/* ---- colors (vim default) ---- */
#define C_HEADER  1
#define C_NUM     2
#define C_PLAYING 3
#define C_VISUAL  4
#define C_STATUS  5
#define C_BAR     6
#define C_SEARCH  7
#define C_MODE    8
#define C_DIM     9
#define C_PLMARK  10
#define C_SMARK   11
#define C_HTITLE  12
#define C_HSUB    13
#define C_HKEY    14
#define C_HSEL    15

/* ---- draw ---- */

static void
draw(void)
{
	int rows, cols, i, visible;
	getmaxyx(stdscr, rows, cols);

	erase();
	visible = rows - 5;
	if (visible < 1) visible = 1;

	if (mode == MODE_HOME) {
		static const char *logo[] = {
			"   ____ ___  ____ ___  ________ ",
			"  /    /   \\/    /   \\/        \\",
			" /         /         /         /",
			"/         /         /   /  /  / ",
			"\\___/____/\\________/\\__/__/__/  ",
		};
		int logo_w = 32;
		int logo_h = 5;
		int mw = 36;
		if (mw > cols - 4) mw = cols - 4;
		int mx = (cols - mw) / 2;
		if (mx < 1) mx = 1;
		int cy = rows / 2 - 7;
		if (cy < 0) cy = 0;
		int lx = mx + (mw - logo_w) / 2;
		if (lx < 0) lx = 0;

		int i;
		attron(A_BOLD | COLOR_PAIR(C_HTITLE));
		for (i = 0; i < logo_h; i++)
			mvprintw(cy + i, lx, "%s", logo[i]);
		attroff(A_BOLD | COLOR_PAIR(C_HTITLE));

		attron(COLOR_PAIR(C_HSUB));
		mvprintw(cy + logo_h + 1, mx + (mw - 10) / 2, "by areofyl");
		attroff(COLOR_PAIR(C_HSUB));

		int my = cy + logo_h + 3;

		static const char keys[]    = { 'p', 'b', '/', 'v' };
		static const char *labels[] = {
			"playlists", "browse library",
			"search youtube", "queue",
		};
		int counts[] = { nplaylists, nlib, -1, nqueue };
		char countbuf[16];

		for (i = 0; i < 4; i++) {
			int y = my + i;
			/* format count string */
			if (counts[i] > 999)
				snprintf(countbuf, sizeof(countbuf), "1000+");
			else if (counts[i] >= 0)
				snprintf(countbuf, sizeof(countbuf), "%d", counts[i]);
			else
				countbuf[0] = '\0';

			int clen = (int)strlen(countbuf);
			/* total row: "  k    label   count  " = mw chars */
			int inner = mw - 9; /* space after key to before trailing "  " */
			int label_max = clen > 0 ? inner - clen - 1 : inner;
			if (label_max < 1) label_max = 1;
			int llen = (int)strlen(labels[i]);
			int trunc = llen > label_max;

			move(y, mx);
			if (sel == i) {
				attron(A_BOLD | COLOR_PAIR(C_HSEL));
				printw("  %c    ", keys[i]);
				if (trunc)
					printw("%.*s..", label_max - 2, labels[i]);
				else
					printw("%-*s", label_max, labels[i]);
				if (clen > 0) printw(" %*s", clen, countbuf);
				printw("  ");
				attroff(A_BOLD | COLOR_PAIR(C_HSEL));
			} else {
				printw("  ");
				attron(A_BOLD | COLOR_PAIR(C_HKEY));
				printw("%c", keys[i]);
				attroff(A_BOLD | COLOR_PAIR(C_HKEY));
				printw("    ");
				if (trunc)
					printw("%.*s..", label_max - 2, labels[i]);
				else
					printw("%-*s", label_max, labels[i]);
				if (clen > 0) {
					attron(COLOR_PAIR(C_HSUB));
					printw(" %*s", clen, countbuf);
					attroff(COLOR_PAIR(C_HSUB));
				}
				printw("  ");
			}
		}

		if (nrecent > 0) {
			int ry = my + 5;
			int tw = mw - 6; /* title width after "  1  " */
			if (tw < 4) tw = 4;
			attron(A_BOLD | COLOR_PAIR(C_HEADER));
			mvprintw(ry, mx, "  recently played");
			attroff(A_BOLD | COLOR_PAIR(C_HEADER));
			ry++;
			for (i = 0; i < nrecent; i++) {
				int ridx = 4 + i;
				int playing = (nowplaying[0] &&
				    strcmp(recent[i].title, nowplaying) == 0);
				int tlen = (int)strlen(recent[i].title);
				mvprintw(ry + i, mx, "  ");
				attron(COLOR_PAIR(playing ? C_PLAYING : C_NUM));
				printw("%d", i + 1);
				attroff(COLOR_PAIR(playing ? C_PLAYING : C_NUM));
				printw("  ");
				if (sel == ridx) attron(A_BOLD | COLOR_PAIR(C_HSEL));
				if (tlen > tw)
					printw("%.*s..", tw - 2, recent[i].title);
				else
					printw("%-*s", tw, recent[i].title);
				if (sel == ridx) attroff(A_BOLD | COLOR_PAIR(C_HSEL));
			}
		}

		int fy = my + 5 + (nrecent > 0 ? nrecent + 2 : 1);
		mvprintw(fy, mx, "  ");
		attron(A_BOLD | COLOR_PAIR(C_HKEY));
		printw("?");
		attroff(A_BOLD | COLOR_PAIR(C_HKEY));
		printw("  help");
		/* right-justify "q  quit" */
		int qx = mx + mw - 8;
		move(fy, qx);
		attron(A_BOLD | COLOR_PAIR(C_HKEY));
		printw("q");
		attroff(A_BOLD | COLOR_PAIR(C_HKEY));
		printw("  quit  ");

	} else if (mode == MODE_HELP) {
		int y = 0;
		attron(A_BOLD | COLOR_PAIR(C_HEADER));
		mvprintw(y++, 0, " hum - help");
		attroff(A_BOLD | COLOR_PAIR(C_HEADER));
		y++;
		attron(A_BOLD | COLOR_PAIR(C_HEADER));
		mvprintw(y++, 0, " navigation");
		attroff(A_BOLD | COLOR_PAIR(C_HEADER));
		mvprintw(y++, 0, "   j/k          move down/up");
		mvprintw(y++, 0, "   g/G          jump to top/bottom");
		mvprintw(y++, 0, "   Esc /        search youtube");
		mvprintw(y++, 0, "   v            view queue");
		mvprintw(y++, 0, "   b            browse library");
		mvprintw(y++, 0, "   Esc p        open playlists");
		mvprintw(y++, 0, "   Esc / q      go back");
		mvprintw(y++, 0, "   ?            this help page");
		y++;
		attron(A_BOLD | COLOR_PAIR(C_HEADER));
		mvprintw(y++, 0, " playback");
		attroff(A_BOLD | COLOR_PAIR(C_HEADER));
		mvprintw(y++, 0, "   l / Enter    play selected");
		mvprintw(y++, 0, "   Space        pause/resume");
		mvprintw(y++, 0, "   n            next track");
		mvprintw(y++, 0, "   p            previous track");
		mvprintw(y++, 0, "   x            stop playback");
		mvprintw(y++, 0, "   ,  .         seek back/forward 5s");
		mvprintw(y++, 0, "   +  -         volume up/down");
		mvprintw(y++, 0, "   m            mute/unmute");
		mvprintw(y++, 0, "   r            cycle repeat (off/one/all)");
		y++;
		attron(A_BOLD | COLOR_PAIR(C_HEADER));
		mvprintw(y++, 0, " queue");
		attroff(A_BOLD | COLOR_PAIR(C_HEADER));
		mvprintw(y++, 0, "   a            add to queue");
		mvprintw(y++, 0, "   d            delete from queue");
		mvprintw(y++, 0, "   c            clear queue");
		mvprintw(y++, 0, "   J/K          move track down/up");
		mvprintw(y++, 0, "   S            shuffle queue");
		mvprintw(y++, 0, "   s            save queue as playlist");
		y++;
		attron(A_BOLD | COLOR_PAIR(C_HEADER));
		mvprintw(y++, 0, " playlists");
		attroff(A_BOLD | COLOR_PAIR(C_HEADER));
		mvprintw(y++, 0, "   l / Enter    enter playlist / play track");
		mvprintw(y++, 0, "   h            go back one level");
		mvprintw(y++, 0, "   A            add selected to playlist");
		mvprintw(y++, 0, "   d            delete playlist / track");
		mvprintw(y++, 0, "   R            rename playlist");
		mvprintw(y++, 0, "   D            download all results (batch)");
		y++;
		attron(A_BOLD | COLOR_PAIR(C_HEADER));
		mvprintw(y++, 0, " visual mode");
		attroff(A_BOLD | COLOR_PAIR(C_HEADER));
		mvprintw(y++, 0, "   V            toggle visual selection");
		mvprintw(y++, 0, "   a/A/d        act on selection");

	} else if (mode == MODE_QUEUE) {
		nfilt = 0;
		{
			int qi;
			for (qi = 0; qi < nqueue; qi++)
				if (filter_match(queue[qi].title))
					filt_idx[nfilt++] = qi;
		}

		attron(A_BOLD | COLOR_PAIR(C_HEADER));
		if (filter_len > 0)
			mvprintw(0, 0, " queue (%d/%d)", nfilt, nqueue);
		else
			mvprintw(0, 0, " queue (%d tracks)", nqueue);
		attroff(A_BOLD | COLOR_PAIR(C_HEADER));

		if (filtering) {
			attron(A_BOLD | COLOR_PAIR(C_SEARCH));
			mvprintw(1, 0, " /");
			attroff(A_BOLD | COLOR_PAIR(C_SEARCH));
			printw(" %s_", filter_buf);
		}

		int qoff = filtering ? 3 : 2;
		int qvis = rows - qoff - 3;
		if (qvis < 1) qvis = 1;
		if (sel >= nfilt) sel = nfilt > 0 ? nfilt - 1 : 0;

		for (i = 0; i < qvis && i + qscroll < nfilt; i++) {
			int fidx = i + qscroll;
			int idx = filt_idx[fidx];
			int playing = (idx == qpos && nowplaying[0]);
			int selected = (fidx == sel || (visual && fidx >= vsel_min() && fidx <= vsel_max()));
			if (playing) attron(COLOR_PAIR(C_PLAYING));
			mvprintw(i + qoff, 0, " %s", idx == qpos ? ">> " : "   ");
			if (playing) attroff(COLOR_PAIR(C_PLAYING));
			attron(COLOR_PAIR(playing ? C_PLAYING : C_NUM));
			printw("%2d", idx + 1);
			attroff(COLOR_PAIR(playing ? C_PLAYING : C_NUM));
			printw("  ");
			if (selected && visual) attron(COLOR_PAIR(C_VISUAL));
			else if (selected) attron(A_REVERSE);
			printw("%.*s", cols - 9, queue[idx].title);
			if (selected && visual) attroff(COLOR_PAIR(C_VISUAL));
			else if (selected) attroff(A_REVERSE);
		}
		if (nfilt == 0 && filter_len > 0)
			mvprintw(qoff, 0, " no matches");
		else if (nqueue == 0)
			mvprintw(qoff, 0, " empty");

	} else if (mode == MODE_LIBRARY) {
		/* build filtered index */
		nfilt = 0;
		{
			int li;
			for (li = 0; li < nlib; li++)
				if (filter_match(library[li].title))
					filt_idx[nfilt++] = li;
		}

		attron(A_BOLD | COLOR_PAIR(C_HEADER));
		if (filtering)
			mvprintw(0, 0, " library (%d/%d)", nfilt, nlib);
		else
			mvprintw(0, 0, " library (%d tracks)", nlib);
		attroff(A_BOLD | COLOR_PAIR(C_HEADER));

		if (filtering) {
			attron(A_BOLD | COLOR_PAIR(C_SEARCH));
			mvprintw(1, 0, " /");
			attroff(A_BOLD | COLOR_PAIR(C_SEARCH));
			printw(" %s_", filter_buf);
		}

		int loff = filtering ? 3 : 2;
		int lvis = rows - loff - 3;
		if (lvis < 1) lvis = 1;
		if (sel >= nfilt) sel = nfilt > 0 ? nfilt - 1 : 0;

		for (i = 0; i < lvis && i + libscroll < nfilt; i++) {
			int fidx = i + libscroll;
			int idx = filt_idx[fidx];
			int selected = (fidx == sel || (visual && fidx >= vsel_min() && fidx <= vsel_max()));
			int playing = (nowplaying[0] &&
			    strcmp(library[idx].title, nowplaying) == 0);
			mvprintw(i + loff, 0, " ");
			attron(COLOR_PAIR(playing ? C_PLAYING : C_NUM));
			printw("%2d", fidx + 1);
			attroff(COLOR_PAIR(playing ? C_PLAYING : C_NUM));
			printw("  ");
			if (selected && visual) attron(COLOR_PAIR(C_VISUAL));
			else if (selected) attron(A_REVERSE);
			printw("%.*s", cols - 6, library[idx].title);
			if (selected && visual) attroff(COLOR_PAIR(C_VISUAL));
			else if (selected) attroff(A_REVERSE);
		}
		if (nfilt == 0 && filtering)
			mvprintw(loff, 0, " no matches");
		else if (nlib == 0)
			mvprintw(loff, 0, " no tracks - play songs to build your library");

	} else if (mode == MODE_PLAYLIST) {
		if (pl_level == 0) {
			nfilt = 0;
			{
				int pi;
				for (pi = 0; pi < nplaylists; pi++)
					if (filter_match(plnames[pi]))
						filt_idx[nfilt++] = pi;
			}
			attron(A_BOLD | COLOR_PAIR(C_HEADER));
			if (filter_len > 0)
				mvprintw(0, 0, " playlists (%d/%d)", nfilt, nplaylists);
			else
				mvprintw(0, 0, " playlists (%d)", nplaylists);
			attroff(A_BOLD | COLOR_PAIR(C_HEADER));
			if (filtering) {
				attron(A_BOLD | COLOR_PAIR(C_SEARCH));
				mvprintw(1, 0, " /");
				attroff(A_BOLD | COLOR_PAIR(C_SEARCH));
				printw(" %s_", filter_buf);
			}
			int ploff = filtering ? 3 : 2;
			int plvis = rows - ploff - 3;
			if (plvis < 1) plvis = 1;
			if (sel >= nfilt) sel = nfilt > 0 ? nfilt - 1 : 0;
			for (i = 0; i < plvis && i + plscroll < nfilt; i++) {
				int fidx = i + plscroll;
				int idx = filt_idx[fidx];
				mvprintw(i + ploff, 0, " ");
				attron(COLOR_PAIR(C_NUM));
				printw("%2d", idx + 1);
				attroff(COLOR_PAIR(C_NUM));
				printw("  ");
				if (fidx == sel) attron(A_REVERSE);
				printw("%.*s", cols - 6, plnames[idx]);
				if (fidx == sel) attroff(A_REVERSE);
			}
			if (nfilt == 0 && filter_len > 0)
				mvprintw(ploff, 0, " no matches");
			else if (nplaylists == 0)
				mvprintw(ploff, 0, " no playlists - save a queue with 's'");
		} else {
			nfilt = 0;
			{
				int pi;
				for (pi = 0; pi < npl_tracks; pi++)
					if (filter_match(pl_tracks[pi].title))
						filt_idx[nfilt++] = pi;
			}
			attron(A_BOLD | COLOR_PAIR(C_HEADER));
			if (filter_len > 0)
				mvprintw(0, 0, " playlist (%d/%d)", nfilt, npl_tracks);
			else
				mvprintw(0, 0, " playlist (%d tracks)", npl_tracks);
			attroff(A_BOLD | COLOR_PAIR(C_HEADER));
			if (filtering) {
				attron(A_BOLD | COLOR_PAIR(C_SEARCH));
				mvprintw(1, 0, " /");
				attroff(A_BOLD | COLOR_PAIR(C_SEARCH));
				printw(" %s_", filter_buf);
			}
			int poff = filtering ? 3 : 2;
			int pvis = rows - poff - 3;
			if (pvis < 1) pvis = 1;
			if (sel >= nfilt) sel = nfilt > 0 ? nfilt - 1 : 0;
			for (i = 0; i < pvis && i + plscroll < nfilt; i++) {
				int fidx = i + plscroll;
				int idx = filt_idx[fidx];
				int selected = (fidx == sel || (visual && fidx >= vsel_min() && fidx <= vsel_max()));
				int playing = (nowplaying[0] &&
				    strcmp(pl_tracks[idx].title, nowplaying) == 0);
				mvprintw(i + poff, 0, " ");
				attron(COLOR_PAIR(playing ? C_PLAYING : C_NUM));
				printw("%2d", idx + 1);
				attroff(COLOR_PAIR(playing ? C_PLAYING : C_NUM));
				printw("  ");
				if (selected && visual) attron(COLOR_PAIR(C_VISUAL));
				else if (selected) attron(A_REVERSE);
				printw("%.*s", cols - 6, pl_tracks[idx].title);
				if (selected && visual) attroff(COLOR_PAIR(C_VISUAL));
				else if (selected) attroff(A_REVERSE);
			}
			if (nfilt == 0 && filter_len > 0)
				mvprintw(poff, 0, " no matches");
			else if (npl_tracks == 0)
				mvprintw(poff, 0, " empty playlist");
		}

	} else if (mode == MODE_PLADD) {
		attron(A_BOLD | COLOR_PAIR(C_HEADER));
		mvprintw(0, 0, " add to playlist:");
		attroff(A_BOLD | COLOR_PAIR(C_HEADER));
		for (i = 0; i < visible && i + pladd_scroll < nplaylists; i++) {
			int idx = i + pladd_scroll;
			mvprintw(i + 2, 0, " ");
			attron(COLOR_PAIR(C_NUM));
			printw("%2d", idx + 1);
			attroff(COLOR_PAIR(C_NUM));
			printw("  ");
			if (idx == sel) attron(A_REVERSE);
			printw("%.*s", cols - 6, plnames[idx]);
			if (idx == sel) attroff(A_REVERSE);
		}
		if (nplaylists == 0)
			mvprintw(2, 0, " no playlists");

	} else if (mode == MODE_CONFIRM) {
		attron(A_BOLD | COLOR_PAIR(C_MODE));
		mvprintw(0, 0, " %s", confirm_msg);
		attroff(A_BOLD | COLOR_PAIR(C_MODE));
		attron(COLOR_PAIR(C_PLAYING));
		mvprintw(2, 0, "   y");
		attroff(COLOR_PAIR(C_PLAYING));
		printw("  confirm");
		mvprintw(3, 0, "   n  cancel");

	} else if (mode == MODE_PLSAVE) {
		attron(A_BOLD | COLOR_PAIR(C_HEADER));
		mvprintw(0, 0, " save playlist:");
		attroff(A_BOLD | COLOR_PAIR(C_HEADER));
		printw(" %s_", input_buf);

	} else if (mode == MODE_PLRENAME) {
		attron(A_BOLD | COLOR_PAIR(C_HEADER));
		mvprintw(0, 0, " rename playlist:");
		attroff(A_BOLD | COLOR_PAIR(C_HEADER));
		printw(" %s_", input_buf);

	} else if (mode == MODE_BATCHNAME) {
		attron(A_BOLD | COLOR_PAIR(C_HEADER));
		mvprintw(0, 0, " playlist name:");
		attroff(A_BOLD | COLOR_PAIR(C_HEADER));
		printw(" %s_", input_buf);

	} else if (mode == MODE_DLNAME) {
		if (dl_batch_count > 0) {
			attron(A_BOLD | COLOR_PAIR(C_HEADER));
			mvprintw(0, 0, " save as (%d/%d):", dl_batch_idx + 1, dl_batch_count);
			attroff(A_BOLD | COLOR_PAIR(C_HEADER));
		} else {
			attron(A_BOLD | COLOR_PAIR(C_HEADER));
			mvprintw(0, 0, " save as:");
			attroff(A_BOLD | COLOR_PAIR(C_HEADER));
		}
		printw(" %s_", input_buf);

	} else {
		nfilt = 0;
		{
			int ri;
			for (ri = 0; ri < nresults; ri++)
				if (filter_match(results[ri].title))
					filt_idx[nfilt++] = ri;
		}

		/* search bar */
		attron(A_BOLD | COLOR_PAIR(C_SEARCH));
		mvprintw(0, 0, " /");
		attroff(A_BOLD | COLOR_PAIR(C_SEARCH));
		printw(" %s", query);
		if (mode == MODE_SEARCH)
			addch('_');

		int boff = 2;
		if (filtering) {
			attron(A_BOLD | COLOR_PAIR(C_SEARCH));
			mvprintw(1, 0, " filter:");
			attroff(A_BOLD | COLOR_PAIR(C_SEARCH));
			printw(" %s_", filter_buf);
			boff = 3;
		}
		int bvis = rows - boff - 3;
		if (bvis < 1) bvis = 1;
		if (sel >= nfilt) sel = nfilt > 0 ? nfilt - 1 : 0;

		for (i = 0; i < bvis && i + rscroll < nfilt; i++) {
			int fidx = i + rscroll;
			int idx = filt_idx[fidx];
			int selected = ((fidx == sel && mode == MODE_BROWSE) ||
			    (visual && fidx >= vsel_min() && fidx <= vsel_max()));
			int playing = (nowplaying[0] &&
			    strcmp(results[idx].title, nowplaying) == 0);
			mvprintw(i + boff, 0, " ");
			attron(COLOR_PAIR(playing ? C_PLAYING : C_NUM));
			printw("%2d", idx + 1);
			attroff(COLOR_PAIR(playing ? C_PLAYING : C_NUM));
			attron(COLOR_PAIR(results[idx].is_playlist ? C_PLMARK : C_SMARK) | A_BOLD);
			printw(" %c ", results[idx].is_playlist ? 'p' : 's');
			attroff(COLOR_PAIR(results[idx].is_playlist ? C_PLMARK : C_SMARK) | A_BOLD);
			if (selected && visual) attron(COLOR_PAIR(C_VISUAL));
			else if (selected) attron(A_REVERSE);
			printw("%.*s", cols - 12, results[idx].title);
			if (selected && visual) attroff(COLOR_PAIR(C_VISUAL));
			else if (selected) attroff(A_REVERSE);
			if (!results[idx].is_playlist && lib_has(results[idx].title)) {
				attron(COLOR_PAIR(C_DIM) | A_BOLD);
				printw(" *");
				attroff(COLOR_PAIR(C_DIM) | A_BOLD);
			}
		}

		/* "download all" button for expanded playlists */
		if (search_is_url && nfilt > 0 && !searching && filter_len == 0 &&
		    i + rscroll >= nfilt && i + boff < rows - 2) {
			if (sel == nfilt && mode == MODE_BROWSE)
				attron(A_REVERSE);
			attron(COLOR_PAIR(C_PLAYING));
			mvprintw(i + boff + 1, 0, "     >> download all (%d tracks)", nresults);
			attroff(COLOR_PAIR(C_PLAYING));
			if (sel == nfilt && mode == MODE_BROWSE)
				attroff(A_REVERSE);
		}

		if (searching && nfilt == 0)
			mvprintw(boff, 0, " searching...");
		else if (nfilt == 0 && filter_len > 0)
			mvprintw(boff, 0, " no matches");
		else if (nresults == 0 && query[0] && mode == MODE_BROWSE && !searching)
			mvprintw(boff, 0, " no results");
	}

	/* status indicators on row before now-playing */
	{
		const char *rep = "";
		if (repeat_mode == REP_ONE) rep = "[rep:1]";
		else if (repeat_mode == REP_ALL) rep = "[rep:all]";
		if (visual) {
			int n = vsel_max() - vsel_min() + 1;
			attron(A_BOLD | COLOR_PAIR(C_MODE));
			mvprintw(rows - 3, 0, " -- VISUAL (%d) --", n);
			attroff(A_BOLD | COLOR_PAIR(C_MODE));
			if (rep[0]) {
				attron(COLOR_PAIR(C_STATUS));
				printw(" %s", rep);
				attroff(COLOR_PAIR(C_STATUS));
			}
		} else if (rep[0]) {
			attron(COLOR_PAIR(C_MODE));
			mvprintw(rows - 3, 0, " %s", rep);
			attroff(COLOR_PAIR(C_MODE));
		}
		if (status_ticks > 0 && status_msg[0]) {
			attron(COLOR_PAIR(C_PLAYING));
			mvprintw(rows - 3, cols - (int)strlen(status_msg) - 2,
			    " %s ", status_msg);
			attroff(COLOR_PAIR(C_PLAYING));
		}
	}

	/* now playing + progress bar */
	if (nowplaying[0]) {
		int pm, ps, dm, ds, barw, filled, bi;
		char timebuf[32];

		pm = (int)cur_pos / 60;
		ps = (int)cur_pos % 60;
		dm = (int)cur_dur / 60;
		ds = (int)cur_dur % 60;
		snprintf(timebuf, sizeof(timebuf), " %d:%02d/%d:%02d ",
		    pm, ps, dm, ds);

		attron(COLOR_PAIR(C_PLAYING));
		mvprintw(rows - 2, 0, " %s", paused ? "||" : ">>");
		attroff(COLOR_PAIR(C_PLAYING));
		attron(A_BOLD);
		printw(" %.*s", cols - 5, nowplaying);
		attroff(A_BOLD);

		attron(COLOR_PAIR(C_NUM));
		mvprintw(rows - 1, 0, "%s", timebuf);
		attroff(COLOR_PAIR(C_NUM));
		barw = cols - (int)strlen(timebuf);
		if (barw < 4) barw = 4;
		filled = (cur_dur > 0) ? (int)(cur_pos / cur_dur * barw) : 0;
		if (filled > barw) filled = barw;

		for (bi = 0; bi < barw; bi++) {
			if (bi < filled) {
				attron(COLOR_PAIR(C_BAR));
				addch(ACS_BLOCK);
				attroff(COLOR_PAIR(C_BAR));
			} else {
				attron(COLOR_PAIR(C_NUM));
				addch(ACS_HLINE);
				attroff(COLOR_PAIR(C_NUM));
			}
		}
	}

	refresh();
}

/* ---- scroll helper ---- */

static void
scroll_into_view(int *scroll)
{
	int rows, cols, visible;
	getmaxyx(stdscr, rows, cols);
	(void)cols;
	visible = rows - 5;
	if (visible < 1) visible = 1;
	if (sel < *scroll)
		*scroll = sel;
	if (sel >= *scroll + visible)
		*scroll = sel - visible + 1;
}

static int *
cur_scroll(void)
{
	if (mode == MODE_QUEUE) return &qscroll;
	if (mode == MODE_LIBRARY) return &libscroll;
	if (mode == MODE_PLAYLIST) return &plscroll;
	if (mode == MODE_BROWSE) return &rscroll;
	if (mode == MODE_PLADD) return &pladd_scroll;
	return NULL;
}

/* ---- input mode handler (shared for search, plsave, plrename) ---- */

static int
handle_text_input(int ch, char *buf, int *len, int maxlen)
{
	if (ch == '\n' || ch == KEY_ENTER)
		return 1; /* submit */
	if (ch == 27)
		return -1; /* cancel */
	if (ch == KEY_BACKSPACE || ch == 127) {
		if (*len > 0)
			buf[--(*len)] = '\0';
	} else if (*len < maxlen - 1 && ch >= 32 && ch < 127) {
		buf[(*len)++] = ch;
		buf[*len] = '\0';
	}
	return 0; /* continue */
}

/* ---- main ---- */

int
main(int argc, char *argv[])
{
	int ch;
	int start_mode = MODE_HOME;

	if (argc > 1) {
		if (strcmp(argv[1], "--help") == 0 || strcmp(argv[1], "-h") == 0) {
			printf("hum - a minimal TUI music player\n\n");
			printf("usage: hum [option]\n\n");
			printf("options:\n");
			printf("  -p, --playlists   open playlists\n");
			printf("  -b, --browse      browse library\n");
			printf("  -s, --search      search youtube\n");
			printf("  -q, --queue       view queue\n");
			printf("  -h, --help        show this help\n\n");
			printf("keys:\n");
			printf("  j/k         move down/up\n");
			printf("  l/Enter     play selected\n");
			printf("  Space       pause/resume\n");
			printf("  n/p         next/previous track\n");
			printf("  Esc /       search YouTube\n");
			printf("  Esc p       open playlists\n");
			printf("  v           view queue\n");
			printf("  b           browse library\n");
			printf("  a           add to queue\n");
			printf("  ?           help (in app)\n\n");
			printf("config: edit config.h and rebuild\n");
			printf("library: %s\n", lib_dir);
			return 0;
		} else if (strcmp(argv[1], "-p") == 0 ||
		    strcmp(argv[1], "--playlists") == 0) {
			start_mode = MODE_PLAYLIST;
		} else if (strcmp(argv[1], "-b") == 0 ||
		    strcmp(argv[1], "--browse") == 0) {
			start_mode = MODE_LIBRARY;
		} else if (strcmp(argv[1], "-s") == 0 ||
		    strcmp(argv[1], "--search") == 0) {
			start_mode = MODE_SEARCH;
		} else if (strcmp(argv[1], "-q") == 0 ||
		    strcmp(argv[1], "--queue") == 0) {
			start_mode = MODE_QUEUE;
		}
	}

	srand(time(NULL));
	snprintf(sock_path, sizeof(sock_path),
	    "/tmp/hum-mpv-%d.sock", getpid());
	resolve_libpath();
	ensure_dir(libpath);
	resolve_plspath();
	ensure_dir(plspath);

	initscr();
	set_escdelay(25);
	cbreak();
	noecho();
	keypad(stdscr, TRUE);
	curs_set(0);
	timeout(200);

	if (has_colors()) {
		start_color();
		use_default_colors();
		init_pair(C_HEADER,  col_header_fg,  col_header_bg);
		init_pair(C_NUM,     col_num_fg,     col_num_bg);
		init_pair(C_PLAYING, col_playing_fg, col_playing_bg);
		init_pair(C_VISUAL,  col_visual_fg,  col_visual_bg);
		init_pair(C_STATUS,  col_status_fg,  col_status_bg);
		init_pair(C_BAR,     col_bar_fg,     col_bar_bg);
		init_pair(C_SEARCH,  col_search_fg,  col_search_bg);
		init_pair(C_MODE,    col_mode_fg,    col_mode_bg);
		init_pair(C_DIM,     col_dim_fg,     col_dim_bg);
		init_pair(C_SMARK,   col_smark_fg,  col_smark_bg);
		init_pair(C_HTITLE,  col_home_title_fg, col_home_title_bg);
		init_pair(C_HSUB,    col_home_sub_fg,   col_home_sub_bg);
		init_pair(C_HKEY,    col_home_key_fg,   col_home_key_bg);
		init_pair(C_HSEL,    col_home_sel_fg,   col_home_sel_bg);
		init_pair(C_PLMARK,  col_plmark_fg, col_plmark_bg);
	}

	atexit(cleanup);

	pl_scan();
	lib_scan();
	history_load();
	history_trim();
	queue_load();
	mode = start_mode;
	sel = 0;
	if (mode == MODE_LIBRARY) {
		lib_scan();
		libscroll = 0;
	} else if (mode == MODE_PLAYLIST) {
		pl_level = 0;
		plscroll = 0;
	} else if (mode == MODE_QUEUE) {
		qscroll = 0;
	} else if (mode == MODE_SEARCH) {
		qlen = 0;
		query[0] = '\0';
	}
	draw();

	for (;;) {
		ch = getch();

		if (ch == ERR) {
			if (mpv_pid > 0 && waitpid(mpv_pid, NULL, WNOHANG) > 0) {
				mpv_pid = -1;
				nowplaying[0] = '\0';
				cur_pos = cur_dur = 0;
				play_next();
			}
			waitpid(-1, NULL, WNOHANG);
			mpv_get_progress();
			search_poll();
			if (status_ticks > 0) status_ticks--;
			draw();
			continue;
		}

		/* ---- text input modes ---- */

		if (mode == MODE_SEARCH) {
			int r = handle_text_input(ch, query, &qlen, MAX_QUERY);
			if (r == 1 && qlen > 0) {
				mode = MODE_BROWSE;
				search_start(query);
			} else if (r == -1) {
				mode = MODE_HOME;
				sel = home_sel;
			}
			draw();
			continue;
		}

		if (mode == MODE_PLSAVE) {
			int r = handle_text_input(ch, input_buf, &input_len, 128);
			if (r == 1 && input_len > 0) {
				pl_save(input_buf);
				status_set("playlist saved");
				mode = MODE_QUEUE;
			} else if (r == -1) {
				mode = MODE_QUEUE;
			}
			draw();
			continue;
		}

		if (mode == MODE_BATCHNAME) {
			int r = handle_text_input(ch, input_buf, &input_len, 128);
			if (r == 1 && input_len > 0) {
				snprintf(dl_batch_plname, sizeof(dl_batch_plname), "%s", input_buf);
				/* start naming tracks */
				dl_batch_idx = 0;
				snprintf(input_buf, sizeof(input_buf), "%s",
				    dl_batch[0].title);
				input_len = strlen(input_buf);
				mode = MODE_DLNAME;
			} else if (r == -1) {
				dl_batch_count = 0;
				mode = MODE_BROWSE;
			}
			draw();
			continue;
		}

		if (mode == MODE_DLNAME) {
			int r = handle_text_input(ch, input_buf, &input_len, 128);
			if (dl_batch_count > 0 && (r == 1 || r == -1)) {
				/* batch mode: Enter confirms name, Esc keeps default */
				const char *name = (r == 1 && input_len > 0) ?
				    input_buf : dl_batch[dl_batch_idx].title;

				snprintf(dl_names[dl_batch_idx], MAX_TITLE, "%s", name);
				snprintf(dl_batch[dl_batch_idx].title, MAX_TITLE,
				    "%s", name);
				lib_download(dl_batch[dl_batch_idx].url, name);

				if (dl_batch_idx == 0) {
					queue_add(&dl_batch[0]);
					qpos = nqueue - 1;
					mpv_play(dl_batch[0].url, dl_batch[0].title);
				} else {
					queue_add(&dl_batch[dl_batch_idx]);
				}

				dl_batch_idx++;
				if (dl_batch_idx < dl_batch_count) {
					snprintf(input_buf, sizeof(input_buf), "%s",
					    dl_batch[dl_batch_idx].title);
					input_len = strlen(input_buf);
				} else {
					char plpath[2048];
					FILE *fp;
					int j;
					snprintf(plpath, sizeof(plpath), "%s/%s.hum",
					    plspath, dl_batch_plname);
					fp = fopen(plpath, "w");
					if (fp) {
						for (j = 0; j < dl_batch_count; j++)
							fprintf(fp, "%s\t%s\n",
							    dl_names[j],
							    dl_batch[j].url);
						fclose(fp);
					}
					status_set("playlist created");
					dl_batch_count = 0;
					mode = MODE_BROWSE;
				}
			} else if (r == 1) {
				/* single track mode */
				const char *name = (input_len > 0) ?
				    input_buf : dl_default;
				lib_download(dl_url, name);
				status_set("downloading");
				mode = dl_retmode;
			} else if (r == -1) {
				mode = dl_retmode;
			}
			draw();
			continue;
		}

		if (mode == MODE_PLRENAME) {
			int r = handle_text_input(ch, input_buf, &input_len, 128);
			if (r == 1 && input_len > 0) {
				pl_rename(plrename_idx, input_buf);
				status_set("playlist renamed");
				mode = MODE_PLAYLIST;
			} else if (r == -1) {
				mode = MODE_PLAYLIST;
			}
			draw();
			continue;
		}

		if (filtering) {
			int r = handle_text_input(ch, filter_buf, &filter_len,
			    MAX_QUERY);
			if (r == 1) {
				filtering = 0;
			} else if (r == -1) {
				filtering = 0;
				filter_len = 0;
				filter_buf[0] = '\0';
			}
			sel = 0;
			int *s = cur_scroll();
			if (s) *s = 0;
			draw();
			continue;
		}

		if (mode == MODE_HOME) {
			int handled = 1;
			int old_sel = sel;
			int home_max = 3 + (nrecent > 0 ? nrecent : 0);
			if (ch == key_up || ch == KEY_UP) {
				if (sel > 0) sel--;
			} else if (ch == key_down || ch == KEY_DOWN) {
				if (sel < home_max) sel++;
			} else if (ch == '\n' || ch == KEY_ENTER || ch == 'l') {
				if (sel == 0) {
					mode = MODE_PLAYLIST;
					pl_scan();
					pl_level = 0;
					sel = 0;
					plscroll = 0;
				} else if (sel == 1) {
					mode = MODE_LIBRARY;
					lib_scan();
					sel = 0;
					libscroll = 0;
				} else if (sel == 2) {
					mode = MODE_SEARCH;
					qlen = 0;
					query[0] = '\0';
				} else if (sel == 3) {
					mode = MODE_QUEUE;
					sel = qpos >= 0 ? qpos : 0;
					qscroll = 0;
				} else if (sel >= 4 && sel < 4 + nrecent) {
					int ri = sel - 4;
					queue_add(&recent[ri]);
					qpos = nqueue - 1;
					mpv_play(recent[ri].url, recent[ri].title);
				}
			} else if (ch == '/') {
				mode = MODE_SEARCH;
				qlen = 0;
				query[0] = '\0';
			} else if (ch == 'p') {
				mode = MODE_PLAYLIST;
				pl_scan();
				pl_level = 0;
				sel = 0;
				plscroll = 0;
			} else if (ch == key_lib) {
				mode = MODE_LIBRARY;
				lib_scan();
				sel = 0;
				libscroll = 0;
			} else if (ch == key_qview) {
				mode = MODE_QUEUE;
				sel = qpos >= 0 ? qpos : 0;
				qscroll = 0;
			} else if (ch == '?') {
				mode = MODE_HELP;
			} else if (ch == key_quit) {
				break;
			} else {
				handled = 0;
			}
			if (handled) {
				if (mode != MODE_HOME)
					home_sel = old_sel;
				draw();
				continue;
			}
			/* fall through for playback keys */
		}

		if (mode == MODE_HELP) {
			mode = MODE_HOME;
			sel = home_sel;
			visual = 0;
			draw();
			continue;
		}

		if (mode == MODE_CONFIRM) {
			if (ch == 'y' || ch == 'Y') {
				if (confirm_action)
					confirm_action();
			}
			mode = confirm_ret;
			draw();
			continue;
		}

		/* ---- add-to-playlist picker ---- */

		if (mode == MODE_PLADD) {
			if (ch == key_up || ch == KEY_UP) {
				if (sel > 0) sel--;
				scroll_into_view(&pladd_scroll);
			} else if (ch == key_down || ch == KEY_DOWN) {
				if (sel < nplaylists - 1) sel++;
				scroll_into_view(&pladd_scroll);
			} else if (ch == '\n' || ch == KEY_ENTER || ch == 'l') {
				if (sel >= 0 && sel < nplaylists) {
					pl_append_tracks(sel, pladd_tracks, npladd);
					status_set("added to playlist");
					mode = pladd_ret;
					visual = 0;
				}
			} else if (ch == 27 || ch == 'h' || ch == key_quit) {
				mode = pladd_ret;
				visual = 0;
			}
			draw();
			continue;
		}

		/* ---- global playback keys (work in all normal modes) ---- */

		if (ch == key_pause) {
			mpv_cmd("{\"command\":[\"cycle\",\"pause\"]}");
			paused = !paused;
			draw();
			continue;
		}
		if (ch == key_next) {
			play_next();
			draw();
			continue;
		}
		if (ch == key_prev) {
			play_prev();
			draw();
			continue;
		}
		if (ch == key_stop) {
			mpv_stop();
			nowplaying[0] = '\0';
			cur_pos = cur_dur = 0;
			paused = 0;
			draw();
			continue;
		}
		if (ch == key_seek_fwd) {
			char cmd[64];
			snprintf(cmd, sizeof(cmd),
			    "{\"command\":[\"seek\",\"%d\"]}", seek_step);
			mpv_cmd(cmd);
			draw();
			continue;
		}
		if (ch == key_seek_bwd) {
			char cmd[64];
			snprintf(cmd, sizeof(cmd),
			    "{\"command\":[\"seek\",\"-%d\"]}", seek_step);
			mpv_cmd(cmd);
			draw();
			continue;
		}
		if (ch == key_vol_up) {
			char cmd[64];
			snprintf(cmd, sizeof(cmd),
			    "{\"command\":[\"add\",\"volume\",%d]}", vol_step);
			mpv_cmd(cmd);
			draw();
			continue;
		}
		if (ch == key_vol_dn) {
			char cmd[64];
			snprintf(cmd, sizeof(cmd),
			    "{\"command\":[\"add\",\"volume\",%d]}", -vol_step);
			mpv_cmd(cmd);
			draw();
			continue;
		}
		if (ch == key_repeat) {
			repeat_mode = (repeat_mode + 1) % 3;
			draw();
			continue;
		}
		if (ch == 'm') {
			mpv_cmd("{\"command\":[\"cycle\",\"mute\"]}");
			draw();
			continue;
		}

		/* ---- navigation (shared) ---- */

		if (ch == key_up || ch == KEY_UP) {
			if (sel > 0) sel--;
			int *s = cur_scroll();
			if (s) scroll_into_view(s);
			draw();
			continue;
		}
		if (ch == key_down || ch == KEY_DOWN) {
			if (sel < list_len() - 1) sel++;
			int *s = cur_scroll();
			if (s) scroll_into_view(s);
			draw();
			continue;
		}
		if (ch == key_top) {
			sel = 0;
			int *s = cur_scroll();
			if (s) *s = 0;
			draw();
			continue;
		}
		if (ch == key_bottom) {
			sel = list_len() - 1;
			if (sel < 0) sel = 0;
			int *s = cur_scroll();
			if (s) scroll_into_view(s);
			draw();
			continue;
		}
		if (ch == key_visual) {
			if (visual) {
				visual = 0;
			} else {
				visual = 1;
				vsel_start = sel;
			}
			draw();
			continue;
		}

		/* ---- shared Esc sequences ---- */

		if (ch == 27 && mode != MODE_BROWSE) {
			int next = getch();
			if (next == '/') {
				mode = MODE_SEARCH;
				qlen = 0;
				query[0] = '\0';
				visual = 0;
				draw();
				continue;
			} else if (next == 'p') {
				mode = MODE_PLAYLIST;
				pl_scan();
				pl_level = 0;
				sel = 0;
				plscroll = 0;
				visual = 0;
				draw();
				continue;
			} else if (next != ERR) {
				ungetch(next);
			}
			/* fall through - mode-specific Esc/q handling below */
		}

		/* ---- shared mode switching ---- */

		if (ch == key_qview && mode != MODE_QUEUE && mode != MODE_BROWSE) {
			mode = MODE_QUEUE;
			sel = qpos >= 0 ? qpos : 0;
			qscroll = 0;
			visual = 0;
			draw();
			continue;
		}
		if (ch == key_lib && mode != MODE_LIBRARY && mode != MODE_BROWSE) {
			mode = MODE_LIBRARY;
			lib_scan();
			sel = 0;
			libscroll = 0;
			visual = 0;
			draw();
			continue;
		}
		if (ch == '?' && mode != MODE_BROWSE) {
			mode = MODE_HELP;
			visual = 0;
			draw();
			continue;
		}

		/* ---- mode-specific keys ---- */

		if (mode == MODE_PLAYLIST) {
			if (ch == 'l' || ch == '\n' || ch == KEY_ENTER) {
				if (pl_level == 0 && sel >= 0 && sel < nfilt) {
					int idx = filt_idx[sel];
					filter_len = 0;
					filter_buf[0] = '\0';
					pl_load(idx);
					pl_level = 1;
					sel = 0;
					plscroll = 0;
					visual = 0;
				} else if (pl_level == 1 && sel >= 0 && sel < nfilt) {
					int j;
					nqueue = 0;
					for (j = 0; j < npl_tracks; j++)
						queue_add(&pl_tracks[j]);
					qpos = filt_idx[sel];
					mpv_play(queue[qpos].url, queue[qpos].title);
					visual = 0;
				}
			} else if (ch == 'h') {
				if (pl_level == 1) {
					filter_len = 0;
					filter_buf[0] = '\0';
					pl_level = 0;
					sel = pl_cur;
					plscroll = 0;
					visual = 0;
				} else {
					mode = MODE_BROWSE;
					sel = 0;
					visual = 0;
				}
			} else if (ch == key_queue) {
				if (pl_level == 1) {
					if (visual) {
						int lo = vsel_min(), hi = vsel_max(), j;
						for (j = lo; j <= hi && j < nfilt; j++)
							queue_add(&pl_tracks[filt_idx[j]]);
						status_set("added to queue");
						visual = 0;
					} else if (sel >= 0 && sel < nfilt) {
						queue_add(&pl_tracks[filt_idx[sel]]);
						status_set("added to queue");
					}
				}
			} else if (ch == key_del) {
				if (pl_level == 0 && sel >= 0 && sel < nfilt) {
					int idx = filt_idx[sel];
					snprintf(confirm_msg, sizeof(confirm_msg),
					    "delete playlist '%s'?", plnames[idx]);
					confirm_arg = idx;
					confirm_action = do_pl_delete;
					confirm_ret = MODE_PLAYLIST;
					mode = MODE_CONFIRM;
				} else if (pl_level == 1) {
					if (visual) {
						int lo = vsel_min(), hi = vsel_max(), j;
						for (j = hi; j >= lo; j--)
							pl_del_track(filt_idx[j]);
						visual = 0;
					} else if (sel >= 0 && sel < nfilt) {
						pl_del_track(filt_idx[sel]);
					}
				}
			} else if (ch == key_rename) {
				if (pl_level == 0 && sel >= 0 && sel < nfilt) {
					int idx = filt_idx[sel];
					plrename_idx = idx;
					input_len = 0;
					input_buf[0] = '\0';
					snprintf(input_buf, 128, "%s", plnames[idx]);
					input_len = strlen(input_buf);
					mode = MODE_PLRENAME;
				}
			} else if (ch == '/') {
				filtering = 1;
				filter_len = 0;
				filter_buf[0] = '\0';
				sel = 0;
				plscroll = 0;
			} else if (ch == key_quit || ch == 27) {
				if (filter_len > 0) {
					filter_len = 0;
					filter_buf[0] = '\0';
					sel = 0;
					plscroll = 0;
				} else if (pl_level == 1) {
					pl_level = 0;
					sel = pl_cur;
					plscroll = 0;
				} else {
					mode = MODE_HOME;
					sel = home_sel;
				}
				visual = 0;
			}

		} else if (mode == MODE_QUEUE) {
			if (ch == '\n' || ch == KEY_ENTER || ch == key_play) {
				if (sel >= 0 && sel < nfilt) {
					qpos = filt_idx[sel];
					mpv_play(queue[qpos].url, queue[qpos].title);
					lib_download(queue[qpos].url, queue[qpos].title);
				}
				visual = 0;
			} else if (ch == key_del) {
				if (visual) {
					int lo = vsel_min(), hi = vsel_max(), j;
					for (j = hi; j >= lo; j--)
						queue_del(filt_idx[j]);
					visual = 0;
				} else if (sel >= 0 && sel < nfilt) {
					queue_del(filt_idx[sel]);
				}
			} else if (ch == key_clear) {
				snprintf(confirm_msg, sizeof(confirm_msg),
				    "clear queue (%d tracks)?", nqueue);
				confirm_action = do_queue_clear;
				confirm_ret = MODE_QUEUE;
				mode = MODE_CONFIRM;
				visual = 0;
			} else if (ch == key_move_up) {
				if (filter_len == 0 && sel > 0) {
					queue_move(sel, sel - 1);
					sel--;
					scroll_into_view(&qscroll);
				}
			} else if (ch == key_move_dn) {
				if (filter_len == 0 && sel < nqueue - 1) {
					queue_move(sel, sel + 1);
					sel++;
					scroll_into_view(&qscroll);
				}
			} else if (ch == key_shuffle) {
				queue_shuffle();
				status_set("queue shuffled");
				visual = 0;
			} else if (ch == key_plsave) {
				if (nqueue > 0) {
					input_len = 0;
					input_buf[0] = '\0';
					mode = MODE_PLSAVE;
				}
			} else if (ch == key_addtopl) {
				if (nplaylists > 0) {
					if (visual) {
						int lo = vsel_min(), hi = vsel_max(), j;
						npladd = 0;
						for (j = lo; j <= hi && j < nfilt; j++)
							pladd_tracks[npladd++] = queue[filt_idx[j]];
					} else if (sel >= 0 && sel < nfilt) {
						npladd = 1;
						pladd_tracks[0] = queue[filt_idx[sel]];
					}
					if (npladd > 0) {
						pl_scan();
						pladd_ret = MODE_QUEUE;
						mode = MODE_PLADD;
						sel = 0;
						pladd_scroll = 0;
					}
				}
			} else if (ch == '/') {
				filtering = 1;
				filter_len = 0;
				filter_buf[0] = '\0';
				sel = 0;
				qscroll = 0;
			} else if (ch == key_quit || ch == 27) {
				filter_len = 0;
				filter_buf[0] = '\0';
				mode = MODE_HOME;
				sel = home_sel;
				visual = 0;
			}

		} else if (mode == MODE_LIBRARY) {
			if (ch == '/') {
				filtering = 1;
				filter_len = 0;
				filter_buf[0] = '\0';
				sel = 0;
				libscroll = 0;
			} else if (ch == '\n' || ch == KEY_ENTER || ch == key_play) {
				play_selected();
				visual = 0;
			} else if (ch == key_queue) {
				if (visual) {
					int lo = vsel_min(), hi = vsel_max(), j;
					for (j = lo; j <= hi && j < nfilt; j++)
						queue_add(&library[filt_idx[j]]);
					status_set("added to queue");
					visual = 0;
				} else if (sel >= 0 && sel < nfilt) {
					queue_add(&library[filt_idx[sel]]);
					status_set("added to queue");
				}
			} else if (ch == key_del) {
				if (sel >= 0 && sel < nfilt) {
					int idx = filt_idx[sel];
					snprintf(confirm_msg, sizeof(confirm_msg),
					    "delete '%s' from library?", library[idx].title);
					confirm_arg = idx;
					confirm_action = do_lib_delete;
					confirm_ret = MODE_LIBRARY;
					mode = MODE_CONFIRM;
				}
			} else if (ch == key_addtopl) {
				if (nplaylists > 0) {
					if (visual) {
						int lo = vsel_min(), hi = vsel_max(), j;
						npladd = 0;
						for (j = lo; j <= hi && j < nfilt; j++)
							pladd_tracks[npladd++] = library[filt_idx[j]];
					} else if (sel >= 0 && sel < nfilt) {
						npladd = 1;
						pladd_tracks[0] = library[filt_idx[sel]];
					}
					if (npladd > 0) {
						pl_scan();
						pladd_ret = MODE_LIBRARY;
						mode = MODE_PLADD;
						sel = 0;
						pladd_scroll = 0;
					}
				}
			} else if (ch == key_quit || ch == 27) {
				filtering = 0;
				filter_len = 0;
				filter_buf[0] = '\0';
				mode = MODE_HOME;
				sel = home_sel;
				visual = 0;
			}

		} else {
			/* MODE_BROWSE */
			if (ch == '/') {
				filtering = 1;
				filter_len = 0;
				filter_buf[0] = '\0';
				sel = 0;
				rscroll = 0;
			} else if (ch == key_quit) {
				if (filter_len > 0) {
					filter_len = 0;
					filter_buf[0] = '\0';
					sel = 0;
					rscroll = 0;
				} else {
					search_cancel();
					mode = MODE_SEARCH;
					qlen = 0;
					query[0] = '\0';
					visual = 0;
				}
			} else if (ch == '\n' || ch == KEY_ENTER || ch == key_play) {
				/* "download all" button */
				if (search_is_url && sel == nfilt && nfilt > 0 && filter_len == 0) {
					int j;
					dl_batch_count = 0;
					for (j = 0; j < nresults; j++) {
						if (!results[j].is_playlist)
							dl_batch[dl_batch_count++] = results[j];
					}
					if (dl_batch_count > 0) {
						input_len = 0;
						input_buf[0] = '\0';
						mode = MODE_BATCHNAME;
					}
				} else {
					play_selected();
				}
				visual = 0;
			} else if (ch == key_queue) {
				if (visual) {
					int lo = vsel_min(), hi = vsel_max(), j;
					for (j = lo; j <= hi && j < nfilt; j++)
						queue_add(&results[filt_idx[j]]);
					status_set("added to queue");
					visual = 0;
				} else if (sel >= 0 && sel < nfilt) {
					queue_add(&results[filt_idx[sel]]);
					status_set("added to queue");
				}
			} else if (ch == key_addtopl) {
				if (nplaylists > 0 && nfilt > 0) {
					if (visual) {
						int lo = vsel_min(), hi = vsel_max(), j;
						npladd = 0;
						for (j = lo; j <= hi && j < nfilt; j++)
							pladd_tracks[npladd++] = results[filt_idx[j]];
					} else if (sel >= 0 && sel < nfilt) {
						npladd = 1;
						pladd_tracks[0] = results[filt_idx[sel]];
					}
					if (npladd > 0) {
						pl_scan();
						pladd_ret = MODE_BROWSE;
						mode = MODE_PLADD;
						sel = 0;
						pladd_scroll = 0;
					}
				}
			} else if (ch == 'D') {
				if (nresults > 0 && !searching) {
					int j;
					dl_batch_count = 0;
					for (j = 0; j < nresults; j++) {
						if (!results[j].is_playlist)
							dl_batch[dl_batch_count++] = results[j];
					}
					if (dl_batch_count > 0) {
						input_len = 0;
						input_buf[0] = '\0';
						mode = MODE_BATCHNAME;
					}
				}
			} else if (ch == key_qview) {
				mode = MODE_QUEUE;
				sel = qpos >= 0 ? qpos : 0;
				qscroll = 0;
				visual = 0;
			} else if (ch == key_lib) {
				mode = MODE_LIBRARY;
				lib_scan();
				sel = 0;
				libscroll = 0;
				visual = 0;
			} else if (ch == '?') {
				mode = MODE_HELP;
				visual = 0;
			} else if (ch == 27) {
				int next = getch();
				if (next == '/') {
					filter_len = 0;
					filter_buf[0] = '\0';
					search_cancel();
					mode = MODE_SEARCH;
					qlen = 0;
					query[0] = '\0';
					visual = 0;
				} else if (next == 'p') {
					filter_len = 0;
					filter_buf[0] = '\0';
					mode = MODE_PLAYLIST;
					pl_scan();
					pl_level = 0;
					sel = 0;
					plscroll = 0;
					visual = 0;
				} else if (next == ERR) {
					/* standalone Esc */
					if (filter_len > 0) {
						filter_len = 0;
						filter_buf[0] = '\0';
						sel = 0;
						rscroll = 0;
					} else {
						search_cancel();
						mode = MODE_SEARCH;
						qlen = 0;
						query[0] = '\0';
						visual = 0;
					}
				} else {
					ungetch(next);
				}
			}
		}

		/* if browse has nothing to show and no query, go home */
		if (mode == MODE_BROWSE && nresults == 0 && !searching && !query[0]) {
			mode = MODE_HOME;
			sel = home_sel;
		}

		draw();
	}
	return 0;
}
