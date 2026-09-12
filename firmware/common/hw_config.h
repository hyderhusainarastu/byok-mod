/* SPDX-License-Identifier: MIT
 * ============================================================================
 * hw_config.h — BYOK v2.1 (ESP32-S3-WROOM-1 N16R2) hardware constants
 * ============================================================================
 *
 * EVERY value in this file was recovered by DISASSEMBLING the stock vendor
 * firmware. Nothing here has been measured on the board with an instrument.
 *
 * Primary source (device-matching build):
 *   The vendor's stock app image, version 1.1.0, extracted from a full
 *   flash backup of the examined unit -- ESP32-S3 app image, project
 *   "BYOK", IDF v5.5.3, 6 segments.
 * Cross-checked against:
 *   The same unit's 1.1.3 app image, extracted the same way.
 *
 * File-offset -> virtual-address arithmetic for the 1.1.0 image
 * (segment data starts 8 bytes after each segment header):
 *   DROM  file 0x000020..0x091640  VA = file + 0x3C16_0000
 *   DRAM  file 0x091648..0x097918  VA = file + 0x3FC0_B4B8
 *   IRAM  file 0x097920..0x0A0018  VA = file + 0x402D_C6E0
 *   IROM  file 0x0A0020..0x1F5688  VA = file + 0x41F6_0000
 *   IRAM2 file 0x1F5690..0x205A4C  VA = file + 0x4018_7068
 *
 * Every #define carries: [GRADE] evidence-offset (1.1.0 file offset unless
 * stated).  Grades follow SAFETY.md §2:
 *   CONFIRMED           - read directly out of the instruction stream
 *   STRONGLY INDICATED  - inferred from surrounding code/log strings
 *   POSSIBLE            - plausible, weakly supported
 *   UNKNOWN             - not determined (guarded with #error below)
 *
 * Full evidence -- file offset, instruction text, and grade -- is inlined
 * against each #define below; docs/hardware.md, docs/pinout.md and
 * docs/display.md carry the same findings in prose.
 *
 * SAFETY: this header describes the STOCK hardware wiring. It says nothing
 * about which flash partition our firmware may be written to. Per the project
 * hard rules our images target ota_1 (0x610000) or the SD-card updater path
 * ONLY — never bootloader/partition-table/nvs/phy_init/factory/ota_0/otadata/
 * assets.
 * ============================================================================
 */

#ifndef BYOK_HW_CONFIG_H
#define BYOK_HW_CONFIG_H

/* ==========================================================================
 * 0. SoC
 * ========================================================================== */

/* [CONFIRMED] esptool image_info of both images: "Detected image type:
 * ESP32-S3", chip id 9, flash 16 MB, DIO, 80 MHz. Device-side identification
 * in docs/flash-layout-and-updater.md agrees (N16R2, 2 MB PSRAM, 40 MHz XTAL). */
#define BYOK_SOC_ESP32S3                1
#define BYOK_FLASH_SIZE_BYTES           (16u * 1024u * 1024u)
#define BYOK_XTAL_FREQ_MHZ              40

/* [CONFIRMED] Every vTaskDelay() wrapper in the vendor image divides a
 * millisecond argument by 10 (magic multiply 0x10624DD3, >>6 after >>32, i.e.
 * ms*100/1000), so CONFIG_FREERTOS_HZ = 100 and portTICK_PERIOD_MS = 10.
 * Evidence: 0x112F3C (display delay helper), 0x0DBE64 (PICO reset pulse). */
#define BYOK_FREERTOS_HZ                100

/* ==========================================================================
 * 1. I2C BUS 0 — display only  (gclcd1611, ./components/gclcd1611/bussim.c)
 * ==========================================================================
 * Recovered from sim_reset() @ file 0x115430 (VA 0x42075430). The whole
 * i2c_master_bus_config_t is zeroed (8 words, loop @0x115452) and then only
 * the fields below are written, so every unlisted field is 0.
 */

/* [CONFIRMED] +0 of the zeroed struct is never written  -> I2C_NUM_0
 * (0x115452 zero-loop; the port field is left at 0). */
#define BYOK_LCD_I2C_PORT               0

/* [CONFIRMED] 0x115459  movi.n a8,18 ; 0x11545E  s32i.n a8,a1,4   (sda_io_num) */
#define BYOK_LCD_I2C_SDA_GPIO           18

/* [CONFIRMED] 0x115460  movi.n a8,10 ; 0x115462  s32i.n a8,a1,8   (scl_io_num) */
#define BYOK_LCD_I2C_SCL_GPIO           10

/* [CONFIRMED] 0x115464  movi.n a8,11 ; 0x115466  s32i.n a8,a1,12
 * clk_source = 11 = SOC_MOD_CLK_XTAL on ESP32-S3 = I2C_CLK_SRC_XTAL
 * (= I2C_CLK_SRC_DEFAULT on this target). */
#define BYOK_LCD_I2C_CLK_SRC_XTAL       1

/* [CONFIRMED] 0x11546A  movi.n a8,7 ; 0x11546F  s8i a8,a1,16 */
#define BYOK_LCD_I2C_GLITCH_IGNORE_CNT  7

/* [CORRECTED 0.1.8 -- was mislabelled CONFIRMED] the triple at 0x11549A/
 * 0x1154A2/0x1154AA (l8ui/or 1/s8i @ offset +68 from the sim_reset() frame
 * base) is dev_cfg_cmd.flags (dev_cfg_cmd is based at a1+52, and
 * i2c_device_config_t.flags sits at +16 -> a1+68) -- the SAME instruction
 * BYOK_LCD_I2C_DISABLE_ACK_CHECK below (correctly) cites. It is the
 * *device* struct's flags, not the *bus* struct's. The bus struct occupies
 * a1+0..a1+31; its own flags word (offset +28, "enable_internal_pullup") is
 * written ONLY by sim_reset()'s 8-word zero loop and never touched again --
 * exactly two l8ui/or-1/s8i idioms exist in the whole function (a1+68
 * command device, a1+48 data device) and neither is the bus. So the vendor
 * actually runs this bus with enable_internal_pullup = 0 (relying on
 * external pull-ups on GPIO 18/10 -- required anyway to reach the
 * BYOK_LCD_I2C_SPEED_HZ = 1 MHz below; the S3's own internal pull-ups,
 * ~45 kOhm, cannot meet that rise time). Identical in 1.1.3
 * (0x114B4A/0x114B5A). CONFIRMED (bus-simulation pass, both images).
 *
 * The VALUE below is left at 1, unchanged from every prior release: an
 * internal pull-up in parallel with a correct external one only lowers the
 * effective resistance and is very unlikely to be harmful, but flipping it
 * to vendor-exact 0 is only safe once external pull-ups are confirmed
 * present (implied, not measured) -- an owner-gated decision, not a
 * transport-correctness one. Only the [CONFIRMED] grade and its rationale
 * were wrong; that is what the 0.1.8 correction fixes. */
#define BYOK_LCD_I2C_INTERNAL_PULLUP    1

/* [CONFIRMED] device configs are i2c_device_config_t (20 bytes, 5 words:
 * dev_addr_length@0, device_address@4 (u16), scl_speed_hz@8, scl_wait_us@12,
 * flags@16). Command device: 0x1154A5 movi.n a9,56 ; 0x1154A7 s16i a9,a1,56
 * (base a1+52, +4). Data device: 0x1154CD movi.n a9,57 ; 0x1154CF s16i a9,
 * a1,36 (base a1+32, +4). Both 7-bit (dev_addr_length = 0 from the zero loop).
 *
 * 0x38 / 0x39 differ only in bit 0 = the UC1611-family "CD" (command/data)
 * bit that is folded into the I2C slave address. */
#define BYOK_LCD_I2C_ADDR_CMD           0x38
#define BYOK_LCD_I2C_ADDR_DATA          0x39

/* [CONFIRMED] 0x11549F l32r a13,=0x000F4240 (=1000000) stored to both device
 * structs at +8 (0x1154AD, 0x1154D2). */
#define BYOK_LCD_I2C_SPEED_HZ           1000000

/* [CONFIRMED] both device structs get flags |= 1 (disable_ack_check):
 * cmd 0x11549A/0x1154AA, data 0x1154BD/0x1154D4. */
#define BYOK_LCD_I2C_DISABLE_ACK_CHECK  1

/* [CONFIRMED] sim_send() @0x1153FC: movi a13,1000 then
 * i2c_master_transmit(dev, buf, 1, 1000). Every LCD byte is its own
 * one-byte I2C transaction — there is no bulk write anywhere in the image
 * (exhaustive xref of i2c_master_transmit VA 0x42151DD8 finds only
 * 0x115413/0x115426 (this function), 0x0DB884 (TUSB320) and 0x117586
 * (PCF8563)). */
#define BYOK_LCD_I2C_TIMEOUT_MS         1000
#define BYOK_LCD_I2C_BYTES_PER_XFER     1

/* ==========================================================================
 * 2. I2C BUS 1 — TUSB320 (USB-C CC) + PCF8563 (RTC)
 * ==========================================================================
 * Recovered from POWER_APP::init() @ file 0x0DB8C8 (VA 0x4203B8C8),
 * ./main/system/power_app.cpp:105/112.
 */

/* [CONFIRMED] 0x0DB8E9 movi.n a12,1 ; 0x0DB8EE s32i.n a12,a1,0  -> I2C_NUM_1 */
#define BYOK_SYS_I2C_PORT               1

/* [CONFIRMED] 0x0DB8F6 movi.n a12,8  ; 0x0DB8FA s32i.n a12,a1,4  (sda) */
#define BYOK_SYS_I2C_SDA_GPIO           8

/* [CONFIRMED] 0x0DB8F0 movi.n a12,9  ; 0x0DB8F4 s32i.n a12,a1,8  (scl) */
#define BYOK_SYS_I2C_SCL_GPIO           9

/* [CONFIRMED] 0x0DB8E5 movi.n a12,11 ; 0x0DB8E7 s32i.n a12,a1,12 -> XTAL */
#define BYOK_SYS_I2C_CLK_SRC_XTAL       1

/* [CONFIRMED] 0x0DB8FE movi.n a12,7 ; 0x0DB908 s8i a12,a1,16 */
#define BYOK_SYS_I2C_GLITCH_IGNORE_CNT  7

/* [CONFIRMED] the flags word (a1+28) is written 0 at 0x0DB906 and never
 * OR'd with 1 -> enable_internal_pullup = 0. This bus needs EXTERNAL
 * pull-ups (and it has two devices on it, so it almost certainly has them). */
#define BYOK_SYS_I2C_INTERNAL_PULLUP    0

/* [CONFIRMED] 0x0DB936 movi a9,0x60 ; 0x0DB939 s16i a9,a1,36 (base a1+32,+4)
 * 0x0DB93F l32r a9,=0x00061A80 (=400000) ; 0x0DB94C s32i.n a9,a1,40 */
#define BYOK_TUSB320_I2C_ADDR           0x60
#define BYOK_TUSB320_I2C_SPEED_HZ       400000

/* [CONFIRMED] the ONLY TUSB320 register the firmware ever touches is 0x09:
 * read 1 byte, then write {0x09, val|0x10} to acknowledge the interrupt
 * (same code in 1.1.0 at 0x0DB2xx; docs/usb-and-boot-modes.md has the prose).
 * Register 0x0A (MODE_SELECT) is NEVER written: the CC role is strap-fixed. */
#define BYOK_TUSB320_REG_STATUS         0x09
#define BYOK_TUSB320_INT_CLEAR_BIT      0x10

/* [CONFIRMED] PCF8563::init(i2c_master_bus_handle_t) @ file 0x117354:
 * 0x117361 movi.n a8,81 ; 0x117363 s16i a8,a1,4      -> address 0x51
 * 0x117369 l32r a8,=0x00061A80 ; 0x117370 s32i a8,a1,8 -> 400 kHz
 * [CONFIRMED] it is on BUS 1: SYS_APP init calls POWER_APP::init (0x0B6A99),
 * then the bus-handle getter 0x4203BB9C (returns *0x3FCA3D6C, the TUSB320
 * bus handle) at 0x0B6A9C, then the RTC init 0x4201769C at 0x0B6A9F with
 * that value still in a10. */
#define BYOK_PCF8563_I2C_ADDR           0x51
#define BYOK_PCF8563_I2C_SPEED_HZ       400000

/* ==========================================================================
 * 3. DISPLAY PANEL — UC1611/UC1611s-class, 240 x 80, 1 bit/pixel
 * ========================================================================== */

/* [CONFIRMED] geometry.
 *  - Clamp constants in the flush routine @0x112B80:
 *      0x112BAF movi a12,239 ; 0x112BB2 bltu a12,a9  (x clamp, 0..239)
 *      0x112BBF movi.n a13,79 ; 0x112BC1 bltu a13,a8 (y clamp, 0..79)
 *    and the "invalidate whole screen" helper @0x112C83:
 *      0x112C9C movi a10,-17(0xEF=239) -> *X_MAX
 *      0x112CA5 movi a10,79            -> *Y_MAX
 *  - Framebuffer clear in the display init @0x112D42:
 *      l32r a12,=0x00000960 (2400) ; l32r a10,=0x3FCA41DC ; memset
 *    2400 = 240 * 80 / 8.  (Same 2400 literal in 1.1.3 @ file 0x1123F2.)
 *  - Page arithmetic in the flush loop @0x112BF0:
 *      srli a9,a8,3 ; slli a8,a9,4 ; sub a8,a8,a9 ; slli a8,a8,4
 *      => byte index = (y>>3)*240 + x     (i.e. stride 240, 8 rows/page)
 *  - "Set COM End" in the init table: cmd 0xF1, data 0x4F (=79) -> 80 COMs.
 */
#define BYOK_DISPLAY_WIDTH              240
#define BYOK_DISPLAY_HEIGHT             80
#define BYOK_DISPLAY_BPP                1
#define BYOK_DISPLAY_PAGE_HEIGHT        8       /* rows per page byte        */
#define BYOK_DISPLAY_PAGES              10      /* 80 / 8                    */
#define BYOK_DISPLAY_STRIDE             240     /* bytes per page row        */
#define BYOK_DISPLAY_FB_BYTES           2400    /* 240 * 80 / 8              */

/* [CONFIRMED] framebuffer byte index; bit n of a byte is row (page*8 + n)
 * — the standard UC1611 page layout (LSB = topmost row of the page). The
 * bit order itself is [STRONGLY INDICATED]: it is the controller's fixed
 * RAM format, not something the driver chooses. */
#define BYOK_DISPLAY_FB_INDEX(x, y) \
    ((size_t)(((y) >> 3) * BYOK_DISPLAY_STRIDE) + (size_t)(x))

/* [CONFIRMED] the vendor clears the framebuffer to 0xFF at init
 * (0x112D37 movi a8,-1 -> the fill byte, then memset at 0x112D4E) and the
 * init sequence ends with command 0xA7 = "Set Inverse Display ON". So the
 * panel is driven inverted and 0xFF is the *blank* pattern. */
#define BYOK_DISPLAY_FB_BLANK_BYTE      0xFF
#define BYOK_DISPLAY_INVERSE_ON         1

/* [STRONGLY INDICATED] Controller family.
 * The vendor's own component is named "gclcd1611" (tag string @ file 0x02020C)
 * and every opcode in its init/window/contrast paths lands exactly in the
 * UC1611/UC1611s command map (0x81 VBIAS+param, 0x88-0x8B RAM address
 * control, 0xA0-0xA3 line rate, 0xA6/0xA7 inverse, 0xC0-0xC7 LCD mapping,
 * 0xC8 N-line inversion+param, 0xE8-0xEB bias ratio, 0xF1/0xF2/0xF3 COM-end /
 * partial start / partial end + param, 0xF4-0xF7 window program + param,
 * 0xF8/0xF9 window program mode). Double-byte command parameters are written
 * through the *data* address 0x39, which is exactly how a UC1611 expects the
 * second byte of a double-byte command (CD = 1). */
#define BYOK_DISPLAY_CTRL_UC1611_CLASS  1

/* --------------------------------------------------------------------------
 * 3.1 Reset / bring-up preamble  [CONFIRMED]  gclcd_hw_init @ file 0x112E54
 * --------------------------------------------------------------------------
 *   0x112E57  call 0x112F34 -> sim_reset()   (I2C bus + 2 devices; no GPIO)
 *   0x112E63  delay 150 ms
 *   0x112E69  send(CMD , 0xE1)
 *   0x112E72  send(DATA, 0xE2)
 *   0x112E7B  delay 150 ms
 *   0x112E81  replay the 21-entry table below
 */
#define BYOK_LCD_RESET_DELAY_MS         150
#define BYOK_LCD_RESET_CMD_BYTE         0xE1    /* see UNKNOWN guard below   */
#define BYOK_LCD_RESET_DATA_BYTE        0xE2    /* UC1611 "Set System Reset" */

/* --------------------------------------------------------------------------
 * 3.1a byok-mod 0.1.2 addition — a genuine UC1611 System Reset, sent as an
 * actual COMMAND (CD=0), before the vendor preamble above.
 * --------------------------------------------------------------------------
 * Not part of the vendor image: BYOK_LCD_RESET_DATA_BYTE above is the same
 * 0xE2 byte value, but sent to the DATA address as the second byte of the
 * vendor's own (UNKNOWN-meaning) 0xE1/0xE2 pair -- it is never issued as a
 * command by either firmware (docs/recovery.md §6,
 * STRONGLY INDICATED). Gated behind CONFIG_BYOK_DISPLAY_TRUE_SYSTEM_RESET
 * (component Kconfig, default y); strictly additive in front of the
 * unchanged vendor sequence -- see byok_display.c's lcd_hw_reset_and_init().
 */
#define BYOK_LCD_TRUE_RESET_CMD_BYTE      0xE2    /* UC1611 "Set System Reset", CD=0 (a real command,
                                                       not the vendor pair's DATA parameter above) */
#define BYOK_LCD_TRUE_RESET_PRE_DELAY_MS  5       /* let the bus settle before the reset command */
#define BYOK_LCD_TRUE_RESET_SETTLE_MS     150     /* controller/charge-pump settle after the reset;
                                                       mirrors BYOK_LCD_RESET_DELAY_MS above rather
                                                       than inventing a new number (docs/recovery.md
                                                       §6) */

/* [CONFIRMED — strong negative] There is NO display reset GPIO and NO
 * display-related GPIO toggle anywhere. sim_reset() (file 0x115430) calls
 * exactly i2c_new_master_bus + i2c_master_bus_add_device x2 and logs
 * "reset"; it touches no GPIO API. The whole panel bring-up is I2C-only. */
#define BYOK_LCD_HAS_RESET_GPIO         0

/* --------------------------------------------------------------------------
 * 3.2 Panel initialisation sequence — interoperability specification
 * --------------------------------------------------------------------------
 * This is the exact 21-entry {is_data, byte} bring-up sequence this panel
 * requires, recovered from the stock firmware's own initialisation routine
 * and cross-checked opcode-by-opcode against the published UltraChip UC1611
 * command table (docs/display.md's datasheet cross-check). It is the
 * complete interface contract needed to bring the panel up correctly:
 * anyone driving this controller family over this bus needs this exact
 * byte sequence, in this exact order, regardless of what firmware issues
 * it. Reproduced byte-for-byte across both vendor firmware releases this
 * project examined, and replayed unmodified by this project's own driver
 * (byok_display.c) for that reason.
 *
 * Every entry below is decoded against the real UC1611 command table where
 * a match exists; three entries have no match in the published Rev-0.81
 * table (see docs/display.md for the full cross-check and why that is
 * expected — this board uses an I2C-capable sibling part, "UC1611s", that
 * revision does not cover) and are marked UNKNOWN. Replay every byte
 * verbatim regardless of whether its meaning is known — do not "translate"
 * or omit an unresolved opcode; see docs/display.md for the reasoning.
 */
#define BYOK_LCD_INIT_SEQ_LEN           21
#define BYOK_LCD_INIT_SEQ                                                      \
    /* is_data, byte    decoded (UC1611 command map)                        */ \
    { 0, 0xC9 }, /* UNKNOWN opcode -- double-byte cmd, param below           */ \
    { 1, 0xAC }, /*   ...its parameter (also UNKNOWN)                       */ \
    { 0, 0x2D }, /* Set Pump Control      (0x2C|PC=1)                       */ \
    { 0, 0x24 }, /* Set Temp Compensation (0x24|TC=0, -0.00 %/degC)         */ \
    { 0, 0xE9 }, /* Set LCD Bias Ratio    (0xE8|BR=1)                       */ \
    { 0, 0xA3 }, /* Set Line Rate         (0xA0|LC=3)                       */ \
    { 0, 0xC4 }, /* Set LCD Mapping Ctrl  (0xC0|MY=1,MX=0,MSF=0)            */ \
    { 0, 0x88 }, /* Set RAM Address Ctrl  (0x88|AC=0)                       */ \
    { 0, 0x81 }, /* Set VBIAS Potentiometer                                 */ \
    { 1, 0x50 }, /*   ...contrast register = 0x50 (80)                      */ \
    { 0, 0xC8 }, /* UNKNOWN opcode -- double-byte cmd, param below          */ \
    { 1, 0x39 }, /*   ...its parameter (also UNKNOWN)                      */ \
    { 0, 0xF1 }, /* Set COM End                                             */ \
    { 1, 0x4F }, /*   ...last COM = 79  => 80 rows                          */ \
    { 0, 0xF2 }, /* Set Partial Display Start                               */ \
    { 1, 0x00 }, /*   ...= 0                                                */ \
    { 0, 0xF3 }, /* Set Partial Display End                                 */ \
    { 1, 0x4F }, /*   ...= 79                                               */ \
    { 0, 0x85 }, /* Set Partial Display Control (0x84-0x87 family, exact opcode match) */ \
    { 0, 0x95 }, /* UNKNOWN opcode -- no match in the public command table */ \
    { 0, 0xA7 }  /* Set Inverse Display ON                                  */

/* --------------------------------------------------------------------------
 * 3.2.1 DISPLAY-ENABLE STEP (0.1.8)  [CONFIRMED bytes/position, STRONGLY
 * INDICATED meaning]  gdisp_init() @ file 0x112CF4, the pair @ file
 * 0x112EC8 (1.1.3: 0x112578) -- docs/display.md Sec.5.
 * --------------------------------------------------------------------------
 * The vendor's full bring-up order is: gclcd_hw_init() (reset preamble +
 * this 21-entry table) -> blank the framebuffer -> ONE full-screen refresh
 * (window_program + all 2400 data bytes) -> THIS pair, sent exactly once,
 * immediately after that first frame is in display RAM -> contrast shadow
 * reset -> flush-if-dirty (a no-op here, since the refresh above already
 * cleared the dirty rect).
 *
 * CMD 0xC9 is the same double-byte opcode as this table's own first entry
 * above, whose parameter there is 0xAC; this second, later send changes
 * ONLY bit 0 of that parameter (0xAC -> 0xAD) and is the only other 0xC9
 * site in either vendor image. A double-byte command sent once, last, right
 * after the first RAM load, toggling one bit relative to its init-time
 * value, is the canonical shape of a "display enable, after RAM is valid"
 * step -- STRONGLY INDICATED, not CONFIRMED: 0xC9 has no row in the public
 * UltraChip UC1611 Rev-0.81 command table (docs/display.md Sec.10), so the
 * decode cannot be checked against a datasheet. Replay verbatim regardless
 * of meaning, same policy as the init table above and hw_config.h Sec.13's
 * other UNKNOWN opcodes.
 *
 * This is the one action in the vendor's display bring-up byok-mod omitted
 * through 0.1.7 (confirmed absent by grep of every prior release's source
 * for byte 0xAD). It is unrelated to I2C transport, which the same analysis
 * pass confirmed byok-mod already reproduces exactly (Sec.0/Sec.1/Sec.12) --
 * this is a missing PROTOCOL step, not a transport fix. */
#define BYOK_LCD_CMD_DISPLAY_ENABLE     0xC9
#define BYOK_LCD_DATA_DISPLAY_ENABLE    0xAD

/* --------------------------------------------------------------------------
 * 3.2.2 SCROLL LINE / VERTICAL DISPLAY ORIGIN (0.1.9)  [STRONGLY INDICATED]
 * docs/troubleshooting.md §1/§2 -- first-light,
 * direct observation on real hardware on 0.1.8 measured the whole image
 * displayed 15 rows too high, not page-granular and not mirrored. An
 * exhaustive scan
 * of every `call8 sim_send` site in the 1.1.0 IROM (20 sites, all
 * accounted for -- see that document Sec.4.2) found NO byte in 0x40-0x7F
 * ever sent by the vendor, at init or at runtime; the 21-entry DROM init
 * table (Sec.3.2 above) contains none either. 0x40-0x7F is the UC1611 "Set
 * Scroll Line" pair -- the controller's only row-granular vertical display
 * origin -- and neither firmware ever programs it, so both inherit
 * whatever the register holds coming out of its own power-on reset, which
 * on this unit is 15 (there is no display reset GPIO -- BYOK_LCD_HAS_
 * RESET_GPIO 0 above -- and CONFIG_BYOK_DISPLAY_TRUE_SYSTEM_RESET=n, so
 * nothing else in bring-up clears it either).
 *
 * NOT part of the vendor sequence -- strictly additive, same policy as
 * 0.1.2's TRUE_SYSTEM_RESET addition. Grade is STRONGLY INDICATED rather
 * than CONFIRMED because Set Scroll Line is the only row-granular vertical
 * origin in the public UC1611 Rev-0.81 map, but this board is a UC1611s
 * (I2C variant, outside that revision) and three opcodes (0xC9, 0x95,
 * 0xE1) remain unplaced -- see docs/troubleshooting.md Sec.2 for the full
 * rationale. */
#define BYOK_LCD_CMD_SCROLL_LINE_MSB    0x50    /* 0x50 | SL[7:4] */
#define BYOK_LCD_CMD_SCROLL_LINE_LSB    0x40    /* 0x40 | SL[3:0] */
#define BYOK_LCD_SCROLL_LINE_RESET      0       /* SL = 0 -> RAM row 0 at glass row 0 */

/* --------------------------------------------------------------------------
 * 3.3 PARTIAL-UPDATE WINDOW  [CONFIRMED]  window_program @ file 0x112DCC
 * --------------------------------------------------------------------------
 * Called as window_program(x_start, y_start, x_end, y_end) — all pixel
 * coordinates; the y arguments are converted to pages inside:
 *   cmd 0x89                       Set RAM Address Control, AC = 1
 *   cmd 0xF8                       Set Window Program Mode = OFF
 *   cmd 0xF5, data (y_start >> 3)  window start PAGE      (0x112DEF srli 3)
 *   cmd 0xF7, data (y_end   >> 3)  window end   PAGE      (0x112E02 srli 3)
 *   cmd 0xF4, data (x_start)       window start COLUMN
 *   cmd 0xF6, data (x_end)         window end   COLUMN
 *   cmd 0xF9                       Set Window Program Mode = ON
 *   cmd 0x01                       [POSSIBLE decode] column-address LSB = 1
 */
#define BYOK_LCD_CMD_RAM_ADDR_CTRL      0x89
#define BYOK_LCD_CMD_WIN_MODE_OFF       0xF8
#define BYOK_LCD_CMD_WIN_PAGE_START     0xF5
#define BYOK_LCD_CMD_WIN_PAGE_END       0xF7
#define BYOK_LCD_CMD_WIN_COL_START      0xF4
#define BYOK_LCD_CMD_WIN_COL_END        0xF6
#define BYOK_LCD_CMD_WIN_MODE_ON        0xF9
#define BYOK_LCD_CMD_WIN_TRAILER        0x01

/* [CONFIRMED] flush semantics, gdisp_flush @ file 0x112B80:
 *   if (y_max < y_min) return;  if (x_max < x_min) return;
 *   clamp x_max to 239 and y_max to 79 (writing the clamp back);
 *   window_program(x_min, y_min, x_max, y_max);
 *   span = x_max - x_min;  y = y_min & 0xF8;      // page-align the start
 *   for (; y <= y_max; y += 8)
 *       for (p = FB + (y>>3)*240 + x_min; ; p++) { send(DATA, *p);
 *                                                 if (p == p_end) break; }
 * i.e. the dirty rectangle is sent page-row by page-row, one I2C byte each,
 * and the y start is always snapped DOWN to a page boundary. */
#define BYOK_LCD_WINDOW_Y_PAGE_ALIGNED  1

/* --------------------------------------------------------------------------
 * 3.4 CONTRAST  [CONFIRMED]  set_contrast @ file 0x112EF8
 * --------------------------------------------------------------------------
 *   send(CMD , 0x81);
 *   send(DATA, (uint8_t)((pct * 255) / 99));   // 0x112F11 slli/sub = *255,
 *                                              // 0x112F17 muluh 0xA57EB503,
 *                                              // 0x112F1C >>6  == /99
 * The UI clamps the percentage first: max(v,0), min(v,99)  (0x112DB9/0x112DBF
 * in the contrast up/down handler @0x112DA4). Stored in RAM at 0x3FCA41D4 and
 * in NVS under key "CONTR".
 */
#define BYOK_LCD_CMD_SET_CONTRAST       0x81
#define BYOK_LCD_CONTRAST_PCT_MIN       0
#define BYOK_LCD_CONTRAST_PCT_MAX       99
#define BYOK_LCD_CONTRAST_REG_DEFAULT   0x50    /* from the init table       */
#define BYOK_LCD_CONTRAST_PCT_TO_REG(p) ((uint8_t)(((unsigned)(p) * 255u) / 99u))
#define BYOK_LCD_NVS_KEY_CONTRAST       "CONTR"

/* --------------------------------------------------------------------------
 * 3.5 TEXT CURSOR / FONT METRICS  [CONFIRMED]  gsetcpos @ file 0x1135E4
 * --------------------------------------------------------------------------
 * gsetcpos(col, row) converts character cells to pixels using the *current
 * font*, whose width/height live at byte offsets +18/+19 of the graphics
 * context struct:
 *      px = font_w * col                       (0x113608 mul16u)
 *      py = (row + 1) * font_h - 1             (0x1135F3/0x1135FC/0x11360E)
 * so the text baseline is the BOTTOM row of the cell. Log line:
 *   "gsetcpos: xpos=%d, ypos=%d, xs=%d, ys=%d"  (@ file 0x0201C8)
 * The compiled-in default font is 6 x 8 (glyph records in DROM around
 * file 0x06D600 all start with the two bytes 06 08). */
#define BYOK_LCD_DEFAULT_FONT_W         6
#define BYOK_LCD_DEFAULT_FONT_H         8
#define BYOK_LCD_TEXT_COLS_DEFAULT      (BYOK_DISPLAY_WIDTH  / BYOK_LCD_DEFAULT_FONT_W)  /* 40 */
#define BYOK_LCD_TEXT_ROWS_DEFAULT      (BYOK_DISPLAY_HEIGHT / BYOK_LCD_DEFAULT_FONT_H)  /* 10 */

/* [CONFIRMED] SD fonts are ".byf" files under /SDCARD/Fonts. Magic is the
 * 4 ASCII bytes "BYFN" (little-endian word 0x4E465942, literal @ file
 * 0x111F50, compared at 0x1115FD after a manual 4-byte big-endian assembly
 * at 0x1115DC..0x1115FA). Log: "Loaded SD font '%s' %ux%u, %lu symbols,
 * %u cp ranges". */
#define BYOK_BYF_MAGIC                  "BYFN"
#define BYOK_BYF_MAGIC_LE32             0x4E465942u
#define BYOK_FONT_DIR                   "/SDCARD/Fonts"

/* ==========================================================================
 * 4. BACKLIGHT  (LEDC)  [CONFIRMED]  backlight_init @ file 0x0CC0CC
 * ==========================================================================
 * Call site: SYS_APP init @ 0x0B6DD3..0x0B6DDA
 *   movi.n a10,2 ; l32r a11,=0x00001388 (5000) ; movi.n a12,13 ;
 *   call8 0x4202C0CC        =>  backlight_init(gpio=2, freq_hz=5000, res=13)
 * Identical in 1.1.3 (call site 0x0B61AB -> 0x4202B540).
 */
#define BYOK_BACKLIGHT_GPIO             2
#define BYOK_BACKLIGHT_LEDC_MODE        0       /* LEDC_LOW_SPEED_MODE       */
#define BYOK_BACKLIGHT_LEDC_TIMER       0       /* struct +8 = 0             */
#define BYOK_BACKLIGHT_LEDC_CHANNEL     0       /* struct +8 = 0             */
#define BYOK_BACKLIGHT_PWM_FREQ_HZ      5000
#define BYOK_BACKLIGHT_DUTY_RES_BITS    13
#define BYOK_BACKLIGHT_DUTY_MAX         ((1u << BYOK_BACKLIGHT_DUTY_RES_BITS) - 1u) /* 8191 */

/* [CONFIRMED] ledc_channel_config_t built at 0x0CC17F..0x0CC198:
 *   +0 gpio_num = 2, +12 intr_type = 1 (LEDC_INTR_FADE_END), all other
 *   fields zeroed by the 8-word loop at 0x0CC186.
 * ledc_timer_config_t at 0x0CC144: speed_mode 0, duty_resolution 13,
 *   timer_num 0, freq_hz 5000, clk_cfg 0 (LEDC_AUTO_CLK), deconfigure 0. */
#define BYOK_BACKLIGHT_USE_FADE         1
#define BYOK_BACKLIGHT_FADE_MS_DEFAULT  1000    /* 0x0CC23A movi a11,1000    */
#define BYOK_BACKLIGHT_FADE_MS_BUTTON   500     /* 0x0B7550 movi a11,500     */

/* [CONFIRMED] duty = pct * DUTY_MAX / 100, pct clamped to <=100
 * (0x0CBFF3 movi a8,100 ; 0x0CBFF6 minu ; 0x0CBFF9 mull ;
 *  0x0CBFFC l32r 0x51EB851F ; muluh ; 0x0CC007 srli 5  == /100). */
#define BYOK_BACKLIGHT_PCT_TO_DUTY(p) \
    ((uint32_t)(((uint32_t)(p) * BYOK_BACKLIGHT_DUTY_MAX) / 100u))

/* [CONFIRMED] the five brightness levels the BRIGHTNESS button cycles
 * through. DROM table @ file 0x038824 (VA 0x3C198824): 00 04 14 32 64.
 * The handler @0x0B74A8 maps the *current* percentage to an index
 * (0->1, 4->2, 20->3, 50->4, else 5), takes it mod 5 (0x0B7507 magic
 * 0xCCCCCCCD >>2, *5, sub) and indexes this table (0x0B7516 l32r
 * =0x3C198824 ; add ; l8ui). Persisted in NVS under key "BLBR". */
#define BYOK_BACKLIGHT_LEVEL_COUNT      5
#define BYOK_BACKLIGHT_LEVELS_PCT       { 0, 4, 20, 50, 100 }
#define BYOK_BACKLIGHT_NVS_KEY          "BLBR"

/* ==========================================================================
 * 5. UART LINK TO THE ESP32-PICO-MINI-02 (Bluetooth keyboard host)
 * ==========================================================================
 * Generic initialiser @ file 0x0DBBA8 (VA 0x4203BBA8):
 *   uart_driver_install(port, 0x800, 0, 0, NULL, 0)     (0x0DBBDD)
 *   uart_param_config(port, &cfg)                        (0x0DBBE4)
 *   uart_set_pin(port, tx, rx, -1, -1)                   (0x0DBBF1)
 * Two call sites, both with the SAME arguments:
 *   0x0BE58D  HID_BT bring-up   (task "hid_bt_protocol")
 *   0x0BA292  UPDATE_APP        (PICO firmware update over the same link)
 *   -> a11=1 (port), a12=36 (tx), a13=37 (rx), a14=0x000E1000 (921600)
 */
#define BYOK_PICO_UART_PORT             1       /* UART_NUM_1                */
#define BYOK_PICO_UART_TX_GPIO          36      /* S3 TX  -> PICO RX         */
#define BYOK_PICO_UART_RX_GPIO          37      /* S3 RX  <- PICO TX         */
#define BYOK_PICO_UART_RTS_GPIO         (-1)    /* UART_PIN_NO_CHANGE        */
#define BYOK_PICO_UART_CTS_GPIO         (-1)    /* UART_PIN_NO_CHANGE        */
#define BYOK_PICO_UART_BAUD             921600
#define BYOK_PICO_UART_DATA_BITS        8       /* cfg+4 = 3 = UART_DATA_8_BITS */
#define BYOK_PICO_UART_PARITY_NONE      1       /* cfg+8 = 0                 */
#define BYOK_PICO_UART_STOP_BITS        1       /* cfg+12 = 1                */
#define BYOK_PICO_UART_FLOWCTRL_NONE    1       /* cfg+16 = 0                */
#define BYOK_PICO_UART_SRC_CLK_APB      1       /* cfg+24 = 4 = SOC_MOD_CLK_APB */
#define BYOK_PICO_UART_RX_BUF_BYTES     2048    /* 0x0DBBC7 l32r =0x800      */
#define BYOK_PICO_UART_TX_BUF_BYTES     0

/* --- inter-MCU handshake lines ------------------------------------------- */

/* [CONFIRMED] file 0x0DBE58: gpio_set_level(17,1); delay 10 ms;
 * gpio_set_level(17,0).  GPIO 17 is configured as an OUTPUT and driven LOW
 * at boot (SYS_APP init, mask 0x00020000 @0x0B69DD, level 0 @0x0B69FE), so
 * idle = LOW and the reset is an active-HIGH 10 ms pulse. The only caller is
 * the PICO updater (0x0BA29B), immediately after the UART is opened. */
#define BYOK_PICO_RESET_GPIO            17
#define BYOK_PICO_RESET_ACTIVE_HIGH     1
#define BYOK_PICO_RESET_PULSE_MS        10

/* [STRONGLY INDICATED] GPIO 35 — S3 -> PICO request / "ready-out".
 * Raised (level 1) at 0x0C06DE immediately before the code waits for the
 * PICO-ready event and logs "Pico is ready to receive"; cleared at 0x0C0823,
 * 0x0BADA2; pulsed 1-then-0 at 0x0BB0FA/0x0BB10A in the updater. The PICO
 * image's own string "Timeout waiting for READY_IN to rise" is the matching
 * far end. OUTPUT, driven LOW at boot. */
#define BYOK_PICO_REQ_GPIO              35

/* [STRONGLY INDICATED] GPIO 47 — S3 -> PICO transfer strobe.
 * Set to 1 at 0x0C0843 just before a framed send, back to 0 at 0x0C0869
 * right after it; also driven with a computed level at 0x0BAE69/0x0BAE8F in
 * the updater. OUTPUT, driven LOW at boot. */
#define BYOK_PICO_STROBE_GPIO           47

/* [CONFIRMED] GPIO 46 — PICO -> S3 READY, active HIGH.
 * gpio_config: INPUT, pull-DOWN, GPIO_INTR_ANYEDGE (mask hi 0x00014000 with
 * GPIO 48, @0x0B69C5..0x0B69DA). ISR @VA 0x40376E08 (file 0x09A728):
 * for pin 46 it posts 4 (bit 2) when the level is HIGH and 8 (bit 3) when
 * LOW. device_event_task @0x0B6F6C turns bit 2 into queue value 2, and the
 * consumer at 0x0C06F4 logs "Pico is ready to receive" on exactly that. */
#define BYOK_PICO_READY_IN_GPIO         46
#define BYOK_PICO_READY_IN_ACTIVE_HIGH  1

/* [CONFIRMED wiring, UNKNOWN meaning] GPIO 48 — second any-edge input,
 * configured identically to 46 (INPUT, pull-down, ANYEDGE). The ISR maps it
 * to queue value 0 when HIGH and 1 when LOW (nsau/srli/addi idiom at
 * 0x09A743..0x09A749); no consumer in the image reacts to 0 or 1. */
#define BYOK_GPIO48_AUX_INPUT           48

/* [CONFIRMED — strong negative] There is NO PICO IO0/BOOT strap GPIO and no
 * ESP32-ROM-bootloader entry sequence in this image. The PICO firmware
 * update is an APPLICATION-level protocol over the same 921600 baud UART
 * ("Checking for pico update", "Sent %d bytes to PICO", "PICO transfer
 * complete", "PICO_INFO: %s"), preceded only by the GPIO 17 reset pulse.
 * Every gpio_set_level() call site in the whole image was enumerated; the
 * complete set of pins written is {17, 35, 39, 42, 47}. */
#define BYOK_PICO_HAS_IO0_STRAP         0

/* [CONFIRMED wiring, UNKNOWN function] GPIO 39 — see Sec.13's
 * BYOK_GPIO39_FUNCTION guard for the (still unresolved) semantic question.
 * This constant is only the pin number, added 0.1.6 so
 * CONFIG_BYOK_STOCK_GPIO_PARITY (firmware/s3/main/app_main.c) has something
 * to name — it is configured OUTPUT and driven LOW once at boot by the SAME
 * single gpio_config() call site that covers 17/35/42/47 (mask hi
 * 0x00008488 → bit 7 → GPIO 32+7 = 39, file 0x0B69F6/set at 0x0B6A1E) and is
 * never written again anywhere in either firmware version — exhaustive
 * gpio_set_level() enumeration, Sec.12. Unlike 35/47 above (both STRONGLY
 * INDICATED PICO-handshake lines) this pin has NO other candidate role
 * anywhere in either image; the "panel supply/level-shifter enable"
 * hypothesis is recorded as POSSIBLE, not CONFIRMED, in
 * docs/recovery.md §6 and docs/pinout.md §5 — driving
 * it low at boot is stock-parity, not a claimed fix. */
#define BYOK_GPIO39_PIN                 39

/* ==========================================================================
 * 6. SD CARD (SDMMC, 1-bit)  [CONFIRMED]  mount wrapper @ file 0x0D0E8C
 * ==========================================================================
 * sdmmc_slot_config_t built on the stack at a1+192:
 *   +0  clk = 5    (0x0D0ECF s32i a5,a1,192, a5=5)
 *   +4  cmd = 3    (0x0D0ED2 s32i a7,a1,196, a7=3)
 *   +8  d0  = 4    (0x0D0EC1 s32i a8,a1,200, a8=4)
 *   +40 cd  = -1 ; +44 wp = -1  (0x0D0EC6/0x0D0EC9)
 *   +48 width = 1  (0x0D0ECC s8i a2,a1,240, a2=1)
 *   +52 flags = 0  (0x0D0ED5)
 * sdmmc_host_t memcpy'd from the SDMMC_HOST_DEFAULT() template at
 * VA 0x3C198AD8 (flags 0x37, slot 1, io_voltage 3.3f), with
 *   max_freq_khz overridden to 40000  (0x0D0EB8 l32r =0x9C40, 0x0D0ED8).
 * esp_vfs_fat_sdmmc_mount("/SDCARD", host, slot, mount, &card) @0x0D0F0A.
 */
#define BYOK_SD_SLOT                    1       /* SDMMC_HOST_SLOT_1         */
#define BYOK_SD_CLK_GPIO                5
#define BYOK_SD_CMD_GPIO                3
#define BYOK_SD_D0_GPIO                 4
#define BYOK_SD_BUS_WIDTH               1
#define BYOK_SD_SLOT_CD_GPIO            (-1)    /* not used by the driver    */
#define BYOK_SD_SLOT_WP_GPIO            (-1)
#define BYOK_SD_MAX_FREQ_KHZ            40000
#define BYOK_SD_MOUNT_POINT             "/SDCARD"
#define BYOK_SD_FORMAT_IF_MOUNT_FAILED  1       /* 0x0D0EA1 s8i a2(1),a6,112 */
#define BYOK_SD_MAX_FILES               5       /* 0x0D0EA4 s32i a5(5),a6,116*/
#define BYOK_SD_ALLOC_UNIT_SIZE         16384   /* 0x0D0EAC s32i a3,a6,120   */

/* [CONFIRMED] card-detect is POLLED on a separate pin, not wired to the
 * SDMMC driver: gpio_config INPUT + pull-UP (mask hi 0x00000100 @0x0B6A5E),
 * then gpio_get_level(40) @0x0B6A76 (discarded) and @0x0B6D4C, where a
 * NON-ZERO reading branches to the warning "SD card missing".
 * => card present == pin LOW. */
#define BYOK_SD_DETECT_GPIO             40
#define BYOK_SD_DETECT_PRESENT_LEVEL    0
#define BYOK_SD_DETECT_INTERNAL_PULLUP  1

/* ==========================================================================
 * 7. USB
 * ==========================================================================
 * [CONFIRMED — strong negative] No GPIO is used for VBUS sense and none for
 * a USB power switch. USB attach/detach state comes exclusively from the
 * TUSB320 status register 0x09 over I2C bus 1 ("USB state: %d").
 * GPIO 19/20 (the S3's native USB D-/D+) are never named by any
 * gpio_config() in the image — an exhaustive scan of every gpio_config()
 * call site, so this is a strong negative rather than an absence of looking.
 */
#define BYOK_USB_VBUS_SENSE_GPIO        (-1)
#define BYOK_USB_POWER_SWITCH_GPIO      (-1)
#define BYOK_USB_DM_GPIO                19      /* native S3 USB, fixed pads */
#define BYOK_USB_DP_GPIO                20

/* [CONFIRMED] TUSB320 interrupt: gpio_config INPUT, no pull,
 * GPIO_INTR_NEGEDGE (mask hi 0x00000200 @0x0B6A2E..0x0B6A43); ISR registered
 * at 0x0B6CFE. The ISR maps a LOW level on pin 41 to bit 4 (0x10), which
 * device_event_task logs as "USB_INT_FALL" (@0x0B6FC7). */
#define BYOK_TUSB320_INT_GPIO           41
#define BYOK_TUSB320_INT_ACTIVE_LOW     1

/* [CONFIRMED] boot-mode word (RAM, not persisted).
 * 1.1.0: 0x3FCA39F0   (SD-missing arm 0x0B6D5A, disk-mode arm 0x0B6D91)
 * 1.1.3: 0x3FCA4318   (same two arms, relocated by the 1.1.3 build)
 * Values seen: 1 = Disk Mode (TinyUSB MSC), 3 = USB console mode
 * (USB HID host suppressed, USB-Serial/JTAG keeps the PHY), other = normal. */
#define BYOK_BOOTMODE_WORD_ADDR_110     0x3FCA39F0u
#define BYOK_BOOTMODE_WORD_ADDR_113     0x3FCA4318u
#define BYOK_BOOTMODE_NORMAL            0
#define BYOK_BOOTMODE_DISK              1
#define BYOK_BOOTMODE_USB_CONSOLE       3

/* ==========================================================================
 * 8. BUTTONS  [CONFIRMED]
 * ==========================================================================
 * SYS_APP init @0x0B69A0: l32r a10,=0x000188C0 ; movi.n a11,0 ;
 * movi.n a12,3 ; call8 buttons_init (VA 0x42049030).
 * 0x000188C0 = bits 6,7,11,15,16.
 * Inside buttons_init the third argument is an ESP-IDF gpio_pull_mode_t:
 *   pull_up_en  = ((mode & ~2) == 0)      (0x0E904D and / 0x0E9054 moveqz)
 *   pull_down_en= derived from (mode-1)   (0x0E9046/0x0E9050/0x0E9057)
 * With mode = 3 = GPIO_FLOATING both come out 0 -> NO internal pulls.
 * mode field = 1 (GPIO_MODE_INPUT), intr_type = 0 (polled by "button_task").
 * Polarity is active-LOW, proven twice: the factory-reset check takes the
 * "not pressed" branch on bnez for GPIO 6 and GPIO 16 (0x0B6AAD/0x0B6ABB).
 */
#define BYOK_BTN_PIN_MASK               0x000188C0u
#define BYOK_BTN_ACTIVE_LOW             1
#define BYOK_BTN_INTERNAL_PULLUP        0       /* board must pull up        */
#define BYOK_BTN_INTERNAL_PULLDOWN      0
#define BYOK_BTN_POLLED                 1       /* intr_type = 0             */

#define BYOK_BTN_WAKE_GPIO              6       /* front power button (SW1)  */
#define BYOK_BTN_DOWN_GPIO              7
#define BYOK_BTN_BRIGHTNESS_GPIO        11
#define BYOK_BTN_UP_GPIO                15
#define BYOK_BTN_EXECUTE_GPIO           16

/* [CONFIRMED] the runtime button ids the Lua layer receives
 * (switch @ 1.1.3 file 0x0B1050; unchanged in 1.1.0). */
#define BYOK_BTN_ID_UP                  1
#define BYOK_BTN_ID_DOWN                2
#define BYOK_BTN_ID_EXECUTE             3
#define BYOK_BTN_ID_BRIGHTNESS          4
#define BYOK_BTN_ID_WAKE                5

/* [CONFIRMED] boot-time button behaviours (docs/usb-and-boot-modes.md and
 * docs/pinout.md carry the same three findings in prose):
 *   GPIO 15 held at boot  -> USB console mode  (single sample, ~4 s in)
 *   GPIO 7  held during the updater -> update escape hatch
 *   GPIO 6 + GPIO 16 held 10 s at startup -> FACTORY RESET (erases NVS) */
#define BYOK_FACTORY_RESET_HOLD_MS      10000   /* 0x0B6AD5 l32r =0x2710     */

/* ==========================================================================
 * 9. POWER
 * ========================================================================== */

/* [CONFIRMED] power latch. GPIO 42 is configured OUTPUT and driven LOW at
 * boot (mask hi 0x00008488 @0x0B69E4, level 0 @0x0B6A26). The shutdown path
 * @0x0B68AA does gpio_set_level(42,1) and then spins forever on
 * vTaskDelay(100) (0x0B68B4..0x0B68BD) — i.e. driving it HIGH cuts power. */
#define BYOK_POWER_HOLD_GPIO            42
#define BYOK_POWER_OFF_LEVEL            1
#define BYOK_POWER_ON_LEVEL             0

/* [CONFIRMED] battery sense. POWER_APP::init @0x0DB8CB:
 *   adc1_config_width(ADC_WIDTH_BIT_12)                 (a10 = 12)
 *   adc1_config_channel_atten(channel 0, atten 3)       (a10=0, a11=3)
 *   esp_adc_cal_characterize(ADC_UNIT_1, 3, 12, ..., &chars)
 * ADC1 channel 0 is GPIO1 on the ESP32-S3. Atten 3 = ADC_ATTEN_DB_12 (11 dB
 * on older headers). Reading helper @0x0DB2BC calls adc1_get_raw. */
#define BYOK_BATT_ADC_UNIT              1       /* ADC_UNIT_1                */
#define BYOK_BATT_ADC_CHANNEL           0       /* ADC1_CHANNEL_0            */
#define BYOK_BATT_ADC_GPIO              1       /* ESP32-S3 ADC1_CH0 = GPIO1 */
#define BYOK_BATT_ADC_WIDTH_BITS        12
#define BYOK_BATT_ADC_ATTEN             3       /* ADC_ATTEN_DB_12           */

/* [CONFIRMED] charger status inputs. gpio_config INPUT + pull-UP, mask
 * 0x00003000 @0x0B6A46..0x0B6A5B. Read as a pair from getChargerStatus()
 * @0x0DB9F4, and open-coded identically at 0x0DB3A2 (monitor loop) and
 * 0x0DBA56 (begin()). Both lines are open-drain, active LOW:
 *
 *     GPIO12  GPIO13   status   meaning
 *        0       1        1     charging
 *        1       0        2     charge complete / standby
 *        1       1        0     no charger
 *
 * Which line is which is fixed by the firmware's own consumers: the status-bar
 * icon selector @0x0E7194 gives status 1 its own charging glyph (icon 5) and
 * renders status 2 as the full-battery glyph (icon 4), and SYS_APP @0x0B5F34
 * raises two different notifications for the two states. That is TP4056-class
 * CHRG/STDBY semantics. Part-level naming (TP4056) is STRONGLY INDICATED, not confirmed from a
 * schematic; the pin->state mapping itself is CONFIRMED from the image. */
#define BYOK_CHARGER_CHRG_GPIO          12
#define BYOK_CHARGER_CHRG_ACTIVE_LEVEL   0      /* LOW = charging            */
#define BYOK_CHARGER_STDBY_GPIO         13
#define BYOK_CHARGER_STDBY_ACTIVE_LEVEL  0      /* LOW = charge complete     */
#define BYOK_CHARGER_STAT_INTERNAL_PU    1      /* both need the internal PU */
#define BYOK_CHARGER_STAT_POLARITY       0      /* 0 = both active LOW       */

/* Back-compat aliases for the pre-0.1.9 neutral names. */
#define BYOK_CHARGER_STAT_A_GPIO        BYOK_CHARGER_CHRG_GPIO
#define BYOK_CHARGER_STAT_B_GPIO        BYOK_CHARGER_STDBY_GPIO

/* Derived status, matching the vendor's three-way test exactly. Same values
 * docs/protocol.md Sec.6.1's STATUS.charging / Sec.6.3 BATTERY.charging use
 * (0 no / 1 charging / 2 on external power / full). */
#define BYOK_CHARGE_STATUS_NONE          0
#define BYOK_CHARGE_STATUS_CHARGING      1
#define BYOK_CHARGE_STATUS_COMPLETE      2

/* ------------------------------------------------------------------------
 * Battery voltage maths. readBatteryVoltage() @0x0DB2BC, disassembled from
 * the stock 1.1.0 image. CONFIRMED; byte-identical in 1.1.3.
 *
 *   raw   = trimmed mean of 200 adc1_get_raw() samples
 *           (plain mean; if (max-min) > 0.1*mean, drop one min + one max
 *            and average the remaining 198)
 *   V_pin = esp_adc_cal_raw_to_voltage(raw, &chars) / 1000.0
 *   V_bat = 1.6666666 * (V_pin + 0.02) + 0.02
 *
 * NOTE: there is NO fixed volts-per-count constant in the vendor image. The
 * raw->mV step is eFuse calibration read at runtime (coeff_a/coeff_b, see
 * 0x13A4D4). A reimplementation MUST call adc_cali_create_scheme_curve_fitting()
 * (ESP32-S3 scheme) or the legacy esp_adc_cal shim - do not bake a constant.
 * Only the divider and the offsets below are image constants.
 * ------------------------------------------------------------------------ */
#define BYOK_BATT_SAMPLES               200     /* adc1_get_raw per reading  */
#define BYOK_BATT_TRIM_SPREAD_PCT       10      /* >10% spread -> drop min+max */

/* Divider is exactly 5/3 (float literal 0x3FD55555 = 1.66666663), i.e. a 2:3
 * network, V_pin = 0.6 * V_batt (e.g. 200k over 300k). Prefer the NUM/DEN
 * form in integer code; the _X1000 form is the historic name. */
#define BYOK_BATT_DIVIDER_RATIO_X1000   1667
#define BYOK_BATT_DIVIDER_NUM           5
#define BYOK_BATT_DIVIDER_DEN           3

/* Vendor calibration offsets (float literal 0x3CA3D70A = 0.02f), applied one
 * before and one after the divider multiply. Combined effect +53.3 mV.
 * Reproduce both if you want the vendor's displayed percentage, because the
 * table below was hand-tuned against this exact expression. */
#define BYOK_BATT_OFFSET_PRE_MV         20      /* added to V_pin, pre-scale  */
#define BYOK_BATT_OFFSET_POST_MV        20      /* added after the scale      */

/* mV at the pin -> mV at the cell. Integer form of the expression above. */
#define BYOK_BATT_PIN_MV_TO_BATT_MV(pin_mv)                                   \
    ((((pin_mv) + BYOK_BATT_OFFSET_PRE_MV) * BYOK_BATT_DIVIDER_NUM            \
      / BYOK_BATT_DIVIDER_DEN) + BYOK_BATT_OFFSET_POST_MV)

/* ------------------------------------------------------------------------
 * Filtering. Read off the vendor's own power_monitor task. CONFIRMED.
 * The monitor task loops every 2000 ms and reads the battery ONLY when the
 * charger status is 0. Thresholds are tested against the EMA, never against
 * an instantaneous reading.
 * ------------------------------------------------------------------------ */
#define BYOK_BATT_SAMPLE_INTERVAL_MS    2000    /* vTaskDelay(200) @100 Hz    */
#define BYOK_BATT_EMA_ALPHA_X100        25      /* ema = .75*ema + .25*new    */
#define BYOK_BATT_RING_LEN              5       /* 5-sample ring @0x3FCA3D24  */
#define BYOK_BATT_RING_INTERVAL_MS      10000   /* ring gate, 1000 ticks      */
#define BYOK_BATT_RING_DROP_STEP_MV     50      /* reject a >50 mV drop       */
#define BYOK_BATT_LOG_HYSTERESIS_MV     100     /* 0.1 V log throttle         */

/* ------------------------------------------------------------------------
 * Thresholds. Read off the vendor's own power_monitor task. CONFIRMED.
 * Latched: each event fires once, re-armed by rising above RECOVER_MV or by
 * the charger being connected.
 * ------------------------------------------------------------------------ */
#define BYOK_BATT_LOW_MV                3500    /* 0x40600000  ~7.8% on table */
#define BYOK_BATT_CRIT_MV               3200    /* 0x404CCCCD  ~2.6% on table */
#define BYOK_BATT_RECOVER_MV            3700    /* 0x406CCCCD  clears latches */
#define BYOK_BATT_CRIT_SHUTDOWN_DELAY_MS 5000   /* vTaskDelay(500) @0x0B695B  */

/* ------------------------------------------------------------------------
 * Voltage -> percent. voltageToPercent() @0x0DB750. CONFIRMED.
 * Vendor table at VA 0x3C198C58 (file 0x038C58, 1.1.0) / 0x3C1985E8 (1.1.3),
 * 21 entries of {float volts; float percent}, strictly descending in voltage.
 * voltageToPercent() @0x0DB750 clamps at both ends and linearly interpolates
 * between the bracketing pair:
 *     v >= 4.16 -> 100 ;  v <= 3.00 -> 0
 *     else  p = p[i] + (p[i+1]-p[i]) * (v - v[i]) / (v[i+1] - v[i])
 * Every float is exact in mV, so this transfers with no rounding.
 *
 * WARNING: the slope is deliberately non-uniform and NOT a smooth curve --
 * 10 mV separates 85% from 80% while 390 mV separates 5% from 0%. Copy it to
 * match the vendor's displayed number; do NOT copy it if you want a
 * well-behaved gauge.
 * ------------------------------------------------------------------------ */
#define BYOK_BATT_PCT_IS_TABLE          1       /* table+lerp, not linear     */
#define BYOK_BATT_PCT_CLAMP_HI_MV       4160    /* >= this -> 100%            */
#define BYOK_BATT_PCT_CLAMP_LO_MV       3000    /* <= this ->   0%            */
#define BYOK_BATT_PCT_TABLE_LEN         21

/* { millivolts, percent }, descending by millivolts. */
#define BYOK_BATT_PCT_TABLE                                                   \
    { 4160, 100 }, { 4100,  95 }, { 4050,  90 }, { 4010,  85 },               \
    { 4000,  80 }, { 3910,  75 }, { 3890,  70 }, { 3850,  65 },               \
    { 3840,  60 }, { 3810,  55 }, { 3780,  50 }, { 3760,  45 },               \
    { 3740,  40 }, { 3720,  35 }, { 3690,  30 }, { 3680,  25 },               \
    { 3660,  20 }, { 3640,  15 }, { 3590,  10 }, { 3390,   5 },               \
    { 3000,   0 }

/* Status-bar icon thresholds, from the vendor selector @0x0E7194:
 * charging -> icon 5; complete -> icon 4; else by percent:
 *   <=5 -> 0, <=25 -> 1, <=50 -> 2, <=75 -> 3, else 4. */
#define BYOK_BATT_ICON_PCT_STEPS        { 5, 25, 50, 75 }

/* ==========================================================================
 * 10. WS2812 STATUS LED  [CONFIRMED]  ws2812_create @ file 0x0E93A8
 * ==========================================================================
 * memcpy(&cfg, VA 0x3C19F348, 44) then cfg.clk_div = 2 (0x0E93B8/0x0E93BC),
 * rmt_config(&cfg), rmt_driver_install(cfg.channel, 0, 0).
 * Template bytes @ file 0x03F348:
 *   rmt_mode=0 (TX) channel=0 gpio_num=14 clk_div=80(->2) mem_block_num=1
 * The strip is then created with {max_leds = 1, dev = channel} (0x0E93F8:
 * movi.n a7,1 ; s32i a7,a1,64 ; s32i a8,a1,68 ; call 0x0E982C).
 */
#define BYOK_WS2812_GPIO                14
#define BYOK_WS2812_RMT_CHANNEL         0
#define BYOK_WS2812_RMT_CLK_DIV         2       /* 80 MHz APB / 2 = 25 ns    */
#define BYOK_WS2812_RMT_MEM_BLOCKS      1
#define BYOK_WS2812_COUNT               1

/* ==========================================================================
 * 11. CONSOLE UART
 * ==========================================================================
 * [STRONGLY INDICATED] IDF defaults, unchanged: the image contains
 * "GPIO %d and %d are used as console UART I/O pins" and registers
 * /dev/uart/0 as the primary console plus /dev/secondary backed by
 * usb_serial_jtag_vfs.c (CONFIG_ESP_CONSOLE_SECONDARY_USB_SERIAL_JTAG=y).
 * No uart_set_pin() call targets UART0, so the pins stay at the S3 defaults.
 */
#define BYOK_CONSOLE_UART_PORT          0
#define BYOK_CONSOLE_UART_TX_GPIO       43
#define BYOK_CONSOLE_UART_RX_GPIO       44
#define BYOK_CONSOLE_SECONDARY_USJ      1

/* ==========================================================================
 * 12. COMPLETE GPIO USAGE TABLE (ESP32-S3 package pins)
 * ==========================================================================
 *  GPIO  dir/cfg                       function                      grade
 *  ----  ----------------------------  ----------------------------  ------
 *   1    ADC1_CH0 (no gpio_config)     battery voltage sense         CONF
 *   2    LEDC ch0 output               LCD backlight PWM             CONF
 *   3    SDMMC slot 1                  SD CMD                        CONF
 *   4    SDMMC slot 1                  SD D0                         CONF
 *   5    SDMMC slot 1                  SD CLK                        CONF
 *   6    IN,  no pull, polled          button WAKE (front power)     CONF
 *   7    IN,  no pull, polled          button DOWN                   CONF
 *   8    I2C1                          SDA (TUSB320 + PCF8563)       CONF
 *   9    I2C1                          SCL (TUSB320 + PCF8563)       CONF
 *  10    I2C0                          SCL (display)                 CONF
 *  11    IN,  no pull, polled          button BRIGHTNESS             CONF
 *  12    IN,  pull-up, polled          charger CHRG (active LOW)     CONF
 *  13    IN,  pull-up, polled          charger STDBY (active LOW)    CONF
 *  14    RMT ch0 output                WS2812 data (1 LED)           CONF
 *  15    IN,  no pull, polled          button UP  (USB console mode) CONF
 *  16    IN,  no pull, polled          button EXECUTE                CONF
 *  17    OUT, low at boot              PICO reset (10 ms HIGH pulse) CONF
 *  18    I2C0                          SDA (display)                 CONF
 *  19    (never gpio_config'd)         native USB D-                 CONF
 *  20    (never gpio_config'd)         native USB D+                 CONF
 *  35    OUT, low at boot              PICO request / ready-out      S.IND
 *  36    UART1 TX                      -> PICO RX                    CONF
 *  37    UART1 RX                      <- PICO TX                    CONF
 *  39    OUT, low at boot, never used  UNKNOWN                       UNKN
 *  40    IN,  pull-up, polled          SD card detect (low=present)  CONF
 *  41    IN,  no pull, NEGEDGE + ISR   TUSB320 INT                   CONF
 *  42    OUT, low at boot              power latch (1 = power off)   CONF
 *  43    UART0 TX (IDF default)        console                       S.IND
 *  44    UART0 RX (IDF default)        console                       S.IND
 *  46    IN,  pull-down, ANYEDGE+ISR   PICO READY (high = ready)     CONF
 *  47    OUT, low at boot              PICO transfer strobe          S.IND
 *  48    IN,  pull-down, ANYEDGE+ISR   UNKNOWN second status line    UNKN
 *
 * Complete list of pins named by a gpio_config() in the image (6 call sites,
 * SYS_APP init @0x0B696C plus buttons_init @0x0E9030):
 *   {6,7,11,15,16} {46,48} {17,35,39,42,47} {41} {12,13} {40}
 * Complete list of pins named by a gpio_set_level():  {17,35,39,42,47}
 * Complete list of pins named by a gpio_get_level():  {6,12,13,16,40}
 *                                                     (+ the ISR's own arg)
 *
 * GPIO 12/13 grade note: the pin->status mapping and both active levels are
 * CONFIRMED (three independent open-codings of the same test in the vendor
 * image, plus two consumers that assign distinct "charging"/"full" meanings
 * to the two states). The part-level names
 * CHRG/STDBY (i.e. identifying the driving IC as TP4056-class) are only
 * STRONGLY INDICATED -- an inference from firmware semantics plus the
 * pull-up/open-drain topology, not a schematic or die-marking read.
 * ========================================================================== */

/* ==========================================================================
 * 13. UNRESOLVED — guarded so a build cannot silently assume a value
 * ==========================================================================
 * Define any of these yourself (with a comment saying how you resolved it)
 * to unblock a build that genuinely needs it.
 */

#ifndef BYOK_GPIO39_FUNCTION
#error "BYOK_GPIO39_FUNCTION: GPIO 39 is configured OUTPUT and driven LOW at boot (SYS_APP init 0x0B69DD/0x0B6A1E) and is never written again anywhere in the stock image. Purpose UNKNOWN - resolve from RE or from a board measurement before driving it."
#endif

#ifndef BYOK_GPIO48_FUNCTION
#error "BYOK_GPIO48_FUNCTION: GPIO 48 is INPUT/pull-down/ANYEDGE with an ISR that posts queue values 0 (high) and 1 (low), but nothing in the image consumes those values. Purpose UNKNOWN - resolve from RE (PICO image) or measurement."
#endif

#ifndef BYOK_LCD_CMD_C9_MEANING
#error "BYOK_LCD_CMD_C9_MEANING: the init sequence opens with cmd 0xC9 + data 0xAC, which is not a documented UC1611/UC1611s opcode pair. Replay it verbatim; do not paraphrase it. Resolve from a real gclcd1611/UC1611s datasheet."
#endif

#ifndef BYOK_LCD_CMD_95_MEANING
#error "BYOK_LCD_CMD_95_MEANING: init byte 0x95 (0x90-0x97 group) is not confidently decoded. Replay verbatim. Resolve from a datasheet."
#endif

#ifndef BYOK_LCD_CMD_E1_MEANING
#error "BYOK_LCD_CMD_E1_MEANING: the soft-reset preamble is cmd 0xE1 followed by data 0xE2 (0xE2 is UC1611 'Set System Reset'). Whether 0xE1 is a double-byte opcode taking 0xE2 as its parameter is UNKNOWN. Replay verbatim."
#endif

#ifndef BYOK_LCD_FB_BIT_ORDER
#error "BYOK_LCD_FB_BIT_ORDER: the framebuffer BYTE index is CONFIRMED ((y>>3)*240+x) but the bit-within-byte order (LSB = top row) is only STRONGLY INDICATED from the UC1611 RAM format, never observed. Define as 1 for LSB-top after you have seen pixels on the panel."
#endif

#endif /* BYOK_HW_CONFIG_H */
