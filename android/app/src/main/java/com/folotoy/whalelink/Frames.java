package com.folotoy.whalelink;

import java.nio.ByteBuffer;
import java.nio.ByteOrder;

/**
 * 与挂件之间的私有协议编码。字段定义见固件仓库
 * docs/development/applications/phone-link-protocol.zh_CN.md。
 *
 * 全部小端、定长：设备端不做变长解析，长度不符直接丢包。
 */
public final class Frames {

    public static final int VERSION = 1;

    public static final int KIND_ACCEL = 0;
    public static final int KIND_GYRO = 1;
    public static final int KIND_GRAVITY = 2;
    public static final int KIND_LOCATION = 9;

    public static final int TYPE_BEGIN = 0;
    public static final int TYPE_CHUNK = 1;
    public static final int TYPE_END = 2;

    public static final int FMT_RGB565_RLE = 0;
    /** 裸 RGB565，整帧原样搬运，设备端只做 memcpy。 */
    public static final int FMT_RAW = 1;
    /**
     * 增量：只发变化过的 8×8 块。载荷 = [块数][块边长] + n×([tx][ty] + 块像素)。
     * 设备端按坐标直接贴进位图，只重绘那一小块。
     */
    public static final int FMT_TILE = 2;

    // ---- 文件传输（手机 → 设备 Flash 槽位）----
    public static final int FILE_CMD_BEGIN = 1;
    public static final int FILE_CMD_DATA = 2;
    public static final int FILE_CMD_END = 3;
    public static final int FILE_CMD_LIST = 4;
    public static final int FILE_CMD_DELETE = 5;
    public static final int FILE_CMD_VERIFY = 6;
    /** 碎片整理：把有效区段前移合并空洞（设备池子是变长分配，删大文件会留洞）。 */
    public static final int FILE_CMD_COMPACT = 7;

    /**
     * 设备端条目上限（固件 whale_file.h 的 WHALE_FILE_SLOT_COUNT）。
     *
     * 注意这里不是「分区被切成 16 份」—— storage 是一整块约 4.94MB 的池子，
     * 每个文件按实际大小分配连续区段，16 只是条目数的上限。
     */
    public static final int FILE_MAX_ENTRIES = 16;

    public static final int FILE_NAME_LEN = 12;

    // 设备 notify 回的状态码（与固件 whale_ble.h 对齐）
    public static final int FILE_ST_BEGIN_ACK = 1;
    public static final int FILE_ST_PROGRESS = 2;
    public static final int FILE_ST_DONE_OK = 3;
    public static final int FILE_ST_DONE_BAD_CRC = 4;
    public static final int FILE_ST_REJECT = 5;
    public static final int FILE_ST_SEQ_ERR = 6;
    public static final int FILE_ST_FLASH_ERR = 7;
    public static final int FILE_ST_SLOT_INFO = 8;
    public static final int FILE_ST_DELETED = 9;
    public static final int FILE_ST_SLOT_EMPTY = 10;
    public static final int FILE_ST_COMPACT_OK = 11;
    public static final int FILE_ST_COMPACT_ERR = 12;
    /**
     * 列表结束。slot 字段 = 条目数，totalLen 字段 = 设备当前能一次分配出的
     * 最大空闲字节数（已扣碎片）。
     *
     * 设备只回报【有内容】的条目再补这一条，不再逐个报空位 —— 条目上限 16，
     * 全报就是 16 条 notify，连续压 mbuf 容易丢包，而手机真正要知道的只是
     * 「有哪些」和「还能放多大」。
     */
    public static final int FILE_ST_LIST_END = 13;

    private Frames() {
    }

    /** 8 字节：version / flags / tz_min / unix_sec */
    public static byte[] time(long unixSec, int tzMinutes) {
        ByteBuffer b = ByteBuffer.allocate(8).order(ByteOrder.LITTLE_ENDIAN);
        b.put((byte) VERSION);
        b.put((byte) 0x03);                    // bit0 有效 + bit1 带时区
        b.putShort((short) tzMinutes);
        b.putInt((int) unixSec);
        return b.array();
    }

    /** 24 字节：version / kind / flags / seq / t_ms / v[3] / aux */
    public static byte[] sensor(int kind, int seq, long tMs,
                                int v0, int v1, int v2, int aux) {
        ByteBuffer b = ByteBuffer.allocate(24).order(ByteOrder.LITTLE_ENDIAN);
        b.put((byte) VERSION);
        b.put((byte) kind);
        b.put((byte) 0x01);
        b.put((byte) (seq & 0xFF));
        b.putInt((int) tMs);
        b.putInt(v0);
        b.putInt(v1);
        b.putInt(v2);
        b.putInt(aux);
        return b.array();
    }

    /** 投屏分块头，16 字节 */
    public static byte[] frameHeader(int type, int fmt, int flags,
                                     int w, int h, int seq,
                                     int idx, int total, int len) {
        ByteBuffer b = ByteBuffer.allocate(16).order(ByteOrder.LITTLE_ENDIAN);
        b.put((byte) VERSION);
        b.put((byte) type);
        b.put((byte) fmt);
        b.put((byte) flags);
        b.putShort((short) w);
        b.putShort((short) h);
        b.putShort((short) seq);
        b.putShort((short) idx);
        b.putShort((short) total);
        b.putShort((short) len);
        return b.array();
    }

    /**
     * 文件传输块头，16 字节。与 frameHeader 同长但字段语义不同：
     * ver / cmd / slot / 保留 / totalLen(u32) / idx / totalBlocks / plen / 保留。
     * BEGIN 的净荷由 caller 另拼（crc32 + 文件名），这里只出头部。
     */
    public static byte[] fileHeader(int cmd, int slot, int totalLen,
                                    int idx, int totalBlocks, int plen) {
        ByteBuffer b = ByteBuffer.allocate(16).order(ByteOrder.LITTLE_ENDIAN);
        b.put((byte) VERSION);
        b.put((byte) cmd);
        b.put((byte) slot);
        b.put((byte) 0);
        b.putInt(totalLen);
        b.putShort((short) idx);
        b.putShort((short) totalBlocks);
        b.putShort((short) plen);
        b.putShort((short) 0);
        return b.array();
    }

    /** BEGIN 净荷：crc32(u32) + 文件名（NUL 填充到 12 字节）。 */
    public static byte[] fileBeginPayload(int crc32, String name) {
        ByteBuffer b = ByteBuffer.allocate(4 + FILE_NAME_LEN).order(ByteOrder.LITTLE_ENDIAN);
        b.putInt(crc32);
        byte[] n = name.getBytes(java.nio.charset.StandardCharsets.US_ASCII);
        int copy = Math.min(n.length, FILE_NAME_LEN);
        b.put(n, 0, copy);
        for (int i = copy; i < FILE_NAME_LEN; i++) {
            b.put((byte) 0);
        }
        return b.array();
    }
}
