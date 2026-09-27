#pragma once
/*
 * Port settings, kept in <save dir>\hydro.ini (g_cfg.save_dir) and edited from the operator menu's PC SETTINGS page
 * (host/menu_pc.c). Graphics settings are pushed to dgVoodoo.conf (dgv.h).
 */
#include "dgv.h"

#include "../common/htnet_proto.h"

/* Cabinet inputs a key or pad button can be bound to. */
enum {
    ACT_STEER_LEFT,
    ACT_STEER_RIGHT,
    ACT_THROTTLE_UP,
    ACT_THROTTLE_DOWN,
    ACT_BOOST, /* switch 0x01: boost / start */
    ACT_VIEW1, /* switch 0x02: pilot view */
    ACT_VIEW2, /* switch 0x04: low view */
    ACT_VIEW3, /* switch 0x08: high view */
    ACT_COIN,
    ACT_SERVICE, /* switch 0x10: service credit */
    ACT_TEST,    /* switch 0x80: operator menu */
    ACT_VOL_DOWN,
    ACT_VOL_UP,
    ACT_COUNT
};

#define BIND_SLOTS 2

/* Gamepad (XInput) buttons: the XINPUT_GAMEPAD_* bit index, plus the two triggers. */
enum {
    PAD_NONE = -1,
    PAD_DPAD_UP = 0,
    PAD_DPAD_DOWN,
    PAD_DPAD_LEFT,
    PAD_DPAD_RIGHT,
    PAD_START,
    PAD_BACK,
    PAD_LS,
    PAD_RS,
    PAD_LB,
    PAD_RB,
    PAD_A = 12,
    PAD_B,
    PAD_X,
    PAD_Y,
    PAD_LT = 16,
    PAD_RT,
    PAD_BUTTON_COUNT
};

typedef struct Controls {
    int gamepad;         /* 0 = ignore the pad */
    int menu_assist;     /* digital steps in the wheel-driven menus (menu_assist.c) */
    int steer_stick;     /* 0 = left stick, 1 = right stick */
    int throttle_source; /* 0 = triggers (RT forward, LT reverse), 1 = right stick Y, 2 = left stick Y, 3 = buttons only */
    int deadzone;        /* stick dead zone, percent */
    int sensitivity;     /* stick steering gain, percent */
    int key_steer;       /* keyboard steering: 0 = instant, 1 = fast, 2 = medium, 3 = slow */
    int key[ACT_COUNT][BIND_SLOTS]; /* virtual-key codes, 0 = unbound */
    int pad[ACT_COUNT][BIND_SLOTS]; /* PAD_*, PAD_NONE = unbound */
} Controls;

/* Linked play (hle_net.c, lobby_client.c). Without a lobby, the unit id and on/off are the game's
 * own, in NETWORK ADJUSTMENTS (CMOS); in a lobby, the lobby assigns the unit. */
/* The master server when none is set (no port: an SRV record _hydrothunder._udp.<name> may name
 * the actual host and port, else the name itself on 27950). */
#define NET_DEFAULT_MASTER "hydrothunder.racing"
#define NET_OLD_DEFAULT_MASTER "lancommander.app" /* the default until 2026-09: moved to the new one */

typedef struct Network {
    int lan_broadcast;   /* find other units / LAN lobbies by broadcast on every adapter's subnet */
    int local_instances; /* also look for units on this PC (127.0.0.1) */
    int base_port;       /* no lobby: unit n listens on UDP base_port + n (0-15) */
    char peers[512];     /* hydro.ini only: "host[:port], ..." for units broadcast can't reach */
    char master[128];    /* lobby master server, "host[:port]": no port = SRV, else 27950 (lobby_client.c) */
    char direct[128];    /* the last DIRECT CONNECT address */
    char player_name[16];
    char lobby_name[24];     /* last lobby hosted */
    char lobby_password[32]; /* its password ("" = none) */
    int lobby_max;           /* its player limit */
    int lobby_delay;         /* its input delay: 0 = auto (1 on a LAN, 3 over the internet), 1-6 frames */
    HtGameOpts lobby_opts;   /* its game options ([Lobby Options]) */
    int input_delay;         /* hydro.ini only: input delay without a lobby (every unit must agree) */
    int lan_port;            /* LAN lobby hosts listen on lan_port + 0..3 */
} Network;

/* The mix of the cabinet's two outputs (hle_audio.c): left = the headrest speakers (speech, music,
 * engines), right = the subwoofer under the seat (engines, impacts, some effects). */
typedef struct Audio {
    int output;    /* 0 = both mixed into the centre (PC speakers), 1 = raw cabinet channels */
    int headrest;  /* left channel level, percent */
    int subwoofer; /* right channel level, percent */
    int crossover; /* subwoofer low-pass, Hz (0 = full range) */
} Audio;

typedef struct Settings {
    DgvGraphics gfx;
    Audio audio;
    Controls ctl;
    Network net;
} Settings;

extern Settings g_settings;

/* One menu/ini setting: a list of named choices, or an integer range. */
typedef struct SettingDesc {
    const char *section, *key; /* hydro.ini */
    const char *label;         /* menu text */
    int *value;
    const char *const *names; /* list choices (count > 0) */
    const int *values;        /* stored value per choice; NULL = the index itself */
    int count;
    int min, max, step; /* range (count == 0) */
    int def;            /* default stored value */
} SettingDesc;

extern const SettingDesc g_setting_desc[];
extern const int g_setting_count;
int setting_index(const SettingDesc *d);         /* list index (or the value, for ranges) */
void setting_set_index(const SettingDesc *d, int i); /* clamps */

void settings_load(void);           /* reads hydro.ini (defaults for anything missing) */
void settings_save(void);           /* writes hydro.ini */
void settings_apply_graphics(void); /* writes dgVoodoo.conf; takes effect at the next grGlideInit */
void settings_default_bindings(Controls *c);

const char *action_name(int act);
int action_from_key(const char *ini_key); /* hydro.ini [Keyboard] key name -> ACT_*, -1 if unknown */
const char *key_name(int vk);  /* "" for 0 */
const char *pad_name(int btn); /* "" for PAD_NONE */
