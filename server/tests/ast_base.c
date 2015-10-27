#include <config.h>
#include <stdlib.h>
#include <math.h>
#include <string.h>
#include <stdio.h>
#include <unistd.h>
#include <signal.h>
#include <wait.h>
#include <sys/select.h>
#include <sys/types.h>
#include <getopt.h>
#include <sys/ioctl.h>

#include "spice.h"
#include <spice/qxl_dev.h>

#include "ast_base.h"
#include "red_channel.h"
#include "test_util.h"

#ifndef PATH_MAX
#define PATH_MAX 4096
#endif

#define MEM_SLOT_GROUP_ID 0

#define NOTIFY_DISPLAY_BATCH (SINGLE_PART/2)
#define NOTIFY_CURSOR_BATCH 10

#define DEFAULT_WIDTH 800
#define DEFAULT_HEIGHT 600

#define _VAR1 1
#define _VAR1_1 0

#define SINGLE_PART 4
static const int angle_parts = 64 / SINGLE_PART;
static int unique = 1;
static int color = -1;
static int c_i = 0;

__attribute__((noreturn))
static void sigchld_handler(SPICE_GNUC_UNUSED int signal_num) // wait for the child process and exit
{
    int status;
    wait(&status);
    exit(0);
}

/* Parts cribbed from spice-display.h/.c/qxl.c */

typedef struct SimpleSpiceUpdate {
    QXLCommandExt ext; // first
    QXLDrawable drawable;
    QXLImage image;
    uint8_t *bitmap;
} SimpleSpiceUpdate;

SimpleSpiceUpdate *cmd_ext = NULL;

typedef struct Path {
    int t;
    int min_t;
    int max_t;
} Path;

static void path_init(Path *path, int min, int max)
{
    path->t = min;
    path->min_t = min;
    path->max_t = max;
}

static void path_progress(Path *path)
{
    path->t = (path->t+1)% (path->max_t - path->min_t) + path->min_t;
}

Path path;

static void create_primary_surface(Test *test, uint32_t width,
                                   uint32_t height)
{
    QXLDevSurfaceCreate surface = { 0, };

    ASSERT(height <= MAX_HEIGHT);
    ASSERT(width <= MAX_WIDTH);
    ASSERT(height > 0);
    ASSERT(width > 0);

    surface.format     = SPICE_SURFACE_FMT_32_xRGB;
    surface.width      = test->primary_width = width;
    surface.height     = test->primary_height = height;
    surface.stride     = -width * 4; /* negative? */
    surface.mouse_mode = TRUE; /* unused by red_worker */
    surface.flags      = 0;
    surface.type       = 0;    /* unused by red_worker */
    surface.position   = 0;    /* unused by red_worker */
    surface.mem        = (uintptr_t)&test->primary_surface;
    surface.group_id   = MEM_SLOT_GROUP_ID;

    test->width = width;
    test->height = height;

    spice_qxl_create_primary_surface(&test->qxl_instance, 0, &surface);
}

void dump_frame(void *mmap)
{
    FILE *fp;
    struct ASTHeader *hdr = (struct ASTHeader *)mmap;
#if 0
    fp = fopen("/tmp/videocap.bin", "w+");
    if (!fp)
        return;

    fwrite(mmap, hdr->comp_size + 0x4000 + 86, 1, fp);

    fclose(fp);
#endif
}

void *load_frame()
{
    FILE *fp;
    struct ASTHeader *hdr;

    fp = fopen("/tmp/videocap.bin", "r");
    if (!fp)
        return;

    hdr = malloc(sizeof(*hdr)+2);
    fread(hdr, sizeof(*hdr), 1, fp);

    fclose(fp);
    return hdr;
}

/* bitmap and rects are freed, so they must be allocated with malloc */
SimpleSpiceUpdate *test_spice_create_update_from_bitmap(Test *test, uint32_t surface_id)
{
    SimpleSpiceUpdate *update;
    QXLDrawable *drawable;
    QXLImage *image;
    uint32_t bw, bh;
#if (_VAR1) && !(_VAR1_1)
    void *bitmap;
#else
    uint8_t bitmap[128];
#endif
    struct ASTHeader *hdr;
    static int i =0;

    bzero(&test->ioc, sizeof(ASTCap_Ioctl));
    test->ioc.OpCode = ASTCAP_IOCTL_GET_VIDEO;
    ioctl(test->videocap_fd, ASTCAP_IOCCMD, &test->ioc);

#if 0
    // Local testing
    if (i++ >= 1) {
        test->ioc.ErrCode=  ASTCAP_IOCTL_NO_VIDEO_CHANGE;
     } else {
        test->ioc.Size = 55844;
    }
#endif

    if (test->ioc.ErrCode == ASTCAP_IOCTL_NO_VIDEO_CHANGE) {
        hdr = load_frame();
    } else {
        dump_frame(test->mmap);
        hdr = (struct ASTHeader *)test->mmap;
        fclose(fopen("/tmp/videocap.use", "w+"));
//        unlink("/tmp/videocap.use");
    }
//    printf("sz=%d\n", test->ioc.Size, hdr->comp_size);

    if (test->primary_width != hdr->src_mode_x ||
        test->primary_height != hdr->src_mode_y) {
        test->primary_width = hdr->src_mode_x;
        test->primary_height = hdr->src_mode_y;

        spice_qxl_destroy_primary_surface(&test->qxl_instance, 0);
        create_primary_surface(test, test->primary_width, test->primary_height);
        printf("Resize to %dx%d\n", test->primary_width, test->primary_height);
    }

#if _VAR1
    QXLRect bbox = {
        .left = 0,
        .right = (test->target_surface == 0 ? test->primary_width : test->width),
        .top = 0,
        .bottom = (test->target_surface == 0 ? test->primary_height : test->height)
    };
#if _VAR1_1
    if (test->ioc.ErrCode == ASTCAP_IOCTL_NO_VIDEO_CHANGE) {
        bitmap = hdr;
    } else {
        bitmap = malloc(test->ioc.Size + 88);
        memcpy(bitmap, test->mmap, 88);
        memcpy(bitmap + 88, test->mmap + 0x4000, test->ioc.Size);
    }
#endif

#else
    QXLRect bbox = {
        .left = 0,
        .right = 2,
        .top = 0,
        .bottom = 2,
    };
#endif
    bh = bbox.bottom - bbox.top;
    bw = bbox.right - bbox.left;

    update   = calloc(sizeof(*update), 1);
    update->bitmap = bitmap;
    drawable = &update->drawable;
    image    = &update->image;

    drawable->surface_id      = surface_id;

    drawable->bbox            = bbox;
    drawable->clip.type       = SPICE_CLIP_TYPE_NONE;
    drawable->effect          = QXL_EFFECT_OPAQUE;
    drawable->release_info.id = (intptr_t)update;
    drawable->type            = QXL_DRAW_COPY;
    drawable->surfaces_dest[0] = -1;
    drawable->surfaces_dest[1] = -1;
    drawable->surfaces_dest[2] = -1;

    drawable->u.copy.rop_descriptor  = SPICE_ROPD_OP_PUT;
    drawable->u.copy.src_bitmap      = (intptr_t)image;
    drawable->u.copy.src_area.right  = bw;
    drawable->u.copy.src_area.bottom = bh;

    QXL_SET_IMAGE_ID(image, QXL_IMAGE_GROUP_DEVICE, unique);

#if _VAR1
    image->descriptor.type   = SPICE_IMAGE_TYPE_AST;
    image->ast.data = (uint8_t *)bitmap;
    image->ast.data_size = test->ioc.Size + 88;

    //printf("ioc.Size=%x comp=%x\n", test->ioc.Size, hdr->comp_size);

#else
    image->descriptor.type   = SPICE_IMAGE_TYPE_BITMAP;
    image->bitmap.flags      = QXL_BITMAP_DIRECT | QXL_BITMAP_TOP_DOWN;
    image->bitmap.stride     = bw * 4;
    image->descriptor.width  = image->bitmap.x = bw;
    image->descriptor.height = image->bitmap.y = bh;
    image->bitmap.data = (intptr_t)bitmap;
    image->bitmap.palette = 0;
    image->bitmap.format = SPICE_BITMAP_FMT_32BIT;
#endif

    update->ext.cmd.type = QXL_CMD_DRAW;
    update->ext.cmd.data = (intptr_t)drawable;
    update->ext.cmd.padding = 0;
    update->ext.group_id = MEM_SLOT_GROUP_ID;
    update->ext.flags = 0;
//    printf("UP type=%d\n", update->ext.cmd.type);

    return update;
}

QXLDevMemSlot slot = {
.slot_group_id = MEM_SLOT_GROUP_ID,
.slot_id = 0,
.generation = 0,
.virt_start = 0,
.virt_end = ~0,
.addr_delta = 0,
.qxl_ram_size = ~0,
};

static void attache_worker(QXLInstance *qin, QXLWorker *_qxl_worker)
{
    Test *test = SPICE_CONTAINEROF(qin, Test, qxl_instance);

    if (test->qxl_worker) {
        if (test->qxl_worker != _qxl_worker)
            printf("%s ignored, %p is set, ignoring new %p\n", __func__,
                   test->qxl_worker, _qxl_worker);
        else
            printf("%s ignored, redundant\n", __func__);
        return;
    }
    printf("%s\n", __func__);
    test->qxl_worker = _qxl_worker;
    spice_qxl_add_memslot(&test->qxl_instance, &slot);
    create_primary_surface(test, DEFAULT_WIDTH, DEFAULT_HEIGHT);
    spice_server_vm_start(test->server);
}

static void set_compression_level(SPICE_GNUC_UNUSED QXLInstance *qin,
                                  SPICE_GNUC_UNUSED int level)
{
    printf("%s\n", __func__);
}

static void set_mm_time(SPICE_GNUC_UNUSED QXLInstance *qin,
                        SPICE_GNUC_UNUSED uint32_t mm_time)
{
}

// we now have a secondary surface
#define MAX_SURFACE_NUM 1

static void get_init_info(SPICE_GNUC_UNUSED QXLInstance *qin,
                          QXLDevInitInfo *info)
{
    memset(info, 0, sizeof(*info));
    info->num_memslots = 1;
    info->num_memslots_groups = 1;
    info->memslot_id_bits = 1;
    info->memslot_gen_bits = 1;
    info->n_surfaces = MAX_SURFACE_NUM;
}

#if 0
int commands_end = 0;
int commands_start = 0;
struct QXLCommandExt* commands[1024];

#define COMMANDS_SIZE COUNT(commands)

static void push_command(QXLCommandExt *ext)
{
    ASSERT(commands_end - commands_start < (int) COMMANDS_SIZE);
    commands[commands_end % COMMANDS_SIZE] = ext;
    commands_end++;
//    printf("push_command[%d] %d = %p\n", commands_end-1, commands_start, ext);
}

static struct QXLCommandExt *get_simple_command(void)
{
    struct QXLCommandExt *ret = commands[commands_start % COMMANDS_SIZE];
    ASSERT(commands_start < commands_end);
    commands_start++;
    return ret;
}

static int get_num_commands(void)
{
    return commands_end - commands_start;
}

// called from spice_server thread (i.e. red_worker thread)
static int get_command(SPICE_GNUC_UNUSED QXLInstance *qin,
                       struct QXLCommandExt *ext)
{
    if (!test->started) return FALSE;

    if (get_num_commands() == 0) {
        return FALSE;
    }

//    memcpy(ext, cmd_ext, sizeof(*ext));
    *ext = *get_simple_command();
    //printf("type=%d %p t=%d %p [%d]\n", ext->cmd.type, ext, cmd_ext->ext.cmd.type, cmd_ext, commands_start);
    return TRUE;
}

#else
// called from spice_server thread (i.e. red_worker thread)
static int get_command(SPICE_GNUC_UNUSED QXLInstance *qin,
                       struct QXLCommandExt *ext)
{
    if ((intptr_t)cmd_ext <= 0) {
        return FALSE;
    }

    memcpy(ext, cmd_ext, sizeof(*ext));
//    *ext = *&cmd_ext->ext;
//    printf("type=%d %p t=%d %p\n", ext->cmd.type, ext, cmd_ext->ext.cmd.type, cmd_ext);
    cmd_ext = (void *)-1;
    return TRUE;
}
#endif

static int req_cmd_notification(QXLInstance *qin)
{
    Test *test = SPICE_CONTAINEROF(qin, Test, qxl_instance);

    test->core->timer_start(test->wakeup_timer, test->wakeup_ms);
    return TRUE;
}

static void do_wakeup(void *opaque)
{
    Test *test = opaque;
    int notify;

    int static init = 0;
    test->cursor_notify = NOTIFY_CURSOR_BATCH;

#if 0
    SimpleSpiceUpdate *update = test_spice_create_update_from_bitmap(test, 0);
    push_command(&update->ext);
#else
    if ((intptr_t)cmd_ext <= 0) {
        //printf("=%p ", cmd_ext);
        cmd_ext = test_spice_create_update_from_bitmap(test, 0);
        //printf("+ %p\n", cmd_ext);
        if (cmd_ext == NULL)
            cmd_ext = (void *)-1;
    }
#endif

//    printf("--do_wakeup: %p\n", cmd_ext);
    test->core->timer_start(test->wakeup_timer, test->wakeup_ms);
    spice_qxl_wakeup(&test->qxl_instance);
}

static void release_resource(SPICE_GNUC_UNUSED QXLInstance *qin,
                             struct QXLReleaseInfoExt release_info)
{
    QXLCommandExt *ext = (QXLCommandExt*)(unsigned long)release_info.info->id;
    if (ext->cmd.type != QXL_CMD_CURSOR) {
    //    printf("release: %p %p\n", release_info.info, ext);
    //    printf("%s t=%d\n", __func__, ext->cmd.type);
    }
    ASSERT(release_info.group_id == MEM_SLOT_GROUP_ID);
    switch (ext->cmd.type) {
        case QXL_CMD_DRAW:
            if (ext) {
                cmd_ext = NULL;
                SimpleSpiceUpdate *update = (SimpleSpiceUpdate *)ext;
#if _VAR1
                free(update->bitmap);
#endif
                free(update);
            }
            break;
        case QXL_CMD_SURFACE:
#if 0
            bzero(&encoder->ioc, sizeof(ASTCap_Ioctl));
            encoder->ioc.OpCode = ASTCAP_IOCTL_STOP_CAPTURE;
            ioctl(encoder->videocap_fd, ASTCAP_IOCCMD, &encoder->ioc);
#endif
            free(ext);
            break;
        case QXL_CMD_CURSOR: {
            QXLCursorCmd *cmd = (QXLCursorCmd *)(unsigned long)ext->cmd.data;
            if (cmd->type == QXL_CURSOR_SET) {
                free(cmd);
            }
            free(ext);
            break;
        }
        default:
            abort();
    }
}

#define CURSOR_WIDTH 32
#define CURSOR_HEIGHT 32

static struct {
    QXLCursor cursor;
    uint8_t data[CURSOR_WIDTH * CURSOR_HEIGHT * 4]; // 32bit per pixel
} cursor;

static void cursor_init()
{
    cursor.cursor.header.unique = 0;
    cursor.cursor.header.type = SPICE_CURSOR_TYPE_COLOR32;
    cursor.cursor.header.width = CURSOR_WIDTH;
    cursor.cursor.header.height = CURSOR_HEIGHT;
    cursor.cursor.header.hot_spot_x = 0;
    cursor.cursor.header.hot_spot_y = 0;
    cursor.cursor.data_size = CURSOR_WIDTH * CURSOR_HEIGHT * 4;

    // X drivers addes it to the cursor size because it could be
    // cursor data information or another cursor related stuffs.
    // Otherwise, the code will break in client/cursor.cpp side,
    // that expect the data_size plus cursor information.
    // Blame cursor protocol for this. :-)
    cursor.cursor.data_size += 128;
    cursor.cursor.chunk.data_size = cursor.cursor.data_size;
    cursor.cursor.chunk.prev_chunk = cursor.cursor.chunk.next_chunk = 0;
}

static int get_cursor_command(QXLInstance *qin, struct QXLCommandExt *ext)
{
    Test *test = SPICE_CONTAINEROF(qin, Test, qxl_instance);
    static int set = 1;
    static int x = 0, y = 0;
    QXLCursorCmd *cursor_cmd;
    QXLCommandExt *cmd;

    if (!test->started) return FALSE;

    if (!test->cursor_notify) {
        return FALSE;
    }

    test->cursor_notify--;
    cmd = calloc(sizeof(QXLCommandExt), 1);
    cursor_cmd = calloc(sizeof(QXLCursorCmd), 1);

    cursor_cmd->release_info.id = (unsigned long)cmd;

    if (set) {
        cursor_cmd->type = QXL_CURSOR_SET;
        cursor_cmd->u.set.position.x = 0;
        cursor_cmd->u.set.position.y = 0;
        cursor_cmd->u.set.visible = TRUE;
        cursor_cmd->u.set.shape = (unsigned long)&cursor;
        // Only a white rect (32x32) as cursor
        memset(cursor.data, 255, sizeof(cursor.data));
        set = 0;
    } else {
        cursor_cmd->type = QXL_CURSOR_MOVE;
        cursor_cmd->u.position.x = x++ % test->primary_width;
        cursor_cmd->u.position.y = y++ % test->primary_height;
    }

    cmd->cmd.data = (unsigned long)cursor_cmd;
    cmd->cmd.type = QXL_CMD_CURSOR;
    cmd->group_id = MEM_SLOT_GROUP_ID;
    cmd->flags    = 0;
    *ext = *cmd;
    //printf("%s\n", __func__);
    return TRUE;
}

static int req_cursor_notification(SPICE_GNUC_UNUSED QXLInstance *qin)
{
    printf("%s\n", __func__);
    return TRUE;
}

static void notify_update(SPICE_GNUC_UNUSED QXLInstance *qin,
                          SPICE_GNUC_UNUSED uint32_t update_id)
{
    printf("%s\n", __func__);
}

static int flush_resources(SPICE_GNUC_UNUSED QXLInstance *qin)
{
    printf("%s\n", __func__);
    return TRUE;
}

static int client_monitors_config(SPICE_GNUC_UNUSED QXLInstance *qin,
                                  VDAgentMonitorsConfig *monitors_config)
{
    if (!monitors_config) {
        printf("%s: NULL monitors_config\n", __func__);
    } else {
        printf("%s: %d\n", __func__, monitors_config->num_of_monitors);
    }
    return 0;
}

void on_client_connected(Test *test)
{
    if (test->started == TRUE) return;

    test->started = TRUE;
}

void on_client_disconnected(Test *test)
{
    test->started = FALSE;
}

static void set_client_capabilities(QXLInstance *qin,
                                    uint8_t client_present,
                                    uint8_t caps[58])
{
    Test *test = SPICE_CONTAINEROF(qin, Test, qxl_instance);

    printf("%s: present %d caps %d\n", __func__, client_present, caps[0]);
    if (test->on_client_connected && client_present) {
        printf("! connected\n");
        test->on_client_connected(test);
    }
    if (test->on_client_disconnected && !client_present) {
        printf("! disconnected\n");
        test->on_client_disconnected(test);
    }
}

QXLInterface display_sif = {
    .base = {
        .type = SPICE_INTERFACE_QXL,
        .description = "AST Video",
        .major_version = SPICE_INTERFACE_QXL_MAJOR,
        .minor_version = SPICE_INTERFACE_QXL_MINOR
    },
    .attache_worker = attache_worker,
    .set_compression_level = set_compression_level,
    .set_mm_time = set_mm_time,
    .get_init_info = get_init_info,

    /* the callbacks below are called from spice server thread context */
    .get_command = get_command,
    .req_cmd_notification = req_cmd_notification,
    .release_resource = release_resource,
    .get_cursor_command = get_cursor_command,
    .req_cursor_notification = req_cursor_notification,
    .notify_update = notify_update,
    .flush_resources = flush_resources,
    .client_monitors_config = client_monitors_config,
    .set_client_capabilities = set_client_capabilities,
};

Test *ast_new(SpiceCoreInterface *core)
{
    int port = 5701; //5912;
    Test *test = spice_new0(Test, 1);
    SpiceServer* server = spice_server_new();

    test->qxl_instance.base.sif = &display_sif.base;
    test->qxl_instance.id = 0;

    test->core = core;
    test->server = server;
    test->wakeup_ms = 50;
    test->cursor_notify = NOTIFY_CURSOR_BATCH;
    // some common initialization for all display tests
    printf("TESTER: listening on port %d (unsecure)\n", port);
    spice_server_set_port(server, port);
    spice_server_set_noauth(server);
    spice_server_init(server, core);
    spice_server_set_video_codecs(test->server, "aspeed:aspeed");
//    spice_server_set_image_compression(test->server, SPICE_IMAGE_COMPRESSION_OFF);
    spice_server_set_streaming_video(test->server, SPICE_STREAM_VIDEO_ALL);

    cursor_init();
    path_init(&path, 0, angle_parts);
    test->has_secondary = 0;
    test->on_client_connected = on_client_connected;
    test->on_client_disconnected = on_client_disconnected;
    test->wakeup_timer = core->timer_add(do_wakeup, test);

    // test_add_display_interface
    spice_server_add_interface(test->server, &test->qxl_instance.base);

//    create_primary_surface(test, 800, 600);
//    spice_qxl_destroy_primary_surface(&test->qxl_instance, 0);
    return test;
}
