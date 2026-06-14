/* hum - config */

/* keybinds */
static const int key_quit     = 'q';
static const int key_up       = 'k';
static const int key_down     = 'j';
static const int key_play     = 'l';
static const int key_pause    = ' ';
static const int key_prev     = 'p';
static const int key_next     = 'n';
static const int key_queue    = 'a';
static const int key_vol_up   = '+';
static const int key_vol_dn   = '-';
static const int key_qview    = 'v';
static const int key_lib      = 'b';
static const int key_seek_fwd = '.';
static const int key_seek_bwd = ',';
static const int key_del      = 'd';
static const int key_clear    = 'c';
static const int key_stop     = 'x';
static const int key_shuffle  = 'S';
static const int key_move_up  = 'K';
static const int key_move_dn  = 'J';
static const int key_top      = 'g';
static const int key_bottom   = 'G';
static const int key_visual   = 'V';
static const int key_repeat   = 'r';
static const int key_rename   = 'R';
static const int key_addtopl  = 'A';
static const int key_plsave   = 's';

/* seek step (seconds) */
static const int seek_step = 5;

/* search results count */
static const int search_count = 10;

/* volume step (percent) */
static const int vol_step = 5;

/* library path (~ is expanded) */
static const char *lib_dir = "~/Music/hum";

/*
 * colors - uses ncurses COLOR_* constants:
 *   COLOR_BLACK, COLOR_RED, COLOR_GREEN, COLOR_YELLOW,
 *   COLOR_BLUE, COLOR_MAGENTA, COLOR_CYAN, COLOR_WHITE
 *   -1 = terminal default
 */
static const int col_header_fg  = COLOR_BLUE;     /* section headers */
static const int col_header_bg  = -1;
static const int col_num_fg     = COLOR_YELLOW;    /* line numbers */
static const int col_num_bg     = -1;
static const int col_playing_fg = COLOR_GREEN;     /* >> and playing number */
static const int col_playing_bg = -1;
static const int col_visual_fg  = COLOR_WHITE;     /* visual selection */
static const int col_visual_bg  = COLOR_BLUE;
static const int col_status_fg  = COLOR_WHITE;     /* status bar */
static const int col_status_bg  = -1;
static const int col_bar_fg     = COLOR_GREEN;     /* progress bar filled */
static const int col_bar_bg     = -1;
static const int col_search_fg  = COLOR_CYAN;      /* search prompt / */
static const int col_search_bg  = -1;
static const int col_mode_fg    = COLOR_YELLOW;    /* mode indicators */
static const int col_mode_bg    = -1;
static const int col_dim_fg     = COLOR_BLACK;     /* dim text (bright black = gray) */
static const int col_dim_bg     = -1;
