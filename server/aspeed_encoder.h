/****************************************************************
 ****************************************************************
 **                                                            **
 **    (C)Copyright 2006-2009, American Megatrends Inc.        **
 **                                                            **
 **            All Rights Reserved.                            **
 **                                                            **
 **        5555 Oakbrook Pkwy Suite 200, Norcross              **
 **                                                            **
 **        Georgia - 30093, USA. Phone-(770)-246-8600.         **
 **                                                            **
 ****************************************************************
 ****************************************************************/

# ifndef __ASPEED_ENCODER_H__
# define __ASPEED_ENCODER_H__

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

#endif /* !__ASPEED_ENCODER_H__ */
