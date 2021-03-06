#ifndef UXDISP_HW_H_
#define UXDISP_HW_H_

#define UXDISP_XTRA_CAPS_PV_VBLANK        0x1
#define UXDISP_XTRA_CAPS_USER_DRAW        0x2

#define UXDISP_XTRA_CTRL_PV_VBLANK_ENABLE 0x1
#define UXDISP_XTRA_CTRL_USER_DRAW_ENABLE 0x2

#define UXDISP_REG_MAGIC                0x00000
#define     UXDISP_MAGIC                            0x7558656e
#define UXDISP_REG_REVISION             0x00004
#define UXDISP_REG_VRAM_SIZE            0x00008
#define UXDISP_REG_BANK_ORDER           0x0000C
#define UXDISP_REG_CRTC_COUNT           0x00010
#define UXDISP_REG_STRIDE_ALIGN         0x00014
#define UXDISP_REG_INTERRUPT            0x00018
#define     UXDISP_INTERRUPT_HOTPLUG                0x1
#define     UXDISP_INTERRUPT_VBLANK                 0x2
#define UXDISP_REG_CURSOR_ENABLE        0x0001C
#define     UXDISP_CURSOR_SHOW                      0x1
#define UXDISP_REG_MODE                 0x00020
#define     UXDISP_MODE_VGA_DISABLED                0x1
#define     UXDISP_MODE_PAGE_TRACKING_DISABLED      0x2
#define UXDISP_REG_INTERRUPT_ENABLE     0x00024
#define UXDISP_REG_VIRTMODE_ENABLED     0x00028

#define UXDISP_REG_XTRA_CAPS            0x0002c
#define UXDISP_REG_XTRA_CTRL            0x00030
#define UXDISP_REG_VSYNC_HZ             0x00034

#define UXDISP_REG_BANK_LEN             0x00004
#define UXDISP_REG_BANK(x)              (0x00100 + (x) * UXDISP_REG_BANK_LEN)
#define UXDISP_REG_BANK_POPULATE        0x0

#define UXDISP_REG_CURSOR_POS_X         0x01000
#define UXDISP_REG_CURSOR_POS_Y         0x01004
#define UXDISP_REG_CURSOR_WIDTH         0x01008
#define UXDISP_REG_CURSOR_HEIGHT        0x0100C
#define UXDISP_REG_CURSOR_HOT_X         0x01010
#define UXDISP_REG_CURSOR_HOT_Y         0x01014
#define UXDISP_REG_CURSOR_CRTC          0x01018
#define UXDISP_REG_CURSOR_FLAGS         0x0101C
#define     UXDISP_CURSOR_FLAG_1BPP                 0x1
#define     UXDISP_CURSOR_FLAG_MASK_PRESENT         0x2

#define UXDISP_CURSOR_WIDTH_MAX         128
#define UXDISP_CURSOR_HEIGHT_MAX        128
#define UXDISP_REG_CURSOR_DATA          (UXDISP_CURSOR_WIDTH_MAX * UXDISP_CURSOR_HEIGHT_MAX * 4)

#define UXDISP_NB_BUFFERS               2
#define UXDISP_NB_CRTCS                 1
#define UXDISP_REG_CRTC_LEN             0x02000
#define UXDISP_REG_CRTC(x)              (0x10000 + (x) * UXDISP_REG_CRTC_LEN)
#define UXDISP_REG_CRTC_STATUS          0x0000
#define UXDISP_REG_CRTC_OFFSET          0x0004
#define UXDISP_REG_CRTC_ENABLE          0x1000
#define UXDISP_REG_CRTC_XRES            0x1004
#define UXDISP_REG_CRTC_YRES            0x1008
#define UXDISP_REG_CRTC_STRIDE          0x100C
#define UXDISP_REG_CRTC_FORMAT          0x1010
#define     UXDISP_CRTC_FORMAT_BGRX_8888            0x00000000
#define     UXDISP_CRTC_FORMAT_BGR_888              0x00000001
#define     UXDISP_CRTC_FORMAT_BGR_565              0x00000002
#define     UXDISP_CRTC_FORMAT_BGR_555              0x00000004
#define UXDISP_REG_CRTC_BUFFERS         0x1014
#define UXDISP_REG_CRTC_EDID_DATA       0x1100

#if defined(_MSC_VER)
#define INLINE __inline
#else
#define INLINE inline
#endif

static INLINE int uxdisp_fmt_to_bpp(int fmt)
{
    switch (fmt) {
    case UXDISP_CRTC_FORMAT_BGRX_8888:
        return 32;
    case UXDISP_CRTC_FORMAT_BGR_888:
        return 24;
    case UXDISP_CRTC_FORMAT_BGR_565:
        return 16;
    case UXDISP_CRTC_FORMAT_BGR_555:
        return 15;
    }

    return -1;
}

#endif /* UXDISP_HW_H_ */
