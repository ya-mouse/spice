#ifndef __AST_BASE_H__
#define __AST_BASE_H__

#include <spice.h>
#include "basic_event_loop.h"

#define COUNT(x) ((sizeof(x)/sizeof(x[0])))

/* Character device IOCTL's */
#define ASTCAP_MAGIC  		'a'
#define ASTCAP_IOCCMD		_IOWR(ASTCAP_MAGIC, 0, ASTCap_Ioctl)

typedef enum FPGA_opcode {
    ASTCAP_IOCTL_RESET_VIDEOENGINE = 0,
    ASTCAP_IOCTL_START_CAPTURE,
    ASTCAP_IOCTL_STOP_CAPTURE,
    ASTCAP_IOCTL_GET_VIDEO,
    ASTCAP_IOCTL_GET_CURSOR,
    ASTCAP_IOCTL_CLEAR_BUFFERS,
    ASTCAP_IOCTL_SET_VIDEOENGINE_CONFIGS,
    ASTCAP_IOCTL_GET_VIDEOENGINE_CONFIGS,
    ASTCAP_IOCTL_SET_SCALAR_CONFIGS,
    ASTCAP_IOCTL_ENABLE_VIDEO_DAC,
} ASTCap_OpCode;

typedef enum {
	ASTCAP_IOCTL_SUCCESS = 0,
	ASTCAP_IOCTL_ERROR,
	ASTCAP_IOCTL_NO_VIDEO_CHANGE,
	ASTCAP_IOCTL_BLANK_SCREEN,
} ASTCap_ErrCode;

typedef struct {
	ASTCap_OpCode OpCode;
	ASTCap_ErrCode ErrCode;
	unsigned long Size;
	void *vPtr;
	unsigned char Reserved [2];
} ASTCap_Ioctl;

/*
 * simple queue for commands.
 * each command can have up to two parameters (grow as needed)
 *
 * TODO: switch to gtk main loop. Then add gobject-introspection. then
 * write tests in python/guile/whatever.
 */
typedef enum {
    PATH_PROGRESS,
    SIMPLE_CREATE_SURFACE,
    SIMPLE_DRAW,
    SIMPLE_DRAW_BITMAP,
    SIMPLE_DRAW_SOLID,
    SIMPLE_COPY_BITS,
    SIMPLE_DESTROY_SURFACE,
    SIMPLE_UPDATE,
    DESTROY_PRIMARY,
    CREATE_PRIMARY,
    SLEEP
} CommandType;

typedef struct CommandCreatePrimary {
    uint32_t width;
    uint32_t height;
} CommandCreatePrimary;

typedef struct CommandCreateSurface {
    uint32_t surface_id;
    uint32_t format;
    uint32_t width;
    uint32_t height;
    uint8_t *data;
} CommandCreateSurface;

typedef struct CommandDrawBitmap {
    QXLRect bbox;
    uint8_t *bitmap;
    uint32_t surface_id;
    uint32_t num_clip_rects;
    QXLRect *clip_rects;
} CommandDrawBitmap;

typedef struct CommandDrawSolid {
    QXLRect bbox;
    uint32_t color;
    uint32_t surface_id;
} CommandDrawSolid;

typedef struct CommandSleep {
    uint32_t secs;
} CommandSleep;

typedef struct Command Command;
typedef struct Test Test;

struct Command {
    CommandType command;
    void (*cb)(Test *test, Command *command);
    void *cb_opaque;
    union {
        CommandCreatePrimary create_primary;
        CommandDrawBitmap bitmap;
        CommandDrawSolid solid;
        CommandSleep sleep;
        CommandCreateSurface create_surface;
    };
};

#define MAX_HEIGHT 2048
#define MAX_WIDTH 2048

#define SURF_WIDTH 320
#define SURF_HEIGHT 240

struct Test {
    SpiceCoreInterface *core;
    SpiceServer *server;

    QXLInstance qxl_instance;
    QXLWorker *qxl_worker;

    uint8_t primary_surface[MAX_HEIGHT * MAX_WIDTH * 4];
    int primary_height;
    int primary_width;

    SpiceTimer *wakeup_timer;
    int wakeup_ms;

    int cursor_notify;

    uint8_t secondary_surface[SURF_WIDTH * SURF_HEIGHT * 4];
    int has_secondary;

    // Current mode (set by create_primary)
    int width;
    int height;

    // qxl scripted rendering commands and io
    Command *commands;
    int num_commands;
    int cmd_index;

    int target_surface;

    // callbacks
    void (*on_client_connected)(Test *test);
    void (*on_client_disconnected)(Test *test);

    int started;

    /* ---------- Aspeed private ---------- */
    int videocap_fd;
    void *mmap;
    ASTCap_Ioctl ioc;
};

struct ASTHeader
{
    short version;
    short headlen;

    short src_mode_x;
    short src_mode_y;
    short src_mode_depth;
    short src_mode_rate;
    char src_mode_index;

    short dst_mode_x;
    short dst_mode_y;
    short dst_mode_depth;
    short dst_mode_rate;
    char dst_mode_index;

    int frame_start;
    int frame_num;
    short frame_vsize;
    short frame_hsize;

    int rsvd[2];

    char compression;
    char jpeg_scale;
    char jpeg_table;
    char jpeg_yuv;
    char sharp_mode;
    char adv_table;
    char adv_scale;
    int num_of_MB;
    char rc4_en;
    char rc4_reset;

    char mode420;

    char inf_downscale;
    char inf_diff;
    short inf_analog_thr;
    short inf_dig_thr;
    char inf_ext_sig;
    char inf_auto_mode;
    char inf_vqmode;

    int comp_frame_size;
    int comp_size;
    int comp_hdebug;
    int comp_vdebug;

    char input_signal;
    short cur_xpos;
    short cur_ypos;
} __attribute__((packed));

void test_set_simple_command_list(Test *test, int *command, int num_commands);
void test_set_command_list(Test *test, Command *command, int num_commands);
void test_add_display_interface(Test *test);
void test_add_agent_interface(SpiceServer *server); // TODO - Test *test
Test* ast_new(SpiceCoreInterface* core);

uint32_t test_get_width(void);
uint32_t test_get_height(void);

void spice_test_config_parse_args(int argc, char **argv);

#endif /* __TEST_DISPLAY_BASE_H__ */
